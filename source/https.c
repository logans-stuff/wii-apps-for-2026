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

int https_get(const char *host, const char *path, const char *cookie,
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

    handle = tcp_open(host, 443, reply->err, sizeof(reply->err));
    if (handle < 0)
        return -1;

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
        goto done;
    }
    snprintf(last_tls, sizeof(last_tls), "%s %s", wolfSSL_get_version(ssl),
             wolfSSL_get_cipher(ssl));

    req = malloc(4096 + (cookie ? strlen(cookie) : 0));
    if (req == NULL)
        goto nomem;
    ret = sprintf(req,
                  "GET %s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "User-Agent: SonoranWii/0.1\r\n"
                  "Accept: application/json\r\n"
                  "Accept-Encoding: identity\r\n"
                  "Connection: close\r\n"
                  "%s%s%s"
                  "\r\n",
                  path, host,
                  cookie ? "Cookie: " : "", cookie ? cookie : "",
                  cookie ? "\r\n" : "");
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

    split = strstr(buf, "\r\n\r\n");
    if (split == NULL || strncmp(buf, "HTTP/1.", 7) != 0) {
        snprintf(reply->err, sizeof(reply->err), "reply is not HTTP");
        goto done;
    }
    split[2] = '\0'; /* head ends with one CRLF now */
    reply->status = atoi(buf + 9);
    header_value(buf, "Location", reply->location, sizeof(reply->location));
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
    reply->body = buf;
    buf = NULL;
    result = 0;
    goto done;

nomem:
    snprintf(reply->err, sizeof(reply->err), "out of memory");
done:
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
