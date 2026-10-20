/* One-shot installer: writes the Meshtoad app (boot.dol, meta.xml, icon.png)
 * to sd:/apps/meshtoad/ so it can be launched from the Homebrew Channel
 * without wiiload. Send it once with wiiload; it does the rest. */
#include <stdio.h>

#include <fat.h>
#include <gccore.h>
#include <sys/stat.h>
#include <wiiuse/wpad.h>

#include "install_data.h"

static void *xfb;

static void video_start(void)
{
    GXRModeObj *r;
    VIDEO_Init();
    r = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(r));
    console_init(xfb, 20, 20, r->fbWidth, r->xfbHeight, r->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(r);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

int main(void)
{
    int i, ok = 0;

    video_start();
    WPAD_Init();
    printf("\x1b[2;1HMeshtoad installer\n\n");

    if (!fatInitDefault()) {
        printf("No SD card found. Push Home to exit.\n");
        goto wait;
    }
    mkdir("sd:/apps", 0777);
    mkdir("sd:/apps/meshtoad", 0777);

    for (i = 0; i < INSTALL_NFILES; i++) {
        char path[64];
        FILE *f;
        size_t n = 0;
        snprintf(path, sizeof(path), "sd:/apps/meshtoad/%s", INSTALL_FILES[i].name);
        f = fopen(path, "wb");
        if (f != NULL) {
            n = fwrite(INSTALL_FILES[i].data, 1, INSTALL_FILES[i].len, f);
            fclose(f);
        }
        if (n == INSTALL_FILES[i].len) {
            ok++;
            printf("  wrote %-10s (%u bytes)\n", INSTALL_FILES[i].name,
                   (unsigned)INSTALL_FILES[i].len);
        } else {
            printf("  FAILED %-10s\n", INSTALL_FILES[i].name);
        }
    }

    printf("\n%s. Meshtoad is now in the Homebrew Channel.\n",
           ok == INSTALL_NFILES ? "Done" : "Some files failed");
wait:
    printf("\nPush Home to exit.\n");
    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            break;
        VIDEO_WaitVSync();
    }
    return 0;
}
