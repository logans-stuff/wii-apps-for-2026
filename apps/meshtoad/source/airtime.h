/* Airtime accounting and the transmit gates, ported from Meshtastic firmware
 * 2.8.1 (src/airtime.cpp, RadioInterface.cpp). Times are passed in so the
 * host tests can drive the clock. */
#ifndef SONORAN_AIRTIME_H
#define SONORAN_AIRTIME_H

#include <stdint.h>

enum { AIRTIME_TX, AIRTIME_RX, AIRTIME_RX_ALL };

/* Firmware AirTime thresholds (hard-coded members there too). */
#define AIRTIME_MAX_CHANNEL_UTIL_PERCENT 40
#define AIRTIME_POLITE_CHANNEL_UTIL_PERCENT 25
#define AIRTIME_POLITE_DUTY_CYCLE_PERCENT 50

void airtime_reset(void);
/* One completed TX, one good RX, or one unparseable RX (CRC error). */
void airtime_log(int type, uint32_t ms, uint32_t now_s);
/* % of the last 60 s the channel was busy (TX + RX + RX_ALL). */
float airtime_channel_util(uint32_t now_s);
/* The same behind a ~21 minute EMA (hop scaling's congestion signal). */
float airtime_channel_util_smoothed(uint32_t now_s);
/* % of the last hour we transmitted. */
float airtime_tx_util(uint32_t now_s);

/* isTxAllowedChannelUtil: polite = 25 % (metadata), otherwise 40 %. */
int airtime_tx_allowed_chanutil(int polite, uint32_t now_s);
/* isTxAllowedAirUtil: below half the duty cycle (always true at 100 %). */
int airtime_tx_allowed_airutil(float duty_cycle, int override_duty_cycle, uint32_t now_s);
/* Router::send's hard stop: hourly TX already above the duty cycle. */
int airtime_duty_cycle_exceeded(float duty_cycle, int override_duty_cycle, uint32_t now_s);

/* LoRa timing for the current modem (SX126x). */
struct lora_timing {
    int sf, bw_khz, cr, preamble;
    uint32_t slot_ms;      /* computeSlotTimeMsec */
};
void lora_timing_init(struct lora_timing *t, int sf, int bw_khz, int cr, int preamble);
/* Time on air of a whole frame (header included), in ms. */
uint32_t lora_time_on_air_ms(const struct lora_timing *t, int frame_len);

#define CW_MIN 3
#define CW_MAX 8
#define PROCESSING_TIME_MSEC 4500

/* map(chanutil, 0, 100, CWmin, CWmax) */
int cw_size_for_util(float util);
/* map(snr, -20, 10, CWmin, CWmax), unclamped like the firmware. */
int cw_size_for_snr(float snr);
/* getTxDelayMsec: random(0, 2^CW(util)) slots, for packets we originate. */
uint32_t tx_delay_ms(const struct lora_timing *t, float util, uint32_t rnd);
/* getTxDelayMsecWeighted for a non-router relay: 2*CWmax slots plus
 * random(0, 2^CW(snr)) slots, so weak copies (far nodes) relay first. */
uint32_t tx_delay_weighted_ms(const struct lora_timing *t, float snr, uint32_t rnd);
/* getRetransmissionMsec for a frame of this length. */
uint32_t retransmission_ms(const struct lora_timing *t, int frame_len, float util);

#endif
