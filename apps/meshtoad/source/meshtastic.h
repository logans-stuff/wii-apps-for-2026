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
#define MESH_PORT_TELEMETRY 67

struct mesh_pkt {
    uint32_t to;        /* destination node (0xffffffff = broadcast) */
    uint32_t from;      /* source node */
    uint32_t id;        /* packet id */
    uint8_t flags;      /* hop limit in the low 3 bits */
    uint8_t channel;    /* channel hash from the header */
    uint32_t portnum;   /* decoded app port, or 0 if the payload didn't parse */
    uint8_t payload[237];
    int payload_len;
    char text[237];     /* NUL-terminated when portnum == MESH_PORT_TEXT */
    char long_name[40]; /* from the User protobuf when portnum == MESH_PORT_NODEINFO */
    char short_name[8];
};

/* Decode one frame. Return 0 on success (out filled), -1 on a malformed
 * frame. A frame that decrypts but isn't a parseable Data protobuf still
 * returns 0 with portnum == 0 and the raw decrypted bytes in payload. */
int mesh_decode(const uint8_t *frame, int len, struct mesh_pkt *out,
                char *err, size_t errlen);

/* Set the channel key (PSK). len is 16 or 32; anything else, or a NULL key,
 * resets to the public default key. Applies to decode and encode. */
void mesh_set_key(const uint8_t *key, int len);

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

#endif
