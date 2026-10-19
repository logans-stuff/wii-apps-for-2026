/* Linux test of the network, TLS and API code that the Wii app uses.
 * Usage: hosttest [path]   (cookie "name=value" from SONORAN_COOKIE, optional) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (argc > 3 && strcmp(argv[1], "--login") == 0) {
        /* hosttest --login <user> <password>: test only, never a real password */
        char cookie[600];
        int rc = api_login(argv[2], argv[3], cookie, sizeof(cookie), err, sizeof(err));
        printf("TLS: %s\nlogin rc=%d %s\n", https_last_tls(), rc,
               rc == API_OK ? "(got a session cookie)" : err);
        return 0;
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
