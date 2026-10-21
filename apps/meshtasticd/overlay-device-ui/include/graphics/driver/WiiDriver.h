#pragma once

#include "graphics/driver/DisplayDriver.h"

/**
 * @brief Nintendo Wii display + input for MUI (libogc).
 * LVGL renders 320x240 RGB565; each pixel is written to the TV's YUYV external
 * framebuffer as one 2x2 block, so the UI fills 640x480. The Wiimote's IR
 * pointer is the touchscreen (A = touch), the D-pad/+ map to LVGL keys, B goes
 * back to the home screen and Home asks the app to quit. Enable with
 * USE_WII_DISPLAY (ARCH_PORTDUINO_WII).
 */
class WiiDriver : public DisplayDriver
{
  public:
    static WiiDriver &create(uint16_t width, uint16_t height);
    void init(DeviceGUI *gui) override;
    bool hasTouch(void) override { return true; }
    bool hasButton(void) override { return true; }
    virtual ~WiiDriver() {}

    // B on the Wiimote: MUI has no "back" key, so the view registers what
    // B should do (TFTView_320x240: press the sidebar's Home button).
    static void (*backHandler)(void);

  private:
    WiiDriver(uint16_t width, uint16_t height);
    static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
    static void readPointer(lv_indev_t *indev, lv_indev_data_t *data);
    static void readKeys(lv_indev_t *indev, lv_indev_data_t *data);
    static uint32_t tick(void);

    static WiiDriver *wiiDriver;
};
