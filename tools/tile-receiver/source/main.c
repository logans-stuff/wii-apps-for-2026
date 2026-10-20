/* Small network bootstrap for installing Meshtoad files without pushing the
 * full payload through HBC's unreliable wiiload connection. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include <fat.h>
#include <gccore.h>
#include <network.h>
#include <sys/stat.h>
#include <wiiuse/wpad.h>

#define TILE_PORT 4312
/* The PC running send_tiles.py / send_app.py. Set it in local.mk:
 *   CFLAGS += -DTILE_HOST='"192.0.2.10"' */
#ifndef TILE_HOST
#define TILE_HOST "127.0.0.1"
#endif

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

static int recv_all(int fd, void *data, unsigned len)
{
    unsigned used = 0;
    while (used < len) {
        int n = net_recv(fd, (char *)data + used, len - used, 0);
        if (n <= 0)
            return -1;
        used += (unsigned)n;
    }
    return 0;
}

static unsigned read_u32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16)
         | ((unsigned)p[2] << 8) | p[3];
}

static void make_parents(const char *path)
{
    char dir[160], *p, *slash;
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash == NULL)
        return;
    *slash = '\0';
    for (p = dir + 4; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(dir, 0777); *p = '/'; }
    mkdir(dir, 0777);
}

int main(void)
{
    const char *host = TILE_HOST;
    struct sockaddr_in sa;
    unsigned char head[8], meta[6], buf[8192], ack = 1;
    unsigned count, i, ok = 0;
    int fd = -1, tries, rc;

    video_start();
    WPAD_Init();
    printf("\x1b[2;1HMeshtoad repair receiver\n\n");
    if (!fatInitDefault()) {
        printf("No SD card found. Push Home to exit.\n");
        goto wait;
    }
    printf("Starting network...\n");
    for (tries = 0; tries < 20; tries++) {
        rc = net_init();
        if (rc == 0) break;
        if (rc != -EAGAIN) break;
        usleep(500000);
    }
    if (rc != 0) {
        printf("Network failed (%d).\n", rc);
        goto wait;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_len = sizeof(sa);
    sa.sin_port = htons(TILE_PORT);
    sa.sin_addr.s_addr = inet_addr(host);
    printf("Connecting to %s:%d...\n", host, TILE_PORT);
    fd = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0 || net_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("Could not connect to tile server.\n");
        goto done;
    }
    if (recv_all(fd, head, sizeof(head)) < 0 || memcmp(head, "MTIL", 4) != 0) {
        printf("Invalid tile stream.\n");
        goto done;
    }
    count = read_u32(head + 4);
    printf("Receiving %u files...\n", count);
    for (i = 0; i < count; i++) {
        char rel[120], path[160];
        unsigned plen, left;
        int compressed_boot;
        FILE *f;
        if (recv_all(fd, meta, sizeof(meta)) < 0) break;
        plen = ((unsigned)meta[0] << 8) | meta[1];
        left = read_u32(meta + 2);
        if (plen == 0 || plen >= sizeof(rel) || left > 4 * 1024 * 1024) break;
        if (recv_all(fd, rel, plen) < 0) break;
        rel[plen] = '\0';
        compressed_boot = strcmp(rel, "boot.dol.z") == 0;
        if ((strncmp(rel, "tiles/", 6) != 0 && strcmp(rel, "boot.dol") != 0 &&
             !compressed_boot &&
             strcmp(rel, "meta.xml") != 0 && strcmp(rel, "icon.png") != 0) ||
            strstr(rel, "..") != NULL) break;
        snprintf(path, sizeof(path), "sd:/apps/meshtoad/%s",
                 compressed_boot ? "boot.dol" : rel);
        make_parents(path);
        if (compressed_boot) {
            unsigned packed_len = left, raw_len;
            unsigned char *packed = malloc(packed_len), *raw = NULL;
            uLongf decoded;
            if (packed == NULL || recv_all(fd, packed, packed_len) < 0 || packed_len < 5) {
                free(packed);
                break;
            }
            raw_len = read_u32(packed);
            raw = malloc(raw_len);
            decoded = raw_len;
            if (raw == NULL || uncompress(raw, &decoded, packed + 4, packed_len - 4) != Z_OK ||
                decoded != raw_len) {
                free(raw);
                free(packed);
                break;
            }
            f = fopen(path, "wb");
            if (f == NULL || fwrite(raw, 1, raw_len, f) != raw_len) {
                if (f != NULL) fclose(f);
                free(raw);
                free(packed);
                break;
            }
            fclose(f);
            free(raw);
            free(packed);
            left = 0;
        } else {
            f = fopen(path, "wb");
            if (f == NULL) break;
            while (left > 0) {
                unsigned want = left < sizeof(buf) ? left : sizeof(buf);
                if (recv_all(fd, buf, want) < 0 || fwrite(buf, 1, want, f) != want)
                    break;
                left -= want;
            }
            fclose(f);
        }
        if (left != 0) break;
        ok++;
        net_send(fd, &ack, 1, 0);
        printf("\x1b[10;1H  installed %u / %u   ", ok, count);
    }
    printf("\n\n%s: %u / %u files installed.\n", ok == count ? "Done" : "Stopped", ok, count);
done:
    if (fd >= 0) net_close(fd);
    /* Do not call net_deinit() here.  HBC reuses the IOS network state after
     * this app exits; tearing it down makes wiiload unavailable until the Wii
     * Settings connection test initializes it again. */
wait:
    printf("\nPush Home to exit.\n");
    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) break;
        VIDEO_WaitVSync();
    }
    return 0;
}
