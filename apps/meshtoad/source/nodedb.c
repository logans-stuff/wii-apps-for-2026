/* NodeDB implementation. See nodedb.h. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

#include "nodedb.h"

#define NODEDB_MAGIC 0x4D4E4442u /* "MNDB" */
#define NODEDB_VERSION 4
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* On-disk v1 layout, retained so upgrading never wipes the user's nodes. */
struct node_v1 {
    uint32_t id;
    char long_name[40];
    char short_name[8];
    int has_name, has_pos;
    double lat, lon;
    int alt, has_batt, battery;
    float voltage;
    int rssi;
    float snr;
    int hops;
    unsigned long long last_heard;
    int packets;
};

/* Briefly shipped development v2 layout (stats, before topology/trails).
 * Flat, like the struct node that wrote it: nesting node_v1 would insert its
 * tail padding before stat_samples. */
struct node_v2 {
    uint32_t id;
    char long_name[40];
    char short_name[8];
    int has_name, has_pos;
    double lat, lon;
    int alt, has_batt, battery;
    float voltage;
    int rssi;
    float snr;
    int hops;
    unsigned long long last_heard;
    int packets;
    int stat_samples;
    long rssi_sum;
    double snr_sum;
    int rssi_best, rssi_worst;
    float snr_best, snr_worst;
    unsigned text_packets, position_packets, telemetry_packets;
};

/* Each old record is a prefix of struct node; copy only its fields, not its
 * trailing padding (which would land on the next new field). */
#define NODE_V1_FIELDS (offsetof(struct node_v1, packets) + sizeof(int))
#define NODE_V2_FIELDS (offsetof(struct node_v2, telemetry_packets) + sizeof(unsigned))
_Static_assert(offsetof(struct node, packets) == offsetof(struct node_v1, packets),
               "v1 NodeDB migration layout changed");
_Static_assert(offsetof(struct node, telemetry_packets) ==
               offsetof(struct node_v2, telemetry_packets),
               "v2 NodeDB migration layout changed");
/* v3 = everything before the traceroute block; route_time is 8-aligned, so
 * its offset equals the padded v3 record size. */
#define NODE_V3_SIZE offsetof(struct node, route_time)

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

const struct node *nodedb_find(uint32_t id)
{
    int i;
    for (i = 0; i < n_nodes; i++)
        if (nodes[i].id == id)
            return &nodes[i];
    return NULL;
}

int nodedb_order_index(uint32_t id)
{
    int i;
    for (i = 0; i < n_nodes; i++)
        if (nodes[order[i]].id == id)
            return i;
    return -1;
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
static int parse_position(const uint8_t *b, int len, struct node *nd)
{
    int pos = 0;
    int saw_lat = 0, saw_lon = 0;
    double lat = nd->lat, lon = nd->lon;
    while (pos < len) {
        uint64_t tag = rd_varint(b, len, &pos);
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (pos > len)
            return 0;
        if (field == 1 && wire == 5) {
            if (pos + 4 > len) return 0;
            lat = rd_i32(b + pos) / 1e7;
            pos += 4;
            saw_lat = 1;
        } else if (field == 2 && wire == 5) {
            if (pos + 4 > len) return 0;
            lon = rd_i32(b + pos) / 1e7;
            pos += 4;
            saw_lon = 1;
        } else if (field == 3 && wire == 0) {
            nd->alt = (int)(int32_t)rd_varint(b, len, &pos);
        } else {
            skip_field(b, len, &pos, wire);
        }
    }
    /* latitude_i and longitude_i are independently optional in the protobuf.
     * Do not turn the first half of a fix into a bogus coordinate at 0.0.
     * A complete stored fix may still receive a one-coordinate refinement. */
    if ((saw_lat && saw_lon) || nd->has_complete_pos) {
        if (lat >= -85.0 && lat <= 85.0 && lon >= -180.0 && lon <= 180.0 &&
            !(lat == 0.0 && lon == 0.0)) {
            nd->lat = lat;
            nd->lon = lon;
            nd->has_pos = 1;
            if (saw_lat && saw_lon)
                nd->has_complete_pos = 1;
            return nd->has_complete_pos;
        }
    }
    return 0;
}

static double distance_km(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
    double dp = (lat2 - lat1) * M_PI / 180.0;
    double dl = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) *
               sin(dl / 2) * sin(dl / 2);
    if (a > 1.0) a = 1.0;
    return 6371.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

static void track_position(struct node *nd, unsigned long long now)
{
    int last;
    double moved = 0.0;

    if (!nd->has_complete_pos)
        return;
    if (nd->track_count > 0) {
        last = (nd->track_head + NODE_TRACK_MAX - 1) % NODE_TRACK_MAX;
        moved = distance_km(nd->track_lat[last], nd->track_lon[last], nd->lat, nd->lon);
        if (moved < 0.01 && now - nd->track_time[last] < 300)
            return;
        nd->distance_km += moved;
        if (moved >= 0.01)
            nd->last_moved = now;
    } else {
        nd->last_moved = now;
    }
    nd->track_lat[nd->track_head] = nd->lat;
    nd->track_lon[nd->track_head] = nd->lon;
    nd->track_time[nd->track_head] = now;
    nd->track_head = (nd->track_head + 1) % NODE_TRACK_MAX;
    if (nd->track_count < NODE_TRACK_MAX)
        nd->track_count++;
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
                } else if (f == 3 && w == 5) {
                    nd->channel_utilization = rd_f32(b + p);
                    p += 4;
                    nd->has_device_metrics = 1;
                } else if (f == 4 && w == 5) {
                    nd->air_util_tx = rd_f32(b + p);
                    p += 4;
                    nd->has_device_metrics = 1;
                } else if (f == 5 && w == 0) {
                    nd->uptime_seconds = (unsigned)rd_varint(b, end, &p);
                    nd->has_device_metrics = 1;
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

static void parse_neighbor_info(const uint8_t *b, int len, struct node *fallback,
                                unsigned long long now)
{
    uint32_t owner = 0, ids[NODE_NEIGHBOR_MAX];
    float snrs[NODE_NEIGHBOR_MAX];
    int count = 0, pos = 0, i;

    while (pos < len) {
        uint64_t tag = rd_varint(b, len, &pos);
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (pos > len) return;
        if (field == 1 && wire == 0) {
            owner = (uint32_t)rd_varint(b, len, &pos);
        } else if (field == 4 && wire == 2) {
            uint64_t sub = rd_varint(b, len, &pos);
            int end = pos + (int)sub, p = pos;
            uint32_t id = 0;
            float snr = 0.0f;
            if (end > len) return;
            while (p < end) {
                uint64_t nt = rd_varint(b, end, &p);
                int nf = (int)(nt >> 3), nw = (int)(nt & 7);
                if (nf == 1 && nw == 0)
                    id = (uint32_t)rd_varint(b, end, &p);
                else if (nf == 2 && nw == 5 && p + 4 <= end) {
                    snr = rd_f32(b + p);
                    p += 4;
                } else
                    skip_field(b, end, &p, nw);
            }
            if (id != 0 && count < NODE_NEIGHBOR_MAX) {
                ids[count] = id;
                snrs[count++] = snr;
            }
            pos = end;
        } else {
            skip_field(b, len, &pos, wire);
        }
    }
    (void)owner; /* Normally identical to packet.from; retain the radio sender. */
    fallback->neighbor_count = count;
    fallback->neighbor_updated = now;
    for (i = 0; i < count; i++) {
        fallback->neighbor_ids[i] = ids[i];
        fallback->neighbor_snr[i] = snrs[i];
    }
}

void nodedb_set_route(uint32_t id, const struct mesh_route *route, unsigned long long now)
{
    int i;
    for (i = 0; i < n_nodes; i++)
        if (nodes[i].id == id) {
            nodes[i].route = *route;
            nodes[i].route_time = now;
            return;
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
    if (nd->stat_samples == 0) {
        nd->rssi_best = nd->rssi_worst = rssi;
        nd->snr_best = nd->snr_worst = snr;
    } else {
        if (rssi > nd->rssi_best) nd->rssi_best = rssi;
        if (rssi < nd->rssi_worst) nd->rssi_worst = rssi;
        if (snr > nd->snr_best) nd->snr_best = snr;
        if (snr < nd->snr_worst) nd->snr_worst = snr;
    }
    nd->stat_samples++;
    nd->rssi_sum += rssi;
    nd->snr_sum += snr;
    nd->packets++;
    if (pkt->portnum == MESH_PORT_TEXT) nd->text_packets++;
    else if (pkt->portnum == MESH_PORT_POSITION) nd->position_packets++;
    else if (pkt->portnum == MESH_PORT_TELEMETRY) nd->telemetry_packets++;
    else if (pkt->portnum == MESH_PORT_NEIGHBORINFO) nd->neighbor_packets++;
    if (hop_start)
        nd->hops = hop_start - hop_limit;

    if (pkt->portnum == MESH_PORT_NODEINFO && pkt->long_name[0]) {
        memcpy(nd->long_name, pkt->long_name, sizeof(nd->long_name));
        memcpy(nd->short_name, pkt->short_name, sizeof(nd->short_name));
        nd->has_name = 1;
    } else if (pkt->portnum == MESH_PORT_POSITION && pkt->payload_len > 0) {
        if (parse_position(pkt->payload, pkt->payload_len, nd))
            track_position(nd, now);
    } else if (pkt->portnum == MESH_PORT_TELEMETRY && pkt->payload_len > 0) {
        parse_telemetry(pkt->payload, pkt->payload_len, nd);
    } else if (pkt->portnum == MESH_PORT_NEIGHBORINFO && pkt->payload_len > 0) {
        parse_neighbor_info(pkt->payload, pkt->payload_len, nd, now);
    }

    move_to_front(idx);
}

int nodedb_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    uint32_t magic = NODEDB_MAGIC, ver = NODEDB_VERSION, cnt = (uint32_t)n_nodes;
    int i, ok = 1;

    if (f == NULL)
        return -1;
    ok &= fwrite(&magic, 4, 1, f) == 1;
    ok &= fwrite(&ver, 4, 1, f) == 1;
    ok &= fwrite(&cnt, 4, 1, f) == 1;
    /* Most-recently-heard first, so a load keeps the ordering. */
    for (i = 0; i < n_nodes; i++)
        ok &= fwrite(&nodes[order[i]], sizeof(struct node), 1, f) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

int nodedb_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint32_t magic = 0, ver = 0, cnt = 0, i;

    if (f == NULL)
        return -1;
    if (fread(&magic, 4, 1, f) != 1 || fread(&ver, 4, 1, f) != 1 ||
        fread(&cnt, 4, 1, f) != 1 || magic != NODEDB_MAGIC ||
        ver < 1 || ver > NODEDB_VERSION) {
        fclose(f);
        return -1;
    }
    nodedb_reset();
    if (cnt > NODEDB_MAX)
        cnt = NODEDB_MAX;
    for (i = 0; i < cnt; i++) {
        if (ver == 1) {
            struct node_v1 old;
            if (fread(&old, sizeof(old), 1, f) != 1)
                break;
            memset(&nodes[i], 0, sizeof(nodes[i]));
            memcpy(&nodes[i], &old, NODE_V1_FIELDS);
        } else if (ver == 2) {
            struct node_v2 old;
            if (fread(&old, sizeof(old), 1, f) != 1)
                break;
            memset(&nodes[i], 0, sizeof(nodes[i]));
            memcpy(&nodes[i], &old, NODE_V2_FIELDS);
        } else if (ver == 3) {
            memset(&nodes[i], 0, sizeof(nodes[i]));
            if (fread(&nodes[i], NODE_V3_SIZE, 1, f) != 1)
                break;
        } else if (fread(&nodes[i], sizeof(struct node), 1, f) != 1) {
            break;
        }
        order[i] = (int)i; /* file is already most-recent-first */
        /* Preserve and expose legacy partial fixes.  The map separately
         * requires both coordinates, but a saved latitude or longitude must
         * not disappear from the node list. */
        if (!nodes[i].has_pos && (nodes[i].lat != 0.0 || nodes[i].lon != 0.0))
            nodes[i].has_pos = 1;
        if (!nodes[i].has_complete_pos && nodes[i].lat != 0.0 && nodes[i].lon != 0.0)
            nodes[i].has_complete_pos = 1;
        n_nodes = (int)i + 1;
    }
    fclose(f);
    return 0;
}
