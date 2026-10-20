/* DAVE link test on the Wii: seed OpenSSL, create a libdave session and
 * make an MLS key package. Logs the time of each step to bigboy. */
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include <openssl/rand.h>
#include <dave/dave.h>

#include "net.h"

int sonoran_seed(unsigned char *out, unsigned int sz);

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

static void on_failure(const char *source, const char *reason, void *user)
{
    (void)user;
    dbg("dave failure: %s: %s", source, reason);
}

static void say(const char *fmt, const char *a, unsigned long long b)
{
    char line[200];

    snprintf(line, sizeof(line), fmt, a, b);
    printf("%s\n", line);
    dbg("%s", line);
}

int main(void)
{
    unsigned char seed[64];
    char err[200];
    unsigned long long t0;
    DAVESessionHandle s;
    uint8_t *kp = NULL;
    size_t kplen = 0;

    video_start();
    WPAD_Init();
    printf("\x1b[2;1HSonoran DAVE test\n\n");
    if (net_start(err, sizeof(err)) != 0)
        printf("network: %s (no log)\n", err);
    dbg("davetest: start");

    /* OpenSSL is built with --with-rand-seed=none: the app seeds it. */
    t0 = ticks_to_millisecs(gettime());
    sonoran_seed(seed, sizeof(seed));
    RAND_seed(seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));
    say("OpenSSL seeded, status %s (%llu ms)", RAND_status() == 1 ? "ok" : "NOT ok",
        ticks_to_millisecs(gettime()) - t0);

    say("libdave max protocol version: %s%llu", "", daveMaxSupportedProtocolVersion());

    t0 = ticks_to_millisecs(gettime());
    s = daveSessionCreate(NULL, NULL, on_failure, NULL);
    say("session create: %s (%llu ms)", s ? "ok" : "FAILED", ticks_to_millisecs(gettime()) - t0);
    if (s != NULL) {
        t0 = ticks_to_millisecs(gettime());
        daveSessionInit(s, daveMaxSupportedProtocolVersion(), 1234567890123ULL, "1234567890");
        say("session init: %s(%llu ms)", "", ticks_to_millisecs(gettime()) - t0);
        t0 = ticks_to_millisecs(gettime());
        daveSessionGetMarshalledKeyPackage(s, &kp, &kplen);
        say("key package: %s, %llu bytes", kp ? "ok" : "FAILED", (unsigned long long)kplen);
        say("key package time: %s%llu ms", "", ticks_to_millisecs(gettime()) - t0);
        daveFree(kp);
        daveSessionDestroy(s);
    }
    dbg("davetest: done");
    printf("\nDone. Push Home to exit.\n");
    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            break;
        VIDEO_WaitVSync();
    }
    return 0;
}
