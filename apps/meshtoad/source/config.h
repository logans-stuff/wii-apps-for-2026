/* Meshtoad config: defaults overridable from sd:/apps/meshtoad/config.txt, so
 * the channel, node name and key can change without rebuilding. */
#ifndef SONORAN_CONFIG_H
#define SONORAN_CONFIG_H

#include <stdint.h>
#include "sx1262.h"

struct mesh_config {
    char long_name[40];
    char short_name[8];
    uint32_t node_id;
    struct lora_cfg lora;   /* freq, sf, bw, cr, sync, preamble */
    uint8_t channel_hash;
    uint8_t key[32];
    int keylen;             /* 16 or 32 for a custom key; 0 = public default */
    int hop_limit;          /* hops a transmitted packet may travel (0..7) */
    int tx_power;           /* transmit power in dBm (SX1262, up to 22) */
    char source[80];        /* "defaults" or the file it loaded, for display */
};

/* Fill cfg with defaults, then apply the SD config file if it is present. */
void config_load(struct mesh_config *cfg);

#endif
