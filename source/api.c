/* Sonoran JSON API client. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "api.h"
#include "https.h"

/* Arizona does not use daylight saving time. It is UTC-7 all year. */
#define AZ_OFFSET_HOURS (-7)

void to_ascii(char *dst, size_t dstlen, const char *src)
{
    size_t o = 0;
    const unsigned char *s = (const unsigned char *)(src ? src : "");

    while (*s && o + 1 < dstlen) {
        unsigned char c = *s;
        if (c < 0x80) {
            dst[o++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : (char)c;
            s++;
            continue;
        }
        /* Skip the whole multi-byte sequence. */
        s++;
        while ((*s & 0xC0) == 0x80)
            s++;
        /* Some common marks have a plain form. */
        dst[o++] = '?';
    }
    dst[o] = '\0';
}

static int days_in_month(int y, int m)
{
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return d[m - 1];
}

/* "2026-10-19T22:31:48.1Z" (UTC) to "10-19 15:31" (Arizona). */
static void az_time(char *dst, size_t dstlen, const char *iso)
{
    int y, mo, d, h, mi;

    if (iso == NULL || sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) {
        snprintf(dst, dstlen, "?");
        return;
    }
    h += AZ_OFFSET_HOURS;
    if (h < 0) {
        h += 24;
        if (--d < 1) {
            if (--mo < 1) {
                mo = 12;
                y--;
            }
            d = days_in_month(y, mo);
        }
    }
    snprintf(dst, dstlen, "%02d-%02d %02d:%02d", mo, d, h, mi);
}

static const char *str_of(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int fetch_array(const char *path, const char *token, cJSON **root,
                       char *total, char *err, size_t errlen)
{
    struct http_reply r;

    (void)total;
    char cookie[4200];
    const char *send = NULL;

    if (token != NULL && token[0] != '\0') {
        if (strchr(token, '=') != NULL)
            snprintf(cookie, sizeof(cookie), "%s", token);
        else
            snprintf(cookie, sizeof(cookie), "%s=%s", SESSION_COOKIE, token);
        send = cookie;
    }
    if (https_get(SONORAN_HOST, path, send, &r) != 0) {
        snprintf(err, errlen, "%s", r.err[0] ? r.err : "request failed");
        http_free(&r);
        return API_FAILED;
    }
    if (r.status == 302 || r.status == 401 || r.status == 403) {
        snprintf(err, errlen, "Login needed (HTTP %d). The token is missing "
                 "or expired.", r.status);
        http_free(&r);
        return API_LOGIN;
    }
    if (r.status != 200) {
        snprintf(err, errlen, "Server answered HTTP %d", r.status);
        http_free(&r);
        return API_FAILED;
    }
    *root = cJSON_ParseWithLength(r.body, r.len);
    http_free(&r);
    if (!cJSON_IsArray(*root)) {
        snprintf(err, errlen, "reply is not a JSON list");
        cJSON_Delete(*root);
        *root = NULL;
        return API_FAILED;
    }
    return API_OK;
}

int api_chat(const char *token, struct chat_msg *out, int max, int *count,
             char *err, size_t errlen)
{
    char path[64];
    cJSON *root = NULL, *item;
    int rc, n = 0;

    snprintf(path, sizeof(path), "/api/chat?limit=%d", max);
    rc = fetch_array(path, token, &root, NULL, err, errlen);
    if (rc != API_OK)
        return rc;
    cJSON_ArrayForEach(item, root) {
        struct chat_msg *m;
        const char *from;
        if (n >= max)
            break;
        m = &out[n++];
        az_time(m->when, sizeof(m->when), str_of(item, "seen_at"));
        from = str_of(item, "from_name");
        to_ascii(m->from, sizeof(m->from), from ? from : str_of(item, "from_node"));
        to_ascii(m->channel, sizeof(m->channel), str_of(item, "channel"));
        to_ascii(m->body, sizeof(m->body), str_of(item, "body"));
    }
    cJSON_Delete(root);
    *count = n;
    return API_OK;
}

int api_nodes(const char *token, struct node_row *out, int max, int *count,
              int *total, char *err, size_t errlen)
{
    char path[80];
    cJSON *root = NULL, *item;
    int rc, n = 0;

    snprintf(path, sizeof(path), "/api/nodes?limit=%d&sort=last_heard&order=desc", max);
    rc = fetch_array(path, token, &root, NULL, err, errlen);
    if (rc != API_OK)
        return rc;
    cJSON_ArrayForEach(item, root) {
        struct node_row *nd;
        if (n >= max)
            break;
        nd = &out[n++];
        to_ascii(nd->id, sizeof(nd->id), str_of(item, "node_id"));
        to_ascii(nd->name, sizeof(nd->name), str_of(item, "display_name"));
        to_ascii(nd->short_name, sizeof(nd->short_name), str_of(item, "short_name"));
        to_ascii(nd->hw, sizeof(nd->hw), str_of(item, "hw_model"));
        to_ascii(nd->role, sizeof(nd->role), str_of(item, "role"));
        az_time(nd->heard, sizeof(nd->heard), str_of(item, "last_heard"));
        nd->gateway = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "is_mqtt_gateway"));
        nd->has_position = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "has_position"));
    }
    cJSON_Delete(root);
    *count = n;
    if (total)
        *total = n; /* X-Total-Count is not read in v0.1 */
    return API_OK;
}

int api_login(const char *user, const char *password, char *cookie, size_t cookielen,
              char *err, size_t errlen)
{
    struct http_reply r;
    cJSON *req, *ans;
    char *json;
    int rc;

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "username", user);
    cJSON_AddStringToObject(req, "password", password);
    cJSON_AddBoolToObject(req, "keepMeLoggedIn", 1);
    cJSON_AddStringToObject(req, "targetURL", "https://" SONORAN_HOST "/");
    cJSON_AddStringToObject(req, "requestMethod", "GET");
    json = cJSON_PrintUnformatted(req);
    /* cJSON keeps a copy of the password. Wipe it before the free. */
    cJSON_ArrayForEach(ans, req) {
        if (cJSON_IsString(ans))
            memset(ans->valuestring, 0, strlen(ans->valuestring));
    }
    cJSON_Delete(req);
    if (json == NULL) {
        snprintf(err, errlen, "out of memory");
        return API_FAILED;
    }

    rc = https_request("POST", AUTH_HOST, "/api/firstfactor", NULL, SESSION_COOKIE,
                       json, &r);
    memset(json, 0, strlen(json));
    cJSON_free(json);
    if (rc != 0) {
        snprintf(err, errlen, "%s", r.err[0] ? r.err : "request failed");
        http_free(&r);
        return API_FAILED;
    }
    if (r.status == 401) {
        snprintf(err, errlen, "Wrong username or password.");
        http_free(&r);
        return API_LOGIN;
    }
    if (r.status == 403) {
        snprintf(err, errlen, "Authelia refused the login (HTTP 403). "
                 "Too many attempts? Wait and try again.");
        http_free(&r);
        return API_FAILED;
    }
    if (r.status != 200 || r.set_cookie[0] == '\0') {
        snprintf(err, errlen, "Login failed (HTTP %d%s)", r.status,
                 r.set_cookie[0] ? "" : ", no session cookie");
        http_free(&r);
        return API_FAILED;
    }
    snprintf(cookie, cookielen, "%s", r.set_cookie);
    memset(r.set_cookie, 0, sizeof(r.set_cookie));
    http_free(&r);
    return API_OK;
}
