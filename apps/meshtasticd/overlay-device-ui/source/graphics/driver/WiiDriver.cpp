#ifdef USE_WII_DISPLAY
#include "graphics/driver/WiiDriver.h"
#include "util/ILog.h"
#include <cstdlib>
#include <malloc.h>
#include <cstring>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

LV_IMG_DECLARE(mouse_cursor_icon);

// Provided by the Wii entry point (portduino_main_wii.cpp).
extern "C" void *wii_xfb;
extern "C" GXRModeObj *wii_rmode;
extern "C" void wii_console_detach(void);
extern "C" void wii_request_quit(void);
extern "C" volatile int wii_ui_active;

WiiDriver *WiiDriver::wiiDriver = nullptr;
void (*WiiDriver::backHandler)(void) = nullptr;

static uint32_t *yuyvLut;    // RGB565 -> one YUYV word (two equal pixels)
static uint32_t padHeld, padDown;
static uint64_t lastScan;
static lv_point_t pointer;

WiiDriver &WiiDriver::create(uint16_t width, uint16_t height)
{
    if (!wiiDriver)
        wiiDriver = new WiiDriver(width, height);
    return *wiiDriver;
}

WiiDriver::WiiDriver(uint16_t width, uint16_t height) : DisplayDriver(width, height) {}

uint32_t WiiDriver::tick(void)
{
    return (uint32_t)ticks_to_millisecs(gettime());
}

static void buildLut(void)
{
    yuyvLut = (uint32_t *)malloc(65536 * sizeof(uint32_t));
    for (uint32_t c = 0; c < 65536; c++) {
        int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
        int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
        yuyvLut[c] = ((uint32_t)y << 24) | ((uint32_t)u << 16) | ((uint32_t)y << 8) | (uint32_t)v;
    }
}

// Scale 2x into the YUYV XFB: one source pixel = one YUYV word on two lines.
void WiiDriver::flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const uint16_t *src = (const uint16_t *)px_map;
    uint32_t *xfb = (uint32_t *)wii_xfb;
    const int stride = wii_rmode->fbWidth / 2; // words per line
    const int top = (wii_rmode->xfbHeight - 480) / 2;
    const int w = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; y++) {
        uint32_t *row0 = xfb + (top + 2 * y) * stride + area->x1;
        uint32_t *row1 = row0 + stride;
        for (int x = 0; x < w; x++) {
            uint32_t word = yuyvLut[*src++];
            row0[x] = word;
            row1[x] = word;
        }
    }
    lv_display_flush_ready(disp);
}

static void scanPads(void)
{
    uint64_t now = gettime();
    if (ticks_to_millisecs(now - lastScan) < 5)
        return; // both indevs read in the same LVGL cycle: scan once
    lastScan = now;
    WPAD_ScanPads();
    padHeld = WPAD_ButtonsHeld(0);
    padDown |= WPAD_ButtonsDown(0);
    if (padDown & WPAD_BUTTON_HOME)
        wii_request_quit();
}

void WiiDriver::readPointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    ir_t ir;
    scanPads();
    WPAD_IR(0, &ir);
    if (ir.valid) {
        pointer.x = (int32_t)(ir.x / 2);
        pointer.y = (int32_t)(ir.y / 2);
        if (pointer.x < 0) pointer.x = 0;
        if (pointer.y < 0) pointer.y = 0;
        if (pointer.x > 319) pointer.x = 319;
        if (pointer.y > 239) pointer.y = 239;
    }
    data->point = pointer;
    data->state = (padHeld & WPAD_BUTTON_A) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// D-pad / + as a keypad (one key event per press); B runs backHandler.
void WiiDriver::readKeys(lv_indev_t *indev, lv_indev_data_t *data)
{
    static uint32_t key, pressedMask;
    static const struct {
        uint32_t button, key;
    } map[] = {{WPAD_BUTTON_UP, LV_KEY_PREV}, {WPAD_BUTTON_DOWN, LV_KEY_NEXT}, {WPAD_BUTTON_LEFT, LV_KEY_LEFT},
               {WPAD_BUTTON_RIGHT, LV_KEY_RIGHT}, {WPAD_BUTTON_PLUS, LV_KEY_ENTER}};
    scanPads();
    if ((padDown & WPAD_BUTTON_B) && backHandler)
        backHandler();
    if (pressedMask && !(padHeld & pressedMask)) {
        pressedMask = 0; // released
    } else if (!pressedMask) {
        for (const auto &m : map)
            if (padDown & m.button) {
                pressedMask = m.button;
                key = m.key;
                break;
            }
    }
    padDown = 0;
    data->key = key;
    data->state = pressedMask ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void WiiDriver::init(DeviceGUI *gui)
{
    ILOG_DEBUG("WiiDriver::init...");
    DisplayDriver::init(gui);
    lv_tick_set_cb(tick);
    buildLut();

    wii_console_detach(); // from here the TV shows MUI, not the boot log
    memset(wii_xfb, 0, wii_rmode->fbWidth * wii_rmode->xfbHeight * VI_DISPLAY_PIX_SZ);

    display = lv_display_create(screenWidth, screenHeight);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    const uint32_t bufBytes = screenWidth * 60 * 2;
    lv_display_set_buffers(display, memalign(32, bufBytes), memalign(32, bufBytes), bufBytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);

    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_0, 640, 480);

    touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, readPointer);
    lv_indev_set_display(touch, display);
    lv_obj_t *cursor = lv_image_create(lv_display_get_screen_active(display));
    lv_image_set_src(cursor, &mouse_cursor_icon);
    lv_indev_set_cursor(touch, cursor);

    lv_indev_t *keys = lv_indev_create();
    lv_indev_set_type(keys, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keys, readKeys);
    lv_indev_set_display(keys, display);
    lv_group_t *group = lv_group_create();
    lv_group_set_default(group);
    lv_indev_set_group(keys, group);

    wii_ui_active = 1; // the quit watcher stops reading the Wiimote now
}
#endif
