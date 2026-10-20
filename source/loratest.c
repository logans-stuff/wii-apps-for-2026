/* Sonoran LoRa node: a minimal Meshtastic node on the Wii over the CH341 +
 * SX1262 Meshtoad. It listens on the channel, decodes packets (text, node
 * names), and can transmit: A sends a text message, B announces this node.
 * Home = exit. Runs on the default channel key. */
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "net.h"
#include "sx1262.h"
#include "meshtastic.h"

/* Meshtastic US MediumFast, frequency slot 18: SF9, BW250, CR4/5, sync 0x2B.
 * Slot->freq (BW250): 902.0 + 0.125 + (slot-1)*0.25 MHz = 906.375 MHz. */
static const struct lora_cfg US_MEDIUMFAST = {
    906375000U, 9, 250, 5, 0x2B, 16
};

/* This node's identity on the mesh. CHAN_HASH matches the channel the mesh
 * uses (seen as 0x1f in received packets). */
#define WII_NODE 0x57494921u    /* shows as !57494921 */
#define WII_LONG "Wii"
#define WII_SHORT "Wii"
#define CHAN_HASH 0x1f

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

/* Scrolling message log in the lower part of the screen. */
#define LOGN 9
#define LOG_TOP 11
static char logbuf[LOGN][72];
static int logcount;

static void log_add(const char *s)
{
    int i;
    if (logcount < LOGN) {
        snprintf(logbuf[logcount++], 72, "%s", s);
    } else {
        for (i = 1; i < LOGN; i++)
            memcpy(logbuf[i - 1], logbuf[i], 72);
        snprintf(logbuf[LOGN - 1], 72, "%s", s);
    }
    for (i = 0; i < logcount; i++)
        printf("\x1b[%d;1H%-71.71s\n", LOG_TOP + i, logbuf[i]);
}

/* A fresh-ish packet id each time. */
static uint32_t next_id(void)
{
    static uint32_t c;
    c++;
    return (uint32_t)gettime() ^ (c * 2654435761u);
}

static void send_text(const char *msg)
{
    uint8_t frame[256];
    char line[72];
    int flen = mesh_encode_text(WII_NODE, 0xffffffffu, next_id(), CHAN_HASH, msg,
                                frame, sizeof(frame));
    int rc = flen > 0 ? sx_send(frame, flen) : -1;
    snprintf(line, sizeof(line), "TX text \"%s\" %s", msg, rc == 0 ? "sent" : "FAILED");
    log_add(line);
    dbg("lora: TX text '%s' rc=%d len=%d", msg, rc, flen);
}

static void send_nodeinfo(void)
{
    uint8_t frame[256];
    char line[72];
    int flen = mesh_encode_nodeinfo(WII_NODE, next_id(), CHAN_HASH, WII_LONG,
                                    WII_SHORT, frame, sizeof(frame));
    int rc = flen > 0 ? sx_send(frame, flen) : -1;
    snprintf(line, sizeof(line), "TX nodeinfo \"%s\" %s", WII_LONG, rc == 0 ? "sent" : "FAILED");
    log_add(line);
    dbg("lora: TX nodeinfo rc=%d len=%d", rc, flen);
}

static int wait_home(void)
{
    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            return 0;
        VIDEO_WaitVSync();
    }
}

int main(void)
{
    char info[200] = "", err[200] = "";
    long packets = 0, crc_errors = 0, sent = 0, polls = 0;

    video_start();
    WPAD_Init();
    printf("\x1b[2;1HSonoran LoRa node  (!%08x \"%s\")\n", WII_NODE, WII_LONG);
    if (net_start(err, sizeof(err)) != 0)
        printf("network: %s (no log)\n", err);
    dbg("lora: start");

    if (sx_open(info, sizeof(info), err, sizeof(err)) != 0) {
        printf("Radio: %s\n\nPush Home to exit.\n", err);
        dbg("lora: open failed: %s", err);
        return wait_home();
    }
    printf("Radio: %s\n", info);
    dbg("lora: %s", info);

    if (sx_start_rx(&US_MEDIUMFAST) != 0) {
        printf("Receive start failed.\n\nPush Home to exit.\n");
        dbg("lora: start_rx failed");
        sx_close();
        return wait_home();
    }
    printf("Listening: %u Hz SF%d BW%d CR4/%d ch %02x\n",
           US_MEDIUMFAST.freq_hz, US_MEDIUMFAST.sf, US_MEDIUMFAST.bw_khz,
           US_MEDIUMFAST.cr, CHAN_HASH);
    printf("A = send text   B = announce node   Home = exit\n");
    dbg("lora: listening %u Hz sf%d bw%d ch=%02x", US_MEDIUMFAST.freq_hz,
        US_MEDIUMFAST.sf, US_MEDIUMFAST.bw_khz, CHAN_HASH);

    /* Announce ourselves so the Wii shows up on the mesh. */
    send_nodeinfo();

    for (;;) {
        uint8_t buf[256];
        int rssi = 0, n;
        float snr = 0.0f;
        u32 down;
        char line[72];

        WPAD_ScanPads();
        down = WPAD_ButtonsDown(0);
        if (down & WPAD_BUTTON_HOME)
            break;
        if (down & WPAD_BUTTON_A) {
            char msg[40];
            snprintf(msg, sizeof(msg), "Hello from Wii #%ld", ++sent);
            send_text(msg);
        }
        if (down & WPAD_BUTTON_B)
            send_nodeinfo();

        if (++polls % 240 == 0)
            printf("\x1b[8;1Hrx %ld  crc %ld  tx %ld             \n",
                   packets, crc_errors, sent);

        n = sx_poll(buf, sizeof(buf), &rssi, &snr);
        if (n < 0) {
            crc_errors++;
        } else if (n > 0) {
            struct mesh_pkt pkt;
            char derr[80], hex[520];
            int i, k = 0;
            for (i = 0; i < n && k < (int)sizeof(hex) - 2; i++)
                k += snprintf(hex + k, sizeof(hex) - k, "%02x", buf[i]);
            packets++;
            dbg("lora: rx %d bytes rssi=%d snr=%.1f %s", n, rssi, snr, hex);

            if (mesh_decode(buf, n, &pkt, derr, sizeof(derr)) == 0) {
                if (pkt.portnum == MESH_PORT_TEXT) {
                    snprintf(line, sizeof(line), "<%08x> %.55s", pkt.from, pkt.text);
                    dbg("lora: rx text from=%08x '%s'", pkt.from, pkt.text);
                } else if (pkt.portnum == MESH_PORT_NODEINFO) {
                    snprintf(line, sizeof(line), "[node %08x] %.30s (%.6s)",
                             pkt.from, pkt.long_name, pkt.short_name);
                    dbg("lora: rx nodeinfo from=%08x '%s' (%s)",
                        pkt.from, pkt.long_name, pkt.short_name);
                } else {
                    snprintf(line, sizeof(line), "(%08x port %u, %d B, %d dBm)",
                             pkt.from, pkt.portnum, pkt.payload_len, rssi);
                    dbg("lora: rx from=%08x port=%u len=%d", pkt.from, pkt.portnum,
                        pkt.payload_len);
                }
                log_add(line);
            } else {
                dbg("lora: mesh decode: %s", derr);
            }
        }
        VIDEO_WaitVSync();
    }

    sx_close();
    dbg("lora: exit (rx %ld, crc %ld, tx %ld)", packets, crc_errors, sent);
    return 0;
}
