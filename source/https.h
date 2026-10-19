/* HTTPS GET over TLS 1.3 with wolfSSL. */
#ifndef SONORAN_HTTPS_H
#define SONORAN_HTTPS_H

#include <stddef.h>

struct http_reply {
    int status;          /* HTTP status, or 0 when the request failed */
    char *body;          /* body, ends with a zero byte; free with http_free */
    size_t len;
    char location[256];  /* Location header, if any */
    char err[200];       /* error text when status is 0 */
};

/* Load the root certificates. Call one time, after net_start. */
int https_init(char *err, size_t errlen);

/* GET https://host/path. cookie ("name=value") may be NULL. */
int https_get(const char *host, const char *path, const char *cookie,
              struct http_reply *reply);

void http_free(struct http_reply *reply);

/* TLS version and cipher of the last connection. */
const char *https_last_tls(void);

#endif
