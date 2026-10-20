/* Small Discord client: REST API v9 with a user session token.
 * Only plain requests that the user starts. Nothing runs by itself. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "api.h"
#include "discord.h"
#include "https.h"
#include "net.h"

static void wipe_json(cJSON *obj)
{
    cJSON *v;

    cJSON_ArrayForEach(v, obj) {
        if (cJSON_IsString(v))
            memset(v->valuestring, 0, strlen(v->valuestring));
    }
}

/* Send a request. auth may be NULL. body (JSON) may be NULL. */
static int dc_call(const char *method, const char *path, const char *auth, cJSON *body,
                   struct http_reply *r, cJSON **answer)
{
    char *headers = NULL, *json = NULL;
    int rc;

    if (auth != NULL) {
        headers = malloc(strlen(auth) + 32);
        if (headers == NULL)
            return -1;
        sprintf(headers, "Authorization: %s\r\n", auth);
    }
    if (body != NULL)
        json = cJSON_PrintUnformatted(body);
    rc = https_request_h(method, DISCORD_HOST, path, headers, NULL, json, r);
    if (headers != NULL) {
        memset(headers, 0, strlen(headers));
        free(headers);
    }
    if (json != NULL) {
        memset(json, 0, strlen(json));
        cJSON_free(json);
    }
    if (rc != 0)
        return -1;
    *answer = r->body ? cJSON_ParseWithLength(r->body, r->len) : NULL;
    return 0;
}

static const char *str_in(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* Read the token or the reason from a login answer. */
static int login_answer(struct http_reply *r, cJSON *a, char *token, size_t tokenlen,
                        char *ticket, size_t ticketlen, char *err, size_t errlen)
{
    const char *t = str_in(a, "token");
    char *text = r->body ? r->body : "";

    if (r->status == 200 && t != NULL) {
        snprintf(token, tokenlen, "%s", t);
        return DC_OK;
    }
    if (r->status == 200 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "mfa"))) {
        const char *tk = str_in(a, "ticket");
        if (ticket != NULL && tk != NULL)
            snprintf(ticket, ticketlen, "%s", tk);
        snprintf(err, errlen, "Discord asks for your 2FA code.");
        return DC_NEED_MFA;
    }
    if (cJSON_GetObjectItemCaseSensitive(a, "captcha_key") != NULL) {
        snprintf(err, errlen, "Discord asks for a captcha. The Wii cannot show it. "
                 "Log in to Discord in a browser one time, then try again later.");
        return DC_CAPTCHA;
    }
    if (strstr(text, "ACCOUNT_LOGIN_VERIFICATION_EMAIL") != NULL) {
        snprintf(err, errlen, "Discord sent you an email to allow this new login. "
                 "Allow it, then log in again.");
        return DC_VERIFY;
    }
    if (strstr(text, "INVALID_LOGIN") != NULL || strstr(text, "INVALID_PASSWORD") != NULL
        || strstr(text, "Invalid two-factor") != NULL || r->status == 401) {
        snprintf(err, errlen, "Discord: wrong email, password or code.");
        return DC_BAD_LOGIN;
    }
    snprintf(err, errlen, "Discord login failed (HTTP %d): %.80s", r->status, text);
    return DC_FAILED;
}

int dc_login(const char *email, const char *password, char *token, size_t tokenlen,
             char *ticket, size_t ticketlen, char *err, size_t errlen)
{
    struct http_reply r;
    cJSON *body = cJSON_CreateObject(), *a = NULL;
    int rc;

    cJSON_AddStringToObject(body, "login", email);
    cJSON_AddStringToObject(body, "password", password);
    cJSON_AddBoolToObject(body, "undelete", 0);
    rc = dc_call("POST", "/api/v9/auth/login", NULL, body, &r, &a);
    wipe_json(body);
    cJSON_Delete(body);
    if (rc != 0) {
        snprintf(err, errlen, "%s", r.err);
        http_free(&r);
        return DC_FAILED;
    }
    dbg("discord login: HTTP %d", r.status);
    rc = login_answer(&r, a, token, tokenlen, ticket, ticketlen, err, errlen);
    if (a != NULL) {
        wipe_json(a);
        cJSON_Delete(a);
    }
    if (r.body)
        memset(r.body, 0, r.len);
    http_free(&r);
    return rc;
}

int dc_mfa(const char *ticket, const char *code, char *token, size_t tokenlen,
           char *err, size_t errlen)
{
    struct http_reply r;
    cJSON *body = cJSON_CreateObject(), *a = NULL;
    int rc;

    cJSON_AddStringToObject(body, "code", code);
    cJSON_AddStringToObject(body, "ticket", ticket);
    rc = dc_call("POST", "/api/v9/auth/mfa/totp", NULL, body, &r, &a);
    wipe_json(body);
    cJSON_Delete(body);
    if (rc != 0) {
        snprintf(err, errlen, "%s", r.err);
        http_free(&r);
        return DC_FAILED;
    }
    dbg("discord mfa: HTTP %d", r.status);
    rc = login_answer(&r, a, token, tokenlen, NULL, 0, err, errlen);
    if (a != NULL) {
        wipe_json(a);
        cJSON_Delete(a);
    }
    if (r.body)
        memset(r.body, 0, r.len);
    http_free(&r);
    return rc;
}

/* GET a list. Return DC_OK and the parsed array in *list. */
static int get_list(const char *token, const char *path, cJSON **list, char *err, size_t errlen)
{
    struct http_reply r;
    int rc = dc_call("GET", path, token, NULL, &r, list);

    if (rc != 0) {
        snprintf(err, errlen, "%s", r.err);
        http_free(&r);
        return DC_FAILED;
    }
    dbg("discord GET %s: HTTP %d, %u bytes", path, r.status, (unsigned)r.len);
    if (r.status == 401) {
        snprintf(err, errlen, "Discord session is not valid. Push 1 to log in.");
        rc = DC_BAD_LOGIN;
    } else if (r.status != 200 || !cJSON_IsArray(*list)) {
        snprintf(err, errlen, "Discord answered HTTP %d", r.status);
        rc = DC_FAILED;
    } else {
        rc = DC_OK;
    }
    if (rc != DC_OK) {
        cJSON_Delete(*list);
        *list = NULL;
    }
    http_free(&r);
    return rc;
}

int dc_guilds(const char *token, struct dc_item *out, int max, int *count,
              char *err, size_t errlen)
{
    cJSON *list, *g;
    int rc = get_list(token, "/api/v9/users/@me/guilds", &list, err, errlen), n = 0;

    if (rc != DC_OK)
        return rc;
    cJSON_ArrayForEach(g, list) {
        if (n >= max)
            break;
        to_ascii(out[n].id, sizeof(out[n].id), str_in(g, "id"));
        to_ascii(out[n].name, sizeof(out[n].name), str_in(g, "name"));
        n++;
    }
    cJSON_Delete(list);
    *count = n;
    return DC_OK;
}

static int by_position(const void *a, const void *b)
{
    const cJSON *x = *(const cJSON *const *)a, *y = *(const cJSON *const *)b;
    int px = cJSON_GetObjectItemCaseSensitive(x, "position")->valueint;
    int py = cJSON_GetObjectItemCaseSensitive(y, "position")->valueint;
    return px - py;
}

int dc_channels(const char *token, const char *guild, struct dc_item *out, int max,
                int *count, char *err, size_t errlen)
{
    char path[96];
    cJSON *list, *c, *pick[500];
    int rc, n = 0, k = 0, i;

    snprintf(path, sizeof(path), "/api/v9/guilds/%s/channels", guild);
    rc = get_list(token, path, &list, err, errlen);
    if (rc != DC_OK)
        return rc;
    /* Text (0) and announcement (5) channels only. */
    cJSON_ArrayForEach(c, list) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(c, "type");
        if (k < 500 && cJSON_IsNumber(type) && (type->valueint == 0 || type->valueint == 5)
            && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(c, "position")))
            pick[k++] = c;
    }
    qsort(pick, (size_t)k, sizeof(pick[0]), by_position);
    for (i = 0; i < k && n < max; i++, n++) {
        to_ascii(out[n].id, sizeof(out[n].id), str_in(pick[i], "id"));
        snprintf(out[n].name, sizeof(out[n].name), "#");
        to_ascii(out[n].name + 1, sizeof(out[n].name) - 1, str_in(pick[i], "name"));
    }
    cJSON_Delete(list);
    *count = n;
    return DC_OK;
}

/* "2026-10-20T15:15:00.123+00:00" (UTC) to "10-20 08:15" (Arizona, UTC-7). */
static void az_short(char *dst, size_t dstlen, const char *iso)
{
    int y, mo, d, h, mi;
    static const int dim[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (iso == NULL || sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) {
        snprintf(dst, dstlen, "?");
        return;
    }
    h -= 7;
    if (h < 0) {
        h += 24;
        if (--d < 1) {
            if (--mo < 1) {
                mo = 12;
                y--;
            }
            d = dim[mo - 1] + (mo == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        }
    }
    snprintf(dst, dstlen, "%02d-%02d %02d:%02d", mo, d, h, mi);
}

int dc_messages(const char *token, const char *channel, struct dc_msg *out, int max,
                int *count, char *err, size_t errlen)
{
    char path[96];
    cJSON *list, *m;
    int rc, n = 0;

    snprintf(path, sizeof(path), "/api/v9/channels/%s/messages?limit=%d", channel, max);
    rc = get_list(token, path, &list, err, errlen);
    if (rc != DC_OK)
        return rc;
    cJSON_ArrayForEach(m, list) {
        const cJSON *author = cJSON_GetObjectItemCaseSensitive(m, "author");
        const char *name = str_in(author, "global_name");
        const cJSON *att = cJSON_GetObjectItemCaseSensitive(m, "attachments");
        const char *text = str_in(m, "content");
        if (n >= max)
            break;
        if (name == NULL)
            name = str_in(author, "username");
        az_short(out[n].when, sizeof(out[n].when), str_in(m, "timestamp"));
        to_ascii(out[n].author, sizeof(out[n].author), name);
        if ((text == NULL || text[0] == '\0') && cJSON_GetArraySize(att) > 0)
            text = "[attachment]";
        to_ascii(out[n].text, sizeof(out[n].text), text);
        n++;
    }
    cJSON_Delete(list);
    *count = n;
    return DC_OK;
}

int dc_send(const char *token, const char *channel, const char *text,
            char *err, size_t errlen)
{
    char path[96];
    struct http_reply r;
    cJSON *body = cJSON_CreateObject(), *a = NULL;
    int rc;

    snprintf(path, sizeof(path), "/api/v9/channels/%s/messages", channel);
    cJSON_AddStringToObject(body, "content", text);
    rc = dc_call("POST", path, token, body, &r, &a);
    cJSON_Delete(body);
    cJSON_Delete(a);
    if (rc != 0) {
        snprintf(err, errlen, "%s", r.err);
        http_free(&r);
        return DC_FAILED;
    }
    dbg("discord send: HTTP %d", r.status);
    if (r.status == 200) {
        rc = DC_OK;
    } else {
        snprintf(err, errlen, "Send failed (HTTP %d)", r.status);
        rc = r.status == 401 ? DC_BAD_LOGIN : DC_FAILED;
    }
    http_free(&r);
    return rc;
}
