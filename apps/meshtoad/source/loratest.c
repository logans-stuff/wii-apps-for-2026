/* Meshtoad: a Meshtastic node on the Wii (SX1262 over a CH341 USB bridge),
 * with a GRRLIB graphical UI. Listens, decodes, keeps a NodeDB, transmits.
 * Controls: D-Pad up/down scroll/select, 1 = Nodes/Messages, A = type a
 * message, 2 = quick chat, B = announce, Home = exit. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <grrlib.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "net.h"
#include "sx1262.h"
#include "meshtastic.h"
#include "nodedb.h"
#include "config.h"

#define NODEDB_PATH "sd:/apps/meshtoad/nodedb.dat"

static struct mesh_config CFG;

/* We use GRRLIB's built-in bitmap font, not TTF, so stub the TTF init/exit
 * that GRRLIB's core calls (GRRLIB_ttf.c is not compiled -> no freetype). */
int GRRLIB_InitTTF(void) { return 0; }
void GRRLIB_ExitTTF(void) {}

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

enum { VIEW_NODES, VIEW_MESSAGES, VIEW_MAP, VIEW_COUNT };

#define SCR_W 640
#define SCR_H 480
#define LIST_TOP 54
#define ROW_H 21
#define VIEW_ROWS 17
#define MSG_MAX 64

static char msgs[MSG_MAX][96];
static int msg_count, msg_top, node_sel;

static void text(int x, int y, int size, u32 col, const char *fmt, ...)
{
    char s[160];
    va_list ap;
    unsigned char *p;

    va_start(ap, fmt);
    vsnprintf(s, sizeof(s), fmt, ap);
    va_end(ap);
    /* Force printable ASCII (emoji/multibyte in mesh names -> '?'). */
    for (p = (unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p > 0x7e)
            *p = '?';
    GRRLIB_PrintfSystemFont(x, y, s, size, col);
}

/* Approximate rendered width of a string in the bitmap font, for alignment. */
static int text_w(const char *s, int size)
{
    return (int)strlen(s) * size * 6 / 10;
}

static void log_add(const char *s)
{
    int following = msg_top >= msg_count - VIEW_ROWS;
    if (msg_count < MSG_MAX)
        snprintf(msgs[msg_count++], sizeof(msgs[0]), "%s", s);
    else {
        int i;
        for (i = 1; i < MSG_MAX; i++)
            memcpy(msgs[i - 1], msgs[i], sizeof(msgs[0]));
        snprintf(msgs[MSG_MAX - 1], sizeof(msgs[0]), "%s", s);
    }
    if (following)
        msg_top = msg_count > VIEW_ROWS ? msg_count - VIEW_ROWS : 0;
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
    else if (view == VIEW_MESSAGES)
        snprintf(cnt, sizeof(cnt), "Messages %d", msg_count);
    else
        snprintf(cnt, sizeof(cnt), "Map");
    w = text_w(cnt, 15);
    text(SCR_W - w - 12, 8, 15, C_DIM, "%s", cnt);
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
        if (nd->has_pos)
            text(405, y + 2, 17, C_ACC, "%.4f,%.4f", nd->lat, nd->lon);
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

#define MAP_TOP 52
#define MAP_BOT (SCR_H - 26)
#define TCACHE 20
static struct { int z, x, y, tried; GRRLIB_texImg *tex; } tcache[TCACHE];

static double lon2px(double lon, int z) { return (lon + 180.0) / 360.0 * (double)(256 << z); }
static double lat2px(double lat, int z)
{
    double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * (double)(256 << z);
}

/* Load an OSM tile sd:/apps/meshtoad/tiles/z/x/y.png (cached). NULL if absent. */
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
    snprintf(path, sizeof(path), "sd:/apps/meshtoad/tiles/%d/%d/%d.png", z, x, y);
    tcache[slot].tex = GRRLIB_LoadTextureFromFile(path);
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
        if (nd == NULL || !nd->has_pos)
            continue;
        np++;
        if (nd->lat < minla) minla = nd->lat;
        if (nd->lat > maxla) maxla = nd->lat;
        if (nd->lon < minlo) minlo = nd->lon;
        if (nd->lon > maxlo) maxlo = nd->lon;
    }
    if (np == 0) {
        text(40, 210, 22, C_DIM, "No node positions yet.");
        text(40, 242, 16, C_DIM, "Nodes that broadcast GPS will plot here.");
        return;
    }
    cla = (minla + maxla) / 2;
    clo = (minlo + maxlo) / 2;
    dla = maxla - minla;
    dlo = maxlo - minlo;
    if (dlo < 0.0008) dlo = 0.0008;
    if (dla < 0.0008) dla = 0.0008;

    /* Largest zoom whose node span still fits the view (with margin). */
    for (z = 16; z > 1; z--) {
        double wpx = dlo / 360.0 * (double)(256 << z);
        double hpx = (lat2px(minla, z) - lat2px(maxla, z));
        if (wpx < vw - 80 && hpx < vh - 80)
            break;
    }
    cx = lon2px(clo, z);
    cy = lat2px(cla, z);

    tx0 = (int)floor((cx - vw / 2.0) / 256.0);
    tx1 = (int)floor((cx + vw / 2.0) / 256.0);
    ty0 = (int)floor((cy - vh / 2.0) / 256.0);
    ty1 = (int)floor((cy + vh / 2.0) / 256.0);
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

    for (i = 0; i < count; i++) {
        const struct node *nd = nodedb_get(i);
        int px, py, r;
        u32 col;
        if (nd == NULL || !nd->has_pos)
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
    if (!drew)
        text(80, 250, 18, C_WARN, "No map tiles on SD yet - download them (map setup).");
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
        else
            draw_messages();
    }
    GRRLIB_Rectangle(0, SCR_H - 26, SCR_W, 26, C_BAR, 1);
    text(10, SCR_H - 22, 16, C_DIM,
         "up/down scroll   1 view   A type   2 quick   B announce   Home exit");
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

static void send_text(const char *msg)
{
    uint8_t frame[256];
    char line[96];
    int flen = mesh_encode_text(CFG.node_id, 0xffffffffu, next_id(), CFG.channel_hash,
                                msg, frame, sizeof(frame));
    int rc = flen > 0 ? sx_send(frame, flen) : -1;
    snprintf(line, sizeof(line), "> you: %.70s  [%s]", msg, rc == 0 ? "sent" : "FAIL");
    log_add(line);
    dbg("lora: TX text '%s' rc=%d", msg, rc);
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
    GRRLIB_Exit();
}

int main(void)
{
    char info[200] = "", err[200] = "";
    long packets = 0, crc_errors = 0, sent = 0, dirty = 0;
    unsigned long long last_save;
    int view = VIEW_NODES;

    GRRLIB_Init();
    WPAD_Init();
    loading("Starting...");
    nodedb_reset();
    loading("Reading config + NodeDB from SD...");
    config_load(&CFG);
    mesh_set_key(CFG.key, CFG.keylen);
    mesh_set_hop_limit(CFG.hop_limit);
    mesh_set_hw_model(CFG.hw_model);
    sx_set_tx_power(CFG.tx_power);
    nodedb_load(NODEDB_PATH);
    last_save = (unsigned long long)time(NULL);

    if (strcmp(DEBUG_HOST, "127.0.0.1") != 0)
        net_start(err, sizeof(err));
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

        WPAD_ScanPads();
        down = WPAD_ButtonsDown(0);
        if (down & WPAD_BUTTON_HOME)
            break;
        if (down & WPAD_BUTTON_UP) {
            if (view == VIEW_MESSAGES)
                msg_top--;
            else
                node_sel--;
        }
        if (down & WPAD_BUTTON_DOWN) {
            if (view == VIEW_MESSAGES)
                msg_top++;
            else
                node_sel++;
        }
        if (down & WPAD_BUTTON_1)
            view = (view + 1) % VIEW_COUNT;
        if (down & WPAD_BUTTON_A) {
            char msg[120] = "";
            if (gfx_osk("Type a message:", msg, sizeof(msg)) && msg[0]) {
                send_text(msg);
                sent++;
            }
        }
        if (down & WPAD_BUTTON_2) {
            static const char *const quick[] = { "On my way", "Ack",
                                                 "Hello from a Wii", "Testing" };
            static int qi;
            send_text(quick[qi++ & 3]);
            sent++;
        }
        if (down & WPAD_BUTTON_B)
            send_nodeinfo();

        n = sx_poll(buf, sizeof(buf), &rssi, &snr);
        if (n < 0) {
            crc_errors++;
        } else if (n > 0) {
            struct mesh_pkt pkt;
            char derr[80], line[96];
            packets++;
            if (mesh_decode(buf, n, &pkt, derr, sizeof(derr)) == 0) {
                nodedb_update(&pkt, rssi, snr, (unsigned long long)time(NULL));
                dirty = 1;
                if (pkt.portnum == MESH_PORT_TEXT)
                    snprintf(line, sizeof(line), "%08x: %.70s", pkt.from, pkt.text);
                else if (pkt.portnum == MESH_PORT_NODEINFO)
                    snprintf(line, sizeof(line), "* %.30s (%.6s) joined", pkt.long_name,
                             pkt.short_name);
                else
                    snprintf(line, sizeof(line), "%08x port %u (%d B)", pkt.from,
                             pkt.portnum, pkt.payload_len);
                log_add(line);
            }
        }

        if (dirty && (unsigned long long)time(NULL) - last_save >= 60) {
            nodedb_save(NODEDB_PATH);
            last_save = (unsigned long long)time(NULL);
            dirty = 0;
        }

        draw_ui(view, packets, sent, crc_errors);
    }

    sx_close();
    nodedb_save(NODEDB_PATH);
    dbg("lora: exit (rx %ld tx %ld, %d nodes)", packets, sent, nodedb_count());
    net_stop();
    GRRLIB_Exit();
    return 0;
}
