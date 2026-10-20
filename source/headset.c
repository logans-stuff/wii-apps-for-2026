/* Headset test: hold A to record from the USB microphone, release A to
 * hear it in the USB headphones. Home = exit. */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gccore.h>
#include <wiiuse/wpad.h>

#include "net.h"
#include "usbaudio.h"

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
    printf("Hold A and speak into the microphone (up to %d s).\n", MAX_SECONDS);
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
