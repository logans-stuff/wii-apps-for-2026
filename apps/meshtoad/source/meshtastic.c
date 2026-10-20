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
static int cur_hw_model = 255; /* PRIVATE_HW; shown as the node's device type */

void mesh_set_hop_limit(int hops)
{
    cur_hops = hops < 0 ? 0 : hops > 7 ? 7 : hops;
}

void mesh_set_hw_model(int model)
{
    cur_hw_model = model & 0xff;
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
            else if (field == 3)
                out->want_response = v != 0;
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
            if (pos + 4 > len)
                return;
            if (field == 4) out->data_dest = rd32(buf + pos);
            else if (field == 5) out->data_source = rd32(buf + pos);
            else if (field == 6) out->request_id = rd32(buf + pos);
            else if (field == 7) out->reply_id = rd32(buf + pos);
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

int mesh_parse_traceroute(const uint8_t *b, int len, struct mesh_route *out)
{
    int pos = 0;
    memset(out, 0, sizeof(*out));
    memset(out->snr_towards, -128, sizeof(out->snr_towards));
    memset(out->snr_back, -128, sizeof(out->snr_back));
    while (pos < len) {
        uint64_t tag = read_varint(b, len, &pos);
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (pos > len) return -1;
        if ((field == 1 || field == 3) && wire == 5) {
            uint32_t id;
            if (pos + 4 > len) return -1;
            id = rd32(b + pos); pos += 4;
            if (field == 1 && out->route_count < 8)
                out->route[out->route_count++] = id;
            else if (field == 3 && out->route_back_count < 8)
                out->route_back[out->route_back_count++] = id;
        } else if ((field == 1 || field == 3) && wire == 2) {
            uint64_t sub = read_varint(b, len, &pos);
            int end = pos + (int)sub;
            if (end < pos || end > len) return -1;
            while (pos + 4 <= end) {
                uint32_t id = rd32(b + pos); pos += 4;
                if (field == 1 && out->route_count < 8)
                    out->route[out->route_count++] = id;
                else if (field == 3 && out->route_back_count < 8)
                    out->route_back[out->route_back_count++] = id;
            }
            if (pos != end) return -1;
        } else if ((field == 2 || field == 4) && wire == 0) {
            int8_t snr = (int8_t)(int32_t)read_varint(b, len, &pos);
            if (pos > len) return -1;
            if (field == 2 && out->snr_towards_count < 8)
                out->snr_towards[out->snr_towards_count++] = snr;
            else if (field == 4 && out->snr_back_count < 8)
                out->snr_back[out->snr_back_count++] = snr;
        } else if ((field == 2 || field == 4) && wire == 2) {
            uint64_t sub = read_varint(b, len, &pos);
            int end = pos + (int)sub;
            if (end < pos || end > len) return -1;
            while (pos < end) {
                int8_t snr = (int8_t)(int32_t)read_varint(b, end, &pos);
                if (pos > end) return -1;
                if (field == 2 && out->snr_towards_count < 8)
                    out->snr_towards[out->snr_towards_count++] = snr;
                else if (field == 4 && out->snr_back_count < 8)
                    out->snr_back[out->snr_back_count++] = snr;
            }
        } else if (wire == 0) {
            read_varint(b, len, &pos);
        } else if (wire == 1) {
            pos += 8;
        } else if (wire == 2) {
            uint64_t sub = read_varint(b, len, &pos);
            pos += (int)sub;
        } else if (wire == 5) {
            pos += 4;
        } else {
            return -1;
        }
        if (pos > len) return -1;
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
                       const uint8_t *data, int dlen, int want_ack,
                       uint8_t *frame, int maxlen)
{
    uint8_t nonce[16];

    if (dlen < 0 || HEADER_LEN + dlen > maxlen)
        return -1;
    wr32(frame, to);
    wr32(frame + 4, from);
    wr32(frame + 8, id);
    frame[12] = (uint8_t)((cur_hops & 7) | (want_ack ? 0x08 : 0) |
                          ((cur_hops & 7) << 5)); /* hop limit + ACK + start */
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
    return build_frame(from, to, id, chan_hash, data, n,
                       to != 0xffffffffu, frame, maxlen);
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
    u += put_varint(user + u, (uint64_t)cur_hw_model);

    /* Data: portnum(1)=NODEINFO, payload(2)=user. */
    data[d++] = 0x08;
    data[d++] = MESH_PORT_NODEINFO;
    data[d++] = 0x12;
    d += put_varint(data + d, (uint64_t)u);
    memcpy(data + d, user, (size_t)u);
    d += u;
    return build_frame(from, 0xffffffffu, id, chan_hash, data, d, 0, frame, maxlen);
}

int mesh_encode_traceroute(uint32_t from, uint32_t to, uint32_t id,
                           uint8_t chan_hash, uint8_t *frame, int maxlen)
{
    uint8_t data[16];
    int n = 0;

    data[n++] = 0x08; /* portnum */
    n += put_varint(data + n, MESH_PORT_TRACEROUTE);
    data[n++] = 0x18; /* want_response */
    data[n++] = 0x01;
    data[n++] = 0x25; /* Data.dest: fixed32, mandatory for RouteDiscovery */
    wr32(data + n, to);
    n += 4;
    return build_frame(from, to, id, chan_hash, data, n, 1, frame, maxlen);
}

int mesh_routing_error(const uint8_t *b, int len)
{
    int pos = 0;

    /* Routing: route_request=1, route_reply=2, error_reason=3 (oneof). An
     * ACK is error_reason NONE; a payload without field 3 is not an error. */
    while (pos < len) {
        uint64_t tag = read_varint(b, len, &pos);
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (pos > len)
            return -1;
        if (field == 3 && wire == 0) {
            uint64_t v = read_varint(b, len, &pos);
            return pos > len ? -1 : (int)v;
        }
        if (wire == 0)
            read_varint(b, len, &pos);
        else if (wire == 1)
            pos += 8;
        else if (wire == 2)
            pos += (int)read_varint(b, len, &pos);
        else if (wire == 5)
            pos += 4;
        else
            return -1;
    }
    return pos > len ? -1 : MESH_ROUTING_NONE;
}

const char *mesh_routing_error_name(int e)
{
    switch (e) {
    case 0: return "ack";
    case 1: return "no route";
    case 2: return "NAK";
    case 3: return "timeout";
    case 4: return "no interface";
    case 5: return "max retransmit";
    case 6: return "no channel";
    case 7: return "too large";
    case 8: return "no response";
    case 9: return "duty cycle";
    case 32: return "bad request";
    case 33: return "not authorized";
    case 34: return "PKI failed";
    case 35: return "unknown pubkey";
    default: return "error";
    }
}

int mesh_encode_ack(uint32_t from, uint32_t to, uint32_t id, uint8_t chan_hash,
                    uint32_t request_id, int error, uint8_t *frame, int maxlen)
{
    uint8_t routing[12], data[32];
    int r = 0, n = 0;

    routing[r++] = 0x18; /* error_reason; written even when NONE (oneof) */
    r += put_varint(routing + r, (uint64_t)error);
    data[n++] = 0x08; /* portnum */
    data[n++] = MESH_PORT_ROUTING;
    data[n++] = 0x12; /* payload = Routing */
    data[n++] = (uint8_t)r;
    memcpy(data + n, routing, (size_t)r);
    n += r;
    data[n++] = 0x35; /* Data.request_id: fixed32 */
    wr32(data + n, request_id);
    n += 4;
    return build_frame(from, to, id, chan_hash, data, n, 0, frame, maxlen);
}

/* Packed repeated field: tag, length, then fixed32 ids or int32 varints. */
static int put_packed_ids(uint8_t *b, int field, const uint32_t *ids, int count)
{
    int n = 0, i;
    if (count <= 0)
        return 0;
    b[n++] = (uint8_t)(field << 3 | 2);
    n += put_varint(b + n, (uint64_t)count * 4);
    for (i = 0; i < count; i++, n += 4)
        wr32(b + n, ids[i]);
    return n;
}

static int put_packed_snrs(uint8_t *b, int field, const int8_t *snrs, int count)
{
    uint8_t tmp[8 * 10];
    int n = 0, t = 0, i;
    if (count <= 0)
        return 0;
    /* int32 in proto3: negatives sign-extend to a 10-byte varint. */
    for (i = 0; i < count; i++)
        t += put_varint(tmp + t, (uint64_t)(int64_t)snrs[i]);
    b[n++] = (uint8_t)(field << 3 | 2);
    n += put_varint(b + n, (uint64_t)t);
    memcpy(b + n, tmp, (size_t)t);
    return n + t;
}

int mesh_encode_traceroute_reply(uint32_t from, uint32_t to, uint32_t id,
                                 uint8_t chan_hash, uint32_t request_id,
                                 const struct mesh_route *r, uint8_t *frame, int maxlen)
{
    uint8_t rd[200], data[220];
    int n = 0, d = 0;

    n += put_packed_ids(rd + n, 1, r->route, r->route_count);
    n += put_packed_snrs(rd + n, 2, r->snr_towards, r->snr_towards_count);
    n += put_packed_ids(rd + n, 3, r->route_back, r->route_back_count);
    n += put_packed_snrs(rd + n, 4, r->snr_back, r->snr_back_count);

    data[d++] = 0x08;
    d += put_varint(data + d, MESH_PORT_TRACEROUTE);
    data[d++] = 0x12;
    d += put_varint(data + d, (uint64_t)n);
    memcpy(data + d, rd, (size_t)n);
    d += n;
    data[d++] = 0x35; /* Data.request_id */
    wr32(data + d, request_id);
    d += 4;
    return build_frame(from, to, id, chan_hash, data, d, 0, frame, maxlen);
}

int mesh_selftest(char *err, size_t errlen)
{
    const char *msg = "hi from wii";
    uint8_t data[64], frame[128], cipher[64], nonce[16];
    struct mesh_pkt pkt;
    int dlen = 0, flen, mlen = (int)strlen(msg);
    uint32_t from = 0x12345678u, id = 0x9abcdef0u, to = 0xffffffffu;
    struct mesh_route route;

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
    flen = mesh_encode_traceroute(from, 0x11223344u, id + 1, 0x08,
                                  frame, sizeof(frame));
    if (flen <= 0 || mesh_decode(frame, flen, &pkt, err, errlen) != 0 ||
        pkt.portnum != MESH_PORT_TRACEROUTE || !pkt.want_response ||
        pkt.data_dest != 0x11223344u || !(pkt.flags & 0x08)) {
        snprintf(err, errlen, "selftest: traceroute encoding failed");
        return -1;
    }
    {
        uint8_t sample[] = {
            0x0a, 0x08, 0x11,0x11,0x11,0x11, 0x22,0x22,0x22,0x22,
            0x12, 0x02, 24, 20,
            0x1d, 0x33,0x33,0x33,0x33,
            0x20, 16
        };
        if (mesh_parse_traceroute(sample, sizeof(sample), &route) != 0 ||
            route.route_count != 2 || route.route[1] != 0x22222222u ||
            route.snr_towards_count != 2 || route.snr_towards[0] != 24 ||
            route.route_back_count != 1 || route.route_back[0] != 0x33333333u ||
            route.snr_back_count != 1 || route.snr_back[0] != 16) {
            snprintf(err, errlen, "selftest: traceroute reply parse failed");
            return -1;
        }
    }
    {
        struct mesh_route back;
        route.snr_towards[1] = -20; /* negative int32 -> 10-byte varint */
        flen = mesh_encode_traceroute_reply(0x11223344u, from, id + 2, 0x08, id + 1,
                                            &route, frame, sizeof(frame));
        if (flen <= 0 || mesh_decode(frame, flen, &pkt, err, errlen) != 0 ||
            pkt.portnum != MESH_PORT_TRACEROUTE || pkt.request_id != id + 1 ||
            mesh_parse_traceroute(pkt.payload, pkt.payload_len, &back) != 0 ||
            back.route_count != 2 || back.route[0] != 0x11111111u ||
            back.snr_towards_count != 2 || back.snr_towards[1] != -20 ||
            back.route_back_count != 1 || back.snr_back[0] != 16) {
            snprintf(err, errlen, "selftest: traceroute reply round trip failed");
            return -1;
        }
    }
    flen = mesh_encode_ack(0x11223344u, from, id + 3, 0x08, id, MESH_ROUTING_NONE,
                           frame, sizeof(frame));
    if (flen <= 0 || mesh_decode(frame, flen, &pkt, err, errlen) != 0 ||
        pkt.portnum != MESH_PORT_ROUTING || pkt.request_id != id ||
        mesh_routing_error(pkt.payload, pkt.payload_len) != MESH_ROUTING_NONE) {
        snprintf(err, errlen, "selftest: ACK round trip failed");
        return -1;
    }
    flen = mesh_encode_ack(0x11223344u, from, id + 4, 0x08, id, 5, frame, sizeof(frame));
    if (flen <= 0 || mesh_decode(frame, flen, &pkt, err, errlen) != 0 ||
        mesh_routing_error(pkt.payload, pkt.payload_len) != 5) {
        snprintf(err, errlen, "selftest: NAK round trip failed");
        return -1;
    }
    return 0;
}
