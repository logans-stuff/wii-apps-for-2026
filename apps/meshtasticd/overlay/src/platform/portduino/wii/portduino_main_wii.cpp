// Wii entry point: the firmware's setup() once, then loop() every tick, the
// same cooperative shape as wasm/portduino_main_wasm.cpp. The firmware's log
// (Serial -> stdout) goes to the libogc text console for bring-up.
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

extern "C" void setup();
extern "C" void loop();
extern void portduinoSetup();
extern void wasm_fs_mount();
extern "C" void wii_default_region();
extern "C" void wii_debuglog_start();
extern "C" void wii_debuglog_stop();

static void console_start()
{
    static void *xfb;
    GXRModeObj *mode;
    VIDEO_Init();
    mode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
    console_init(xfb, 20, 20, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(mode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (mode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    console_start();
    WPAD_Init();
    printf("\x1b[2;0HMeshtastic firmware on Wii - Home exits\n");
    if (!fatInitDefault()) {
        printf("No SD card. Press Home.\n");
        for (;;) {
            WPAD_ScanPads();
            if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
                exit(0);
            VIDEO_WaitVSync();
        }
    }
    wii_debuglog_start(); // no-op unless built with WII_DEBUG_HOST
    wasm_fs_mount();
    portduinoSetup();
    setup();
    wii_default_region();
    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            break;
        loop();
    }
    wii_debuglog_stop();
    exit(0);
}
