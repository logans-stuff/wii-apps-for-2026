/* Decode a raw Meshtastic LoRa frame (as sx_poll returns it): parse the
 * packet header, decrypt the channel payload with AES-CTR, and pull the
 * Data protobuf apart far enough to show text messages.
 *
 * Only the default public channel key is tried (PSK index 1, "LongFast").
 * Frame layout: 16-byte header + AES-CTR(channel payload). */
#ifndef SONORAN_MESHTASTIC_H
#define SONORAN_MESHTASTIC_H

#include <stddef.h>
#include <stdint.h>

/* Meshtastic PortNum values we name; others pass through as a number. */
#define MESH_PORT_TEXT 1
#define MESH_PORT_POSITION 3
#define MESH_PORT_NODEINFO 4
#define MESH_PORT_ROUTING 5
#define MESH_PORT_TELEMETRY 67
#define MESH_PORT_TRACEROUTE 70
#define MESH_PORT_NEIGHBORINFO 71

#define MESH_BROADCAST 0xffffffffu
#define MESH_HEADER_LEN 16
#define MESH_HOP_MAX 7

/* Data.bitfield bits (firmware Router.h). */
#define MESH_BITFIELD_OK_TO_MQTT 0x01
#define MESH_BITFIELD_WANT_RESPONSE 0x02

/* The plaintext 16-byte radio header. flags = hop_limit (bits 0-2) |
 * want_ack (3) | via_mqtt (4) | hop_start (5-7). */
struct mesh_hdr {
    uint32_t to, from, id;
    uint8_t hop_limit, hop_start, want_ack, via_mqtt;
    uint8_t channel;    /* channel hash */
    uint8_t next_hop;   /* last byte of the preferred relay, 0 = flood */
    uint8_t relay_node; /* last byte of the node that transmitted this copy */
};

struct mesh_pkt {
    uint32_t to;        /* destination node (0xffffffff = broadcast) */
    uint32_t from;      /* source node */
    uint32_t id;        /* packet id */
    uint8_t flags;      /* hop limit in the low 3 bits */
    uint8_t channel;    /* channel hash from the header */
    uint8_t hop_limit, hop_start, want_ack, via_mqtt, next_hop, relay_node;
    int opaque;         /* channel hash is not ours: header only, not decrypted */
    int data_ok;        /* decrypted to a well-formed Data protobuf with a portnum */
    int has_bitfield;   /* sender is firmware >= 2.5 (bitfield always present) */
    uint32_t bitfield;
    uint32_t portnum;   /* decoded app port, or 0 if the payload didn't parse */
    int want_response;
    uint32_t data_dest, data_source, request_id, reply_id;
    uint8_t payload[237];
    int payload_len;
    char text[237];     /* NUL-terminated when portnum == MESH_PORT_TEXT */
    char long_name[40]; /* from the User protobuf when portnum == MESH_PORT_NODEINFO */
    char short_name[8];
};

struct mesh_route {
    uint32_t route[8], route_back[8];
    int8_t snr_towards[8], snr_back[8];
    int route_count, route_back_count;
    int snr_towards_count, snr_back_count;
};

/* Hops this packet has travelled, or -1 when it cannot be known (pre-2.3
 * senders left hop_start 0; firmware getHopsAway). */
int mesh_hops_away(const struct mesh_pkt *p);

/* Only decrypt frames whose header carries this channel hash; others decode
 * as opaque (header only). -1 (the default) decrypts everything. */
void mesh_set_channel_hash(int hash);
/* Firmware xorHash(name) ^ xorHash(psk): the channel hash for a name/key. */
uint8_t mesh_channel_hash(const char *name, const uint8_t *key, int keylen);
/* config.lora.config_ok_to_mqtt, carried in Data.bitfield of our packets. */
void mesh_set_ok_to_mqtt(int ok);

/* Rewrite the plaintext header of a raw frame (relaying never re-encrypts). */
void mesh_write_header(uint8_t *frame, const struct mesh_hdr *h);

/* Generic Data for mesh_encode. The bitfield is always sent, like firmware's
 * Router::send does for packets it originates. */
struct mesh_data {
    uint32_t portnum;
    const uint8_t *payload;
    int payload_len;
    int want_response;
    uint32_t dest, source, request_id, reply_id;
};
int mesh_encode(const struct mesh_hdr *h, const struct mesh_data *d, uint8_t *frame, int maxlen);

/* Payload encoders (protobuf bytes for mesh_data.payload); return length. */
int mesh_pb_position(uint8_t *out, int max, int32_t lat_i, int32_t lon_i, int has_alt, int32_t alt,
                     uint32_t time, uint32_t precision_bits);
int mesh_pb_device_metrics(uint8_t *out, int max, uint32_t time, uint32_t battery_level,
                           float channel_util, float air_util_tx, uint32_t uptime);
int mesh_pb_neighborinfo(uint8_t *out, int max, uint32_t node_id, uint32_t last_sent_by,
                         uint32_t interval_secs, const uint32_t *ids, const float *snrs, int count);
int mesh_pb_user(uint8_t *out, int max, uint32_t node, const char *long_name, const char *short_name,
                 int hw_model, int role);
/* NeighborInfo pieces needed to relay one (firmware rewrites last_sent_by_id). */
int mesh_parse_neighborinfo(const uint8_t *b, int len, uint32_t *node_id, uint32_t *last_sent_by,
                            uint32_t *interval, uint32_t *ids, float *snrs, int max, int *count);

/* Decode one frame. Return 0 on success (out filled), -1 on a malformed
 * frame. A frame that decrypts but isn't a parseable Data protobuf still
 * returns 0 with portnum == 0 and the raw decrypted bytes in payload. */
int mesh_decode(const uint8_t *frame, int len, struct mesh_pkt *out,
                char *err, size_t errlen);

/* Set the channel key (PSK). len is 16 or 32; anything else, or a NULL key,
 * resets to the public default key. Applies to decode and encode. */
void mesh_set_key(const uint8_t *key, int len);

/* Hop limit (and hop start) for packets we transmit, 0..7. */
void mesh_set_hop_limit(int hops);

/* Hardware-model byte advertised in our NodeInfo (default 255 = PRIVATE_HW). */
void mesh_set_hw_model(int model);

/* Self-test: build a text packet, encrypt it, decode it back. Returns 0 on
 * success. Host-runnable; no radio needed. */
int mesh_selftest(char *err, size_t errlen);

/* Build a transmittable frame on the default channel (header + AES-CTR Data).
 * Return the frame length (>0), or -1 if it would not fit. to is usually
 * 0xffffffff (broadcast); chan_hash must match the channel (0x1f on this mesh). */
int mesh_encode_text(uint32_t from, uint32_t to, uint32_t id, uint8_t chan_hash,
                     const char *text, uint8_t *frame, int maxlen);
int mesh_encode_nodeinfo(uint32_t from, uint32_t id, uint8_t chan_hash,
                         const char *long_name, const char *short_name,
                         uint8_t *frame, int maxlen);
int mesh_encode_traceroute(uint32_t from, uint32_t to, uint32_t id,
                           uint8_t chan_hash, uint8_t *frame, int maxlen);
int mesh_parse_traceroute(const uint8_t *payload, int len, struct mesh_route *out);
/* Reply to a traceroute request (request_id = the request's packet id). */
int mesh_encode_traceroute_reply(uint32_t from, uint32_t to, uint32_t id,
                                 uint8_t chan_hash, uint32_t request_id,
                                 const struct mesh_route *route, uint8_t *frame, int maxlen);

/* Routing (port 5) ACK/NAK. mesh_routing_error returns the error_reason
 * (MESH_ROUTING_NONE = delivered) or -1 if the payload is malformed. */
#define MESH_ROUTING_NONE 0
int mesh_routing_error(const uint8_t *payload, int len);
const char *mesh_routing_error_name(int error);
int mesh_encode_ack(uint32_t from, uint32_t to, uint32_t id, uint8_t chan_hash,
                    uint32_t request_id, int error, uint8_t *frame, int maxlen);

#endif
