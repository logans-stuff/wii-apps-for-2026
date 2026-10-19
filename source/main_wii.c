/* Sonoran for Wii: shows view.azmsh.net chat and nodes.
 * Wii Remote: Left/Right = screen, Up/Down = scroll, A = open,
 * B = back, + = refresh, Home = exit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

#include "api.h"
#include "https.h"
#include "net.h"

#define MAX_CHAT 60
#define MAX_NODES 150
#define TOKEN_MAX 4096

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

enum screen { SCR_CHAT, SCR_NODES };

static void *xfb;
static GXRModeObj *rmode;
static int cols = 72, rows = 26;

static char token[TOKEN_MAX];
static char token_path[256];
static struct chat_msg chat[MAX_CHAT];
static struct node_row nodes[MAX_NODES];
static int chat_n, nodes_n;
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
    printf(WHITE "Sonoran" GREY " - view.azmsh.net   ");
    printf("%s[ Chat ]%s ", scr == SCR_CHAT ? YELLOW : GREY, GREY);
    printf("%s[ Nodes ]%s\n", scr == SCR_NODES ? YELLOW : GREY, GREY);
}

static void footer(void)
{
    printf("\x1b[%d;1H" GREY, rows);
    put_cut(status_line, cols);
    printf("\x1b[%d;1H", rows + 1);
    put_cut("<-/-> screen  Up/Down scroll  A open  B back  + refresh  Home exit", cols);
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

    snprintf(status_line, sizeof(status_line), "Loading...");
    footer();
    VIDEO_WaitVSync();
    if (scr == SCR_CHAT)
        rc = api_chat(token, chat, MAX_CHAT, &chat_n, err, sizeof(err));
    else
        rc = api_nodes(token, nodes, MAX_NODES, &nodes_n, &total, err, sizeof(err));
    if (rc == API_LOGIN && token[0] == '\0')
        snprintf(status_line, sizeof(status_line), "TLS OK (%s). Login needed: "
                 "put a token in sd:/apps/sonoran/token.txt", https_last_tls());
    else if (rc == API_OK)
        snprintf(status_line, sizeof(status_line), "OK  %s  token: %s",
                 https_last_tls(), token_path);
    else if (rc == API_LOGIN)
        snprintf(status_line, sizeof(status_line), "%s Put a new token in %s",
                 err, token_path[0] ? token_path : "sd:/apps/sonoran/token.txt");
    else
        snprintf(status_line, sizeof(status_line), "Error: %s", err);
}

int main(int argc, char **argv)
{
    char err[200];
    enum screen scr = SCR_CHAT;
    int sel[2] = { 0, 0 }, top[2] = { 0, 0 };
    int detail = 0, redraw = 1;
    int loaded[2] = { 0, 0 };

    video_start();
    WPAD_Init();
    fatInitDefault();

    printf(CLEAR AT(2, 1) WHITE "Sonoran for Wii 0.1\n\n" GREY);
    /* Without a token the app still starts. The server then answers
     * with a login redirect, and the status line says so. */
    if (!load_token(argc > 0 ? argv[0] : NULL))
        printf("No token in sd:/apps/sonoran/token.txt. See README.txt.\n");

    printf("Starting the network...\n");
    if (net_start(err, sizeof(err)) != 0 || https_init(err, sizeof(err)) != 0) {
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

        if (!loaded[scr]) {
            refresh(scr);
            loaded[scr] = 1;
            redraw = 1;
        }
        if (redraw) {
            count = scr == SCR_CHAT ? chat_n : nodes_n;
            if (sel[scr] >= count)
                sel[scr] = count > 0 ? count - 1 : 0;
            if (detail && count > 0) {
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

        count = scr == SCR_CHAT ? chat_n : nodes_n;
        if (down & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)) {
            scr = scr == SCR_CHAT ? SCR_NODES : SCR_CHAT;
            detail = 0;
            redraw = 1;
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
    printf(CLEAR);
    return 0;
}
