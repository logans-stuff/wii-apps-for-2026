/* NodeDB: a table of every Meshtastic node we have heard, built from the
 * decoded packets (NodeInfo, Position, Telemetry, and any traffic). Nodes are
 * kept most-recently-heard first. */
#ifndef SONORAN_NODEDB_H
#define SONORAN_NODEDB_H

#include <stdint.h>
#include "meshtastic.h"

#define NODEDB_MAX 64
#define NODE_TRACK_MAX 8
#define NODE_NEIGHBOR_MAX 10

struct node {
    uint32_t id;
    char long_name[40];
    char short_name[8];
    int has_name;
    int has_pos;
    double lat, lon;       /* degrees */
    int alt;               /* metres */
    int has_batt;
    int battery;           /* percent */
    float voltage;         /* volts */
    int rssi;              /* of the last packet heard */
    float snr;
    int hops;              /* hops away, or -1 if unknown */
    unsigned long long last_heard; /* wall-clock epoch seconds */
    int packets;
    int stat_samples;
    long rssi_sum;
    double snr_sum;
    int rssi_best, rssi_worst;
    float snr_best, snr_worst;
    unsigned text_packets, position_packets, telemetry_packets;
    unsigned neighbor_packets;
    int has_complete_pos;
    int track_count, track_head;
    double track_lat[NODE_TRACK_MAX], track_lon[NODE_TRACK_MAX];
    unsigned long long track_time[NODE_TRACK_MAX];
    double distance_km;
    unsigned long long last_moved;
    int neighbor_count;
    uint32_t neighbor_ids[NODE_NEIGHBOR_MAX];
    float neighbor_snr[NODE_NEIGHBOR_MAX];
    unsigned long long neighbor_updated;
    int has_device_metrics;
    float channel_utilization, air_util_tx;
    unsigned uptime_seconds;
    /* Most recent traceroute we ran to this node (v4). route_time first so
     * its 8-byte alignment makes offsetof() equal the v3 record size. */
    unsigned long long route_time; /* 0 = no route yet */
    struct mesh_route route;
};

void nodedb_reset(void);
/* Fold one decoded packet into the table. now = current epoch seconds. */
void nodedb_update(const struct mesh_pkt *pkt, int rssi, float snr, unsigned long long now);
int nodedb_count(void);
/* 0 = most recently heard. Returns NULL if i is out of range. */
const struct node *nodedb_get(int i);
const struct node *nodedb_find(uint32_t id);
int nodedb_order_index(uint32_t id);
/* Remember the latest traceroute result for node id (ignored if unknown). */
void nodedb_set_route(uint32_t id, const struct mesh_route *route, unsigned long long now);

/* Persist the table to an SD file and load it back (so nodes survive a
 * relaunch). The SD must already be mounted. Return 0 on success. */
int nodedb_save(const char *path);
int nodedb_load(const char *path);

#endif
