/* Sonoran LoRa node: a Meshtastic node on the Wii over the CH341 + SX1262
 * Meshtoad. Listens, decodes, keeps a NodeDB of everyone it hears, and can
 * transmit. Controls:
 *   D-Pad up/down = scroll/select   1 = toggle Nodes / Messages
 *   A = type a message   2 = quick chat   B = announce this node   Home = exit */
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "net.h"
#include "sx1262.h"
#include "meshtastic.h"
#include "nodedb.h"
#include "osk.h"

/* Meshtastic US MediumFast, frequency slot 18: SF9, BW250, CR4/5, sync 0x2B. */
static const struct lora_cfg US_MEDIUMFAST = {
    906375000U, 9, 250, 5, 0x2B, 16
};

#define WII_NODE 0x57494921u    /* shows as !57494921 */
#define WII_LONG "Wii"
#define WII_SHORT "Wii"
#define CHAN_HASH 0x1f

enum { VIEW_NODES, VIEW_MESSAGES };

#define BODY_TOP 9
#define VIEW_ROWS 12
#define MSG_MAX 64

static void *xfb;

static char msgs[MSG_MAX][72];
static int msg_count;
static int msg_top;     /* first visible message in Messages view */
static int node_sel;    /* selected node in Nodes view */

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

static void log_add(const char *s)
{
    int following = msg_top >= msg_count - VIEW_ROWS;
    if (msg_count < MSG_MAX) {
        snprintf(msgs[msg_count++], 72, "%s", s);
    } else {
        int i;
        for (i = 1; i < MSG_MAX; i++)
            memcpy(msgs[i - 1], msgs[i], 72);
        snprintf(msgs[MSG_MAX - 1], 72, "%s", s);
    }
    if (following) /* keep the newest in view */
        msg_top = msg_count > VIEW_ROWS ? msg_count - VIEW_ROWS : 0;
}

static void clear_body(void)
{
    int r;
    for (r = BODY_TOP - 1; r < BODY_TOP + VIEW_ROWS; r++)
        printf("\x1b[%d;1H%-71s", r, "");
}

static void render_messages(void)
{
    int i, maxtop = msg_count > VIEW_ROWS ? msg_count - VIEW_ROWS : 0;
    int last;
    if (msg_top > maxtop)
        msg_top = maxtop;
    if (msg_top < 0)
        msg_top = 0;
    last = msg_top + VIEW_ROWS < msg_count ? msg_top + VIEW_ROWS : msg_count;
    printf("\x1b[%d;1HMessages %d-%d/%d   up/down scroll   1=nodes          \n",
           BODY_TOP - 1, msg_count ? msg_top + 1 : 0, last, msg_count);
    for (i = 0; i < VIEW_ROWS; i++) {
        int idx = msg_top + i;
        printf("\x1b[%d;1H%-71.71s", BODY_TOP + i, idx < msg_count ? msgs[idx] : "");
    }
}

static void fmt_age(unsigned s, char *out, int n)
{
    if (s < 100)
        snprintf(out, n, "%us", s);
    else if (s < 6000)
        snprintf(out, n, "%um", s / 60);
    else
        snprintf(out, n, "%uh", s / 3600);
}

static void node_row(const struct node *nd, unsigned long long now, char *out, int n)
{
    char nm[8], age[12], hop[16], pos[36] = "", batt[16] = "";
    snprintf(nm, sizeof(nm), "%-4.4s", nd->has_name ? nd->short_name : "----");
    fmt_age((unsigned)((now - nd->last_heard) / 1000), age, sizeof(age));
    if (nd->hops >= 0)
        snprintf(hop, sizeof(hop), "%dh", nd->hops);
    else
        snprintf(hop, sizeof(hop), "?");
    if (nd->has_pos)
        snprintf(pos, sizeof(pos), " %.4f,%.4f", nd->lat, nd->lon);
    if (nd->has_batt)
        snprintf(batt, sizeof(batt), " %d%%", nd->battery);
    snprintf(out, n, "%s !%08x %-3s %5.1fdB %-4s%s%s",
             nm, nd->id, hop, nd->snr, age, pos, batt);
}

static void render_nodes(void)
{
    int i, count = nodedb_count(), top, maxtop;
    unsigned long long now = net_ms();

    if (node_sel >= count)
        node_sel = count > 0 ? count - 1 : 0;
    if (node_sel < 0)
        node_sel = 0;
    top = node_sel - VIEW_ROWS / 2;
    maxtop = count > VIEW_ROWS ? count - VIEW_ROWS : 0;
    if (top > maxtop)
        top = maxtop;
    if (top < 0)
        top = 0;

    printf("\x1b[%d;1HNodes %d/%d   up/down select   1=messages              \n",
           BODY_TOP - 1, count ? node_sel + 1 : 0, count);
    for (i = 0; i < VIEW_ROWS; i++) {
        int idx = top + i;
        const struct node *nd = idx < count ? nodedb_get(idx) : NULL;
        char line[80] = "";
        if (nd != NULL)
            node_row(nd, now, line, sizeof(line));
        if (nd != NULL && idx == node_sel)
            printf("\x1b[%d;1H\x1b[7m%-71.71s\x1b[0m", BODY_TOP + i, line);
        else
            printf("\x1b[%d;1H%-71.71s", BODY_TOP + i, line);
    }
}

static void redraw_all(const char *info, int view, long rx, long crc, long tx)
{
    printf("\x1b[2J");
    printf("\x1b[2;1HSonoran LoRa node  (!%08x \"%s\")\n", WII_NODE, WII_LONG);
    printf("\x1b[3;1HRadio: %s\n", info);
    printf("\x1b[4;1H%u Hz SF%d BW%d ch %02x  up/down scroll 1=view A=type 2=quick B=ann Home=exit\n",
           US_MEDIUMFAST.freq_hz, US_MEDIUMFAST.sf, US_MEDIUMFAST.bw_khz, CHAN_HASH);
    printf("\x1b[8;1Hrx %ld  crc %ld  tx %ld\n", rx, crc, tx);
    if (view == VIEW_NODES)
        render_nodes();
    else
        render_messages();
}

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
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) {
            net_stop();
            return 0;
        }
        VIDEO_WaitVSync();
    }
}

int main(void)
{
    char info[200] = "", err[200] = "";
    long packets = 0, crc_errors = 0, sent = 0, polls = 0;
    int view = VIEW_NODES;

    video_start();
    WPAD_Init();
    nodedb_reset();
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
    dbg("lora: listening %u Hz sf%d bw%d ch=%02x", US_MEDIUMFAST.freq_hz,
        US_MEDIUMFAST.sf, US_MEDIUMFAST.bw_khz, CHAN_HASH);

    send_nodeinfo(); /* announce so the Wii shows up on the mesh */
    redraw_all(info, view, packets, crc_errors, sent);

    for (;;) {
        uint8_t buf[256];
        int rssi = 0, n;
        float snr = 0.0f;
        u32 down;

        WPAD_ScanPads();
        down = WPAD_ButtonsDown(0);
        if (down & WPAD_BUTTON_HOME)
            break;
        if (down & (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN)) {
            int dir = (down & WPAD_BUTTON_UP) ? -1 : 1;
            if (view == VIEW_NODES)
                node_sel += dir;
            else
                msg_top += dir;
            if (view == VIEW_NODES)
                render_nodes();
            else
                render_messages();
        }
        if (down & WPAD_BUTTON_1) {
            view = view == VIEW_NODES ? VIEW_MESSAGES : VIEW_NODES;
            clear_body();
            if (view == VIEW_NODES)
                render_nodes();
            else
                render_messages();
        }
        if (down & WPAD_BUTTON_A) {
            char msg[120] = "";
            if (osk_input("Type a message for the mesh:", msg, sizeof(msg), 0) && msg[0]) {
                send_text(msg);
                sent++;
            }
            redraw_all(info, view, packets, crc_errors, sent);
        }
        if (down & WPAD_BUTTON_2) {
            static const char *const quick[] = {
                "On my way", "Ack", "Hello from a Wii", "Testing"
            };
            static int qi;
            send_text(quick[qi++ & 3]);
            sent++;
            if (view == VIEW_MESSAGES)
                render_messages();
        }
        if (down & WPAD_BUTTON_B) {
            send_nodeinfo();
            if (view == VIEW_MESSAGES)
                render_messages();
        }

        if (++polls % 60 == 0) {
            printf("\x1b[8;1Hrx %ld  crc %ld  tx %ld             \n",
                   packets, crc_errors, sent);
            if (view == VIEW_NODES)
                render_nodes(); /* refresh ages ~1x/s */
        }

        n = sx_poll(buf, sizeof(buf), &rssi, &snr);
        if (n < 0) {
            crc_errors++;
        } else if (n > 0) {
            struct mesh_pkt pkt;
            char derr[80], line[72];
            packets++;
            if (mesh_decode(buf, n, &pkt, derr, sizeof(derr)) == 0) {
                nodedb_update(&pkt, rssi, snr, net_ms());
                if (pkt.portnum == MESH_PORT_TEXT) {
                    snprintf(line, sizeof(line), "<%08x> %.55s", pkt.from, pkt.text);
                    dbg("lora: rx text from=%08x '%s'", pkt.from, pkt.text);
                } else if (pkt.portnum == MESH_PORT_NODEINFO) {
                    snprintf(line, sizeof(line), "[node %08x] %.30s (%.6s)",
                             pkt.from, pkt.long_name, pkt.short_name);
                    dbg("lora: rx nodeinfo from=%08x '%s'", pkt.from, pkt.long_name);
                } else {
                    snprintf(line, sizeof(line), "(%08x port %u, %d B)",
                             pkt.from, pkt.portnum, pkt.payload_len);
                    dbg("lora: rx from=%08x port=%u len=%d", pkt.from, pkt.portnum,
                        pkt.payload_len);
                }
                log_add(line);
                if (view == VIEW_NODES)
                    render_nodes();
                else
                    render_messages();
            } else {
                dbg("lora: mesh decode: %s", derr);
            }
        }
        VIDEO_WaitVSync();
    }

    sx_close();
    dbg("lora: exit (rx %ld, crc %ld, tx %ld, %d nodes)", packets, crc_errors,
        sent, nodedb_count());
    net_stop();
    return 0;
}
