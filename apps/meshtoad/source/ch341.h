/* CH341 (1a86:5512) USB-to-SPI bridge driver for the Wii.
 * Commands from the Meshtastic firmware's WebUSB CH341 driver
 * (src/platform/portduino/wasm/js/protocol.js). */
#ifndef SONORAN_CH341_H
#define SONORAN_CH341_H

#include <stddef.h>
#include <stdint.h>

/* USB transfer in flight ("out", "in", or "idle"), for the debug watchdog. */
extern volatile const char *ch341_stage;

/* Open the first CH341 and make SCK (D3), MOSI (D5) and the pins in
 * out_mask outputs. CS is the pin cs_pin (active low). Return 0 on success. */
int ch341_open(int cs_pin, unsigned out_mask, char *info, size_t infolen, char *err, size_t errlen);
void ch341_close(void);

/* Full-duplex SPI with CS held low for the whole transfer. rx may be NULL. */
int ch341_spi(const uint8_t *tx, uint8_t *rx, int len);
/* Set one output pin (D0..D5). */
int ch341_write_pin(int pin, int level);
/* Read the level of pin D0..D7, or negative on error. */
int ch341_read_pin(int pin);

#endif
