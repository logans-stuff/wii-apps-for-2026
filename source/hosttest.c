/* Linux test of the network, TLS and API code that the Wii app uses.
 * Usage: hosttest [path]   (cookie "name=value" from SONORAN_COOKIE, optional) */
#include <stdio.h>
#include <stdlib.h>

#include "api.h"
#include "https.h"
#include "net.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/api/chat?limit=3";
    const char *token = getenv("SONORAN_COOKIE");
    const char *host = argc > 2 ? argv[2] : SONORAN_HOST;
    char err[200];
    struct http_reply r;

    if (net_start(err, sizeof(err)) != 0 || https_init(err, sizeof(err)) != 0) {
        printf("init: %s\n", err);
        return 1;
    }
    if (https_get(host, path, token, &r) != 0) {
        printf("GET failed: %s\n", r.err);
        return 1;
    }
    printf("TLS: %s\nHTTP %d, %zu bytes\nLocation: %.120s\n", https_last_tls(),
           r.status, r.len, r.location);
    printf("Body: %.300s\n", r.body ? r.body : "");
    http_free(&r);
    return 0;
}
