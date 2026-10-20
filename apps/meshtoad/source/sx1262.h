/* SX1262 LoRa radio over the CH341, wired as on the Meshtoad/MeshTadpole
 * (meshtastic firmware bin/config.d/lora-usb-meshtoad-e22.yaml):
 * CS D0, RXen D1, Reset D2, Busy D4, IRQ (DIO1) D6, DIO2 = RF switch,
 * DIO3 = TCXO. */
#ifndef SONORAN_SX1262_H
#define SONORAN_SX1262_H

#include <stddef.h>
#include <stdint.h>

struct lora_cfg {
    uint32_t freq_hz;
    int sf;          /* 7..12 */
    int bw_khz;      /* 125, 250, 500 */
    int cr;          /* 5..8 means 4/5..4/8 */
    uint8_t sync;    /* Meshtastic: 0x2B */
    int preamble;    /* Meshtastic: 16 */
};

/* Open the CH341, reset the radio and check that it answers. */
int sx_open(char *info, size_t infolen, char *err, size_t errlen);
/* Set the LoRa settings and start continuous receive. */
int sx_start_rx(const struct lora_cfg *c);
/* Transmit one frame, then return to continuous receive. Return 0 on success.
 * sx_start_rx must have been called first (it sets the modulation/channel). */
int sx_send(const uint8_t *buf, int len);
/* Transmit power in dBm (-9..22), applied to subsequent sends. */
void sx_set_tx_power(int dbm);
/* Check for a received packet. Return its length (> 0), 0 when none,
 * or -1 for a packet with a CRC error. */
int sx_poll(uint8_t *buf, int max, int *rssi, float *snr);
/* Instantaneous RSSI in dBm (the current noise/signal floor), or 1 on error. */
int sx_rssi_inst(void);
/* One-shot: dump raw SPI read buffers to the debug log to calibrate the
 * CH341 read alignment (GetStatus and the sync-word register we wrote). */
void sx_debug_reads(void);
void sx_close(void);

#endif
