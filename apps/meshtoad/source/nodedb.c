/* NodeDB implementation. See nodedb.h. */
#include <string.h>

#include "nodedb.h"

static struct node nodes[NODEDB_MAX];
static int order[NODEDB_MAX]; /* node indices, most-recently-heard first */
static int n_nodes;

void nodedb_reset(void)
{
    n_nodes = 0;
    memset(nodes, 0, sizeof(nodes));
}

int nodedb_count(void)
{
    return n_nodes;
}

const struct node *nodedb_get(int i)
{
    if (i < 0 || i >= n_nodes)
        return NULL;
    return &nodes[order[i]];
}

/* Find the node with this id, or make a new one. Return its array index. */
static int find_or_add(uint32_t id)
{
    int i;
    for (i = 0; i < n_nodes; i++)
        if (nodes[i].id == id)
            return i;
    if (n_nodes < NODEDB_MAX) {
        i = n_nodes++;
        memset(&nodes[i], 0, sizeof(nodes[i]));
        nodes[i].id = id;
        nodes[i].hops = -1;
        order[i] = i; /* appended; move_to_front fixes the order */
        return i;
    }
    /* Table full: reuse the least-recently-heard slot (last in order). */
    i = order[n_nodes - 1];
    memset(&nodes[i], 0, sizeof(nodes[i]));
    nodes[i].id = id;
    nodes[i].hops = -1;
    return i;
}

static void move_to_front(int idx)
{
    int i, pos = -1;
    for (i = 0; i < n_nodes; i++)
        if (order[i] == idx) {
            pos = i;
            break;
        }
    if (pos < 0)
        pos = n_nodes - 1; /* not listed yet (shouldn't happen) */
    for (i = pos; i > 0; i--)
        order[i] = order[i - 1];
    order[0] = idx;
}

/* ---- tiny protobuf readers ---- */

static uint64_t rd_varint(const uint8_t *b, int len, int *pos)
{
    uint64_t v = 0;
    int shift = 0;
    while (*pos < len && shift < 64) {
        uint8_t c = b[(*pos)++];
        v |= (uint64_t)(c & 0x7f) << shift;
        if (!(c & 0x80))
            return v;
        shift += 7;
    }
    *pos = len + 1;
    return v;
}

static int32_t rd_i32(const uint8_t *b)
{
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

static float rd_f32(const uint8_t *b)
{
    uint32_t u = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* Skip a field of the given wire type, advancing *pos. */
static void skip_field(const uint8_t *b, int len, int *pos, int wire)
{
    if (wire == 0)
        rd_varint(b, len, pos);
    else if (wire == 5)
        *pos += 4;
    else if (wire == 1)
        *pos += 8;
    else if (wire == 2) {
        uint64_t n = rd_varint(b, len, pos);
        *pos += (int)n;
    } else
        *pos = len + 1;
}

/* Position: sfixed32 latitude_i=1, longitude_i=2 (deg*1e7), int32 altitude=3. */
static void parse_position(const uint8_t *b, int len, struct node *nd)
{
    int pos = 0;
    while (pos < len) {
        uint64_t tag = rd_varint(b, len, &pos);
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (pos > len)
            return;
        if (field == 1 && wire == 5) {
            nd->lat = rd_i32(b + pos) / 1e7;
            pos += 4;
            nd->has_pos = 1;
        } else if (field == 2 && wire == 5) {
            nd->lon = rd_i32(b + pos) / 1e7;
            pos += 4;
            nd->has_pos = 1;
        } else if (field == 3 && wire == 0) {
            nd->alt = (int)(int32_t)rd_varint(b, len, &pos);
        } else {
            skip_field(b, len, &pos, wire);
        }
    }
}

/* Telemetry: device_metrics=2 -> { battery_level=1 (uint32), voltage=2 (float) }. */
static void parse_telemetry(const uint8_t *b, int len, struct node *nd)
{
    int pos = 0;
    while (pos < len) {
        uint64_t tag = rd_varint(b, len, &pos);
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (pos > len)
            return;
        if (field == 2 && wire == 2) {
            uint64_t sub = rd_varint(b, len, &pos);
            int end = pos + (int)sub, p = pos;
            if (end > len)
                return;
            while (p < end) {
                uint64_t t = rd_varint(b, end, &p);
                int f = (int)(t >> 3), w = (int)(t & 7);
                if (p > end)
                    break;
                if (f == 1 && w == 0) {
                    nd->battery = (int)rd_varint(b, end, &p);
                    nd->has_batt = 1;
                } else if (f == 2 && w == 5) {
                    nd->voltage = rd_f32(b + p);
                    p += 4;
                    nd->has_batt = 1;
                } else {
                    skip_field(b, end, &p, w);
                }
            }
            pos = end;
        } else {
            skip_field(b, len, &pos, wire);
        }
    }
}

void nodedb_update(const struct mesh_pkt *pkt, int rssi, float snr, unsigned long long now)
{
    int idx = find_or_add(pkt->from);
    struct node *nd = &nodes[idx];
    int hop_limit = pkt->flags & 0x07;
    int hop_start = (pkt->flags >> 5) & 0x07;

    nd->last_heard = now;
    nd->rssi = rssi;
    nd->snr = snr;
    nd->packets++;
    if (hop_start)
        nd->hops = hop_start - hop_limit;

    if (pkt->portnum == MESH_PORT_NODEINFO && pkt->long_name[0]) {
        memcpy(nd->long_name, pkt->long_name, sizeof(nd->long_name));
        memcpy(nd->short_name, pkt->short_name, sizeof(nd->short_name));
        nd->has_name = 1;
    } else if (pkt->portnum == MESH_PORT_POSITION && pkt->payload_len > 0) {
        parse_position(pkt->payload, pkt->payload_len, nd);
    } else if (pkt->portnum == MESH_PORT_TELEMETRY && pkt->payload_len > 0) {
        parse_telemetry(pkt->payload, pkt->payload_len, nd);
    }

    move_to_front(idx);
}
