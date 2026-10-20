/* Headset test: hold A to record from the USB microphone, release A to
 * hear it in the USB headphones. Home = exit. */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "net.h"
#include "usbaudio.h"

#include <opus.h>

#define MAX_SECONDS 8
#define MAX_SAMPLES (UA_RATE * MAX_SECONDS)

static void *xfb;

static void video_start(void)
{
    GXRModeObj *rmode;

    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

static int a_held(void)
{
    WPAD_ScanPads();
    return (WPAD_ButtonsHeld(0) & WPAD_BUTTON_A) != 0;
}

int main(void)
{
    int16_t *rec = malloc(MAX_SAMPLES * sizeof(int16_t));
    char info[200] = "", err[200] = "";

    video_start();
    WPAD_Init();
    printf("\x1b[2;1HSonoran headset test\n\n");
    if (net_start(err, sizeof(err)) != 0)
        printf("network: %s (no log)\n", err);
    dbg("headset: start");
    if (rec == NULL || ua_open(info, sizeof(info), err, sizeof(err)) != 0) {
        printf("Headset: %s\n\nPush Home to exit.\n", rec ? err : "out of memory");
        dbg("headset: open failed: %s", err);
        for (;;) {
            WPAD_ScanPads();
            if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
                return 0;
            VIDEO_WaitVSync();
        }
    }
    printf("Headset: %s\n\n", info);
    printf("Hold A and speak (up to %d s). It plays back through Opus 64 kbps.\n", MAX_SECONDS);
    printf("Release A to hear it in the headset. Home = exit.\n\n");

    for (;;) {
        u32 held;
        WPAD_ScanPads();
        held = WPAD_ButtonsHeld(0);
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            break;
        if (held & WPAD_BUTTON_A) {
            int n, peak = 0, i;
            printf("\x1b[10;1HRecording...                          ");
            {
                unsigned long long t0 = net_ms();
                n = ua_record(rec, MAX_SAMPLES, a_held);
                dbg("headset: recorded %d samples (%.1f s of sound) in %llu ms", n,
                    (double)n / UA_RATE, net_ms() - t0);
            }
            if (n < 0) {
                printf("\x1b[11;1Hrecord error %d\n", n);
                dbg("headset: record error %d", n);
                n = 0;
            }
            for (i = 0; i < n; i++) {
                int v = abs(rec[i]);
                if (v > peak)
                    peak = v;
            }
            if (n >= 960) {
                /* Opus round trip, the same settings as Discord voice:
                 * 48 kHz, 20 ms frames (960 samples), 64 kbps. */
                int err2, f, frames = n / 960;
                static unsigned char pkt[1500];
                static OpusEncoder *enc;
                static OpusDecoder *dec;
                unsigned long long te = 0, td = 0, worst_e = 0, worst_d = 0;
                long bytes = 0;
                if (enc == NULL) {
                    enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err2);
                    dec = opus_decoder_create(48000, 1, &err2);
                    opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
                }
                for (f = 0; f < frames; f++) {
                    unsigned long long t0 = gettime(), t1, t2;
                    int len = opus_encode(enc, rec + f * 960, 960, pkt, sizeof(pkt));
                    t1 = gettime();
                    if (len > 0) {
                        bytes += len;
                        if (opus_decode(dec, pkt, len, rec + f * 960, 960, 0) != 960)
                            dbg("opus: decode failed at frame %d", f);
                    }
                    t2 = gettime();
                    te += t1 - t0;
                    td += t2 - t1;
                    if (t1 - t0 > worst_e)
                        worst_e = t1 - t0;
                    if (t2 - t1 > worst_d)
                        worst_d = t2 - t1;
                }
                n = frames * 960;
                printf("\x1b[12;1HOpus: encode %.2f ms/frame (worst %.2f), decode %.2f ms/frame,"
                       " %ld bytes\n", ticks_to_microsecs(te) / 1000.0 / frames,
                       ticks_to_microsecs(worst_e) / 1000.0, ticks_to_microsecs(td) / 1000.0 / frames,
                       bytes);
                dbg("opus: %d frames, encode avg %u us (worst %u), decode avg %u us (worst %u),"
                    " %ld bytes (%ld bps)", frames,
                    (unsigned)(ticks_to_microsecs(te) / frames), (unsigned)ticks_to_microsecs(worst_e),
                    (unsigned)(ticks_to_microsecs(td) / frames), (unsigned)ticks_to_microsecs(worst_d),
                    bytes, bytes * 8 * 50 / frames);
            }
            printf("\x1b[10;1HRecorded %.1f s, peak level %d of 32767.   \n",
                   (double)n / UA_RATE, peak);
            dbg("headset: recorded %d samples, peak %d", n, peak);
            {
                unsigned long long t0 = net_ms();
                i = ua_play(rec, n);
                dbg("headset: played %d samples (%.1f s of sound) in %llu ms, rc %d", n,
                    (double)n / UA_RATE, net_ms() - t0, i);
            }
            printf("\x1b[11;1HPlaying...                              ");
            printf("\x1b[11;1H%s                                 \n",
                   i == 0 ? "Played. Hold A to record again." : "play error");
            dbg("headset: play rc %d", i);
        }
        VIDEO_WaitVSync();
    }
    ua_close();
    dbg("headset: exit");
    return 0;
}
