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
static void reset_pressed(u32, void *) { g_quit = true; }

static void *quit_watch(void *)
{
    int waited = 0;
    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            g_quit = true;
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
        loop();
        wii_config_tick();
    }
    wii_debuglog_stop();
    exit(0);
}
