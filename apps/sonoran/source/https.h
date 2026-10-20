/* HTTPS GET over TLS 1.3 with wolfSSL. */
#ifndef SONORAN_HTTPS_H
#define SONORAN_HTTPS_H

#include <stddef.h>

struct http_reply {
    int status;          /* HTTP status, or 0 when the request failed */
    char *body;          /* body, ends with a zero byte; free with http_free */
    size_t len;
    char location[256];  /* Location header, if any */
    char set_cookie[512]; /* "name=value" of the wanted Set-Cookie */
    char err[200];       /* error text when status is 0 */
};

/* Load the root certificates. Call one time, after net_start. */
int https_init(char *err, size_t errlen);

/* GET https://host/path. cookie ("name=value") may be NULL. */
int https_get(const char *host, const char *path, const char *cookie,
              struct http_reply *reply);

/* Any method. json (may be NULL) is sent as an application/json body.
 * want_cookie names a Set-Cookie to copy into reply->set_cookie. */
int https_request(const char *method, const char *host, const char *path,
                  const char *cookie, const char *want_cookie, const char *json,
                  struct http_reply *reply);

/* Same, with raw header lines (each ends with CRLF) instead of a cookie. */
int https_request_h(const char *method, const char *host, const char *path,
                    const char *headers, const char *want_cookie, const char *json,
                    struct http_reply *reply);

void http_free(struct http_reply *reply);

/* A TLS connection that stays open (for WebSocket). Keep the struct at
 * a fixed address while it is open: wolfSSL keeps a pointer to it. */
struct tls_conn {
    void *ssl;
    int handle;
};

int tls_open(struct tls_conn *c, const char *host, char *err, size_t errlen);
int tls_write_all(struct tls_conn *c, const void *data, int len);
/* Read some bytes. Return > 0 bytes, 0 when closed, < 0 on error. */
int tls_read(struct tls_conn *c, void *buf, int len);
/* 1 when data can be read now, or comes inside ms milliseconds. */
int tls_readable(struct tls_conn *c, int ms);
void tls_close(struct tls_conn *c);

/* TLS version and cipher of the last connection. */
const char *https_last_tls(void);

#endif
