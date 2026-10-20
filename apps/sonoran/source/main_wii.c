/* Sonoran for Wii: shows s.azmsh.dev chat and nodes.
 * Wii Remote: Left/Right = screen, Up/Down = scroll, A = open,
 * B = back, + = refresh, Home = exit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <fat.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

#include "api.h"
#include "map.h"
#include "discord.h"
#include "dc_qr.h"
#include "qrcodegen.h"
#include "https.h"
#include "net.h"

#define MAX_CHAT 60
#define MAX_NODES 150
#define MAX_MAP_NODES 3000
#define TOKEN_MAX 4096
#define PAIR_PORT 4300

/* ANSI codes of the libogc console */
#define CLEAR "\x1b[2J"
#define AT(r, c) "\x1b[" #r ";" #c "H"
#define WHITE "\x1b[37;1m"
#define GREY "\x1b[37;0m"
#define YELLOW "\x1b[33;1m"
#define CYAN "\x1b[36;1m"
#define RED "\x1b[31;1m"
#define SEL "\x1b[44m"
#define UNSEL "\x1b[40m"

enum screen { SCR_CHAT, SCR_NODES, SCR_MAP, SCR_DISCORD, SCR_COUNT };

static void *xfb;
static GXRModeObj *rmode;
static int cols = 72, rows = 26;

static char token[TOKEN_MAX];
static char token_path[256];
static struct chat_msg chat[MAX_CHAT];
static struct node_row nodes[MAX_NODES];
static int chat_n, nodes_n;
static struct map_node map_nodes[MAX_MAP_NODES];
static int map_n;
static enum screen cur_scr;

/* Discord */
#define DC_GUILDS 100
#define DC_CHANS 200
#define DC_MSGS 50
static char dc_token[256];
static struct dc_item dc_guild[DC_GUILDS], dc_chan[DC_CHANS];
static struct dc_msg dc_msgs[DC_MSGS];
static int dc_level;              /* 0 servers, 1 channels, 2 messages */
static int dc_n[3], dc_sel[3], dc_top[3];
static int dc_detail;
/* Phoenix, the middle of the azmsh mesh */
static struct map_view view = { 33.45, -112.07, 9 };
static char status_line[512];

static void video_start(void)
{
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
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    CON_GetMetrics(&cols, &rows);
    cols -= 2;
    rows -= 1;
}

/* Read the token from the app folder. Return 1 when a token is there. */
static int load_token(const char *argv0)
{
    const char *tries[3];
    char here[256];
    int i;

    here[0] = '\0';
    if (argv0 != NULL && strchr(argv0, '/') != NULL) {
        snprintf(here, sizeof(here), "%s", argv0);
        *(strrchr(here, '/') + 1) = '\0';
        strncat(here, "token.txt", sizeof(here) - strlen(here) - 1);
    }
    tries[0] = here;
    tries[1] = "sd:/apps/sonoran/token.txt";
    tries[2] = "usb:/apps/sonoran/token.txt";

    for (i = 0; i < 3; i++) {
        FILE *f;
        size_t n;
        if (tries[i][0] == '\0')
            continue;
        f = fopen(tries[i], "rb");
        if (f == NULL)
            continue;
        n = fread(token, 1, sizeof(token) - 1, f);
        fclose(f);
        token[n] = '\0';
        /* Keep the first line only, without spaces. */
        token[strcspn(token, "\r\n \t")] = '\0';
        snprintf(token_path, sizeof(token_path), "%s", tries[i]);
        return token[0] != '\0';
    }
    return 0;
}

/* Print text cut to width w. */
static void put_cut(const char *s, int w)
{
    printf("%-*.*s", w, w, s);
}

static void header(enum screen scr)
{
    printf(CLEAR AT(1, 1));
    printf(WHITE "Sonoran" GREY " - s.azmsh.dev   ");
    printf("%s[ Chat ]%s ", scr == SCR_CHAT ? YELLOW : GREY, GREY);
    printf("%s[ Nodes ]%s ", scr == SCR_NODES ? YELLOW : GREY, GREY);
    printf("%s[ Map ]%s ", scr == SCR_MAP ? YELLOW : GREY, GREY);
    printf("%s[ Discord ]%s\n", scr == SCR_DISCORD ? YELLOW : GREY, GREY);
}

static void footer(void)
{
    printf("\x1b[%d;1H" GREY, rows);
    put_cut(status_line, cols);
    printf("\x1b[%d;1H", rows + 1);
    if (cur_scr == SCR_DISCORD)
        put_cut("Up/Dn select  A open  B back  1 login/write  + reload  - screen  Home exit", cols);
    else if (cur_scr == SCR_MAP)
        put_cut("D-Pad move  A zoom in  B zoom out  - screen  + reload  Home exit", cols);
    else
        put_cut("<-/-> or - screen  Up/Dn scroll  A open  B back  + reload  1 log in  2 pair", cols);
}

static void draw_chat(int top, int sel)
{
    int i, line = 3, last = rows - 2;

    header(SCR_CHAT);
    if (chat_n == 0)
        printf(AT(3, 1) "No messages loaded.");
    for (i = top; i < chat_n && line <= last; i++, line++) {
        char row[256];
        snprintf(row, sizeof(row), "%s %-10.10s %s", chat[i].when + 6,
                 chat[i].from, chat[i].body);
        printf("\x1b[%d;1H%s%s", line, i == sel ? SEL WHITE : UNSEL GREY,
               i == sel ? "" : "");
        put_cut(row, cols);
        printf(UNSEL);
    }
    footer();
}

static void draw_nodes(int top, int sel)
{
    int i, line = 3, last = rows - 2;

    header(SCR_NODES);
    if (nodes_n == 0)
        printf(AT(3, 1) "No nodes loaded.");
    for (i = top; i < nodes_n && line <= last; i++, line++) {
        char row[256];
        snprintf(row, sizeof(row), "%-4.4s %-28.28s %s %s", nodes[i].short_name,
                 nodes[i].name, nodes[i].heard, nodes[i].gateway ? "GW" : "");
        printf("\x1b[%d;1H%s", line, i == sel ? SEL WHITE : UNSEL GREY);
        put_cut(row, cols);
        printf(UNSEL);
    }
    footer();
}

/* Show long text with word wrap. */
static void wrap(const char *s, int width)
{
    int len = (int)strlen(s);

    while (len > 0) {
        int n = len < width ? len : width;
        if (n < len) {
            int k = n;
            while (k > 0 && s[k] != ' ')
                k--;
            if (k > 0)
                n = k;
        }
        printf("  %.*s\n", n, s);
        s += n;
        len -= n;
        while (*s == ' ') {
            s++;
            len--;
        }
    }
}

static void draw_chat_detail(const struct chat_msg *m)
{
    header(SCR_CHAT);
    printf(AT(3, 1) CYAN "From:    " WHITE "%s\n", m->from);
    printf(CYAN "Time:    " WHITE "%s (Arizona)\n", m->when);
    printf(CYAN "Channel: " WHITE "%s\n\n" GREY, m->channel);
    wrap(m->body, cols - 4);
    footer();
}

static void draw_node_detail(const struct node_row *n)
{
    header(SCR_NODES);
    printf(AT(3, 1) CYAN "Name:       " WHITE "%s\n", n->name);
    printf(CYAN "Short name: " WHITE "%s\n", n->short_name);
    printf(CYAN "Node ID:    " WHITE "%s\n", n->id);
    printf(CYAN "Hardware:   " WHITE "%s\n", n->hw[0] ? n->hw : "-");
    printf(CYAN "Role:       " WHITE "%s\n", n->role[0] ? n->role : "-");
    printf(CYAN "Last heard: " WHITE "%s (Arizona)\n", n->heard);
    printf(CYAN "Gateway:    " WHITE "%s\n", n->gateway ? "yes (MQTT)" : "no");
    printf(CYAN "Position:   " WHITE "%s\n" GREY, n->has_position ? "yes" : "no");
    footer();
}

static void show_message(const char *colour, const char *text)
{
    printf(CLEAR AT(3, 1) "%s", colour);
    wrap(text, cols - 4);
    printf(GREY);
}

static void refresh(enum screen scr)
{
    char err[200];
    int rc, total;

    dbg("refresh %s", scr == SCR_CHAT ? "chat" : scr == SCR_NODES ? "nodes" : "map");
    snprintf(status_line, sizeof(status_line), "Loading...");
    footer();
    VIDEO_WaitVSync();
    if (scr == SCR_CHAT)
        rc = api_chat(token, chat, MAX_CHAT, &chat_n, err, sizeof(err));
    else if (scr == SCR_MAP)
        rc = api_map_nodes(token, map_nodes, MAX_MAP_NODES, &map_n, err, sizeof(err));
    else
        rc = api_nodes(token, nodes, MAX_NODES, &nodes_n, &total, err, sizeof(err));
    if (rc == API_LOGIN && token[0] == '\0')
        snprintf(status_line, sizeof(status_line), "TLS OK (%s). Login needed: "
                 "push 1 to log in", https_last_tls());
    else if (rc == API_OK)
        snprintf(status_line, sizeof(status_line), "OK  %s  token: %s",
                 https_last_tls(), token_path);
    else if (rc == API_LOGIN)
        snprintf(status_line, sizeof(status_line), "%s Push 1 to log in again.", err);
    else
        snprintf(status_line, sizeof(status_line), "Error: %s", err);
}

/* Write the token to the SD card. Return 0 on success. */
static int save_token(char *err, size_t errlen)
{
    FILE *f;

    mkdir("sd:/apps", 0777);
    mkdir("sd:/apps/sonoran", 0777);
    f = fopen("sd:/apps/sonoran/token.txt", "wb");
    if (f == NULL) {
        snprintf(err, errlen, "cannot write sd:/apps/sonoran/token.txt. "
                 "Is an SD card in the Wii?");
        return -1;
    }
    fprintf(f, "%s\n", token);
    fclose(f);
    snprintf(token_path, sizeof(token_path), "sd:/apps/sonoran/token.txt");
    return 0;
}

/* Pairing: get a login over the LAN from pair.sh on a computer.
 * pair.sh sends "LOGIN\n<user>\n<password>\n" and the app logs in to
 * Authelia itself, or "COOKIE\n<authelia_session value>\n". */
static void pair(void)
{
    static char got[TOKEN_MAX];
    char ip[20], err[200], cookie[600];
    char *line1, *line2, *line3;
    int n, rc;

    net_my_ip(ip, sizeof(ip));
    printf(CLEAR AT(3, 1) WHITE "Log in from a computer\n\n" GREY);
    printf("  On bigboy, run:\n\n");
    printf(YELLOW "    ~/wii-sonoran/pair.sh %s\n\n" GREY, ip);
    printf("  Type the username and the password there.\n");
    printf("  Waiting on port %d...\n", PAIR_PORT);
    VIDEO_WaitVSync();

    n = tcp_receive_once(PAIR_PORT, got, sizeof(got), err, sizeof(err));
    dbg("pair: received %d bytes", n);
    if (n <= 0) {
        snprintf(status_line, sizeof(status_line), "Pairing failed: %s", n < 0 ? err : "no data");
        return;
    }
    line1 = got;
    line2 = strchr(line1, '\n');
    if (line2 != NULL)
        *line2++ = '\0';
    line3 = line2 ? strchr(line2, '\n') : NULL;
    if (line3 != NULL)
        *line3++ = '\0';
    if (line3 != NULL)
        line3[strcspn(line3, "\r\n")] = '\0';
    if (line2 != NULL)
        line2[strcspn(line2, "\r")] = '\0';

    if (strcmp(line1, "LOGIN") == 0 && line2 && line3 && line2[0]) {
        printf("  Logging in to " AUTH_HOST " as %s...\n", line2);
        VIDEO_WaitVSync();
        dbg("pair: login start");
        rc = api_login(line2, line3, cookie, sizeof(cookie), err, sizeof(err));
        dbg("pair: login rc=%d %s", rc, rc == API_OK ? "" : err);
        memset(got, 0, sizeof(got));
        if (rc != API_OK) {
            snprintf(status_line, sizeof(status_line), "Login: %s Push 2 to try again.", err);
            return;
        }
        snprintf(token, sizeof(token), "%s", cookie);
        memset(cookie, 0, sizeof(cookie));
    } else if (strcmp(line1, "DTOKEN") == 0 && line2 && strlen(line2) >= 20) {
        FILE *f;
        snprintf(dc_token, sizeof(dc_token), "%s", line2);
        memset(got, 0, sizeof(got));
        if ((f = fopen("sd:/apps/sonoran/discord.txt", "wb")) != NULL) {
            fprintf(f, "%s\n", dc_token);
            fclose(f);
        }
        snprintf(status_line, sizeof(status_line), "Discord token saved.");
        return;
    } else if (strcmp(line1, "COOKIE") == 0 && line2 && strlen(line2) >= 16) {
        snprintf(token, sizeof(token), "%s", line2);
        memset(got, 0, sizeof(got));
    } else {
        memset(got, 0, sizeof(got));
        snprintf(status_line, sizeof(status_line), "Pairing: the data was not valid.");
        return;
    }
    if (save_token(err, sizeof(err)) == 0)
        snprintf(status_line, sizeof(status_line), "Logged in. Session saved to %s", token_path);
    else
        snprintf(status_line, sizeof(status_line), "Logged in (memory only): %s", err);
}

/* On-screen keyboard for the Wii Remote. It shows every printable ASCII
 * character at the same time, so no shift key is necessary.
 * D-Pad = move, A = type the key, B = delete, 2 = show or hide a
 * password, + = OK, Home = cancel. */
static const char *const osk_rows[] = {
    "!@#$%^&*()-_",
    "=+[]{};:'\"\\|",
    ",.<>/?`~    ",
    "0123456789  ",
    "abcdefghijkl",
    "mnopqrstuvwx",
    "yzABCDEFGHIJ",
    "KLMNOPQRSTUV",
    "WXYZ        ",
};
#define OSK_COLS 12
#define OSK_CHAR_ROWS ((int)(sizeof(osk_rows) / sizeof(osk_rows[0])))
#define OSK_ROWS (OSK_CHAR_ROWS + 1) /* last row: SPACE DEL OK */

static const char *const osk_specials[3] = { "SPACE", "DEL", "OK" };

static void osk_draw(const char *title, const char *buf, int masked, int shown,
                     int r, int c)
{
    int i, j, len = (int)strlen(buf);

    printf(CLEAR AT(2, 1) WHITE "%s\n\n" GREY "  > " WHITE, title);
    for (i = 0; i < len; i++)
        putchar(masked && !shown ? '*' : buf[i]);
    printf(YELLOW "_" GREY "\n\n");
    for (i = 0; i < OSK_CHAR_ROWS; i++) {
        printf("    ");
        for (j = 0; j < OSK_COLS; j++) {
            char ch = osk_rows[i][j];
            int on = (i == r && j == c);
            printf("%s %c %s", on ? SEL WHITE : UNSEL GREY, ch == ' ' ? '_' : ch, UNSEL);
        }
        printf("\n");
    }
    printf("\n    ");
    for (j = 0; j < 3; j++) {
        int on = (r == OSK_CHAR_ROWS && j == c);
        printf("%s %s %s  ", on ? SEL WHITE : UNSEL GREY, osk_specials[j], UNSEL);
    }
    printf(GREY "\n\n  D-Pad move  A type  B delete  1 types !  + OK  Home cancel");
    if (masked)
        printf("  2 show/hide");
    printf("\n");
}

/* Return 1 when the user pushed OK, 0 on cancel. */
static int osk_input(const char *title, char *buf, size_t buflen, int masked)
{
    int r = 0, c = 0, shown = 0, redraw = 1;

    for (;;) {
        u32 down;
        size_t len = strlen(buf);
        int width;

        if (redraw) {
            osk_draw(title, buf, masked, shown, r, c);
            redraw = 0;
        }
        VIDEO_WaitVSync();
        WPAD_ScanPads();
        down = WPAD_ButtonsDown(0);
        if (down == 0)
            continue;
        entropy_add(down);
        redraw = 1;
        if (down & WPAD_BUTTON_HOME)
            return 0;
        if (down & WPAD_BUTTON_PLUS)
            return 1;
        if ((down & WPAD_BUTTON_2) && masked)
            shown = !shown;
        if ((down & WPAD_BUTTON_1) && len + 1 < buflen) {
            /* Button 1 types '!' directly. */
            buf[len] = '!';
            buf[len + 1] = '\0';
        }
        if ((down & WPAD_BUTTON_B) && len > 0)
            buf[len - 1] = '\0';
        if (down & WPAD_BUTTON_UP)
            r = (r + OSK_ROWS - 1) % OSK_ROWS;
        if (down & WPAD_BUTTON_DOWN)
            r = (r + 1) % OSK_ROWS;
        if (down & WPAD_BUTTON_LEFT)
            c--;
        if (down & WPAD_BUTTON_RIGHT)
            c++;
        width = r == OSK_CHAR_ROWS ? 3 : OSK_COLS;
        if (c < 0)
            c = width - 1;
        if (c >= width)
            c = 0;
        if (!(down & WPAD_BUTTON_A))
            continue;
        if (r == OSK_CHAR_ROWS) {
            if (c == 0 && len + 1 < buflen) {
                buf[len] = ' ';
                buf[len + 1] = '\0';
            } else if (c == 1 && len > 0) {
                buf[len - 1] = '\0';
            } else if (c == 2) {
                return 1;
            }
        } else if (osk_rows[r][c] != ' ' && len + 1 < buflen) {
            buf[len] = osk_rows[r][c];
            buf[len + 1] = '\0';
        }
    }
}

/* Log in to Authelia with a username and a password from the keyboard. */
static int login(void)
{
    static char user[64];
    static char password[128];
    char cookie[600], err[200];
    FILE *f;
    int rc;

    if (user[0] == '\0' && (f = fopen("sd:/apps/sonoran/user.txt", "rb")) != NULL) {
        size_t n = fread(user, 1, sizeof(user) - 1, f);
        fclose(f);
        user[n] = '\0';
        user[strcspn(user, "\r\n")] = '\0';
    }
    if (!osk_input("Log in to s.azmsh.dev - Username", user, sizeof(user), 0)
        || user[0] == '\0') {
        snprintf(status_line, sizeof(status_line), "Login cancelled.");
        return 0;
    }
    password[0] = '\0';
    if (!osk_input("Log in to s.azmsh.dev - Password", password, sizeof(password), 1)) {
        memset(password, 0, sizeof(password));
        snprintf(status_line, sizeof(status_line), "Login cancelled.");
        return 0;
    }

    printf(CLEAR AT(3, 1) GREY "  Logging in to " AUTH_HOST "...\n");
    VIDEO_WaitVSync();
    dbg("login: start");
    rc = api_login(user, password, cookie, sizeof(cookie), err, sizeof(err));
    memset(password, 0, sizeof(password));
    dbg("login: rc=%d %s", rc, rc == API_OK ? "" : err);
    if (rc != API_OK) {
        snprintf(status_line, sizeof(status_line), "Login: %s Push 1 to try again.", err);
        return 0;
    }
    snprintf(token, sizeof(token), "%s", cookie);
    memset(cookie, 0, sizeof(cookie));

    mkdir("sd:/apps", 0777);
    mkdir("sd:/apps/sonoran", 0777);
    if ((f = fopen("sd:/apps/sonoran/user.txt", "wb")) != NULL) {
        fprintf(f, "%s\n", user);
        fclose(f);
    }
    dbg("login: saving session");
    if (save_token(err, sizeof(err)) == 0)
        snprintf(status_line, sizeof(status_line), "Logged in. Session saved to %s",
                 token_path);
    else
        snprintf(status_line, sizeof(status_line), "Logged in (memory only): %s", err);
    return 1;
}

static void dc_load_token(void)
{
    FILE *f = fopen("sd:/apps/sonoran/discord.txt", "rb");
    size_t n;

    if (f == NULL)
        return;
    n = fread(dc_token, 1, sizeof(dc_token) - 1, f);
    fclose(f);
    dc_token[n] = '\0';
    dc_token[strcspn(dc_token, "\r\n \t")] = '\0';
}

static void dc_refresh(void)
{
    char err[200];
    int rc = DC_OK;

    if (dc_token[0] == '\0') {
        snprintf(status_line, sizeof(status_line),
                 "Not logged in to Discord. Push 1 to log in with a QR code.");
        return;
    }
    snprintf(status_line, sizeof(status_line), "Loading...");
    footer();
    if (dc_level == 0)
        rc = dc_guilds(dc_token, dc_guild, DC_GUILDS, &dc_n[0], err, sizeof(err));
    else if (dc_level == 1)
        rc = dc_channels(dc_token, dc_guild[dc_sel[0]].id, dc_chan, DC_CHANS, &dc_n[1],
                         err, sizeof(err));
    else
        rc = dc_messages(dc_token, dc_chan[dc_sel[1]].id, dc_msgs, DC_MSGS, &dc_n[2],
                         err, sizeof(err));
    if (rc == DC_OK)
        snprintf(status_line, sizeof(status_line), "Discord OK");
    else
        snprintf(status_line, sizeof(status_line), "%s", err);
}

static void draw_discord(void)
{
    int i, line = 4, last = rows - 2, n = dc_n[dc_level];
    int top = dc_top[dc_level], sel = dc_sel[dc_level];

    header(SCR_DISCORD);
    if (dc_level == 0)
        printf(AT(2, 1) CYAN "Servers" GREY);
    else if (dc_level == 1)
        printf(AT(2, 1) CYAN "%s" GREY, dc_guild[dc_sel[0]].name);
    else
        printf(AT(2, 1) CYAN "%s  %s" GREY, dc_guild[dc_sel[0]].name, dc_chan[dc_sel[1]].name);

    if (dc_level == 2 && dc_detail && n > 0) {
        const struct dc_msg *m = &dc_msgs[sel];
        printf(AT(4, 1) CYAN "From: " WHITE "%s\n" CYAN "Time: " WHITE "%s (Arizona)\n\n" GREY,
               m->author, m->when);
        wrap(m->text, cols - 4);
        footer();
        return;
    }
    if (dc_token[0] == '\0')
        printf(AT(4, 1) "Push 1 to log in to Discord with a QR code.");
    else if (n == 0)
        printf(AT(4, 1) "Nothing loaded.");
    for (i = top; i < n && line <= last; i++, line++) {
        char row[480];
        if (dc_level == 0)
            snprintf(row, sizeof(row), "%s", dc_guild[i].name);
        else if (dc_level == 1)
            snprintf(row, sizeof(row), "%s", dc_chan[i].name);
        else
            snprintf(row, sizeof(row), "%s %-12.12s %s", dc_msgs[i].when + 6,
                     dc_msgs[i].author, dc_msgs[i].text);
        printf("\x1b[%d;1H%s", line, i == sel ? SEL WHITE : UNSEL GREY);
        put_cut(row, cols);
        printf(UNSEL);
    }
    footer();
}

/* Draw the QR code in the middle of the screen. */
static void draw_qr(const char *text, const char *line1, const char *line2)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    int size, scale, quiet = 4, px, x0, y0, x, y;

    printf(CLEAR AT(2, 1) WHITE "%s\n" GREY "%s\n", line1, line2);
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 40,
                              qrcodegen_Mask_AUTO, true)) {
        printf(RED "QR code failed." GREY);
        return;
    }
    size = qrcodegen_getSize(qr);
    scale = (rmode->xfbHeight - 120) / (size + 2 * quiet);
    if (scale > 8)
        scale = 8;
    if (scale < 2)
        scale = 2;
    px = (size + 2 * quiet) * scale;
    x0 = (rmode->fbWidth - px) / 2;
    y0 = 70 + (rmode->xfbHeight - 100 - px) / 2;
    fb_rect(x0, y0, px, px, 1);
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
            if (qrcodegen_getModule(qr, x, y))
                fb_rect(x0 + (x + quiet) * scale, y0 + (y + quiet) * scale, scale, scale, 0);
}

/* Discord QR code login. Return 1 when a token was saved. */
static int dc_qr_login(void)
{
    char qr[200] = "", user[80] = "", tok[256], err[200];
    int st, done = 0;

    printf(CLEAR AT(3, 1) WHITE "Discord login\n\n" GREY);
    printf("  Making an RSA key and connecting to Discord.\n");
    printf("  This can take up to one minute on the Wii...\n");
    VIDEO_WaitVSync();
    if (ra_start(err, sizeof(err)) != 0) {
        ra_end();
        snprintf(status_line, sizeof(status_line), "Discord: %s", err);
        return 0;
    }
    printf("  Connected. Waiting for the QR code...\n");
    for (;;) {
        u32 down;
        st = ra_poll(100, qr, sizeof(qr), user, sizeof(user), tok, sizeof(tok),
                     err, sizeof(err));
        if (st == RA_QR) {
            draw_qr(qr, "Discord login: scan this with the Discord app on your phone",
                    "(Phone: your profile > Scan QR Code.)  B or Home = cancel");
        } else if (st == RA_SCANNED) {
            printf(CLEAR AT(3, 1) WHITE "Discord login\n\n" GREY);
            printf("  Scanned by: " YELLOW "%s\n\n" GREY, user);
            printf("  Push \"Log in\" on your phone to allow it.\n");
            printf("  B or Home = cancel\n");
        } else if (st == RA_DONE) {
            FILE *f;
            snprintf(dc_token, sizeof(dc_token), "%s", tok);
            memset(tok, 0, sizeof(tok));
            mkdir("sd:/apps", 0777);
            mkdir("sd:/apps/sonoran", 0777);
            if ((f = fopen("sd:/apps/sonoran/discord.txt", "wb")) != NULL) {
                fprintf(f, "%s\n", dc_token);
                fclose(f);
                snprintf(status_line, sizeof(status_line), "Discord: logged in as %s. Saved.", user);
            } else {
                snprintf(status_line, sizeof(status_line), "Discord: logged in as %s (not saved).", user);
            }
            done = 1;
            break;
        } else if (st == RA_ERROR || st == RA_CANCELLED) {
            snprintf(status_line, sizeof(status_line), "Discord: %s", err);
            break;
        }
        WPAD_ScanPads();
        down = WPAD_ButtonsDown(0);
        if (down & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) {
            snprintf(status_line, sizeof(status_line), "Discord login cancelled.");
            break;
        }
    }
    ra_end();
    return done;
}

static void dc_write(void)
{
    static char text[400];
    char err[200];

    text[0] = '\0';
    if (!osk_input("Write a message (+ = send, Home = cancel)", text, sizeof(text), 0)
        || text[0] == '\0') {
        snprintf(status_line, sizeof(status_line), "Message not sent.");
        return;
    }
    if (dc_send(dc_token, dc_chan[dc_sel[1]].id, text, err, sizeof(err)) == DC_OK)
        snprintf(status_line, sizeof(status_line), "Message sent.");
    else
        snprintf(status_line, sizeof(status_line), "%s", err);
}

static void draw_map(void)
{
    int near;

    header(SCR_MAP);
    snprintf(status_line, sizeof(status_line), "Loading map tiles...");
    footer();
    map_draw(&view, map_nodes, map_n);
    near = map_nearest(&view, map_nodes, map_n);
    if (near >= 0)
        snprintf(status_line, sizeof(status_line), "%-4.4s %s  (zoom %d, %d nodes%s)",
                 map_nodes[near].short_name, map_nodes[near].name, view.zoom, map_n,
                 map_failed_tiles() ? ", some tiles failed" : "");
    else
        snprintf(status_line, sizeof(status_line), "Zoom %d, %d nodes%s. Map (c) OpenStreetMap",
                 view.zoom, map_n, map_failed_tiles() ? ", some tiles failed" : "");
    footer();
}

int main(int argc, char **argv)
{
    char err[200];
    enum screen scr = SCR_CHAT;
    int sel[SCR_COUNT] = { 0 }, top[SCR_COUNT] = { 0 };
    int detail = 0, redraw = 1;
    int loaded[SCR_COUNT] = { 0 };

    video_start();
    WPAD_Init();
    fatInitDefault();

    /* Map area: below text row 1, above the two footer rows. */
    map_setup(xfb, rmode->fbWidth, rmode->xfbHeight, 20 + 16 + 4, 20 + 16 * (rows - 1) - 4);

    printf(CLEAR AT(2, 1) WHITE "Sonoran for Wii 0.3\n\n" GREY);
    /* Without a token the app still starts. The server then answers
     * with a login redirect, and the status line says so. */
    if (!load_token(argc > 0 ? argv[0] : NULL))
        printf("Not logged in yet. Push 1 to log in.\n");

    dc_load_token();
    printf("Starting the network...\n");
    if (net_start(err, sizeof(err)) != 0 || (dbg("start: token %s",
            token[0] ? "present" : "none"), https_init(err, sizeof(err))) != 0) {
        show_message(RED, err);
        printf("\n  Push Home to exit.");
        for (;;) {
            WPAD_ScanPads();
            if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
                return 0;
            VIDEO_WaitVSync();
        }
    }

    for (;;) {
        u32 down;
        int count, page = rows - 4;

        cur_scr = scr;
        if (!loaded[scr] && scr == SCR_DISCORD) {
            dc_refresh();
            loaded[scr] = 1;
            redraw = 1;
        } else if (!loaded[scr]) {
            refresh(scr);
            loaded[scr] = 1;
            redraw = 1;
        }
        if (redraw) {
            count = scr == SCR_CHAT ? chat_n : nodes_n;
            if (scr != SCR_MAP && sel[scr] >= count)
                sel[scr] = count > 0 ? count - 1 : 0;
            if (scr == SCR_DISCORD) {
                draw_discord();
            } else if (scr == SCR_MAP) {
                draw_map();
            } else if (detail && count > 0) {
                if (scr == SCR_CHAT)
                    draw_chat_detail(&chat[sel[scr]]);
                else
                    draw_node_detail(&nodes[sel[scr]]);
            } else if (scr == SCR_CHAT) {
                draw_chat(top[scr], sel[scr]);
            } else {
                draw_nodes(top[scr], sel[scr]);
            }
            redraw = 0;
        }

        VIDEO_WaitVSync();
        WPAD_ScanPads();
        down = WPAD_ButtonsDown(0);
        if (down)
            entropy_add(down);
        if (down & WPAD_BUTTON_HOME)
            break;

        if (scr == SCR_DISCORD) {
            int n = dc_n[dc_level], page = rows - 5;
            if (down & (WPAD_BUTTON_MINUS | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)) {
                int step = (down & WPAD_BUTTON_LEFT) ? SCR_COUNT - 1 : 1;
                scr = (enum screen)((scr + step) % SCR_COUNT);
                redraw = 1;
            } else if (down & WPAD_BUTTON_1) {
                if (dc_token[0] != '\0' && dc_level == 2) {
                    dc_write();
                    loaded[scr] = 0;
                } else if (dc_qr_login()) {
                    dc_level = 0;
                    loaded[scr] = 0;
                }
                redraw = 1;
            } else if (down & WPAD_BUTTON_2) {
                pair();
                memset(loaded, 0, sizeof(loaded));
            } else if (down & WPAD_BUTTON_PLUS) {
                dc_detail = 0;
                loaded[scr] = 0;
            } else if (down & WPAD_BUTTON_A) {
                if (dc_level == 2) {
                    dc_detail = n > 0;
                } else if (n > 0) {
                    dc_level++;
                    dc_sel[dc_level] = dc_top[dc_level] = 0;
                    loaded[scr] = 0;
                }
                redraw = 1;
            } else if (down & WPAD_BUTTON_B) {
                if (dc_detail)
                    dc_detail = 0;
                else if (dc_level > 0)
                    dc_level--;
                redraw = 1;
            } else if (!dc_detail && (down & (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN))) {
                int *sl = &dc_sel[dc_level], *tp = &dc_top[dc_level];
                *sl += (down & WPAD_BUTTON_DOWN) ? 1 : -1;
                if (*sl < 0)
                    *sl = 0;
                if (*sl >= n)
                    *sl = n > 0 ? n - 1 : 0;
                if (*sl < *tp)
                    *tp = *sl;
                if (*sl >= *tp + page)
                    *tp = *sl - page + 1;
                redraw = 1;
            }
            continue;
        }

        if (scr == SCR_MAP) {
            int step_x = rmode->fbWidth / 4, step_y = (rows * 16) / 4;
            if (down & WPAD_BUTTON_MINUS) {
                scr = SCR_CHAT;
                redraw = 1;
            } else if (down & WPAD_BUTTON_PLUS) {
                loaded[scr] = 0;
            } else if (down & WPAD_BUTTON_A) {
                if (view.zoom < 17) {
                    view.zoom++;
                    redraw = 1;
                }
            } else if (down & WPAD_BUTTON_B) {
                if (view.zoom > 3) {
                    view.zoom--;
                    redraw = 1;
                }
            } else if (down & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_UP
                               | WPAD_BUTTON_DOWN)) {
                map_pan(&view,
                        (down & WPAD_BUTTON_LEFT) ? -step_x : (down & WPAD_BUTTON_RIGHT) ? step_x : 0,
                        (down & WPAD_BUTTON_UP) ? -step_y : (down & WPAD_BUTTON_DOWN) ? step_y : 0);
                redraw = 1;
            } else if (down & WPAD_BUTTON_1) {
                if (login())
                    memset(loaded, 0, sizeof(loaded));
                redraw = 1;
            } else if (down & WPAD_BUTTON_2) {
                pair();
                memset(loaded, 0, sizeof(loaded));
            }
            continue;
        }

        count = scr == SCR_CHAT ? chat_n : nodes_n;
        if (down & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_MINUS)) {
            int step = (down & WPAD_BUTTON_LEFT) ? SCR_COUNT - 1 : 1;
            scr = (enum screen)((scr + step) % SCR_COUNT);
            detail = 0;
            redraw = 1;
        } else if (down & WPAD_BUTTON_1) {
            /* Load again only after a good login, so that an error stays
             * on the status line. */
            if (login())
                memset(loaded, 0, sizeof(loaded));
            detail = 0;
            redraw = 1;
        } else if (down & WPAD_BUTTON_2) {
            pair();
            memset(loaded, 0, sizeof(loaded));
            detail = 0;
        } else if (down & WPAD_BUTTON_PLUS) {
            loaded[scr] = 0;
            detail = 0;
        } else if (down & WPAD_BUTTON_A) {
            detail = count > 0;
            redraw = 1;
        } else if (down & WPAD_BUTTON_B) {
            detail = 0;
            redraw = 1;
        } else if (!detail && (down & (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN))) {
            sel[scr] += (down & WPAD_BUTTON_DOWN) ? 1 : -1;
            if (sel[scr] < 0)
                sel[scr] = 0;
            if (sel[scr] >= count)
                sel[scr] = count > 0 ? count - 1 : 0;
            if (sel[scr] < top[scr])
                top[scr] = sel[scr];
            if (sel[scr] >= top[scr] + page)
                top[scr] = sel[scr] - page + 1;
            redraw = 1;
        }
    }
    dbg("exit: Home pushed");
    printf(CLEAR);
    return 0;
}
