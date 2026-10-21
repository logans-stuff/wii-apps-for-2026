// Wii entry point: the firmware's setup() once, then loop() every tick, the
// same cooperative shape as wasm/portduino_main_wasm.cpp. The firmware's log
// (Serial -> stdout) goes to the libogc text console for bring-up.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
#include <ogc/lwp.h>
#include <sys/iosupport.h>
#include <wiiuse/wpad.h>

extern "C" void setup();
extern "C" void loop();
extern void portduinoSetup();
extern void wasm_fs_mount();
extern "C" void wii_default_region();
extern "C" void wii_debuglog_start();
extern "C" void wii_debuglog_stop();
extern "C" void wii_config_load();
extern "C" void wii_config_tick();

// The boot log shows on a text console until MUI's WiiDriver takes over the
// same framebuffer (wii_console_detach).
extern "C" void *wii_xfb = nullptr;
extern "C" GXRModeObj *wii_rmode = nullptr;
extern "C" volatile int wii_ui_active = 0;
extern "C" int wii_debuglog_detach_console(void);
extern "C" void wii_tft_tick(void);

static void console_start()
{
    VIDEO_Init();
    wii_rmode = VIDEO_GetPreferredMode(NULL);
    wii_xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(wii_rmode));
    console_init(wii_xfb, 20, 20, wii_rmode->fbWidth, wii_rmode->xfbHeight, wii_rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(wii_rmode);
    VIDEO_SetNextFramebuffer(wii_xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (wii_rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
}

static ssize_t null_write(struct _reent *, void *, const char *, size_t len)
{
    return (ssize_t)len;
}

// Stop the text console drawing over MUI. With log streaming the log keeps
// going to the build host; otherwise stdout goes nowhere.
extern "C" void wii_console_detach(void)
{
    if (wii_debuglog_detach_console())
        return;
    static devoptab_t quiet;
    quiet = *devoptab_list[STD_OUT];
    quiet.write_r = null_write;
    devoptab_list[STD_OUT] = &quiet;
    devoptab_list[STD_ERR] = &quiet;
}

// Every exit() - the firmware's own on fatal errors, Home, reboot() - comes
// through here (-Wl,--wrap=exit): say why, flush the log, and on an error keep
// the screen up until a button is pressed so it can be read.
extern "C" void __real_exit(int code) __attribute__((noreturn));
extern "C" void __wrap_exit(int code)
{
    printf("exit(%d)%s\n", code, code ? " - press A or Home to return to the loader" : "");
    fflush(stdout);
    wii_debuglog_stop();
    if (code != 0) {
        for (;;) {
            WPAD_ScanPads();
            if (WPAD_ButtonsDown(0) & (WPAD_BUTTON_A | WPAD_BUTTON_HOME))
                break;
            VIDEO_WaitVSync();
        }
    }
    __real_exit(code);
}

// Home on the Wiimote or the console's Reset button quits. A small thread
// watches for them, so quitting also works while setup() (key generation,
// radio init) is still running: the main loop gets 3 s to stop on its own
// before the thread exits for it.
static volatile bool g_quit;
// Main-loop progress for the watchdog: a counter and the stage it is in.
static volatile unsigned g_frames;
static const char *volatile g_stage = "setup";
extern "C" const char *volatile wii_usb_stage;
static void reset_pressed(u32, void *) { g_quit = true; }
extern "C" void wii_request_quit(void) { g_quit = true; }

static void *quit_watch(void *)
{
    int waited = 0, stuck = 0;
    unsigned last = 0;
    for (;;) {
        // Watchdog: report where the main loop is when it stops advancing
        // (logged from this thread, so it still reaches the debug stream).
        if (g_frames == last && strcmp(g_stage, "setup") != 0) {
            if (++stuck % 20 == 0) // every 2 s while stuck
                printf("WATCHDOG main loop stuck %d s in %s (usb %s)\n", stuck / 10, g_stage,
                       wii_usb_stage);
        } else {
            if (stuck >= 20)
                printf("WATCHDOG main loop moving again after %d s\n", stuck / 10);
            stuck = 0;
            last = g_frames;
        }
        if (!wii_ui_active) { // once MUI runs, its driver reads the Wiimote
            WPAD_ScanPads();
            if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
                g_quit = true;
        }
        if (g_quit && (waited += 100) >= 3000)
            exit(0);
        usleep(100 * 1000);
    }
    return NULL;
}

// Seed config.txt from loader arguments (wiiload file.dol preset=MEDIUM_FAST
// slot=18 ...) when the SD card has none yet, so a first boot can join a mesh.
static void seed_config(int argc, char **argv)
{
    if (argc < 2)
        return;
    if (FILE *f = fopen("sd:/apps/meshtasticd/config.txt", "r")) {
        fclose(f);
        return;
    }
    FILE *f = fopen("sd:/apps/meshtasticd/config.txt", "w");
    if (!f)
        return;
    fputs("# written from the loader arguments on first boot\n", f);
    for (int i = 1; i < argc; i++)
        if (strchr(argv[i], '='))
            fprintf(f, "%s\n", argv[i]);
    fclose(f);
}

int main(int argc, char **argv)
{
    console_start();
    setvbuf(stdout, NULL, _IOLBF, 0); // line-buffered: a crash loses at most one line
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
    SYS_SetResetCallback(reset_pressed);
    static lwp_t watcher;
    LWP_CreateThread(&watcher, quit_watch, NULL, NULL, 16 * 1024, 80);
    wii_debuglog_start(); // no-op unless built with WII_DEBUG_HOST
    wasm_fs_mount();
    seed_config(argc, argv);
    portduinoSetup();
    setup();
    wii_default_region();
    wii_config_load(); // sd:/apps/meshtasticd/config.txt, via AdminMessages
    while (!g_quit) {
        g_frames++;
        g_stage = "firmware loop()";
        loop();
        g_stage = "MUI tick";
        wii_tft_tick(); // MUI (device-ui / LVGL)
        wii_config_tick();
    }
    wii_debuglog_stop();
    exit(0);
}
