/* Airtime accounting. See airtime.h; mirrors firmware 2.8.1 airtime.cpp,
 * including its bucket layout, so the percentages match what a Meshtastic
 * node would report on the same channel. */
#include <math.h>
#include <string.h>

#include "airtime.h"

#define UTIL_PERIODS 6       /* 6 x 10 s = the 60 s channel-utilization window */
#define EMA_DIVISOR 128      /* ~21 minute time constant per 10 s bucket */
#define MINUTES_IN_HOUR 60
#define MS_IN_HOUR (60u * 60u * 1000u)

static struct {
    uint32_t channel_util[UTIL_PERIODS]; /* ms busy per 10 s bucket */
    uint32_t util_tx[MINUTES_IN_HOUR];   /* ms we transmitted per minute */
    uint32_t sec;                        /* last synced uptime second */
    int first;
    int has_avg;
    float avg;
} w = { .first = 1 };

void airtime_reset(void)
{
    memset(&w, 0, sizeof(w));
    w.first = 1;
}

static float util_raw(void)
{
    uint32_t sum = 0;
    int i;
    for (i = 0; i < UTIL_PERIODS; i++)
        sum += w.channel_util[i];
    return (float)sum / (float)(UTIL_PERIODS * 10 * 1000) * 100.0f;
}

static void fold(float sample, uint32_t steps)
{
    float base = 1.0f - 1.0f / (float)EMA_DIVISOR, retained = 1.0f;
    uint32_t e;
    if (steps == 0)
        return;
    if (!w.has_avg) {
        w.avg = sample; /* seed, so a busy channel at boot isn't reported quiet */
        w.has_avg = 1;
        steps--;
    }
    for (e = steps; e != 0; e >>= 1) {
        if (e & 1)
            retained *= base;
        base *= base;
    }
    w.avg = sample + (w.avg - sample) * retained;
}

/* Rotate every bucket crossed since the last call (firmware syncNow). */
static void sync_now(uint32_t now)
{
    uint32_t old, el, stepped, i;

    if (w.first) {
        memset(&w, 0, sizeof(w));
        w.sec = now;
        return;
    }
    if (now <= w.sec)
        return;
    old = w.sec;
    w.sec = now;

    el = now / 10 - old / 10;
    stepped = el < UTIL_PERIODS ? el : UTIL_PERIODS;
    for (i = 1; i <= stepped; i++) {
        fold(util_raw(), 1);
        w.channel_util[(old / 10 + i) % UTIL_PERIODS] = 0;
    }
    fold(0.0f, el - stepped);

    el = now / 60 - old / 60;
    if (el >= MINUTES_IN_HOUR)
        memset(w.util_tx, 0, sizeof(w.util_tx));
    else
        for (i = 1; i <= el; i++)
            w.util_tx[(old / 60 + i) % MINUTES_IN_HOUR] = 0;
}

void airtime_log(int type, uint32_t ms, uint32_t now_s)
{
    sync_now(now_s);
    if (type == AIRTIME_TX)
        w.util_tx[(w.sec / 60) % MINUTES_IN_HOUR] += ms;
    w.channel_util[(w.sec / 10) % UTIL_PERIODS] += ms;
}

float airtime_channel_util(uint32_t now_s)
{
    sync_now(now_s);
    return util_raw();
}

float airtime_channel_util_smoothed(uint32_t now_s)
{
    sync_now(now_s);
    return w.has_avg ? w.avg : util_raw();
}

float airtime_tx_util(uint32_t now_s)
{
    uint32_t sum = 0;
    int i;
    sync_now(now_s);
    for (i = 0; i < MINUTES_IN_HOUR; i++)
        sum += w.util_tx[i];
    return (float)sum / (float)MS_IN_HOUR * 100.0f;
}

int airtime_tx_allowed_chanutil(int polite, uint32_t now_s)
{
    float pct = polite ? AIRTIME_POLITE_CHANNEL_UTIL_PERCENT : AIRTIME_MAX_CHANNEL_UTIL_PERCENT;
    return airtime_channel_util(now_s) < pct;
}

int airtime_tx_allowed_airutil(float duty_cycle, int override_duty_cycle, uint32_t now_s)
{
    if (override_duty_cycle || duty_cycle >= 100.0f)
        return 1;
    return airtime_tx_util(now_s) < duty_cycle * AIRTIME_POLITE_DUTY_CYCLE_PERCENT / 100.0f;
}

int airtime_duty_cycle_exceeded(float duty_cycle, int override_duty_cycle, uint32_t now_s)
{
    if (override_duty_cycle || duty_cycle >= 100.0f)
        return 0;
    return airtime_tx_util(now_s) > duty_cycle;
}

void lora_timing_init(struct lora_timing *t, int sf, int bw_khz, int cr, int preamble)
{
    float symbol = (float)(1u << sf) / (float)bw_khz; /* ms */
    t->sf = sf;
    t->bw_khz = bw_khz;
    t->cr = cr;
    t->preamble = preamble;
    /* SX126x CAD: NUM_SYM_CAD + 0.5 symbols, plus propagation/turnaround/MAC. */
    t->slot_ms = (uint32_t)(2.5f * symbol + 0.2f + 0.4f + 7.0f);
}

uint32_t lora_time_on_air_ms(const struct lora_timing *t, int frame_len)
{
    /* Semtech AN1200.13 / RadioLib SX126x: explicit header, CRC on. */
    float symbol = (float)(1u << t->sf) / (float)t->bw_khz;
    int ldro = symbol > 16.0f;
    int bits = 8 * frame_len - 4 * t->sf + 28 + 16;
    int div = 4 * (t->sf - 2 * ldro);
    int payload = 8 + (bits > 0 ? (bits + div - 1) / div : 0) * (t->cr);
    float syms = (float)t->preamble + 4.25f + (float)payload;
    return (uint32_t)ceilf(syms * symbol);
}

/* Arduino map() on longs, which is what the firmware calls. */
static long amap(long x, long in_min, long in_max, long out_min, long out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

int cw_size_for_util(float util)
{
    return (int)amap((long)util, 0, 100, CW_MIN, CW_MAX);
}

int cw_size_for_snr(float snr)
{
    return (int)amap((long)snr, -20, 10, CW_MIN, CW_MAX);
}

uint32_t tx_delay_ms(const struct lora_timing *t, float util, uint32_t rnd)
{
    int cw = cw_size_for_util(util);
    return (rnd % (1u << cw)) * t->slot_ms;
}

uint32_t tx_delay_weighted_ms(const struct lora_timing *t, float snr, uint32_t rnd)
{
    int cw = cw_size_for_snr(snr);
    if (cw < 0)
        cw = 0;
    if (cw > 16)
        cw = 16;
    return (2u * CW_MAX * t->slot_ms) + (rnd % (1u << cw)) * t->slot_ms;
}

uint32_t retransmission_ms(const struct lora_timing *t, int frame_len, float util)
{
    uint32_t air = lora_time_on_air_ms(t, frame_len);
    int cw = cw_size_for_util(util);
    return 2u * air + ((1u << cw) + 2u * CW_MAX + (1u << ((CW_MAX + CW_MIN) / 2))) * t->slot_ms +
           PROCESSING_TIME_MSEC;
}
