/* Example: hold A to record from the USB headset microphone, release A
 * to hear it in the headset. Home = exit. */
#include <stdio.h>
#include <stdlib.h>

#include <gccore.h>
#include <wiiuse/wpad.h>

#include "usbaudio.h"

#define MAX_SAMPLES (UA_RATE * 8)   /* 8 seconds */

static int a_held(void)
{
    WPAD_ScanPads();
    return (WPAD_ButtonsHeld(0) & WPAD_BUTTON_A) != 0;
}

int main(void)
{
    static int16_t rec[MAX_SAMPLES];
    char info[200] = "", err[200] = "";
    GXRModeObj *rmode;
    void *xfb;

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
    WPAD_Init();

    printf("\x1b[2;1HUSB headset example\n\n");
    if (ua_open(info, sizeof(info), err, sizeof(err)) != 0) {
        printf("Headset: %s\nPush Home to exit.\n", err);
        while (!(WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)) {
            WPAD_ScanPads();
            VIDEO_WaitVSync();
        }
        return 0;
    }
    printf("Headset: %s\n\nHold A to record (up to 8 s), release A to play.\nHome = exit.\n",
           info);
    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            break;
        if (WPAD_ButtonsHeld(0) & WPAD_BUTTON_A) {
            int n, i, peak = 0;
            printf("\x1b[9;1HRecording...                    ");
            n = ua_record(rec, MAX_SAMPLES, a_held);
            for (i = 0; i < n; i++)
                if (abs(rec[i]) > peak)
                    peak = abs(rec[i]);
            printf("\x1b[9;1HRecorded %.1f s, peak %d. Playing... ", (double)(n > 0 ? n : 0) / UA_RATE,
                   peak);
            ua_play(rec, n > 0 ? n : 0);
            printf("\x1b[10;1HDone. Hold A to record again.   ");
        }
        VIDEO_WaitVSync();
    }
    ua_close();
    return 0;
}
