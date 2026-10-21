// libpinedio-usb backend for the Wii: drives the CH341 USB-to-SPI bridge of a
// MeshToad (1a86:5512) through libogc's USB API. Same 12-function surface the
// firmware's Ch341Hal (USBHal.h) compiles against; the wire protocol matches the
// wasm build's js/protocol.js:
//   SPI   0xA8 + up to 31 bit-reversed bytes per 32-byte packet; the chip
//         answers one (reversed) byte per byte sent
//   GPIO  0xAB, 0x80|levels(D0..D5), 0x40|directions(D0..D5), 0x20
//   input 0xA0, then 6 bytes back; byte 0 = D0..D7
// Like js/ch341.js and libch341, each 32-byte packet is written and its reply
// read before the next. (Submitting a whole multi-packet transfer as one bulk
// write deadlocks: the CH341 stops taking OUT data once a few unread replies
// fill its IN buffer, e.g. on a 256-byte RX buffer read. The byte drops that
// once seemed to call for it were really the MEM1 DMA problem below.)
#include "libpinedio-usb.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/usb.h>

#define EP_OUT 0x02
#define EP_IN 0x82
#define PKT 32
#define SPI_MAX 512
#define WIRE_MAX (SPI_MAX + (SPI_MAX + PKT - 2) / (PKT - 1))

static s32 fd = -1;
// What the radio link is doing right now, for the main-loop watchdog.
const char *volatile wii_usb_stage = "idle";
static uint8_t dir_bits, out_bits;
// Separate 32-byte aligned DMA buffers per direction. With one shared buffer a
// short IN transfer could hand back the OUT bytes still sitting in it (the
// bit-reversed 0xA8 + echoed command RadioLib read as "command failed").
static uint8_t *outbuf, *inbuf;
#define DMA_SIZE ((WIRE_MAX + 31) & ~31)

static uint8_t rev(uint8_t x)
{
    x = (uint8_t)(((x >> 1) & 0x55) | ((x << 1) & 0xaa));
    x = (uint8_t)(((x >> 2) & 0x33) | ((x << 2) & 0xcc));
    return (uint8_t)(((x >> 4) & 0x0f) | ((x << 4) & 0xf0));
}

static int usb_out(const uint8_t *data, int len)
{
    s32 r;
    memcpy(outbuf, data, (size_t)len);
    DCFlushRange(outbuf, DMA_SIZE);
    wii_usb_stage = "bulk OUT";
    r = USB_WriteBlkMsg(fd, EP_OUT, (u16)len, outbuf);
    wii_usb_stage = "idle";
    if (r < 0)
        printf("ch341: bulk OUT %d bytes failed (%d)\n", len, (int)r);
    return r < 0 ? -1 : 0;
}

// Read exactly len bytes (the chip may answer in several bulk reads).
// Read exactly len bytes. Always ask IOS for a whole 32-byte packet: on the
// Wii a bulk IN of a length that isn't a multiple of 4 only delivers the whole
// 4-byte words, so a 2-byte status read came back empty. The CH341 ends every
// reply with a short packet, so asking for more never over-reads; anything
// beyond len (not expected) is carried into the next read.
static uint8_t carry[PKT];
static int carry_len;

static int usb_in(uint8_t *data, int len)
{
    int got = 0, tries = 0;
    if (carry_len > 0) {
        int n = carry_len < len ? carry_len : len;
        memcpy(data, carry, (size_t)n);
        memmove(carry, carry + n, (size_t)(carry_len - n));
        carry_len -= n;
        got = n;
    }
    while (got < len && tries++ < 32) {
        s32 n;
        memset(inbuf, 0xEE, PKT);
        DCFlushRange(inbuf, DMA_SIZE);
        wii_usb_stage = "bulk IN";
        n = USB_ReadBlkMsg(fd, EP_IN, PKT, inbuf);
        wii_usb_stage = "idle";
        if (n < 0) {
            printf("ch341: bulk IN failed (%d) after %d/%d bytes\n", (int)n, got, len);
            return -1;
        }
        DCInvalidateRange(inbuf, DMA_SIZE);
        if (n > PKT)
            n = PKT;
        int take = n < len - got ? n : len - got;
        memcpy(data + got, inbuf, (size_t)take);
        got += take;
        if (n > take) {
            memcpy(carry, inbuf + take, (size_t)(n - take));
            carry_len = n - take;
        }
    }
    if (got != len)
        printf("ch341: short read %d/%d bytes\n", got, len);
    return got == len ? 0 : -1;
}

static int send_pins(void)
{
    uint8_t cmd[4] = {0xAB, (uint8_t)(0x80 | (out_bits & 0x3f)), (uint8_t)(0x40 | (dir_bits & 0x3f)), 0x20};
    return usb_out(cmd, 4);
}

// Open the first matching device, retrying ~5 s while the CH341 enumerates
// (the same loop as apps/meshtoad's ch341_open).
static int open_device(uint16_t vid, uint16_t pid)
{
    static usb_device_entry list[16];
    int tries, i;
    for (tries = 0; tries < 25; tries++) {
        u8 cnt = 0;
        if (USB_GetDeviceList(list, 16, 0, &cnt) < 0)
            cnt = 0;
        for (i = 0; i < cnt; i++)
            if (list[i].vid == vid && list[i].pid == pid &&
                USB_OpenDevice(list[i].device_id, list[i].vid, list[i].pid, &fd) >= 0)
                return 0;
        usleep(200 * 1000);
    }
    return -1;
}

int32_t pinedio_set_option(struct pinedio_inst *inst, enum pinedio_option option, uint32_t value)
{
    if (option < PINEDIO_OPTION_MAX)
        inst->options[option] = value;
    return 0;
}

int32_t pinedio_init(struct pinedio_inst *inst, void *driver)
{
    uint16_t vid = inst->options[PINEDIO_OPTION_VID] ? (uint16_t)inst->options[PINEDIO_OPTION_VID] : 0x1A86;
    uint16_t pid = inst->options[PINEDIO_OPTION_PID] ? (uint16_t)inst->options[PINEDIO_OPTION_PID] : 0x5512;
    int i;
    (void)driver;

    inst->in_error = false;
    for (i = 0; i < PINEDIO_INT_PIN_MAX; i++)
        inst->interrupts[i].callback = NULL;
    // DMA buffers from an IOS heap (MEM2). From memalign (MEM1) only whole
    // 4-byte words of an IN transfer arrived: a 2-byte SPI reply never landed.
    if (outbuf == NULL || inbuf == NULL) {
        s32 hid = iosCreateHeap(2 * DMA_SIZE + 256);
        if (hid >= 0) {
            outbuf = (uint8_t *)iosAlloc(hid, DMA_SIZE);
            inbuf = (uint8_t *)iosAlloc(hid, DMA_SIZE);
        }
        printf("ch341: DMA buffers in %s\n", hid >= 0 ? "IOS heap (MEM2)" : "-");
    }
    if (outbuf == NULL || inbuf == NULL)
        return -1;
    USB_Initialize();
    if (open_device(vid, pid) != 0) {
        printf("ch341: no %04x:%04x on USB (is the MeshToad plugged in?)\n", vid, pid);
        inst->in_error = true;
        return -2;
    }
    printf("ch341: opened %04x:%04x\n", vid, pid);
    snprintf(inst->serial_number, sizeof(inst->serial_number), "wii");
    snprintf(inst->product_string, sizeof(inst->product_string), "CH341 %04x:%04x (Wii USB)", vid, pid);
    // SCK (D3) and MOSI (D5) must be outputs or every MISO byte reads 0xFF.
    dir_bits = (1 << 3) | (1 << 5);
    out_bits = 0;
    return send_pins() == 0 ? 0 : -3;
}

int32_t pinedio_set_pin_mode(struct pinedio_inst *inst, uint32_t pin, uint32_t mode)
{
    if (pin > 5)
        return 0; // D6/D7 are inputs only
    if (mode)
        dir_bits |= (uint8_t)(1 << pin);
    else
        dir_bits &= (uint8_t)~(1 << pin);
    if (send_pins() != 0) {
        inst->in_error = true;
        return -1;
    }
    return 0;
}

int32_t pinedio_digital_write(struct pinedio_inst *inst, uint32_t pin, bool active)
{
    if (pin > 5)
        return -1;
    if (active)
        out_bits |= (uint8_t)(1 << pin);
    else
        out_bits &= (uint8_t)~(1 << pin);
    if (send_pins() != 0) {
        inst->in_error = true;
        return -1;
    }
    return 0;
}

int32_t pinedio_set_cs(struct pinedio_inst *inst, bool active)
{
    return pinedio_digital_write(inst, 0, !active); // NSS on D0, active low
}

int32_t pinedio_transceive(struct pinedio_inst *inst, uint8_t *write_buf, uint8_t *read_buf, uint32_t count)
{
    uint8_t pkt[PKT], back[PKT];
    uint32_t off = 0;
    int auto_cs = inst->options[PINEDIO_OPTION_AUTO_CS] != 0, rc = 0;

    if (auto_cs && pinedio_set_cs(inst, true) != 0)
        return -1;
    while (off < count) {
        uint32_t n = count - off > PKT - 1 ? PKT - 1 : count - off, i;
        pkt[0] = 0xA8;
        for (i = 0; i < n; i++)
            pkt[1 + i] = rev(write_buf[off + i]);
        if (usb_out(pkt, (int)n + 1) != 0 || usb_in(back, (int)n) != 0) {
            inst->in_error = true;
            rc = -1;
            break;
        }
        if (read_buf != NULL)
            for (i = 0; i < n; i++)
                read_buf[off + i] = rev(back[i]);
        off += n;
    }
    if (auto_cs)
        pinedio_set_cs(inst, false);
    return rc;
}

int32_t pinedio_write_read(struct pinedio_inst *inst, uint8_t *writearr, uint32_t writecnt, uint8_t *readarr, uint32_t readcnt)
{
    uint8_t buf[SPI_MAX];
    uint32_t total = writecnt + readcnt;
    if (total > SPI_MAX)
        return -1;
    memcpy(buf, writearr, writecnt);
    memset(buf + writecnt, 0, readcnt);
    if (pinedio_transceive(inst, buf, buf, total) != 0)
        return -1;
    memcpy(readarr, buf + writecnt, readcnt);
    return 0;
}

int32_t pinedio_digital_read(struct pinedio_inst *inst, uint32_t pin)
{
    uint8_t cmd = 0xA0, reply[6];
    if (usb_out(&cmd, 1) != 0 || usb_in(reply, 6) != 0) {
        inst->in_error = true;
        return -1;
    }
    return (reply[0] >> pin) & 1;
}

int32_t pinedio_get_irq_state(struct pinedio_inst *inst, uint32_t pin)
{
    return pinedio_digital_read(inst, pin);
}

// The firmware polls the radio's IRQ flags every loop tick (pollMissedIrqs), as
// in the wasm build, so callbacks are only recorded.
int32_t pinedio_attach_interrupt(struct pinedio_inst *inst, enum pinedio_int_pin int_pin, enum pinedio_int_mode int_mode,
                                 void (*callback)(void))
{
    if (int_pin >= PINEDIO_INT_PIN_MAX)
        return -1;
    inst->interrupts[int_pin].previous_state = 255;
    inst->interrupts[int_pin].mode = int_mode;
    inst->interrupts[int_pin].callback = callback;
    return 0;
}

int32_t pinedio_deattach_interrupt(struct pinedio_inst *inst, enum pinedio_int_pin int_pin)
{
    if (int_pin >= PINEDIO_INT_PIN_MAX)
        return -1;
    inst->interrupts[int_pin].callback = NULL;
    return 0;
}

void pinedio_deinit(struct pinedio_inst *inst)
{
    if (fd >= 0)
        USB_CloseDevice(&fd);
    fd = -1;
    inst->in_error = false;
}
