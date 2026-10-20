/* HTTPS GET over TLS 1.3 with wolfSSL.
 * The Sonoran hosts are behind Cloudflare. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

#include "https.h"
#include "net.h"
#include "cacerts.h"

#define MAX_REPLY (4 * 1024 * 1024)

static WOLFSSL_CTX *ctx;
static char last_tls[64];

const char *https_last_tls(void)
{
    return last_tls;
}

static int io_recv(WOLFSSL *ssl, char *buf, int sz, void *arg)
{
    int handle = *(int *)arg;
    int n = tcp_recv(handle, buf, sz);
    (void)ssl;
    if (n > 0)
        return n;
    if (n == 0)
        return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return WOLFSSL_CBIO_ERR_GENERAL;
}

static int io_send(WOLFSSL *ssl, char *buf, int sz, void *arg)
{
    int handle = *(int *)arg;
    int n = tcp_send(handle, buf, sz);
    (void)ssl;
    if (n > 0)
        return n;
    return WOLFSSL_CBIO_ERR_GENERAL;
}

int https_init(char *err, size_t errlen)
{
    int ret;

    if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
        snprintf(err, errlen, "wolfSSL_Init failed");
        return -1;
    }
    /* TLS 1.2 or 1.3. Older versions are not in this build (NO_OLD_TLS). */
    ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (ctx == NULL) {
        snprintf(err, errlen, "wolfSSL_CTX_new failed");
        return -1;
    }
    /* Some roots in the bundle use algorithms that this build does not
     * have. Skip them and keep the others. */
    ret = wolfSSL_CTX_load_verify_buffer_ex(ctx, (const unsigned char *)CA_BUNDLE,
                                            (long)sizeof(CA_BUNDLE) - 1,
                                            WOLFSSL_FILETYPE_PEM, 0,
                                            WOLFSSL_LOAD_FLAG_IGNORE_ERR);
    if (ret != WOLFSSL_SUCCESS) {
        snprintf(err, errlen, "cannot load root certificates (%d)", ret);
        return -1;
    }
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, NULL);
    wolfSSL_CTX_SetIORecv(ctx, io_recv);
    wolfSSL_CTX_SetIOSend(ctx, io_send);
    return 0;
}

static void tls_error(WOLFSSL *ssl, int ret, char *err, size_t errlen)
{
    int code = wolfSSL_get_error(ssl, ret);
    char text[80];

    wolfSSL_ERR_error_string_n((unsigned long)code, text, sizeof(text));
    if (code == ASN_AFTER_DATE_E || code == ASN_BEFORE_DATE_E)
        snprintf(err, errlen, "TLS: certificate date is not valid. "
                 "Set the Wii clock. (%d)", code);
    else
        snprintf(err, errlen, "TLS error %d: %s", code, text);
}

/* Find a header value in the head block. Return length, or -1. */
static int header_value(const char *head, const char *name, char *out, size_t outlen)
{
    size_t nlen = strlen(name);
    const char *p = strstr(head, "\r\n");

    while (p != NULL && p[2] != '\r') {
        const char *line = p + 2;
        const char *end = strstr(line, "\r\n");
        if (end == NULL)
            break;
        if ((size_t)(end - line) > nlen && strncasecmp(line, name, nlen) == 0
            && line[nlen] == ':') {
            const char *v = line + nlen + 1;
            size_t vlen;
            while (*v == ' ' || *v == '\t')
                v++;
            vlen = (size_t)(end - v);
            if (vlen >= outlen)
                vlen = outlen - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return (int)vlen;
        }
        p = end;
    }
    return -1;
}

/* Decode a chunked body in place. Return the new length, or -1. */
static long dechunk(char *body, size_t len)
{
    size_t in = 0, out = 0;

    for (;;) {
        char *end;
        unsigned long size;
        char *crlf;

        if (in >= len)
            return -1;
        size = strtoul(body + in, &end, 16);
        crlf = strstr(end, "\r\n");
        if (crlf == NULL)
            return -1;
        in = (size_t)(crlf - body) + 2;
        if (size == 0)
            break;
        if (in + size > len)
            return -1;
        memmove(body + out, body + in, size);
        out += size;
        in += size + 2;
    }
    body[out] = '\0';
    return (long)out;
}

/* Copy the value of the Set-Cookie header for the cookie name. */
static void find_set_cookie(const char *head, const char *name, char *out, size_t outlen)
{
    const char *p = head;
    size_t nlen = strlen(name);

    out[0] = '\0';
    while ((p = strstr(p, "\r\n")) != NULL) {
        const char *v;
        size_t vlen;
        p += 2;
        if (strncasecmp(p, "Set-Cookie:", 11) != 0)
            continue;
        v = p + 11;
        while (*v == ' ')
            v++;
        if (strncmp(v, name, nlen) != 0 || v[nlen] != '=')
            continue;
        vlen = strcspn(v, ";\r\n");
        if (vlen >= outlen)
            vlen = outlen - 1;
        memcpy(out, v, vlen);
        out[vlen] = '\0';
        return;
    }
}

int https_get(const char *host, const char *path, const char *cookie,
              struct http_reply *reply)
{
    return https_request("GET", host, path, cookie, NULL, NULL, reply);
}

int https_request(const char *method, const char *host, const char *path,
                  const char *cookie, const char *want_cookie, const char *json,
                  struct http_reply *reply)
{
    char *headers = NULL;
    int rc;

    if (cookie != NULL) {
        headers = malloc(strlen(cookie) + 16);
        if (headers == NULL) {
            memset(reply, 0, sizeof(*reply));
            snprintf(reply->err, sizeof(reply->err), "out of memory");
            return -1;
        }
        sprintf(headers, "Cookie: %s\r\n", cookie);
    }
    rc = https_request_h(method, host, path, headers, want_cookie, json, reply);
    if (headers != NULL) {
        memset(headers, 0, strlen(headers));
        free(headers);
    }
    return rc;
}

int https_request_h(const char *method, const char *host, const char *path,
                    const char *headers, const char *want_cookie, const char *json,
                    struct http_reply *reply)
{
    WOLFSSL *ssl = NULL;
    int handle = -1;
    char *req = NULL;
    char *buf = NULL;
    size_t cap = 64 * 1024, used = 0;
    int ret, result = -1;
    char *split;
    char value[64];

    memset(reply, 0, sizeof(*reply));

    dbg("%s %s%s: connect", method, host, path);
    handle = tcp_open(host, 443, reply->err, sizeof(reply->err));
    if (handle < 0) {
        dbg("tcp_open failed: %s", reply->err);
        return -1;
    }

    ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        snprintf(reply->err, sizeof(reply->err), "wolfSSL_new failed");
        goto done;
    }
    wolfSSL_SetIOReadCtx(ssl, &handle);
    wolfSSL_SetIOWriteCtx(ssl, &handle);
    wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, (unsigned short)strlen(host));
    wolfSSL_check_domain_name(ssl, host);

    ret = wolfSSL_connect(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        tls_error(ssl, ret, reply->err, sizeof(reply->err));
        dbg("handshake failed: %s", reply->err);
        goto done;
    }
    dbg("handshake ok");
    snprintf(last_tls, sizeof(last_tls), "%s %s", wolfSSL_get_version(ssl),
             wolfSSL_get_cipher(ssl));

    req = malloc(4096 + (headers ? strlen(headers) : 0) + (json ? strlen(json) : 0));
    if (req == NULL)
        goto nomem;
    ret = sprintf(req,
                  "%s %s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "User-Agent: SonoranWii/0.3 (personal Wii homebrew viewer for s.azmsh.dev)\r\n"
                  "Accept: application/json\r\n"
                  "Accept-Encoding: identity\r\n"
                  "Connection: close\r\n"
                  "%s",
                  method, path, host, headers ? headers : "");
    if (json != NULL)
        ret += sprintf(req + ret, "Content-Type: application/json\r\n"
                       "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(json), json);
    else
        ret += sprintf(req + ret, "\r\n");
    if (wolfSSL_write(ssl, req, ret) != ret) {
        tls_error(ssl, ret, reply->err, sizeof(reply->err));
        goto done;
    }

    buf = malloc(cap + 1);
    if (buf == NULL)
        goto nomem;
    for (;;) {
        if (used == cap) {
            char *bigger;
            if (cap >= MAX_REPLY) {
                snprintf(reply->err, sizeof(reply->err), "reply is too large");
                goto done;
            }
            cap *= 2;
            bigger = realloc(buf, cap + 1);
            if (bigger == NULL)
                goto nomem;
            buf = bigger;
        }
        ret = wolfSSL_read(ssl, buf + used, (int)(cap - used));
        if (ret > 0) {
            used += (size_t)ret;
            continue;
        }
        ret = wolfSSL_get_error(ssl, ret);
        if (ret == WOLFSSL_ERROR_ZERO_RETURN || ret == SOCKET_PEER_CLOSED_E)
            break;
        if (used > 0)
            break; /* the server closed without close_notify */
        tls_error(ssl, ret, reply->err, sizeof(reply->err));
        goto done;
    }
    buf[used] = '\0';
    entropy_add(used);
    dbg("read %u bytes", (unsigned)used);

    split = strstr(buf, "\r\n\r\n");
    if (split == NULL || strncmp(buf, "HTTP/1.", 7) != 0) {
        snprintf(reply->err, sizeof(reply->err), "reply is not HTTP");
        goto done;
    }
    split[2] = '\0'; /* head ends with one CRLF now */
    reply->status = atoi(buf + 9);
    header_value(buf, "Location", reply->location, sizeof(reply->location));
    if (want_cookie != NULL)
        find_set_cookie(buf, want_cookie, reply->set_cookie, sizeof(reply->set_cookie));
    value[0] = '\0';
    header_value(buf, "Transfer-Encoding", value, sizeof(value));

    reply->len = used - (size_t)(split + 4 - buf);
    memmove(buf, split + 4, reply->len);
    buf[reply->len] = '\0';
    if (strcasecmp(value, "chunked") == 0) {
        long n = dechunk(buf, reply->len);
        if (n < 0) {
            snprintf(reply->err, sizeof(reply->err), "bad chunked body");
            goto done;
        }
        reply->len = (size_t)n;
    }
    dbg("HTTP %d, body %u bytes, set-cookie %s", reply->status, (unsigned)reply->len,
        reply->set_cookie[0] ? "yes" : "no");
    reply->body = buf;
    buf = NULL;
    result = 0;
    goto done;

nomem:
    snprintf(reply->err, sizeof(reply->err), "out of memory");
done:
    if (req != NULL)
        memset(req, 0, 4096 + (headers ? strlen(headers) : 0) + (json ? strlen(json) : 0));
    free(req);
    free(buf);
    if (ssl != NULL) {
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
    }
    if (handle >= 0)
        tcp_close(handle);
    return result;
}

void http_free(struct http_reply *reply)
{
    free(reply->body);
    reply->body = NULL;
}

int tls_open(struct tls_conn *c, const char *host, char *err, size_t errlen)
{
    WOLFSSL *ssl;
    int ret;

    c->ssl = NULL;
    c->handle = tcp_open(host, 443, err, errlen);
    if (c->handle < 0)
        return -1;
    ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        snprintf(err, errlen, "wolfSSL_new failed");
        tcp_close(c->handle);
        return -1;
    }
    wolfSSL_SetIOReadCtx(ssl, &c->handle);
    wolfSSL_SetIOWriteCtx(ssl, &c->handle);
    wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, (unsigned short)strlen(host));
    wolfSSL_check_domain_name(ssl, host);
    ret = wolfSSL_connect(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        tls_error(ssl, ret, err, errlen);
        wolfSSL_free(ssl);
        tcp_close(c->handle);
        return -1;
    }
    c->ssl = ssl;
    dbg("tls_open %s: ok", host);
    return 0;
}

int tls_write_all(struct tls_conn *c, const void *data, int len)
{
    return wolfSSL_write((WOLFSSL *)c->ssl, data, len) == len ? 0 : -1;
}

int tls_read(struct tls_conn *c, void *buf, int len)
{
    int n = wolfSSL_read((WOLFSSL *)c->ssl, buf, len);

    if (n > 0)
        return n;
    n = wolfSSL_get_error((WOLFSSL *)c->ssl, n);
    if (n == WOLFSSL_ERROR_ZERO_RETURN || n == SOCKET_PEER_CLOSED_E)
        return 0;
    return -1;
}

int tls_readable(struct tls_conn *c, int ms)
{
    if (wolfSSL_pending((WOLFSSL *)c->ssl) > 0)
        return 1;
    return tcp_wait_readable(c->handle, ms) > 0;
}

void tls_close(struct tls_conn *c)
{
    if (c->ssl != NULL) {
        wolfSSL_shutdown((WOLFSSL *)c->ssl);
        wolfSSL_free((WOLFSSL *)c->ssl);
        c->ssl = NULL;
    }
    if (c->handle >= 0)
        tcp_close(c->handle);
    c->handle = -1;
}
