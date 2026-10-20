/* CH341 (1a86:5512) USB-to-SPI bridge driver for the Wii.
 *
 * Protocol (from the Meshtastic firmware's WebUSB driver protocol.js):
 *   bulk OUT 0x02, bulk IN 0x82, 32-byte packets
 *   SPI:      0xA8 + up to 31 bytes, every byte bit-reversed (LSB first);
 *             the chip answers the same number of bytes (also reversed)
 *   GPIO out: 0xAB, 0x80 | levels(D0..D5), 0x40 | directions(D0..D5), 0x20
 *   GPIO in:  0xA0, then read 6 bytes; byte 0 holds D0..D7
 * SCK (D3) and MOSI (D5) must be outputs, or every MISO byte reads 0xFF. */
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/usb.h>

#include "ch341.h"
#include "net.h" /* dbg() */

#define EP_OUT 0x02
#define EP_IN 0x82
#define PKT 32

static s32 fd;
static int is_open, cs;
static uint8_t dir_bits, out_bits;
static uint8_t *iobuf;
static int dbgn; /* log only the first few transfers */

static uint8_t rev(uint8_t x)
{
    x = (uint8_t)(((x >> 1) & 0x55) | ((x << 1) & 0xaa));
    x = (uint8_t)(((x >> 2) & 0x33) | ((x << 2) & 0xcc));
    return (uint8_t)(((x >> 4) & 0x0f) | ((x << 4) & 0xf0));
}

static int out(const uint8_t *data, int len)
{
    s32 r;
    memcpy(iobuf, data, (size_t)len);
    DCFlushRange(iobuf, PKT);
    r = USB_WriteBlkMsg(fd, EP_OUT, (u16)len, iobuf);
    if (dbgn < 12)
        dbg("ch341 OUT ep=%02x len=%d rc=%d [%02x %02x %02x %02x]", EP_OUT, len,
            (int)r, data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0,
            len > 3 ? data[3] : 0);
    return r < 0 ? -1 : 0;
}

/* Read exactly len bytes (the chip can answer in parts). */
static int in(uint8_t *data, int len)
{
    int got = 0, tries = 0;

    while (got < len && tries++ < 8) {
        s32 n;
        DCInvalidateRange(iobuf, PKT);
        n = USB_ReadBlkMsg(fd, EP_IN, (u16)(len - got), iobuf);
        if (dbgn < 12)
            dbg("ch341 IN  ep=%02x want=%d rc=%d got=%d [%02x %02x %02x %02x %02x %02x]",
                EP_IN, len - got, (int)n, got, iobuf[0], iobuf[1], iobuf[2],
                iobuf[3], iobuf[4], iobuf[5]);
        if (n < 0)
            return -1;
        DCInvalidateRange(iobuf, PKT);
        memcpy(data + got, iobuf, (size_t)n);
        got += n;
    }
    if (dbgn < 12)
        dbgn++;
    return got == len ? 0 : -1;
}

static int send_pins(void)
{
    uint8_t cmd[4] = { 0xAB, (uint8_t)(0x80 | (out_bits & 0x3f)),
                       (uint8_t)(0x40 | (dir_bits & 0x3f)), 0x20 };
    return out(cmd, 4);
}

int ch341_write_pin(int pin, int level)
{
    if (!is_open || pin < 0 || pin > 5)
        return -1;
    if (level)
        out_bits |= (uint8_t)(1 << pin);
    else
        out_bits &= (uint8_t)~(1 << pin);
    return send_pins();
}

int ch341_read_pin(int pin)
{
    uint8_t cmd = 0xA0, reply[6];

    if (!is_open || out(&cmd, 1) != 0 || in(reply, 6) != 0)
        return -1;
    return (reply[0] >> pin) & 1;
}

int ch341_spi(const uint8_t *tx, uint8_t *rx, int len)
{
    uint8_t pkt[PKT], back[PKT];
    int off = 0, rc = 0;

    if (!is_open || ch341_write_pin(cs, 0) != 0)
        return -1;
    while (off < len) {
        int n = len - off > PKT - 1 ? PKT - 1 : len - off, i;
        pkt[0] = 0xA8;
        for (i = 0; i < n; i++)
            pkt[1 + i] = rev(tx[off + i]);
        if (out(pkt, n + 1) != 0 || in(back, n) != 0) {
            rc = -1;
            break;
        }
        if (rx != NULL)
            for (i = 0; i < n; i++)
                rx[off + i] = rev(back[i]);
        off += n;
    }
    if (ch341_write_pin(cs, 1) != 0)
        rc = -1;
    return rc;
}

int ch341_open(int cs_pin, unsigned out_mask, char *info, size_t infolen, char *err, size_t errlen)
{
    static usb_device_entry list[16];
    u8 cnt = 0;
    int i, tries;

    ch341_close();
    if (iobuf == NULL)
        iobuf = memalign(32, PKT);
    if (iobuf == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    USB_Initialize();
    /* The CH341 may still be enumerating when we start, so wait for it. */
    for (tries = 0; tries < 25 && !is_open; tries++) {
        cnt = 0;
        if (USB_GetDeviceList(list, 16, 0, &cnt) < 0)
            cnt = 0;
        for (i = 0; i < cnt; i++) {
            if (list[i].vid != 0x1a86 || list[i].pid != 0x5512)
                continue;
            if (USB_OpenDevice(list[i].device_id, list[i].vid, list[i].pid, &fd) >= 0) {
                is_open = 1;
                snprintf(info, infolen, "CH341 1a86:5512 (device %08x)",
                         (unsigned)list[i].device_id);
                dbg("ch341 open ok: device_id=%08x fd=%d (try %d)",
                    (unsigned)list[i].device_id, (int)fd, tries);
                break;
            }
            dbg("ch341 open failed for device_id=%08x", (unsigned)list[i].device_id);
        }
        if (!is_open)
            usleep(200 * 1000); /* 200 ms, up to ~5 s total */
    }
    if (!is_open) {
        snprintf(err, errlen, "no CH341 (1a86:5512) found (is the Meshtoad plugged in?)");
        return -1;
    }
    cs = cs_pin;
    dir_bits = (uint8_t)((1 << 3) | (1 << 5) | (1 << cs_pin) | out_mask);
    out_bits = (uint8_t)(1 << cs_pin); /* CS high = not selected */
    if (send_pins() != 0) {
        snprintf(err, errlen, "CH341 pin setup failed");
        ch341_close();
        return -1;
    }
    return 0;
}

void ch341_close(void)
{
    if (is_open)
        USB_CloseDevice(&fd);
    is_open = 0;
}
