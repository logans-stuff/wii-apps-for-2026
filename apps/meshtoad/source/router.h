/* Mesh router: receive filtering, managed-flood relaying, reliable unicast and
 * the TX queue, following Meshtastic firmware 2.8.1 (Router, FloodingRouter,
 * NextHopRouter, ReliableRouter, the traffic-management position dedup).
 *
 * Receive flow, mirroring firmware's module order (traffic management first,
 * the routing module - ACK and relay - last):
 *     if (router_rx(frame, len, rssi, snr, &pkt)) {
 *         replied = <app/modules handle pkt>;
 *         router_rx_done(&pkt, replied);
 *     }
 * and call router_tick() every frame to transmit and retransmit. */
#ifndef SONORAN_ROUTER_H
#define SONORAN_ROUTER_H

#include <stdint.h>
#include "airtime.h"
#include "meshtastic.h"

/* Config.DeviceConfig.Role values (only the client roles make sense here). */
#define ROLE_CLIENT 0
#define ROLE_CLIENT_MUTE 1
#define ROLE_CLIENT_HIDDEN 8

/* Config.DeviceConfig.RebroadcastMode */
#define REBROADCAST_ALL 0
#define REBROADCAST_ALL_SKIP_DECODING 1
#define REBROADCAST_LOCAL_ONLY 2
#define REBROADCAST_KNOWN_ONLY 3
#define REBROADCAST_NONE 4
#define REBROADCAST_CORE_PORTNUMS_ONLY 5

/* MeshPacket.Priority */
#define PRIO_BACKGROUND 10
#define PRIO_DEFAULT 64
#define PRIO_RELIABLE 70
#define PRIO_RESPONSE 80
#define PRIO_ACK 120

/* Routing.Error values we raise */
#define ROUTING_NONE 0
#define ROUTING_MAX_RETRANSMIT 5
#define ROUTING_NO_RESPONSE 8
#define ROUTING_DUTY_CYCLE_LIMIT 9

/* Delivery events for packets we sent (router_ops.status). */
enum {
    DELIVERY_RELAYED = 1,  /* a relay rebroadcast it: implicit ACK */
    DELIVERY_ACKED,        /* the destination (or a relay's NAK=0) answered */
    DELIVERY_NAKED,        /* a Routing error came back; err holds it */
    DELIVERY_FAILED,       /* out of retransmissions (MAX_RETRANSMIT) */
};

struct router_cfg {
    uint32_t node_id;
    uint8_t channel_hash;
    int role, rebroadcast;
    int hop_limit;                 /* config.lora.hop_limit */
    float duty_cycle;              /* region duty cycle, 100 = none */
    int override_duty_cycle;
    int ignore_mqtt;
    int neighbor_info;             /* module enabled: else NeighborInfo is dropped */
    uint32_t position_dedup_secs;  /* traffic_management.position_min_interval_secs */
    uint32_t channel_precision;    /* channel position_precision (0 = none set) */
    struct lora_timing timing;
};

struct router_ops {
    int (*radio_send)(const uint8_t *frame, int len); /* blocking TX; 0 = sent */
    int (*radio_busy)(void);                          /* receiving right now */
    uint64_t (*now_ms)(void);
    uint32_t (*rand32)(void);
    int (*node_known)(uint32_t node);                 /* has User info (KNOWN_ONLY) */
    void (*status)(uint32_t id, int event, int err, uint32_t from);
};

void router_init(const struct router_cfg *cfg, const struct router_ops *ops);
const struct router_cfg *router_config(void);
int router_is_rebroadcaster(void);

/* Cap applied to our own broadcast Position/Telemetry/NodeInfo/NeighborInfo
 * (hop scaling). MESH_HOP_MAX = no cap. */
void router_set_broadcast_hop_cap(int hops);

/* Receive one frame. Returns 1 when pkt should be handled by the app and
 * modules (then call router_rx_done), 0 when it was consumed or dropped. */
int router_rx(const uint8_t *frame, int len, int rssi, float snr, struct mesh_pkt *pkt);
/* After the app/modules: ACK (unless a module replied), NO_RESPONSE NAK for an
 * unanswered request, and the managed-flood relay. replied: 1 = a module sent
 * a reply, -1 = a module deliberately ignored the request (throttled). */
void router_rx_done(const struct mesh_pkt *pkt, int replied);

/* Send a packet we originate. hop_limit < 0 = configured limit. Returns the
 * packet id (0 if refused: duty cycle or encode failure). */
uint32_t router_send(uint32_t to, const struct mesh_data *d, int want_ack, int priority,
                     int hop_limit);
/* Routing ACK/NAK for request_id to node `to`. */
uint32_t router_send_ack(uint32_t to, uint32_t request_id, int err, int hop_limit, int want_ack);
/* getHopLimitForResponse */
int router_response_hop_limit(const struct mesh_pkt *p);

/* Transmit whatever is due, run retransmissions. */
void router_tick(void);

struct router_stats {
    unsigned tx, tx_relay, tx_relay_canceled, tx_dropped, rx, rx_dupe, rx_bad;
};
const struct router_stats *router_get_stats(void);
int router_queue_len(void);

#endif
