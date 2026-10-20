/* Small WebSocket client (RFC 6455) over a TLS connection. Text only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/coding.h>

#include "net.h"
#include "ws.h"

#define WS_MAX_MSG (256 * 1024)

static WC_RNG ws_rng;
static int ws_rng_ready;

static void random_bytes(unsigned char *out, unsigned int n)
{
    if (!ws_rng_ready && wc_InitRng(&ws_rng) == 0)
        ws_rng_ready = 1;
    if (ws_rng_ready)
        wc_RNG_GenerateBlock(&ws_rng, out, n);
}

static int read_exact(struct ws *w, unsigned char *buf, size_t n)
{
    size_t got = 0;

    while (got < n) {
        int r = tls_read(&w->tls, buf + got, (int)(n - got));
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

int ws_connect(struct ws *w, const char *host, const char *path, const char *origin,
               char *err, size_t errlen)
{
    unsigned char nonce[16];
    char key[32], req[768], head[2048];
    word32 keylen = sizeof(key);
    size_t used = 0;

    memset(w, 0, sizeof(*w));
    w->tls.handle = -1;
    if (tls_open(&w->tls, host, err, errlen) != 0)
        return -1;
    random_bytes(nonce, sizeof(nonce));
    Base64_Encode_NoNl(nonce, sizeof(nonce), (byte *)key, &keylen);
    key[keylen] = '\0';
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "Origin: %s\r\n"
             "User-Agent: SonoranWii/0.3\r\n"
             "\r\n",
             path, host, key, origin);
    if (tls_write_all(&w->tls, req, (int)strlen(req)) != 0) {
        snprintf(err, errlen, "WebSocket: cannot send the request");
        ws_close(w);
        return -1;
    }
    /* Read the answer head one byte at a time, so that no frame data is
     * taken by mistake. */
    while (used + 1 < sizeof(head)) {
        if (read_exact(w, (unsigned char *)head + used, 1) != 0)
            break;
        used++;
        if (used >= 4 && memcmp(head + used - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    head[used] = '\0';
    if (strncmp(head, "HTTP/1.1 101", 12) != 0) {
        snprintf(err, errlen, "WebSocket refused: %.60s", head);
        ws_close(w);
        return -1;
    }
    dbg("ws: connected to %s", host);
    return 0;
}

static int send_frame(struct ws *w, int opcode, const unsigned char *data, size_t len)
{
    unsigned char hdr[14], mask[4];
    unsigned char *frame;
    size_t h = 0, i;
    int rc;

    hdr[h++] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        hdr[h++] = (unsigned char)(0x80 | len);
    } else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (unsigned char)(len >> 8);
        hdr[h++] = (unsigned char)len;
    } else {
        hdr[h++] = 0x80 | 127;
        for (i = 0; i < 8; i++)
            hdr[h++] = (unsigned char)((unsigned long long)len >> (56 - 8 * i));
    }
    random_bytes(mask, 4);
    memcpy(hdr + h, mask, 4);
    h += 4;
    frame = malloc(h + len);
    if (frame == NULL)
        return -1;
    memcpy(frame, hdr, h);
    for (i = 0; i < len; i++)
        frame[h + i] = data[i] ^ mask[i & 3];
    rc = tls_write_all(&w->tls, frame, (int)(h + len));
    memset(frame, 0, h + len);
    free(frame);
    return rc;
}

int ws_send_text(struct ws *w, const char *text)
{
    return send_frame(w, 0x1, (const unsigned char *)text, strlen(text));
}

int ws_send_binary(struct ws *w, const unsigned char *data, size_t len)
{
    return send_frame(w, 0x2, data, len);
}

/* Read and drop n bytes. */
static int drop_bytes(struct ws *w, unsigned long long n)
{
    unsigned char tmp[1024];

    while (n > 0) {
        size_t k = n > sizeof(tmp) ? sizeof(tmp) : (size_t)n;
        if (read_exact(w, tmp, k) != 0)
            return -1;
        n -= k;
    }
    return 0;
}

int ws_recv(struct ws *w, int ms)
{
    unsigned long long start = net_ms();
    size_t max = w->max_msg ? w->max_msg : WS_MAX_MSG;
    int dropping = 0;

    free(w->msg);
    w->msg = NULL;
    w->msg_len = 0;
    for (;;) {
        unsigned char h[2], ext[8];
        unsigned long long len = 0;
        int opcode, fin, i, left = ms - (int)(net_ms() - start);
        unsigned char *payload;

        if (left < 0)
            left = 0;
        if (w->msg == NULL && !dropping && !tls_readable(&w->tls, left))
            return 0;
        if (read_exact(w, h, 2) != 0)
            return -1;
        fin = h[0] & 0x80;
        opcode = h[0] & 0x0f;
        len = h[1] & 0x7f;
        if (len == 126) {
            if (read_exact(w, ext, 2) != 0)
                return -1;
            len = ((unsigned)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            if (read_exact(w, ext, 8) != 0)
                return -1;
            len = 0;
            for (i = 0; i < 8; i++)
                len = (len << 8) | ext[i];
        }
        if (h[1] & 0x80) /* a server must not mask */
            return -1;
        if (opcode < 0x8 && (dropping || len > max || w->msg_len + len > max)) {
            /* Too large (for example the Discord READY event): drop it. */
            if (drop_bytes(w, len) != 0)
                return -1;
            free(w->msg);
            w->msg = NULL;
            w->msg_len = 0;
            dropping = 1;
            if (fin) {
                dbg("ws: dropped a large message");
                return 2;
            }
            continue;
        }
        if (len > 125 && opcode >= 0x8)
            return -1;
        payload = malloc((size_t)len + 1);
        if (payload == NULL || read_exact(w, payload, (size_t)len) != 0) {
            free(payload);
            return -1;
        }
        payload[len] = '\0';

        if (opcode == 0x8) { /* close */
            dbg("ws: closed by server (%u bytes)", (unsigned)len);
            free(payload);
            return -1;
        }
        if (opcode == 0x9) { /* ping */
            send_frame(w, 0xA, payload, (size_t)len);
            free(payload);
            continue;
        }
        if (opcode == 0xA) { /* pong */
            free(payload);
            continue;
        }
        /* text (1), binary (2) or continuation (0) */
        if (w->msg == NULL) {
            w->msg = (char *)payload;
            w->msg_len = (size_t)len;
            w->binary = opcode == 0x2;
        } else {
            char *joined = realloc(w->msg, w->msg_len + (size_t)len + 1);
            if (joined == NULL) {
                free(payload);
                return -1;
            }
            memcpy(joined + w->msg_len, payload, (size_t)len + 1);
            w->msg = joined;
            w->msg_len += (size_t)len;
            free(payload);
        }
        if (fin)
            return 1;
    }
}

void ws_close(struct ws *w)
{
    if (w->tls.ssl != NULL)
        send_frame(w, 0x8, (const unsigned char *)"\x03\xe8", 2);
    tls_close(&w->tls);
    free(w->msg);
    w->msg = NULL;
}
