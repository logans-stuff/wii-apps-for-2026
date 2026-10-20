/* Discord QR code login ("remote auth" gateway, protocol version 2).
 *
 * 1. Connect to wss://remote-auth-gateway.discord.gg/?v=2.
 * 2. Send our RSA-2048 public key. Discord sends a nonce encrypted to it.
 *    We send back SHA-256 of the nonce (base64url) as proof.
 * 3. Discord sends a fingerprint. The QR code is
 *    https://discord.com/ra/<fingerprint>.
 * 4. When the phone scans it, Discord sends the user (encrypted). When
 *    the user confirms, Discord sends a ticket.
 * 5. POST the ticket to /api/v9/users/@me/remote-auth/login. The answer
 *    holds the token, encrypted to our key. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/rsa.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include "cJSON.h"
#include "dc_qr.h"
#include "discord.h"
#include "https.h"
#include "net.h"
#include "ws.h"

#define RA_HOST "remote-auth-gateway.discord.gg"

static struct ws ra_ws;
static RsaKey ra_key;
static WC_RNG ra_rng;
static int key_ready, rng_ready, connected;
static int heartbeat_ms = 41250;
static unsigned long long last_beat;

/* base64 text to bytes, then RSA-OAEP (SHA-256) decrypt. */
static int rsa_decrypt_b64(const char *b64, unsigned char *out, int outlen)
{
    unsigned char raw[512];
    word32 rawlen = sizeof(raw);
    int n;

    if (b64 == NULL
        || Base64_Decode((const byte *)b64, (word32)strlen(b64), raw, &rawlen) != 0)
        return -1;
    n = wc_RsaPrivateDecrypt_ex(raw, rawlen, out, (word32)outlen, &ra_key,
                                WC_RSA_OAEP_PAD, WC_HASH_TYPE_SHA256, WC_MGF1SHA256, NULL, 0);
    memset(raw, 0, sizeof(raw));
    return n;
}

static int send_op(const char *op, const char *key, const char *value)
{
    cJSON *o = cJSON_CreateObject();
    char *text;
    int rc;

    cJSON_AddStringToObject(o, "op", op);
    if (key != NULL)
        cJSON_AddStringToObject(o, key, value);
    text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (text == NULL)
        return -1;
    rc = ws_send_text(&ra_ws, text);
    cJSON_free(text);
    return rc;
}

int ra_start(char *err, size_t errlen)
{
    int ret;

    ra_end();
    if (wc_InitRng(&ra_rng) != 0) {
        snprintf(err, errlen, "random generator failed");
        return -1;
    }
    rng_ready = 1;
    wc_InitRsaKey(&ra_key, NULL);
    dbg("ra: making RSA key");
    ret = wc_MakeRsaKey(&ra_key, 2048, WC_RSA_EXPONENT, &ra_rng);
    if (ret != 0) {
        snprintf(err, errlen, "RSA key failed (%d)", ret);
        return -1;
    }
    wc_RsaSetRNG(&ra_key, &ra_rng);
    key_ready = 1;
    dbg("ra: RSA key ok");
    if (ws_connect(&ra_ws, RA_HOST, "/?v=2", "https://discord.com", err, errlen) != 0)
        return -1;
    connected = 1;
    last_beat = net_ms();
    return 0;
}

static int exchange_ticket(const char *ticket, char *token, size_t tokenlen,
                           char *err, size_t errlen)
{
    struct http_reply r;
    cJSON *body = cJSON_CreateObject(), *a;
    char *json;
    unsigned char plain[256];
    int n, rc = RA_ERROR;

    cJSON_AddStringToObject(body, "ticket", ticket);
    json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json == NULL)
        return RA_ERROR;
    n = https_request_h("POST", DISCORD_HOST, "/api/v9/users/@me/remote-auth/login",
                        NULL, NULL, json, &r);
    cJSON_free(json);
    if (n != 0) {
        snprintf(err, errlen, "%s", r.err);
        http_free(&r);
        return RA_ERROR;
    }
    dbg("ra: ticket exchange HTTP %d", r.status);
    a = r.body ? cJSON_ParseWithLength(r.body, r.len) : NULL;
    if (r.status == 200 && a != NULL) {
        const cJSON *enc = cJSON_GetObjectItemCaseSensitive(a, "encrypted_token");
        n = cJSON_IsString(enc) ? rsa_decrypt_b64(enc->valuestring, plain, sizeof(plain) - 1)
                                : -1;
        if (n > 0 && (size_t)n < tokenlen) {
            memcpy(token, plain, (size_t)n);
            token[n] = '\0';
            rc = RA_DONE;
        } else {
            snprintf(err, errlen, "cannot decrypt the token (%d)", n);
        }
        memset(plain, 0, sizeof(plain));
    } else if (a != NULL && cJSON_GetObjectItemCaseSensitive(a, "captcha_key") != NULL) {
        snprintf(err, errlen, "Discord asks for a captcha at the last step. "
                 "The Wii cannot show it.");
    } else {
        snprintf(err, errlen, "ticket exchange failed (HTTP %d)", r.status);
    }
    cJSON_Delete(a);
    if (r.body)
        memset(r.body, 0, r.len);
    http_free(&r);
    return rc;
}

int ra_poll(int ms, char *qr_text, size_t qrlen, char *user, size_t userlen,
            char *token, size_t tokenlen, char *err, size_t errlen)
{
    cJSON *m;
    const char *op;
    int got, rc = RA_WAIT;

    if (!connected) {
        snprintf(err, errlen, "not connected");
        return RA_ERROR;
    }
    if (net_ms() - last_beat >= (unsigned long long)heartbeat_ms) {
        send_op("heartbeat", NULL, NULL);
        last_beat = net_ms();
    }
    got = ws_recv(&ra_ws, ms);
    if (got == 0)
        return RA_WAIT;
    if (got < 0) {
        snprintf(err, errlen, "Discord closed the QR login. Push 1 to try again.");
        return RA_ERROR;
    }
    m = cJSON_Parse(ra_ws.msg);
    op = m ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "op")) : NULL;
    dbg("ra: op %s", op ? op : "?");
    if (op == NULL) {
        rc = RA_WAIT;
    } else if (strcmp(op, "hello") == 0) {
        unsigned char der[512];
        char b64[800];
        word32 b64len = sizeof(b64);
        int derlen;
        const cJSON *hb = cJSON_GetObjectItemCaseSensitive(m, "heartbeat_interval");
        if (cJSON_IsNumber(hb) && hb->valueint > 5000)
            heartbeat_ms = hb->valueint;
        derlen = wc_RsaKeyToPublicDer(&ra_key, der, sizeof(der));
        if (derlen <= 0 || Base64_Encode_NoNl(der, (word32)derlen, (byte *)b64, &b64len) != 0) {
            snprintf(err, errlen, "cannot export the public key (%d)", derlen);
            rc = RA_ERROR;
        } else {
            b64[b64len] = '\0';
            send_op("init", "encoded_public_key", b64);
        }
    } else if (strcmp(op, "nonce_proof") == 0) {
        unsigned char nonce[256], hash[WC_SHA256_DIGEST_SIZE];
        char proof[64];
        word32 plen = sizeof(proof);
        int n = rsa_decrypt_b64(cJSON_GetStringValue(
                    cJSON_GetObjectItemCaseSensitive(m, "encrypted_nonce")), nonce, sizeof(nonce));
        if (n <= 0) {
            snprintf(err, errlen, "cannot decrypt the nonce (%d)", n);
            rc = RA_ERROR;
        } else {
            size_t i;
            wc_Sha256Hash(nonce, (word32)n, hash);
            Base64_Encode_NoNl(hash, sizeof(hash), (byte *)proof, &plen);
            proof[plen] = '\0';
            /* base64url without padding */
            for (i = 0; i < plen; i++) {
                if (proof[i] == '+')
                    proof[i] = '-';
                else if (proof[i] == '/')
                    proof[i] = '_';
                else if (proof[i] == '=')
                    proof[i] = '\0';
            }
            send_op("nonce_proof", "proof", proof);
        }
    } else if (strcmp(op, "pending_remote_init") == 0) {
        const char *fp = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "fingerprint"));
        if (fp != NULL) {
            snprintf(qr_text, qrlen, "https://discord.com/ra/%s", fp);
            rc = RA_QR;
        }
    } else if (strcmp(op, "pending_ticket") == 0) {
        unsigned char plain[256];
        int n = rsa_decrypt_b64(cJSON_GetStringValue(
                    cJSON_GetObjectItemCaseSensitive(m, "encrypted_user_payload")),
                    plain, sizeof(plain) - 1);
        /* "id:discriminator:avatar:username" */
        if (n > 0) {
            char *name;
            plain[n] = '\0';
            name = strrchr((char *)plain, ':');
            snprintf(user, userlen, "%s", name ? name + 1 : "?");
        } else {
            snprintf(user, userlen, "?");
        }
        rc = RA_SCANNED;
    } else if (strcmp(op, "pending_login") == 0) {
        const char *ticket = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "ticket"));
        rc = ticket ? exchange_ticket(ticket, token, tokenlen, err, errlen) : RA_ERROR;
    } else if (strcmp(op, "cancel") == 0) {
        snprintf(err, errlen, "Login cancelled on the phone.");
        rc = RA_CANCELLED;
    }
    cJSON_Delete(m);
    return rc;
}

void ra_end(void)
{
    if (connected) {
        ws_close(&ra_ws);
        connected = 0;
    }
    if (key_ready) {
        wc_FreeRsaKey(&ra_key);
        key_ready = 0;
    }
    if (rng_ready) {
        wc_FreeRng(&ra_rng);
        rng_ready = 0;
    }
}
