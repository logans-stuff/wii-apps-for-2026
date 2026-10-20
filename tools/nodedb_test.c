#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "nodedb.h"

#define TEST_DB "/tmp/meshtoad-nodedb-test.dat"
#define MAGIC 0x4D4E4442u

struct node_v1_fixture {
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

static void fail(const char *why)
{
    fprintf(stderr, "nodedb test failed: %s\n", why);
    remove(TEST_DB);
    exit(1);
}

static void le32(uint8_t *p, int32_t v)
{
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)u;
    p[1] = (uint8_t)(u >> 8);
    p[2] = (uint8_t)(u >> 16);
    p[3] = (uint8_t)(u >> 24);
}

static int varint(uint8_t *p, uint32_t v)
{
    int n = 0;
    do {
        p[n] = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v) p[n] |= 0x80;
        n++;
    } while (v);
    return n;
}

static void f32(uint8_t *p, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    le32(p, (int32_t)u);
}

static void send_position(uint32_t from, double lat, double lon,
                          unsigned long long now)
{
    struct mesh_pkt p;
    int n = 0;
    memset(&p, 0, sizeof(p));
    p.from = from;
    p.portnum = MESH_PORT_POSITION;
    p.payload[n++] = 0x0d;
    le32(p.payload + n, (int32_t)(lat * 1e7)); n += 4;
    p.payload[n++] = 0x15;
    le32(p.payload + n, (int32_t)(lon * 1e7)); n += 4;
    p.payload[n++] = 0x18;
    n += varint(p.payload + n, 350);
    p.payload_len = n;
    nodedb_update(&p, -90, 6.5f, now);
}

static void send_telemetry(uint32_t from, unsigned long long now)
{
    struct mesh_pkt p;
    uint8_t inner[32];
    int i = 0, n = 0;
    memset(&p, 0, sizeof(p));
    p.from = from;
    p.portnum = MESH_PORT_TELEMETRY;
    inner[i++] = 0x08; inner[i++] = 87;
    inner[i++] = 0x15; f32(inner + i, 4.12f); i += 4;
    inner[i++] = 0x1d; f32(inner + i, 21.5f); i += 4;
    inner[i++] = 0x25; f32(inner + i, 1.25f); i += 4;
    inner[i++] = 0x28; i += varint(inner + i, 7200);
    p.payload[n++] = 0x12; p.payload[n++] = (uint8_t)i;
    memcpy(p.payload + n, inner, (size_t)i); n += i;
    p.payload_len = n;
    nodedb_update(&p, -89, 7.0f, now);
}

static void send_neighbor(uint32_t from, uint32_t peer, unsigned long long now)
{
    struct mesh_pkt p;
    uint8_t inner[16];
    int i = 0, n = 0;
    memset(&p, 0, sizeof(p));
    p.from = from;
    p.portnum = MESH_PORT_NEIGHBORINFO;
    p.payload[n++] = 0x08; n += varint(p.payload + n, from);
    inner[i++] = 0x08; i += varint(inner + i, peer);
    inner[i++] = 0x15; f32(inner + i, 8.25f); i += 4;
    p.payload[n++] = 0x22; p.payload[n++] = (uint8_t)i;
    memcpy(p.payload + n, inner, (size_t)i); n += i;
    p.payload_len = n;
    nodedb_update(&p, -88, 8.0f, now);
}

static void test_packets_and_v4_roundtrip(void)
{
    const uint32_t id = 0xaabbccddu, peer = 0x11223344u;
    const struct node *nd;
    nodedb_reset();
    send_position(id, 33.4314, -112.4121, 1000);
    send_position(id, 33.4414, -112.4221, 1400);
    send_telemetry(id, 1500);
    send_neighbor(id, peer, 1600);
    nd = nodedb_find(id);
    if (!nd || !nd->has_complete_pos || nd->track_count != 2 || nd->distance_km < 1.0)
        fail("position trail");
    if (!nd->has_batt || nd->battery != 87 || !nd->has_device_metrics ||
        fabs(nd->channel_utilization - 21.5f) > 0.01f || nd->uptime_seconds != 7200)
        fail("device telemetry");
    if (nd->neighbor_count != 1 || nd->neighbor_ids[0] != peer ||
        fabs(nd->neighbor_snr[0] - 8.25f) > 0.01f)
        fail("neighbor info");
    {
        struct mesh_route r;
        memset(&r, 0, sizeof(r));
        r.route[0] = peer; r.route_count = 1;
        r.snr_towards[0] = 24; r.snr_towards[1] = -12; r.snr_towards_count = 2;
        nodedb_set_route(id, &r, 1700);
    }
    if (nodedb_save(TEST_DB) != 0)
        fail("save v4");
    nodedb_reset();
    if (nodedb_load(TEST_DB) != 0)
        fail("load v4");
    nd = nodedb_find(id);
    if (!nd || nd->track_count != 2 || nd->neighbor_count != 1 || nd->battery != 87)
        fail("v4 round trip");
    if (nd->route_time != 1700 || nd->route.route_count != 1 ||
        nd->route.route[0] != peer || nd->route.snr_towards[1] != -12)
        fail("saved traceroute");
}

/* A v3 file is the v4 record minus the trailing traceroute block. */
static void test_v3_migration(void)
{
    FILE *f;
    uint32_t magic = MAGIC, version = 3, count = 1;
    struct node old;
    const struct node *nd;
    memset(&old, 0, sizeof(old));
    old.id = 0x99aabbccu;
    old.packets = 7;
    old.uptime_seconds = 3600;
    f = fopen(TEST_DB, "wb");
    if (!f) fail("open v3 fixture");
    fwrite(&magic, 4, 1, f); fwrite(&version, 4, 1, f); fwrite(&count, 4, 1, f);
    fwrite(&old, offsetof(struct node, route_time), 1, f); fclose(f);
    if (nodedb_load(TEST_DB) != 0)
        fail("load v3");
    nd = nodedb_find(old.id);
    if (!nd || nd->packets != 7 || nd->uptime_seconds != 3600 || nd->route_time != 0)
        fail("v3 migration content");
}

static void test_v1_migration(void)
{
    FILE *f;
    uint32_t magic = MAGIC, version = 1, count = 1;
    struct node_v1_fixture old;
    const struct node *nd;
    memset(&old, 0, sizeof(old));
    old.id = 0x55667788u;
    strcpy(old.long_name, "Legacy node");
    strcpy(old.short_name, "OLD");
    old.has_name = old.has_pos = 1;
    old.lat = 33.5; old.lon = -112.5;
    old.packets = 42; old.hops = 2;
    f = fopen(TEST_DB, "wb");
    if (!f) fail("open v1 fixture");
    fwrite(&magic, 4, 1, f); fwrite(&version, 4, 1, f); fwrite(&count, 4, 1, f);
    fwrite(&old, sizeof(old), 1, f); fclose(f);
    if (nodedb_load(TEST_DB) != 0)
        fail("load v1");
    nd = nodedb_find(old.id);
    if (!nd || strcmp(nd->long_name, "Legacy node") || nd->packets != 42 ||
        !nd->has_complete_pos || nd->lat != old.lat || nd->lon != old.lon)
        fail("v1 migration content");
}

int main(void)
{
    test_packets_and_v4_roundtrip();
    test_v1_migration();
    test_v3_migration();
    remove(TEST_DB);
    puts("nodedb tests OK (position, telemetry, topology, trails, routes, persistence)");
    return 0;
}
