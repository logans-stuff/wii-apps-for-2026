/* On-screen keyboard. See osk.h. Extracted from the Sonoran app. */
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <wiiuse/wpad.h>

#include "net.h" /* entropy_add */
#include "osk.h"

#define CLEAR "\x1b[2J"
#define AT(r, c) "\x1b[" #r ";" #c "H"
#define WHITE "\x1b[37;1m"
#define GREY "\x1b[37;0m"
#define YELLOW "\x1b[33;1m"
#define SEL "\x1b[44m"
#define UNSEL "\x1b[40m"

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

int osk_input(const char *title, char *buf, size_t buflen, int masked)
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
