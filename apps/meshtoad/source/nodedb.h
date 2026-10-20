/* NodeDB: a table of every Meshtastic node we have heard, built from the
 * decoded packets (NodeInfo, Position, Telemetry, and any traffic). Nodes are
 * kept most-recently-heard first. */
#ifndef SONORAN_NODEDB_H
#define SONORAN_NODEDB_H

#include <stdint.h>
#include "meshtastic.h"

#define NODEDB_MAX 64

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
};

void nodedb_reset(void);
/* Fold one decoded packet into the table. now = current epoch seconds. */
void nodedb_update(const struct mesh_pkt *pkt, int rssi, float snr, unsigned long long now);
int nodedb_count(void);
/* 0 = most recently heard. Returns NULL if i is out of range. */
const struct node *nodedb_get(int i);

/* Persist the table to an SD file and load it back (so nodes survive a
 * relaunch). The SD must already be mounted. Return 0 on success. */
int nodedb_save(const char *path);
int nodedb_load(const char *path);

#endif
