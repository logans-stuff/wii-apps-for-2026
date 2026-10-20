/* Small WebSocket client (RFC 6455) over a TLS connection. Text only. */
#ifndef SONORAN_WS_H
#define SONORAN_WS_H

#include <stddef.h>
#include "https.h"

struct ws {
    struct tls_conn tls;
    char *msg;          /* last message, ends with a zero byte */
    size_t msg_len;
};

int ws_connect(struct ws *w, const char *host, const char *path, const char *origin,
               char *err, size_t errlen);
int ws_send_text(struct ws *w, const char *text);
/* Wait up to ms for one text message. Return 1 with w->msg set, 0 on
 * timeout, -1 when the connection is closed or broken. */
int ws_recv(struct ws *w, int ms);
void ws_close(struct ws *w);

#endif
