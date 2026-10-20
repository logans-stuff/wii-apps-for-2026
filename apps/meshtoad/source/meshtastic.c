/* Meshtastic LoRa frame decode: header + AES-CTR channel decrypt + a small
 * protobuf reader for the Data message. See meshtastic.h. */
#include <stdio.h>
#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/aes.h>

#include "meshtastic.h"

#define HEADER_LEN 16

/* Default public channel key (PSK index 1, base64 "1PG7OiApB1nwvP+rz05pAQ=="). */
static const uint8_t DEFAULT_KEY[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

/* The channel key in use (defaults to the public key; config can override). */
static uint8_t cur_key[32] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};
static int cur_keylen = 16;
static int cur_hops = 3; /* hop limit / hop start for packets we send */

void mesh_set_hop_limit(int hops)
{
    cur_hops = hops < 0 ? 0 : hops > 7 ? 7 : hops;
}

void mesh_set_key(const uint8_t *key, int len)
{
    if (key != NULL && (len == 16 || len == 32)) {
        memcpy(cur_key, key, (size_t)len);
        cur_keylen = len;
    } else {
        memcpy(cur_key, DEFAULT_KEY, sizeof(DEFAULT_KEY));
        cur_keylen = 16;
    }
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Meshtastic CTR nonce: packet id (8 bytes LE) then sender node (4 bytes LE),
 * the last 4 bytes left zero for the block counter. */
static void make_nonce(uint8_t nonce[16], uint32_t id, uint32_t from)
{
    memset(nonce, 0, 16);
    wr32(nonce, id);       /* id in the low 32 bits of the 64-bit field */
    wr32(nonce + 8, from);
}

/* AES-CTR is its own inverse, so this both encrypts and decrypts. */
static int aes_ctr(const uint8_t *key, int keylen, const uint8_t nonce[16],
                   const uint8_t *in, uint8_t *out, int len)
{
    Aes aes;
    int rc;

    if (wc_AesInit(&aes, NULL, INVALID_DEVID) != 0)
        return -1;
    rc = wc_AesSetKeyDirect(&aes, key, (word32)keylen, nonce, AES_ENCRYPTION);
    if (rc == 0)
        rc = wc_AesCtrEncrypt(&aes, out, in, (word32)len);
    wc_AesFree(&aes);
    return rc == 0 ? 0 : -1;
}

/* Read a base-128 varint. Advance *pos; return the value, or set *pos past
 * the end on a malformed varint. */
static uint64_t read_varint(const uint8_t *buf, int len, int *pos)
{
    uint64_t v = 0;
    int shift = 0;

    while (*pos < len && shift < 64) {
        uint8_t b = buf[(*pos)++];
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return v;
        shift += 7;
    }
    *pos = len + 1; /* mark malformed */
    return v;
}

/* Parse a Data protobuf: field 1 = portnum (varint), field 2 = payload. */
static void parse_data(const uint8_t *buf, int len, struct mesh_pkt *out)
{
    int pos = 0;

    while (pos < len) {
        uint64_t tag = read_varint(buf, len, &pos);
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (pos > len)
            return;
        if (wire == 0) {            /* varint */
            uint64_t v = read_varint(buf, len, &pos);
            if (field == 1)
                out->portnum = (uint32_t)v;
        } else if (wire == 2) {     /* length-delimited */
            uint64_t n = read_varint(buf, len, &pos);
            if (pos > len || n > (uint64_t)(len - pos))
                return;
            if (field == 2) {
                int m = (int)n;
                if (m > (int)sizeof(out->payload))
                    m = (int)sizeof(out->payload);
                memcpy(out->payload, buf + pos, (size_t)m);
                out->payload_len = m;
            }
            pos += (int)n;
        } else if (wire == 5) {     /* 32-bit */
            pos += 4;
        } else if (wire == 1) {     /* 64-bit */
            pos += 8;
        } else {
            return;                 /* unknown wire type */
        }
    }
}

/* Parse a User protobuf (NodeInfo payload): field 2 = long name,
 * field 3 = short name. */
static void parse_user(const uint8_t *buf, int len, struct mesh_pkt *out)
{
    int pos = 0;

    while (pos < len) {
        uint64_t tag = read_varint(buf, len, &pos);
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (pos > len)
            return;
        if (wire == 2) {
            uint64_t n = read_varint(buf, len, &pos);
            if (pos > len || n > (uint64_t)(len - pos))
                return;
            if (field == 2 || field == 3) {
                char *dst = field == 2 ? out->long_name : out->short_name;
                int cap = field == 2 ? (int)sizeof(out->long_name) : (int)sizeof(out->short_name);
                int m = (int)n;
                if (m > cap - 1)
                    m = cap - 1;
                memcpy(dst, buf + pos, (size_t)m);
                dst[m] = '\0';
            }
            pos += (int)n;
        } else if (wire == 0) {
            read_varint(buf, len, &pos);
        } else if (wire == 5) {
            pos += 4;
        } else if (wire == 1) {
            pos += 8;
        } else {
            return;
        }
    }
}

int mesh_decode(const uint8_t *frame, int len, struct mesh_pkt *out,
                char *err, size_t errlen)
{
    uint8_t nonce[16], plain[256];
    int plen;

    memset(out, 0, sizeof(*out));
    if (len < HEADER_LEN) {
        snprintf(err, errlen, "frame too short (%d bytes)", len);
        return -1;
    }
    out->to = rd32(frame);
    out->from = rd32(frame + 4);
    out->id = rd32(frame + 8);
    out->flags = frame[12];
    out->channel = frame[13];

    plen = len - HEADER_LEN;
    if (plen > (int)sizeof(plain))
        plen = (int)sizeof(plain);

    make_nonce(nonce, out->id, out->from);
    if (aes_ctr(cur_key, cur_keylen, nonce,
                frame + HEADER_LEN, plain, plen) != 0) {
        snprintf(err, errlen, "AES-CTR decrypt failed");
        return -1;
    }

    parse_data(plain, plen, out);
    if (out->portnum == MESH_PORT_TEXT && out->payload_len > 0) {
        int n = out->payload_len;
        if (n > (int)sizeof(out->text) - 1)
            n = (int)sizeof(out->text) - 1;
        memcpy(out->text, out->payload, (size_t)n);
        out->text[n] = '\0';
    } else if (out->portnum == MESH_PORT_NODEINFO && out->payload_len > 0) {
        parse_user(out->payload, out->payload_len, out);
    }
    return 0;
}

/* Append a base-128 varint; return bytes written. */
static int put_varint(uint8_t *buf, uint64_t v)
{
    int n = 0;
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if (v)
            b |= 0x80;
        buf[n++] = b;
    } while (v);
    return n;
}

/* Header + AES-CTR(data) -> frame. Return frame length, or -1 if too big. */
static int build_frame(uint32_t from, uint32_t to, uint32_t id, uint8_t chan,
                       const uint8_t *data, int dlen, uint8_t *frame, int maxlen)
{
    uint8_t nonce[16];

    if (dlen < 0 || HEADER_LEN + dlen > maxlen)
        return -1;
    wr32(frame, to);
    wr32(frame + 4, from);
    wr32(frame + 8, id);
    frame[12] = (uint8_t)((cur_hops & 7) | ((cur_hops & 7) << 5)); /* hop limit + start */
    frame[13] = chan;
    frame[14] = 0;     /* next_hop */
    frame[15] = 0;     /* relay_node */
    make_nonce(nonce, id, from);
    if (aes_ctr(cur_key, cur_keylen, nonce, data, frame + HEADER_LEN, dlen) != 0)
        return -1;
    return HEADER_LEN + dlen;
}

int mesh_encode_text(uint32_t from, uint32_t to, uint32_t id, uint8_t chan_hash,
                     const char *text, uint8_t *frame, int maxlen)
{
    uint8_t data[256];
    int n = 0, tlen = (int)strlen(text);

    if (tlen > 200)
        tlen = 200;
    data[n++] = 0x08;               /* field 1 (portnum) varint */
    data[n++] = MESH_PORT_TEXT;
    data[n++] = 0x12;               /* field 2 (payload) length-delimited */
    n += put_varint(data + n, (uint64_t)tlen);
    memcpy(data + n, text, (size_t)tlen);
    n += tlen;
    return build_frame(from, to, id, chan_hash, data, n, frame, maxlen);
}

int mesh_encode_nodeinfo(uint32_t from, uint32_t id, uint8_t chan_hash,
                         const char *long_name, const char *short_name,
                         uint8_t *frame, int maxlen)
{
    uint8_t user[128], data[160];
    char idstr[10];
    int u = 0, d = 0, ln, sn, il;

    snprintf(idstr, sizeof(idstr), "!%08x", from);
    il = (int)strlen(idstr);
    ln = (int)strlen(long_name);
    sn = (int)strlen(short_name);
    if (ln > 39)
        ln = 39;
    if (sn > 4)
        sn = 4;
    /* User protobuf: id(1), long_name(2), short_name(3), hw_model(5)=0. */
    user[u++] = 0x0a;
    u += put_varint(user + u, (uint64_t)il);
    memcpy(user + u, idstr, (size_t)il);
    u += il;
    user[u++] = 0x12;
    u += put_varint(user + u, (uint64_t)ln);
    memcpy(user + u, long_name, (size_t)ln);
    u += ln;
    user[u++] = 0x1a;
    u += put_varint(user + u, (uint64_t)sn);
    memcpy(user + u, short_name, (size_t)sn);
    u += sn;
    user[u++] = 0x28; /* field 5 hw_model varint */
    user[u++] = 0x00; /* UNSET */

    /* Data: portnum(1)=NODEINFO, payload(2)=user. */
    data[d++] = 0x08;
    data[d++] = MESH_PORT_NODEINFO;
    data[d++] = 0x12;
    d += put_varint(data + d, (uint64_t)u);
    memcpy(data + d, user, (size_t)u);
    d += u;
    return build_frame(from, 0xffffffffu, id, chan_hash, data, d, frame, maxlen);
}

int mesh_selftest(char *err, size_t errlen)
{
    const char *msg = "hi from wii";
    uint8_t data[64], frame[128], cipher[64], nonce[16];
    struct mesh_pkt pkt;
    int dlen = 0, flen, mlen = (int)strlen(msg);
    uint32_t from = 0x12345678u, id = 0x9abcdef0u, to = 0xffffffffu;

    /* Build a Data protobuf: portnum=1 (text), payload=msg. */
    data[dlen++] = 0x08;            /* field 1, varint */
    data[dlen++] = MESH_PORT_TEXT;
    data[dlen++] = 0x12;            /* field 2, length-delimited */
    data[dlen++] = (uint8_t)mlen;
    memcpy(data + dlen, msg, (size_t)mlen);
    dlen += mlen;

    /* Encrypt and wrap in a header, the way a sender would. */
    make_nonce(nonce, id, from);
    if (aes_ctr(cur_key, cur_keylen, nonce, data, cipher, dlen) != 0) {
        snprintf(err, errlen, "selftest: encrypt failed");
        return -1;
    }
    wr32(frame, to);
    wr32(frame + 4, from);
    wr32(frame + 8, id);
    frame[12] = 0x03;  /* hop limit 3 */
    frame[13] = 0x08;  /* channel hash */
    frame[14] = 0;
    frame[15] = 0;
    memcpy(frame + HEADER_LEN, cipher, (size_t)dlen);
    flen = HEADER_LEN + dlen;

    if (mesh_decode(frame, flen, &pkt, err, errlen) != 0)
        return -1;
    if (pkt.from != from || pkt.to != to || pkt.id != id) {
        snprintf(err, errlen, "selftest: header mismatch");
        return -1;
    }
    if (pkt.portnum != MESH_PORT_TEXT || strcmp(pkt.text, msg) != 0) {
        snprintf(err, errlen, "selftest: port %u text '%s'", pkt.portnum, pkt.text);
        return -1;
    }
    return 0;
}
