/* Meshtoad: a Meshtastic node on the Wii (SX1262 over a CH341 USB bridge),
 * with a GRRLIB graphical UI. Listens, decodes, keeps a NodeDB, transmits.
 * Controls: D-Pad up/down scroll/select, 1 = Nodes/Messages, A = type a
 * message, 2 = quick chat, B = announce, Home = exit. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <grrlib.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "net.h"
#include "sx1262.h"
#include "ch341.h"
#include "meshtastic.h"
#include "nodedb.h"
#include "config.h"
#include "font_dejavu.h"

#define NODEDB_PATH "sd:/apps/meshtoad/nodedb.dat"
#define MSGDB_PATH "sd:/apps/meshtoad/messages.dat"
#define MSGDB_MAGIC 0x4D4D5347u

static struct mesh_config CFG;

/* Debug builds only: a watchdog thread reports where the main loop is when
 * frames stop advancing, plus fps and the slowest frame every 5 s. */
static volatile const char *stage = "start";
static volatile unsigned frame_no, frame_ms_max;
static volatile int wd_view, wd_run;
static lwp_t wd_thread;

static void *watchdog(void *arg)
{
    unsigned last = 0, since = 0;
    int stuck = 0, tick = 0;
    (void)arg;
    while (wd_run) {
        usleep(1000000);
        if (frame_no == last) {
            stuck++;
            dbg("WATCHDOG stuck %ds: stage=%s usb=%s view=%d frame=%u", stuck,
                stage, ch341_stage, wd_view, frame_no);
        } else {
            if (stuck)
                dbg("WATCHDOG recovered after %ds", stuck);
            stuck = 0;
            last = frame_no;
        }
        if (++tick % 5 == 0) {
            dbg("hb: %u fps, slowest frame %u ms, view=%d stage=%s",
                (frame_no - since) / 5, frame_ms_max, wd_view, stage);
            since = frame_no;
            frame_ms_max = 0;
        }
    }
    return NULL;
}
static GRRLIB_ttfFont *FONT;

/* Palette (RGBA8). */
#define C_BG     0x0E1522FF
#define C_BAR    0x1B2B49FF
#define C_SEL    0x2E5A94FF
#define C_ROW    0x14203AFF
#define C_TEXT   0xE6ECF5FF
#define C_DIM    0x8FA2C0FF
#define C_ACC    0x6CC0FFFF
#define C_GOOD   0x7FE08BFF
#define C_WARN   0xF0C05AFF

enum { VIEW_NODES, VIEW_DETAIL, VIEW_CHAT, VIEW_MESSAGES, VIEW_MAP, VIEW_COUNT };

#define SCR_W 640
#define SCR_H 480
#define KEY_TOP 50   /* controls key strip under the header */
#define KEY_H 22
#define LIST_TOP (KEY_TOP + KEY_H + 4)
#define ROW_H 21
#define VIEW_ROWS 17
#define MSG_MAX 64

static char msgs[MSG_MAX][96];
static uint32_t msg_peer[MSG_MAX];
static uint32_t msg_pkt[MSG_MAX]; /* our packet id for sent DMs, else 0 */
static unsigned char msg_direct[MSG_MAX];
static int msg_count, msg_top, chat_top, node_sel;

/* Send the exact decrypted Position protobuf to the host diagnostics
 * listener.  The payload is small in normal Meshtastic traffic; cap it so a
 * malformed packet cannot overflow dbg()'s line buffer. */
static void debug_position_payload(const struct mesh_pkt *pkt)
{
    char hex[161];
    int i, n;

    if (pkt == NULL || pkt->portnum != MESH_PORT_POSITION)
        return;
    n = pkt->payload_len;
    if (n > 80)
        n = 80;
    for (i = 0; i < n; i++)
        snprintf(hex + i * 2, sizeof(hex) - (size_t)i * 2, "%02x", pkt->payload[i]);
    hex[n * 2] = '\0';
    dbg("position from=%08x len=%d raw=%s%s", pkt->from, pkt->payload_len,
        hex, pkt->payload_len > n ? "..." : "");
}

static int complete_coordinates(const struct node *nd)
{
    return nd != NULL && nd->has_complete_pos && fabs(nd->lat) <= 85.0 &&
           fabs(nd->lon) <= 180.0;
}

static void text(int x, int y, int size, u32 col, const char *fmt, ...)
{
    char s[160];
    va_list ap;
    unsigned char *p;

    if (FONT == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(s, sizeof(s), fmt, ap);
    va_end(ap);
    /* Force printable ASCII (emoji/multibyte in mesh names -> '?'). */
    for (p = (unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p > 0x7e)
            *p = '?';
    GRRLIB_PrintfTTF(x, y, FONT, s, size, col);
}

/* Append a log line; return its index. */
static int log_add_peer(const char *s, uint32_t peer, int direct)
{
    int following = msg_top >= msg_count - VIEW_ROWS, at;
    if (msg_count < MSG_MAX) {
        at = msg_count++;
    } else {
        int i;
        for (i = 1; i < MSG_MAX; i++) {
            memcpy(msgs[i - 1], msgs[i], sizeof(msgs[0]));
            msg_peer[i - 1] = msg_peer[i];
            msg_pkt[i - 1] = msg_pkt[i];
            msg_direct[i - 1] = msg_direct[i];
        }
        at = MSG_MAX - 1;
    }
    snprintf(msgs[at], sizeof(msgs[0]), "%s", s);
    msg_peer[at] = peer;
    msg_pkt[at] = 0;
    msg_direct[at] = (unsigned char)direct;
    if (following)
        msg_top = msg_count > VIEW_ROWS ? msg_count - VIEW_ROWS : 0;
    return at;
}

static void log_add(const char *s) { log_add_peer(s, 0, 0); }

static int messages_save(void)
{
    FILE *f = fopen(MSGDB_PATH, "wb");
    uint32_t magic = MSGDB_MAGIC, version = 1, count = 0;
    int i, ok = 1;
    if (f == NULL) return -1;
    for (i = 0; i < msg_count; i++)
        if (msg_direct[i]) count++;
    ok &= fwrite(&magic, 4, 1, f) == 1;
    ok &= fwrite(&version, 4, 1, f) == 1;
    ok &= fwrite(&count, 4, 1, f) == 1;
    for (i = 0; i < msg_count; i++) {
        if (!msg_direct[i]) continue;
        ok &= fwrite(&msg_peer[i], 4, 1, f) == 1;
        ok &= fwrite(msgs[i], sizeof(msgs[i]), 1, f) == 1;
    }
    fclose(f);
    return ok ? 0 : -1;
}

static void messages_load(void)
{
    FILE *f = fopen(MSGDB_PATH, "rb");
    uint32_t magic, version, count, i;
    if (f == NULL) return;
    if (fread(&magic, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1 ||
        fread(&count, 4, 1, f) != 1 || magic != MSGDB_MAGIC || version != 1) {
        fclose(f);
        return;
    }
    if (count > MSG_MAX) count = MSG_MAX;
    for (i = 0; i < count; i++) {
        uint32_t peer;
        char line[96];
        if (fread(&peer, 4, 1, f) != 1 || fread(line, sizeof(line), 1, f) != 1)
            break;
        line[sizeof(line) - 1] = '\0';
        log_add_peer(line, peer, 1);
    }
    fclose(f);
}

/* Replace the trailing "[status]" of the sent DM with this packet id. */
static void msg_set_status(uint32_t pkt_id, const char *status)
{
    int i;
    for (i = msg_count - 1; i >= 0; i--)
        if (msg_pkt[i] == pkt_id) {
            char *br = strrchr(msgs[i], '[');
            size_t room;
            if (br == NULL)
                br = msgs[i] + strlen(msgs[i]);
            room = sizeof(msgs[0]) - (size_t)(br - msgs[i]);
            snprintf(br, room, "[%s]", status);
            messages_save();
            return;
        }
}

/* Short name, or the node id when the name is unknown. 0xffffffff is the
 * placeholder traceroute uses for a relay that did not record itself. */
static const char *node_label(uint32_t id, char *buf, int n)
{
    const struct node *nd;
    if (id == 0xffffffffu)
        return "?";
    if (id == CFG.node_id)
        return CFG.short_name;
    nd = nodedb_find(id);
    if (nd != NULL && nd->has_name && nd->short_name[0])
        snprintf(buf, (size_t)n, "%s", nd->short_name);
    else
        snprintf(buf, (size_t)n, "!%08x", id);
    return buf;
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

/* ---- graphical on-screen keyboard ---- */
static const char *const KB[] = {
    "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm ,.", "!?@#$%&*-_",
};
#define KB_ROWS 5

static int gfx_osk(const char *title, char *buf, int buflen)
{
    int r = 1, c = 0;
    for (;;) {
        u32 d;
        int row, i;
        WPAD_ScanPads();
        d = WPAD_ButtonsDown(0);
        if (d)
            entropy_add(d);
        if (d & WPAD_BUTTON_HOME)
            return 0;
        if (d & WPAD_BUTTON_PLUS)
            return 1;
        if (d & WPAD_BUTTON_B) {
            int l = strlen(buf);
            if (l > 0)
                buf[l - 1] = '\0';
        }
        if (d & WPAD_BUTTON_UP)
            r = (r + KB_ROWS - 1) % KB_ROWS;
        if (d & WPAD_BUTTON_DOWN)
            r = (r + 1) % KB_ROWS;
        if (d & WPAD_BUTTON_LEFT)
            c = (c + (int)strlen(KB[r]) - 1) % (int)strlen(KB[r]);
        if (d & WPAD_BUTTON_RIGHT)
            c = (c + 1) % (int)strlen(KB[r]);
        if (c >= (int)strlen(KB[r]))
            c = strlen(KB[r]) - 1;
        if (d & WPAD_BUTTON_A) {
            int l = strlen(buf);
            if (l + 1 < buflen) {
                buf[l] = KB[r][c];
                buf[l + 1] = '\0';
            }
        }

        GRRLIB_FillScreen(C_BG);
        GRRLIB_Rectangle(0, 0, SCR_W, 44, C_BAR, 1);
        text(16, 10, 24, C_TEXT, "%s", title);
        GRRLIB_Rectangle(24, 70, SCR_W - 48, 40, C_ROW, 1);
        text(34, 78, 24, C_ACC, "%s_", buf);
        for (row = 0; row < KB_ROWS; row++) {
            int len = strlen(KB[row]);
            for (i = 0; i < len; i++) {
                int kx = 40 + i * 52, ky = 150 + row * 52;
                u32 bg = (row == r && i == c) ? C_SEL : C_ROW;
                char ch[2] = { KB[row][i], 0 };
                GRRLIB_Rectangle(kx, ky, 46, 46, bg, 1);
                text(kx + (ch[0] == ' ' ? 10 : 16), ky + 8, 26, C_TEXT,
                     ch[0] == ' ' ? "sp" : ch);
            }
        }
        text(24, 430, 20, C_DIM,
             "D-Pad move   A type   B delete   + send   Home cancel");
        GRRLIB_Render();
    }
}

/* ---- main views ---- */
static const char *key_text(int view);

static void draw_header(int view, long rx, long tx, long crc)
{
    char cnt[40];
    int w;

    GRRLIB_Rectangle(0, 0, SCR_W, 50, C_BAR, 1);
    text(12, 3, 22, C_TEXT, "Meshtoad");
    text(168, 7, 16, C_ACC, "!%08x  %s", CFG.node_id, CFG.long_name);
    text(12, 30, 13, C_DIM, "SF%d BW%d ch%02x   hop %d  %ddBm   rx %ld tx %ld crc %ld",
         CFG.lora.sf, CFG.lora.bw_khz, CFG.channel_hash, CFG.hop_limit, CFG.tx_power,
         rx, tx, crc);
    if (view == VIEW_NODES)
        snprintf(cnt, sizeof(cnt), "Nodes %d/%d",
                 nodedb_count() ? node_sel + 1 : 0, nodedb_count());
    else if (view == VIEW_DETAIL)
        snprintf(cnt, sizeof(cnt), "Node details");
    else if (view == VIEW_CHAT) {
        const struct node *nd = nodedb_get(node_sel);
        snprintf(cnt, sizeof(cnt), "DM %.8s", nd && nd->has_name ? nd->short_name : "node");
    }
    else if (view == VIEW_MESSAGES)
        snprintf(cnt, sizeof(cnt), "Messages %d", msg_count);
    else
        snprintf(cnt, sizeof(cnt), "Map");
    w = FONT ? GRRLIB_WidthTTF(FONT, cnt, 15) : 0;
    text(SCR_W - w - 12, 8, 15, C_DIM, "%s", cnt);
    GRRLIB_Rectangle(0, KEY_TOP, SCR_W, KEY_H, C_ROW, 1);
    text(10, KEY_TOP + 3, 15, C_DIM, "%s", key_text(view));
}

static double segment_km(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
    double dp = p2 - p1, dl = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    if (a > 1.0) a = 1.0;
    return 6371.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

/* One direction of a route as "A > B 6.2 > C 3.0": each hop shows the SNR
 * it was received with (-128 = unknown). */
static void fmt_route(char *out, int n, uint32_t start, const uint32_t *hops, int count,
                      uint32_t end, const int8_t *snr, int snr_count)
{
    char lb[12];
    int i, len = snprintf(out, (size_t)n, "%s", node_label(start, lb, sizeof(lb)));
    for (i = 0; i <= count && len < n; i++) {
        uint32_t id = i < count ? hops[i] : end;
        len += snprintf(out + len, (size_t)(n - len), " > %s", node_label(id, lb, sizeof(lb)));
        if (len < n && i < snr_count && snr[i] != -128)
            len += snprintf(out + len, (size_t)(n - len), " %.1f", snr[i] / 4.0f);
    }
}

static void draw_saved_route(const struct node *nd)
{
    char line[160], age[16];
    const struct mesh_route *r = &nd->route;

    if (!nd->route_time) {
        text(24, 404, 15, C_DIM, "No traceroute yet  (press - to trace)");
        return;
    }
    fmt_age((unsigned)((unsigned long long)time(NULL) - nd->route_time), age, sizeof(age));
    fmt_route(line, sizeof(line), CFG.node_id, r->route, r->route_count, nd->id,
              r->snr_towards, r->snr_towards_count);
    text(24, 400, 14, C_ACC, "Route %s ago  out: %s", age, line);
    fmt_route(line, sizeof(line), nd->id, r->route_back, r->route_back_count, CFG.node_id,
              r->snr_back, r->snr_back_count);
    text(24, 420, 14, C_ACC, "back: %s", line);
}

static void draw_node_detail(void)
{
    const struct node *nd = nodedb_get(node_sel);
    char age[16];
    double avg_rssi, avg_snr;

    if (nd == NULL) {
        text(30, 100, 20, C_DIM, "No node selected.");
        return;
    }
    fmt_age((unsigned)((unsigned long long)time(NULL) - nd->last_heard), age, sizeof(age));
    avg_rssi = nd->stat_samples ? (double)nd->rssi_sum / nd->stat_samples : nd->rssi;
    avg_snr = nd->stat_samples ? nd->snr_sum / nd->stat_samples : nd->snr;
    text(24, 76, 25, C_TEXT, "%s", nd->has_name ? nd->long_name : "Unknown node");
    text(24, 106, 18, C_ACC, "!%08x   %.8s   last heard %s", nd->id,
         nd->has_name ? nd->short_name : "--------", age);
    text(24, 140, 18, C_TEXT, "Packets %d   text %u   position %u   telemetry %u",
         nd->packets, nd->text_packets, nd->position_packets, nd->telemetry_packets);
    text(24, 172, 18, C_TEXT, "RSSI now %d   avg %.1f   best %d   worst %d dBm",
         nd->rssi, avg_rssi, nd->rssi_best, nd->rssi_worst);
    text(24, 204, 18, C_TEXT, "SNR  now %.1f   avg %.1f   best %.1f   worst %.1f dB",
         nd->snr, avg_snr, nd->snr_best, nd->snr_worst);
    text(24, 236, 18, C_TEXT, "Hops %s%d", nd->hops < 0 ? "unknown " : "", nd->hops < 0 ? 0 : nd->hops);
    if (complete_coordinates(nd))
        text(24, 268, 18, C_ACC, "Location %.6f, %.6f   altitude %dm", nd->lat, nd->lon, nd->alt);
    else
        text(24, 268, 18, C_WARN, "Location incomplete");
    if (nd->has_batt)
        text(24, 298, 18, C_GOOD, "Battery %d%%   %.2fV", nd->battery, nd->voltage);
    if (nd->has_device_metrics)
        text(280, 298, 17, C_TEXT, "channel %.1f%%  air TX %.1f%%  up %uh",
             nd->channel_utilization, nd->air_util_tx, nd->uptime_seconds / 3600);
    {
        char moved[16] = "never";
        double kmh = 0.0;
        if (nd->last_moved)
            fmt_age((unsigned)((unsigned long long)time(NULL) - nd->last_moved),
                    moved, sizeof(moved));
        if (nd->track_count >= 2) {
            /* Speed over the last trail segment. */
            int a = (nd->track_head + NODE_TRACK_MAX - 2) % NODE_TRACK_MAX;
            int b = (nd->track_head + NODE_TRACK_MAX - 1) % NODE_TRACK_MAX;
            unsigned long long dt = nd->track_time[b] - nd->track_time[a];
            if (dt > 0)
                kmh = segment_km(nd->track_lat[a], nd->track_lon[a],
                                 nd->track_lat[b], nd->track_lon[b]) * 3600.0 / (double)dt;
        }
        text(24, 326, 17, C_TEXT, "Trail %d pts   %.2f km   moved %s ago   %.1f km/h",
             nd->track_count, nd->distance_km, moved, kmh);
    }
    if (nd->neighbor_count > 0) {
        int i, x = 24;
        text(24, 354, 15, C_DIM, "Links:");
        for (i = 0; i < nd->neighbor_count && i < 4; i++) {
            const struct node *peer = nodedb_find(nd->neighbor_ids[i]);
            text(x, 376, 14, C_ACC, "%.6s %.1fdB",
                 peer && peer->has_name ? peer->short_name : "????", nd->neighbor_snr[i]);
            x += 150;
        }
    }
    draw_saved_route(nd);
}

static void draw_nodes(void)
{
    int count = nodedb_count(), top, maxtop, i;
    unsigned long long now = (unsigned long long)time(NULL);

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

    for (i = 0; i < VIEW_ROWS; i++) {
        int idx = top + i, y = LIST_TOP + i * ROW_H;
        const struct node *nd = idx < count ? nodedb_get(idx) : NULL;
        char age[12];
        if (nd == NULL)
            continue;
        if (idx == node_sel)
            GRRLIB_Rectangle(6, y - 1, SCR_W - 12, ROW_H, C_SEL, 1);
        fmt_age((unsigned)(now - nd->last_heard), age, sizeof(age));
        text(14, y + 2, 17, nd->has_name ? C_TEXT : C_DIM, "%-4.4s",
             nd->has_name ? nd->short_name : "----");
        text(70, y + 2, 17, C_DIM, "!%08x", nd->id);
        text(210, y + 2, 17, nd->hops == 0 ? C_GOOD : C_TEXT,
             nd->hops >= 0 ? "%dh" : "?h", nd->hops);
        text(252, y + 2, 17, C_TEXT, "%5.1fdB", nd->snr);
        text(345, y + 2, 17, C_DIM, "%s", age);
        if (complete_coordinates(nd))
            text(405, y + 2, 17, C_ACC, "%.4f,%.4f", nd->lat, nd->lon);
        else if (nd->has_pos && nd->lat != 0.0)
            text(405, y + 2, 17, C_WARN, "%.4f,?", nd->lat);
        else if (nd->has_pos && nd->lon != 0.0)
            text(405, y + 2, 17, C_WARN, "?,%.4f", nd->lon);
        if (nd->has_batt)
            text(575, y + 2, 17, C_GOOD, "%d%%", nd->battery);
    }
}

static void draw_messages(void)
{
    int maxtop = msg_count > VIEW_ROWS ? msg_count - VIEW_ROWS : 0, i;
    if (msg_top > maxtop)
        msg_top = maxtop;
    if (msg_top < 0)
        msg_top = 0;
    for (i = 0; i < VIEW_ROWS; i++) {
        int idx = msg_top + i, y = LIST_TOP + i * ROW_H;
        if (idx < msg_count)
            text(14, y + 2, 17, C_TEXT, "%s", msgs[idx]);
    }
}

static void draw_chat(void)
{
    const struct node *nd = nodedb_get(node_sel);
    int indices[MSG_MAX], count = 0, i, top, maxtop;
    if (nd == NULL) {
        text(30, 100, 20, C_DIM, "No node selected.");
        return;
    }
    for (i = 0; i < msg_count; i++)
        if (msg_direct[i] && msg_peer[i] == nd->id)
            indices[count++] = i;
    maxtop = count > VIEW_ROWS ? count - VIEW_ROWS : 0;
    if (chat_top > maxtop) chat_top = maxtop;
    if (chat_top < 0) chat_top = 0;
    top = chat_top;
    if (count == 0)
        text(30, 100, 18, C_DIM, "No direct messages with this node yet.");
    for (i = 0; i < VIEW_ROWS && top + i < count; i++)
        text(14, LIST_TOP + i * ROW_H + 2, 17, C_TEXT, "%s", msgs[indices[top + i]]);
}

#define MAP_TOP (KEY_TOP + KEY_H)
#define MAP_BOT (SCR_H - 26)
#define TCACHE 20
#define MAP_DEFAULT_LAT 33.4314
#define MAP_DEFAULT_LON -112.4121
static struct { int z, x, y, tried; GRRLIB_texImg *tex; } tcache[TCACHE];
static int tile_load_budget;
static int map_zoom_adjust;

/* Meshtastic packets can contain one coordinate while leaving the other at
 * zero.  Treat that as an incomplete fix instead of centering the map at the
 * equator or prime meridian. */
static int valid_position(const struct node *nd)
{
    return complete_coordinates(nd);
}

static double lon2px(double lon, int z) { return (lon + 180.0) / 360.0 * (double)(256 << z); }
static double lat2px(double lat, int z)
{
    double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * (double)(256 << z);
}

/* Load a pre-swizzled native Wii RGB565 tile.  This intentionally avoids
 * GRRLIB's PNG decoder, which can lock real hardware on these map images. */
static GRRLIB_texImg *load_native_tile(const char *path)
{
    const size_t bytes = 256u * 256u * 2u;
    GRRLIB_texImg *tex;
    FILE *f = fopen(path, "rb");
    size_t got;
    if (f == NULL)
        return NULL;
    tex = GRRLIB_CreateEmptyTextureFmt(256, 256, GX_TF_RGB565);
    if (tex == NULL || tex->data == NULL) {
        if (tex != NULL) GRRLIB_FreeTexture(tex);
        fclose(f);
        return NULL;
    }
    got = fread(tex->data, 1, bytes, f);
    fclose(f);
    if (got != bytes) {
        GRRLIB_FreeTexture(tex);
        return NULL;
    }
    GRRLIB_FlushTex(tex);
    return tex;
}

/* Load a map tile from SD (cached). NULL if absent or deferred to a later
 * frame.  Only one new tile is loaded per frame so input remains responsive. */
static GRRLIB_texImg *get_tile(int z, int x, int y)
{
    char path[96];
    int i, slot = -1;
    if (x < 0 || y < 0 || x >= (1 << z) || y >= (1 << z))
        return NULL;
    for (i = 0; i < TCACHE; i++)
        if (tcache[i].tried && tcache[i].z == z && tcache[i].x == x && tcache[i].y == y)
            return tcache[i].tex;
    for (i = 0; i < TCACHE; i++)
        if (!tcache[i].tried) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        if (tcache[0].tex)
            GRRLIB_FreeTexture(tcache[0].tex);
    }
    if (tile_load_budget <= 0)
        return NULL;
    tile_load_budget--;
    snprintf(path, sizeof(path), "sd:/apps/meshtoad/tiles/%d/%d/%d.rgb565", z, x, y);
    tcache[slot].tex = load_native_tile(path);
    tcache[slot].z = z;
    tcache[slot].x = x;
    tcache[slot].y = y;
    tcache[slot].tried = 1;
    return tcache[slot].tex;
}

static void draw_map(void)
{
    int count = nodedb_count(), i, np = 0, z, tx, ty, tx0, tx1, ty0, ty1, drew = 0;
    double minla = 1e9, maxla = -1e9, minlo = 1e9, maxlo = -1e9, cla, clo, dla, dlo;
    double cx, cy;
    int scx = SCR_W / 2, scy = (MAP_TOP + MAP_BOT) / 2, vw = SCR_W, vh = MAP_BOT - MAP_TOP;

    for (i = 0; i < count; i++) {
        const struct node *nd = nodedb_get(i);
        if (!valid_position(nd))
            continue;
        np++;
        if (nd->lat < minla) minla = nd->lat;
        if (nd->lat > maxla) maxla = nd->lat;
        if (nd->lon < minlo) minlo = nd->lon;
        if (nd->lon > maxlo) maxlo = nd->lon;
        {
            int j;
            for (j = 0; j < nd->track_count; j++) {
                if (nd->track_lat[j] < minla) minla = nd->track_lat[j];
                if (nd->track_lat[j] > maxla) maxla = nd->track_lat[j];
                if (nd->track_lon[j] < minlo) minlo = nd->track_lon[j];
                if (nd->track_lon[j] > maxlo) maxlo = nd->track_lon[j];
            }
        }
    }
    if (np == 0) {
        /* Show the installed Phoenix-area map until a complete GPS fix is
         * received, rather than presenting an empty map screen. */
        cla = MAP_DEFAULT_LAT;
        clo = MAP_DEFAULT_LON;
        minla = maxla = cla;
        minlo = maxlo = clo;
    } else {
        cla = (minla + maxla) / 2;
        clo = (minlo + maxlo) / 2;
    }
    dla = maxla - minla;
    dlo = maxlo - minlo;
    if (dlo < 0.0008) dlo = 0.0008;
    if (dla < 0.0008) dla = 0.0008;

    /* Largest zoom whose node span fits the view, clamped to the range we
     * pre-download (11..14) so the chosen zoom matches the tiles on SD. */
    for (z = 14; z > 2; z--) {
        double wpx = dlo / 360.0 * (double)(256 << z);
        double hpx = (lat2px(minla, z) - lat2px(maxla, z));
        if (wpx < vw - 80 && hpx < vh - 80)
            break;
    }
    if (z < 11) z = 11;
    if (z > 14) z = 14;
    z += map_zoom_adjust;
    if (z < 11) z = 11;
    if (z > 14) z = 14;
    cx = lon2px(clo, z);
    cy = lat2px(cla, z);

    tx0 = (int)floor((cx - vw / 2.0) / 256.0);
    tx1 = (int)floor((cx + vw / 2.0) / 256.0);
    ty0 = (int)floor((cy - vh / 2.0) / 256.0);
    ty1 = (int)floor((cy + vh / 2.0) / 256.0);
    tile_load_budget = 1;
    for (ty = ty0; ty <= ty1; ty++)
        for (tx = tx0; tx <= tx1; tx++) {
            GRRLIB_texImg *t = get_tile(z, tx, ty);
            int px = scx + (int)(tx * 256 - cx), py = scy + (int)(ty * 256 - cy);
            if (t != NULL) {
                GRRLIB_DrawImg(px, py, t, 0, 1, 1, 0xFFFFFFFF);
                drew++;
            } else {
                GRRLIB_Rectangle(px, py, 256, 256, C_ROW, 1);
            }
        }

    /* Persistent breadcrumb trails for each positioned node. */
    for (i = 0; i < count; i++) {
        const struct node *nd = nodedb_get(i);
        int j, start, prevx = 0, prevy = 0;
        if (nd == NULL || nd->track_count < 2)
            continue;
        start = nd->track_count == NODE_TRACK_MAX ? nd->track_head : 0;
        for (j = 0; j < nd->track_count; j++) {
            int k = (start + j) % NODE_TRACK_MAX;
            int px = scx + (int)(lon2px(nd->track_lon[k], z) - cx);
            int py = scy + (int)(lat2px(nd->track_lat[k], z) - cy);
            if (j > 0)
                GRRLIB_Line(prevx, prevy, px, py, 0x6CC0FFB0);
            prevx = px;
            prevy = py;
        }
    }

    /* NeighborInfo edges form a live topology overlay when both endpoints
     * have positions. */
    for (i = 0; i < count; i++) {
        const struct node *nd = nodedb_get(i);
        int j;
        if (!valid_position(nd))
            continue;
        for (j = 0; j < nd->neighbor_count; j++) {
            const struct node *peer = nodedb_find(nd->neighbor_ids[j]);
            if (valid_position(peer) && nd->id < peer->id) {
                int x1 = scx + (int)(lon2px(nd->lon, z) - cx);
                int y1 = scy + (int)(lat2px(nd->lat, z) - cy);
                int x2 = scx + (int)(lon2px(peer->lon, z) - cx);
                int y2 = scy + (int)(lat2px(peer->lat, z) - cy);
                GRRLIB_Line(x1, y1, x2, y2, 0x8FA2C080);
            }
        }
    }

    for (i = 0; i < count; i++) {
        const struct node *nd = nodedb_get(i);
        int px, py, r;
        u32 col;
        if (!valid_position(nd))
            continue;
        px = scx + (int)(lon2px(nd->lon, z) - cx);
        py = scy + (int)(lat2px(nd->lat, z) - cy);
        col = (i == node_sel) ? C_WARN : C_GOOD;
        r = (i == node_sel) ? 6 : 4;
        GRRLIB_Rectangle(px - r - 1, py - r - 1, r * 2 + 2, r * 2 + 2, 0x000000FF, 1);
        GRRLIB_Rectangle(px - r, py - r, r * 2, r * 2, col, 1);
        text(px + 8, py - 9, 15, C_TEXT, "%-4.4s", nd->has_name ? nd->short_name : "----");
    }

    text(10, MAP_BOT - 20, 14, C_ACC, "z%d   %d nodes located", z, np);
    if (!drew) {
        int ctx = (int)(cx / 256.0), cty = (int)(cy / 256.0);
        text(60, 236, 18, C_WARN, "No tiles loaded for this area.");
        text(60, 262, 15, C_DIM, "want: z%d  center %.4f,%.4f", z, cla, clo);
        text(60, 284, 15, C_DIM, "tile sd:/apps/meshtoad/tiles/%d/%d/%d.rgb565", z, ctx, cty);
    }
}

/* Controls for this view; shown under the header and in the footer, since
 * TV overscan can hide the bottom of the screen. */
static const char *key_text(int view)
{
    switch (view) {
    case VIEW_DETAIL:   return "A chat   - traceroute   up/down other node   B back";
    case VIEW_CHAT:     return "A type   2 quick reply   - traceroute   up/down scroll   B back";
    case VIEW_MESSAGES: return "A broadcast   2 quick   up/down scroll   1 next   B back";
    case VIEW_MAP:      return "+/- zoom   A open node   1 next   B back";
    default:            return "up/down select   A open   + announce   1 next screen   Home exit";
    }
}

/* Nodes, Messages and Map are top-level screens (1 cycles them); Details
 * and the DM chat sit under Nodes. B goes up one level. */
static int parent_view(int view)
{
    return view == VIEW_CHAT ? VIEW_DETAIL : VIEW_NODES;
}

static int next_top_view(int view)
{
    if (view == VIEW_MESSAGES) return VIEW_MAP;
    if (view == VIEW_MAP) return VIEW_NODES;
    return VIEW_MESSAGES;
}

static void draw_ui(int view, long rx, long tx, long crc)
{
    GRRLIB_FillScreen(C_BG);
    if (view == VIEW_MAP) {
        draw_map();                 /* tiles can overflow; header masks the top */
        draw_header(view, rx, tx, crc);
    } else {
        draw_header(view, rx, tx, crc);
        if (view == VIEW_NODES)
            draw_nodes();
        else if (view == VIEW_DETAIL)
            draw_node_detail();
        else if (view == VIEW_CHAT)
            draw_chat();
        else
            draw_messages();
    }
    GRRLIB_Rectangle(0, SCR_H - 26, SCR_W, 26, C_BAR, 1);
    text(10, SCR_H - 22, 16, C_DIM, "%s", key_text(view));
    GRRLIB_Render();
}

/* A splash during startup so the screen is never a blank black wait, and so a
 * hang shows the last stage reached. */
static void loading(const char *msg)
{
    GRRLIB_FillScreen(C_BG);
    GRRLIB_Rectangle(0, 0, SCR_W, 46, C_BAR, 1);
    text(14, 8, 26, C_TEXT, "Meshtoad");
    text(40, 210, 24, C_ACC, "%s", msg);
    GRRLIB_Render();
}

static uint32_t next_id(void)
{
    static uint32_t c;
    c++;
    return (uint32_t)gettime() ^ (c * 2654435761u);
}

/* DMs go out with want_ack. Like the firmware's ReliableRouter we resend
 * until the destination ACKs, a relay is heard rebroadcasting it (implicit
 * ACK: stop resending but keep waiting for the real one), or we give up. */
#define PENDING_MAX 8
#define RETRY_SECS 20
#define RETRY_TX 3      /* total transmissions */
#define RELAYED_WAIT 60 /* how long to wait for an ACK after a relay took it */
static struct {
    uint32_t id, to;
    uint8_t frame[256];
    int flen, tries, relayed;
    unsigned long long due;
} pending[PENDING_MAX];

static void pending_add(uint32_t id, uint32_t to, const uint8_t *frame, int flen)
{
    int i, slot = 0;
    for (i = 0; i < PENDING_MAX; i++)
        if (pending[i].id == 0) { slot = i; break; }
    if (pending[slot].id != 0)
        msg_set_status(pending[slot].id, "no ack");
    pending[slot].id = id;
    pending[slot].to = to;
    memcpy(pending[slot].frame, frame, (size_t)flen);
    pending[slot].flen = flen;
    pending[slot].tries = 1;
    pending[slot].relayed = 0;
    pending[slot].due = (unsigned long long)time(NULL) + RETRY_SECS;
}

static int pending_find(uint32_t id)
{
    int i;
    for (i = 0; id != 0 && i < PENDING_MAX; i++)
        if (pending[i].id == id)
            return i;
    return -1;
}

static void pending_tick(long *sent)
{
    unsigned long long now = (unsigned long long)time(NULL);
    int i;
    for (i = 0; i < PENDING_MAX; i++) {
        char st[24];
        if (pending[i].id == 0 || now < pending[i].due)
            continue;
        if (pending[i].relayed || pending[i].tries >= RETRY_TX) {
            msg_set_status(pending[i].id, pending[i].relayed ? "relayed" : "no ack");
            pending[i].id = 0;
            continue;
        }
        pending[i].tries++;
        pending[i].due = now + RETRY_SECS;
        sx_send(pending[i].frame, pending[i].flen);
        (*sent)++;
        snprintf(st, sizeof(st), "retry %d", pending[i].tries - 1);
        msg_set_status(pending[i].id, st);
        dbg("lora: retry id=%08x try %d", pending[i].id, pending[i].tries);
    }
}

static void send_text(const char *msg, uint32_t to)
{
    uint8_t frame[256];
    char line[96];
    uint32_t id = next_id();
    int flen = mesh_encode_text(CFG.node_id, to, id, CFG.channel_hash,
                                msg, frame, sizeof(frame));
    int rc = flen > 0 ? sx_send(frame, flen) : -1, at;
    snprintf(line, sizeof(line), "> %s%08x: %.58s [%s]",
             to == 0xffffffffu ? "all " : "!", to, msg, rc == 0 ? "sent" : "FAIL");
    at = log_add_peer(line, to == 0xffffffffu ? 0 : to, to != 0xffffffffu);
    if (to != 0xffffffffu) {
        if (rc == 0) {
            msg_pkt[at] = id;
            pending_add(id, to, frame, flen);
        }
        messages_save();
    }
    dbg("lora: TX text to=%08x id=%08x '%s' rc=%d", to, id, msg, rc);
}

/* Routing ACK back to the sender of a want_ack packet addressed to us. */
static void send_ack(const struct mesh_pkt *pkt)
{
    uint8_t frame[64];
    int flen = mesh_encode_ack(CFG.node_id, pkt->from, next_id(), CFG.channel_hash,
                               pkt->id, MESH_ROUTING_NONE, frame, sizeof(frame));
    int rc = flen > 0 ? sx_send(frame, flen) : -1;
    dbg("lora: TX ack to=%08x for %08x rc=%d", pkt->from, pkt->id, rc);
}

/* Relays that don't record themselves leave the route shorter than the hops
 * the packet actually took; pad with "unknown" so the hop count is right
 * (firmware's insertUnknownHops). */
static void pad_unknown_hops(const struct mesh_pkt *pkt, uint32_t *route, int *count,
                             int8_t *snr, int *snr_count)
{
    int hop_start = (pkt->flags >> 5) & 7, hop_limit = pkt->flags & 7;
    int taken = hop_start - hop_limit;
    while (hop_start && *count < taken && *count < 8)
        route[(*count)++] = 0xffffffffu;
    while (*snr_count < *count)
        snr[(*snr_count)++] = -128;
}

static void answer_traceroute(const struct mesh_pkt *pkt, float snr)
{
    struct mesh_route tr;
    uint8_t frame[256];
    int flen, rc;
    char line[96], lb[12];

    if (mesh_parse_traceroute(pkt->payload, pkt->payload_len, &tr) != 0)
        return;
    pad_unknown_hops(pkt, tr.route, &tr.route_count, tr.snr_towards, &tr.snr_towards_count);
    if (tr.snr_towards_count < 8) /* we are the last hop: add our SNR only */
        tr.snr_towards[tr.snr_towards_count++] = (int8_t)(snr * 4.0f);
    flen = mesh_encode_traceroute_reply(CFG.node_id, pkt->from, next_id(), CFG.channel_hash,
                                        pkt->id, &tr, frame, sizeof(frame));
    rc = flen > 0 ? sx_send(frame, flen) : -1;
    snprintf(line, sizeof(line), "< traceroute from %s answered (%d relays) [%s]",
             node_label(pkt->from, lb, sizeof(lb)), tr.route_count, rc == 0 ? "sent" : "FAIL");
    log_add(line);
}

/* Recently seen (from, id) pairs: relays repeat every packet. */
#define SEEN_MAX 32
static struct { uint32_t from, id; } seen[SEEN_MAX];
static int seen_head;

static int seen_before(const struct mesh_pkt *pkt)
{
    int i;
    for (i = 0; i < SEEN_MAX; i++)
        if (seen[i].id == pkt->id && seen[i].from == pkt->from)
            return 1;
    seen[seen_head].from = pkt->from;
    seen[seen_head].id = pkt->id;
    seen_head = (seen_head + 1) % SEEN_MAX;
    return 0;
}

static void send_traceroute(uint32_t to)
{
    uint8_t frame[256];
    char line[96];
    uint32_t id = next_id();
    int flen = mesh_encode_traceroute(CFG.node_id, to, id, CFG.channel_hash,
                                      frame, sizeof(frame));
    int rc = flen > 0 ? sx_send(frame, flen) : -1;
    snprintf(line, sizeof(line), "> traceroute !%08x [%s]", to,
             rc == 0 ? "sent" : "FAIL");
    log_add(line);
    dbg("lora: TX traceroute to=%08x id=%08x rc=%d", to, id, rc);
}

static void log_traceroute(const struct mesh_pkt *pkt, float snr)
{
    struct mesh_route tr;
    int i;
    char line[96];

    if (mesh_parse_traceroute(pkt->payload, pkt->payload_len, &tr) != 0) {
        snprintf(line, sizeof(line), "Traceroute from !%08x: malformed reply", pkt->from);
        log_add(line);
        return;
    }
    if (pkt->to == CFG.node_id && pkt->request_id) {
        /* Our own trace came back: complete the return path and keep it. */
        pad_unknown_hops(pkt, tr.route_back, &tr.route_back_count, tr.snr_back,
                         &tr.snr_back_count);
        if (tr.snr_back_count < 8)
            tr.snr_back[tr.snr_back_count++] = (int8_t)(snr * 4.0f);
        nodedb_set_route(pkt->from, &tr, (unsigned long long)time(NULL));
    }

    snprintf(line, sizeof(line), "Traceroute %s !%08x: %d out / %d back relays",
             pkt->request_id ? "reply from" : "seen from", pkt->from,
             tr.route_count, tr.route_back_count);
    log_add(line);
    for (i = 0; i <= tr.route_count; i++) {
        uint32_t a = i == 0 ? CFG.node_id : tr.route[i - 1];
        uint32_t b = i < tr.route_count ? tr.route[i] : pkt->from;
        if (i < tr.snr_towards_count && tr.snr_towards[i] != -128)
            snprintf(line, sizeof(line), "  out %d: !%08x -> !%08x  %.1fdB",
                     i + 1, a, b, tr.snr_towards[i] / 4.0f);
        else
            snprintf(line, sizeof(line), "  out %d: !%08x -> !%08x", i + 1, a, b);
        log_add(line);
    }
    for (i = 0; i <= tr.route_back_count; i++) {
        uint32_t a = i == 0 ? pkt->from : tr.route_back[i - 1];
        uint32_t b = i < tr.route_back_count ? tr.route_back[i] : CFG.node_id;
        if (i < tr.snr_back_count && tr.snr_back[i] != -128)
            snprintf(line, sizeof(line), "  back %d: !%08x -> !%08x  %.1fdB",
                     i + 1, a, b, tr.snr_back[i] / 4.0f);
        else
            snprintf(line, sizeof(line), "  back %d: !%08x -> !%08x", i + 1, a, b);
        log_add(line);
    }
}

/* Handle one decoded packet from another node (not a duplicate). */
static void handle_packet(const struct mesh_pkt *pkt, int rssi, float snr, long *sent)
{
    const struct node *selected = nodedb_get(node_sel);
    uint32_t selected_id = selected ? selected->id : 0;
    char line[96];

    debug_position_payload(pkt);
    nodedb_update(pkt, rssi, snr, (unsigned long long)time(NULL));
    if (selected_id) {
        int stable_index = nodedb_order_index(selected_id);
        if (stable_index >= 0)
            node_sel = stable_index;
    }
    line[0] = '\0';
    if (pkt->portnum == MESH_PORT_TEXT) {
        snprintf(line, sizeof(line), "%s%08x: %.67s",
                 pkt->to == CFG.node_id ? "DM !" : "!", pkt->from, pkt->text);
        if (pkt->to == CFG.node_id) {
            log_add_peer(line, pkt->from, 1);
            messages_save();
            line[0] = '\0';
        }
    } else if (pkt->portnum == MESH_PORT_NODEINFO)
        snprintf(line, sizeof(line), "* %.30s (%.6s) joined", pkt->long_name,
                 pkt->short_name);
    else if (pkt->portnum == MESH_PORT_ROUTING && pkt->request_id) {
        int e = mesh_routing_error(pkt->payload, pkt->payload_len);
        int p = pending_find(pkt->request_id);
        if (p >= 0) {
            char lb[12], lb2[12];
            /* A relay's NAK (e.g. no route) is final too: the
             * firmware would have stopped retrying. */
            msg_set_status(pkt->request_id, e == MESH_ROUTING_NONE
                               ? (pkt->from == pending[p].to ? "delivered" : "ack")
                               : mesh_routing_error_name(e));
            if (e != MESH_ROUTING_NONE) {
                snprintf(line, sizeof(line), "! %s for DM to %s (from %s)",
                         mesh_routing_error_name(e),
                         node_label(pending[p].to, lb, sizeof(lb)),
                         node_label(pkt->from, lb2, sizeof(lb2)));
            }
            pending[p].id = 0;
        }
    } else if (pkt->portnum == MESH_PORT_TRACEROUTE) {
        if (pkt->to == CFG.node_id && pkt->want_response && !pkt->request_id) {
            answer_traceroute(pkt, snr);
            (*sent)++;
        } else {
            log_traceroute(pkt, snr);
        }
    } else
        snprintf(line, sizeof(line), "%08x port %u (%d B)", pkt->from,
                 pkt->portnum, pkt->payload_len);
    if (line[0])
        log_add(line);
}

static void send_nodeinfo(void)
{
    uint8_t frame[256];
    int flen = mesh_encode_nodeinfo(CFG.node_id, next_id(), CFG.channel_hash,
                                    CFG.long_name, CFG.short_name, frame, sizeof(frame));
    int rc = flen > 0 ? sx_send(frame, flen) : -1;
    char line[96];
    snprintf(line, sizeof(line), "> announced \"%s\"  [%s]", CFG.long_name,
             rc == 0 ? "sent" : "FAIL");
    log_add(line);
    dbg("lora: TX nodeinfo rc=%d", rc);
}

static void fatal(const char *msg)
{
    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            break;
        GRRLIB_FillScreen(C_BG);
        draw_header(VIEW_NODES, 0, 0, 0);
        text(20, 120, 22, C_WARN, "%s", msg);
        text(20, 160, 18, C_DIM, "Push Home to exit.");
        GRRLIB_Render();
    }
    net_stop();
    GRRLIB_FreeTTF(FONT);
    GRRLIB_Exit();
}

int main(void)
{
    char info[200] = "", err[200] = "";
    long packets = 0, crc_errors = 0, sent = 0, dirty = 0;
    unsigned long long last_save;
    int view = VIEW_NODES, last_view = VIEW_NODES;
    unsigned long long frame_start = 0;

    GRRLIB_Init();
    WPAD_Init();
    FONT = GRRLIB_LoadTTF(dejavu_ttf, dejavu_ttf_len);
    loading("Starting...");
    nodedb_reset();
    loading("Reading config + NodeDB from SD...");
    config_load(&CFG);
    mesh_set_key(CFG.key, CFG.keylen);
    mesh_set_hop_limit(CFG.hop_limit);
    mesh_set_hw_model(CFG.hw_model);
    sx_set_tx_power(CFG.tx_power);
    nodedb_load(NODEDB_PATH);
    messages_load();
    last_save = (unsigned long long)time(NULL);

    if (strcmp(DEBUG_HOST, "127.0.0.1") != 0) {
        net_start(err, sizeof(err));
        wd_run = 1;
        LWP_CreateThread(&wd_thread, watchdog, NULL, NULL, 16 * 1024, 100);
    }
    dbg("lora: start (%s)", CFG.source);

    loading("Detecting the Meshtoad radio over USB...");
    if (sx_open(info, sizeof(info), err, sizeof(err)) != 0) {
        fatal(err);
        return 0;
    }
    loading("Starting receive...");
    if (sx_start_rx(&CFG.lora) != 0) {
        sx_close();
        fatal("Receive start failed.");
        return 0;
    }
    dbg("lora: listening %u Hz sf%d ch=%02x", CFG.lora.freq_hz, CFG.lora.sf,
        CFG.channel_hash);
    send_nodeinfo();

    for (;;) {
        uint8_t buf[256];
        int rssi = 0, n;
        float snr = 0.0f;
        u32 down;

        {
            unsigned long long t = ticks_to_millisecs(gettime());
            if (frame_start && t - frame_start > frame_ms_max)
                frame_ms_max = (unsigned)(t - frame_start);
            frame_start = t;
        }
        frame_no++;
        wd_view = view;
        stage = "input";
        WPAD_ScanPads();
        down = WPAD_ButtonsDown(0);
        if (down & WPAD_BUTTON_HOME)
            break;
        if (down & WPAD_BUTTON_B)
            view = parent_view(view);
        if (down & WPAD_BUTTON_1)
            view = next_top_view(view);
        if (down & (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN)) {
            int d = (down & WPAD_BUTTON_UP) ? -1 : 1;
            if (view == VIEW_MESSAGES)
                msg_top += d;
            else if (view == VIEW_CHAT)
                chat_top += d;
            else
                node_sel += d;
        }
        if (node_sel >= nodedb_count())
            node_sel = nodedb_count() - 1;
        if (node_sel < 0)
            node_sel = 0;
        if (down & WPAD_BUTTON_PLUS) {
            if (view == VIEW_MAP) {
                if (map_zoom_adjust < 3) map_zoom_adjust++;
            } else if (view == VIEW_NODES) {
                stage = "announce";
                send_nodeinfo();
                sent++;
            }
        }
        if (down & WPAD_BUTTON_MINUS) {
            if (view == VIEW_MAP) {
                if (map_zoom_adjust > -3) map_zoom_adjust--;
            } else if (view == VIEW_DETAIL || view == VIEW_CHAT) {
                const struct node *nd = nodedb_get(node_sel);
                if (nd != NULL) {
                    stage = "send traceroute";
                    send_traceroute(nd->id);
                    sent++;
                }
            }
        }
        if (down & WPAD_BUTTON_A) {
            const struct node *nd = nodedb_get(node_sel);
            if ((view == VIEW_NODES || view == VIEW_MAP) && nd != NULL) {
                view = VIEW_DETAIL;
            } else if (view == VIEW_DETAIL && nd != NULL) {
                chat_top = MSG_MAX; /* clamped to the newest lines when drawn */
                view = VIEW_CHAT;
            } else if (view == VIEW_CHAT || view == VIEW_MESSAGES) {
                char msg[120] = "", prompt[80];
                uint32_t to = view == VIEW_CHAT && nd != NULL ? nd->id : 0xffffffffu;
                if (to == 0xffffffffu)
                    snprintf(prompt, sizeof(prompt), "Broadcast message:");
                else
                    snprintf(prompt, sizeof(prompt), "Direct message to %.32s:",
                             nd->has_name ? nd->long_name : nd->short_name);
                stage = "osk";
                if (gfx_osk(prompt, msg, sizeof(msg)) && msg[0]) {
                    stage = "send text";
                    send_text(msg, to);
                    sent++;
                }
            }
        }
        if ((down & WPAD_BUTTON_2) && (view == VIEW_CHAT || view == VIEW_MESSAGES)) {
            static const char *const quick[] = { "On my way", "Ack",
                                                 "Hello from a Wii", "Testing" };
            static int qi;
            const struct node *nd = nodedb_get(node_sel);
            uint32_t to = view == VIEW_CHAT && nd != NULL ? nd->id : 0xffffffffu;
            stage = "send quick";
            send_text(quick[qi++ & 3], to);
            sent++;
        }
        if (view != last_view) {
            dbg("ui: view %d -> %d (node %d/%d)", last_view, view, node_sel, nodedb_count());
            last_view = view;
        }

        stage = "sx_poll";
        n = sx_poll(buf, sizeof(buf), &rssi, &snr);
        if (n < 0) {
            crc_errors++;
        } else if (n > 0) {
            struct mesh_pkt pkt;
            char derr[80];
            packets++;
            if (mesh_decode(buf, n, &pkt, derr, sizeof(derr)) != 0) {
                /* undecodable: counted in packets only */
            } else if (pkt.from == CFG.node_id) {
                /* Our own packet rebroadcast by a relay: implicit ACK. Stop
                 * resending, keep waiting a while for the destination's ACK. */
                int p = pending_find(pkt.id);
                if (p >= 0 && !pending[p].relayed) {
                    pending[p].relayed = 1;
                    pending[p].due = (unsigned long long)time(NULL) + RELAYED_WAIT;
                    msg_set_status(pkt.id, "relayed");
                }
            } else {
                /* ACK every copy: a repeat means our earlier ACK was lost. */
                if (pkt.to == CFG.node_id && (pkt.flags & 0x08) &&
                    pkt.portnum != MESH_PORT_ROUTING && pkt.portnum != MESH_PORT_TRACEROUTE) {
                    stage = "send ack";
                    send_ack(&pkt);
                    sent++;
                }
                if (!seen_before(&pkt)) {
                    stage = "handle packet";
                    handle_packet(&pkt, rssi, snr, &sent);
                    dirty = 1;
                }
            }
        }
        stage = "retries";
        pending_tick(&sent);

        if (dirty && (unsigned long long)time(NULL) - last_save >= 60) {
            stage = "save nodedb";
            nodedb_save(NODEDB_PATH);
            last_save = (unsigned long long)time(NULL);
            dirty = 0;
        }

        stage = "draw";
        draw_ui(view, packets, sent, crc_errors);
    }
    wd_run = 0;

    sx_close();
    nodedb_save(NODEDB_PATH);
    messages_save();
    dbg("lora: exit (rx %ld tx %ld, %d nodes)", packets, sent, nodedb_count());
    net_stop();
    GRRLIB_FreeTTF(FONT);
    GRRLIB_Exit();
    return 0;
}
