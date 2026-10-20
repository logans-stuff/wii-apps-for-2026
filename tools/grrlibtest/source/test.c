/* Minimal GRRLIB smoke test: init graphics, draw a panel and TTF text. */
#include <grrlib.h>
#include <wiiuse/wpad.h>

#include "font_dejavu.h"

int main(void)
{
    GRRLIB_ttfFont *font;

    GRRLIB_Init();
    WPAD_Init();
    font = GRRLIB_LoadTTF(dejavu_ttf, dejavu_ttf_len);

    while (1) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            break;
        GRRLIB_FillScreen(0x101828FF);
        GRRLIB_Rectangle(20, 20, 600, 56, 0x2A3A5AFF, 1);
        GRRLIB_PrintfTTF(32, 30, font, "Meshtoad - GRRLIB test", 26, 0xFFFFFFFF);
        GRRLIB_PrintfTTF(32, 110, font, "If you can read this, graphics work.", 20, 0xA0C0FFFF);
        GRRLIB_Render();
    }

    GRRLIB_FreeTTF(font);
    GRRLIB_Exit();
    return 0;
}
