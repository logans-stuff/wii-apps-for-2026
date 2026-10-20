/* Linux test of the network, TLS and API code that the Wii app uses.
 * Usage: hosttest [path]   (cookie "name=value" from SONORAN_COOKIE, optional) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "discord.h"
#include "dc_qr.h"
#include "qrcodegen.h"
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
    if (argc > 1 && strcmp(argv[1], "--qr") == 0) {
        /* hosttest --qr: run the QR login until the QR code shows. */
        char qr[200], user[80], tok[256];
        int st, i;
        if (ra_start(err, sizeof(err)) != 0) {
            printf("ra_start: %s\n", err);
            return 1;
        }
        for (i = 0; i < 40; i++) {
            st = ra_poll(500, qr, sizeof(qr), user, sizeof(user), tok, sizeof(tok),
                         err, sizeof(err));
            if (st == RA_QR) {
                uint8_t q[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
                printf("QR text length %zu (fingerprint received)\n", strlen(qr));
                if (qrcodegen_encodeText(qr, tmp, q, qrcodegen_Ecc_MEDIUM, 1, 40,
                                         qrcodegen_Mask_AUTO, true))
                    printf("QR code: %d x %d modules\n", qrcodegen_getSize(q),
                           qrcodegen_getSize(q));
                ra_end();
                return 0;
            }
            if (st == RA_ERROR || st == RA_CANCELLED) {
                printf("ra error: %s\n", err);
                ra_end();
                return 1;
            }
        }
        printf("no QR code in 20 s\n");
        ra_end();
        return 1;
    }
    if (argc > 3 && strcmp(argv[1], "--dclogin") == 0) {
        /* hosttest --dclogin <email> <password>: test only, never a real account */
        char tok[256], ticket[256];
        int rc = dc_login(argv[2], argv[3], tok, sizeof(tok), ticket, sizeof(ticket),
                          err, sizeof(err));
        printf("TLS: %s\ndiscord login rc=%d %s\n", https_last_tls(), rc,
               rc == DC_OK ? "(got a token)" : err);
        return 0;
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
