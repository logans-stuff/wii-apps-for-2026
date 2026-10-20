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
#include "meshtastic.h"
#include "net.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/api/chat?limit=3";
    const char *token = getenv("SONORAN_COOKIE");
    const char *host = argc > 2 ? argv[2] : SONORAN_HOST;
    char err[200];
    struct http_reply r;

    if (argc > 1 && strcmp(argv[1], "--mesh") == 0) {
        /* hosttest --mesh: Meshtastic decode round-trip, no network. */
        if (mesh_selftest(err, sizeof(err)) != 0) {
            printf("mesh selftest FAILED: %s\n", err);
            return 1;
        }
        printf("mesh selftest OK (header + AES-CTR + Data protobuf round-trip)\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--meshtx") == 0) {
        /* hosttest --meshtx: encode text + nodeinfo, decode back, verify. */
        uint8_t frame[256];
        struct mesh_pkt pkt;
        int flen;
        uint32_t me = 0x77494921u;

        flen = mesh_encode_text(me, 0xffffffffu, 0x1234, 0x1f, "Hello from Wii!",
                                frame, sizeof(frame));
        if (flen < 0 || mesh_decode(frame, flen, &pkt, err, sizeof(err)) != 0 ||
            pkt.portnum != MESH_PORT_TEXT || strcmp(pkt.text, "Hello from Wii!") != 0 ||
            pkt.from != me) {
            printf("meshtx TEXT FAILED (port %u text '%s')\n", pkt.portnum, pkt.text);
            return 1;
        }
        printf("meshtx TEXT ok: from=%08x -> %08x '%s'\n", pkt.from, pkt.to, pkt.text);

        flen = mesh_encode_nodeinfo(me, 0x5678, 0x1f, "Wii Node", "WII", frame, sizeof(frame));
        if (flen < 0 || mesh_decode(frame, flen, &pkt, err, sizeof(err)) != 0 ||
            pkt.portnum != MESH_PORT_NODEINFO || strcmp(pkt.long_name, "Wii Node") != 0) {
            printf("meshtx NODEINFO FAILED (port %u name '%s')\n", pkt.portnum, pkt.long_name);
            return 1;
        }
        printf("meshtx NODEINFO ok: from=%08x name='%s' (%s)\n",
               pkt.from, pkt.long_name, pkt.short_name);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--meshhex") == 0) {
        /* hosttest --meshhex <hex>: decode one captured frame with the
         * default key and dump every field. */
        const char *h = argv[2];
        uint8_t frame[256];
        struct mesh_pkt pkt;
        int n = 0, i;
        while (h[0] && h[1] && n < (int)sizeof(frame)) {
            unsigned b;
            if (sscanf(h, "%2x", &b) != 1)
                break;
            frame[n++] = (uint8_t)b;
            h += 2;
        }
        printf("frame %d bytes\n", n);
        if (mesh_decode(frame, n, &pkt, err, sizeof(err)) != 0) {
            printf("decode failed: %s\n", err);
            return 1;
        }
        printf("from=%08x to=%08x id=%08x ch=%02x flags=%02x\n",
               pkt.from, pkt.to, pkt.id, pkt.channel, pkt.flags);
        printf("port=%u payload_len=%d\n", pkt.portnum, pkt.payload_len);
        printf("payload:");
        for (i = 0; i < pkt.payload_len; i++)
            printf(" %02x", pkt.payload[i]);
        printf("\n");
        if (pkt.portnum == MESH_PORT_TEXT)
            printf("text: %s\n", pkt.text);
        if (pkt.portnum == MESH_PORT_NODEINFO)
            printf("node: '%s' (%s)\n", pkt.long_name, pkt.short_name);
        return 0;
    }

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
