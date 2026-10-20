/* USB Audio Class 1.0 headset driver for the Wii.
 *
 * The descriptors of the tested headset (Razer, 1532:0a49):
 *   interface 1 alt 1: microphone, iso IN  0x84, 96 bytes/ms  (48 kHz mono 16-bit)
 *   interface 2 alt 1: headphones, iso OUT 0x05, 192 bytes/ms (48 kHz stereo 16-bit)
 * The driver reads the descriptors and does not assume these numbers. */
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/usb.h>

#include "net.h"
#include "usbaudio.h"

#define ISO_PACKETS 8          /* 8 ms for each IOS call */

struct stream {
    int iface, alt, ep, maxpkt, channels, bits;
};

static s32 fd = -1;
static struct stream mic, spk;
static u8 *iso_buf;
static u16 *iso_sizes;

/* Walk the configuration descriptor and find the two streams. */
static void parse_config(const u8 *b, int n)
{
    int i = 0, iface = -1, alt = 0, cls = 0, sub = 0, ch = 0, bits = 0;

    while (i + 2 <= n && b[i] >= 2 && i + b[i] <= n) {
        const u8 *d = b + i;
        if (d[1] == USB_DT_INTERFACE) {
            iface = d[2];
            alt = d[3];
            cls = d[5];
            sub = d[6];
            ch = bits = 0;
        } else if (d[1] == 0x24 && cls == 1 && sub == 2 && d[2] == 2 && d[0] >= 8) {
            ch = d[4];
            bits = d[6];
        } else if (d[1] == USB_DT_ENDPOINT && cls == 1 && sub == 2 && (d[3] & 3) == 1) {
            struct stream *s = (d[2] & 0x80) ? &mic : &spk;
            if (s->ep == 0) {
                s->iface = iface;
                s->alt = alt;
                s->ep = d[2];
                s->maxpkt = d[4] | (d[5] << 8);
                s->channels = ch;
                s->bits = bits;
            }
        }
        i += b[i];
    }
}

static int set_alt(int iface, int alt)
{
    return USB_WriteCtrlMsg(fd, USB_CTRLTYPE_DIR_HOST2DEVICE | USB_CTRLTYPE_TYPE_STANDARD
                            | USB_CTRLTYPE_REC_INTERFACE, USB_REQ_SETINTERFACE,
                            (u16)alt, (u16)iface, 0, NULL);
}

int ua_open(char *info, size_t infolen, char *err, size_t errlen)
{
    static usb_device_entry list[16];
    u8 cnt = 0, *cfg;
    s32 ret;
    int i, total;

    ua_close();
    USB_Initialize();
    if (USB_GetDeviceList(list, 16, 0, &cnt) < 0 || cnt == 0) {
        snprintf(err, errlen, "no USB device found");
        return -1;
    }
    cfg = memalign(32, 1024);
    iso_buf = memalign(32, ISO_PACKETS * 512);
    iso_sizes = memalign(32, 32);
    if (cfg == NULL || iso_buf == NULL || iso_sizes == NULL) {
        snprintf(err, errlen, "out of memory");
        free(cfg);
        return -1;
    }
    for (i = 0; i < cnt && fd < 0; i++) {
        memset(&mic, 0, sizeof(mic));
        memset(&spk, 0, sizeof(spk));
        if (USB_OpenDevice(list[i].device_id, list[i].vid, list[i].pid, &fd) < 0) {
            fd = -1;
            continue;
        }
        memset(cfg, 0, 1024);
        ret = USB_WriteCtrlMsg(fd, USB_CTRLTYPE_DIR_DEVICE2HOST, USB_REQ_GETDESCRIPTOR,
                               USB_DT_CONFIG << 8, 0, 9, cfg);
        total = ret >= 4 ? (cfg[2] | (cfg[3] << 8)) : 0;
        if (total > 1024)
            total = 1024;
        if (total > 0)
            ret = USB_WriteCtrlMsg(fd, USB_CTRLTYPE_DIR_DEVICE2HOST, USB_REQ_GETDESCRIPTOR,
                                   USB_DT_CONFIG << 8, 0, (u16)total, cfg);
        parse_config(cfg, total);
        if (mic.ep == 0 && spk.ep == 0) {
            USB_CloseDevice(&fd);
            fd = -1;
            continue;
        }
        snprintf(info, infolen, "%04x:%04x mic ep %02x %d ch %d bit, headphones ep %02x %d ch %d bit",
                 list[i].vid, list[i].pid, mic.ep, mic.channels, mic.bits, spk.ep,
                 spk.channels, spk.bits);
    }
    free(cfg);
    if (fd < 0) {
        snprintf(err, errlen, "no USB audio headset found");
        return -1;
    }
    dbg("ua: %s", info);
    if (mic.ep) {
        ret = set_alt(mic.iface, mic.alt);
        dbg("ua: mic set_alt(%d,%d) = %d", mic.iface, mic.alt, (int)ret);
    }
    if (spk.ep) {
        ret = set_alt(spk.iface, spk.alt);
        dbg("ua: spk set_alt(%d,%d) = %d", spk.iface, spk.alt, (int)ret);
    }
    return 0;
}

void ua_close(void)
{
    if (fd >= 0) {
        if (mic.ep)
            set_alt(mic.iface, 0);
        if (spk.ep)
            set_alt(spk.iface, 0);
        USB_CloseDevice(&fd);
    }
    fd = -1;
}

int ua_record(int16_t *out, int max_samples)
{
    int got = 0, frame = UA_RATE / 1000;          /* 48 samples each ms */
    int bytes = frame * (mic.bits / 8) * mic.channels;
    static int logged;

    if (fd < 0 || mic.ep == 0 || bytes <= 0 || bytes > 512)
        return -1;
    while (got + frame * ISO_PACKETS <= max_samples) {
        s32 ret;
        int p, s;
        for (p = 0; p < ISO_PACKETS; p++)
            iso_sizes[p] = (u16)bytes;
        DCFlushRange(iso_sizes, 32);
        ret = USB_ReadIsoMsg(fd, (u8)mic.ep, ISO_PACKETS, iso_sizes, iso_buf);
        if (!logged) {
            dbg("ua: first iso read = %d, sizes %u %u", (int)ret, iso_sizes[0], iso_sizes[1]);
            logged = 1;
        }
        if (ret < 0)
            return got > 0 ? got : (int)ret;
        DCInvalidateRange(iso_buf, ISO_PACKETS * 512);
        /* Packets are packed one after the other at the requested size. */
        for (p = 0; p < ISO_PACKETS; p++) {
            const u8 *src = iso_buf + p * bytes;
            for (s = 0; s < frame; s++) {
                /* little-endian 16-bit; channel 0 only */
                const u8 *x = src + s * 2 * mic.channels;
                out[got++] = (int16_t)(x[0] | (x[1] << 8));
            }
        }
    }
    return got;
}

int ua_play(const int16_t *in, int samples)
{
    int frame = UA_RATE / 1000, done = 0;
    int bytes = frame * (spk.bits / 8) * spk.channels;
    static int logged;

    if (fd < 0 || spk.ep == 0 || bytes <= 0 || bytes > 512)
        return -1;
    while (done + frame * ISO_PACKETS <= samples) {
        s32 ret;
        int p, s, c;
        for (p = 0; p < ISO_PACKETS; p++) {
            u8 *dst = iso_buf + p * bytes;
            iso_sizes[p] = (u16)bytes;
            for (s = 0; s < frame; s++) {
                int16_t v = in[done + p * frame + s];
                for (c = 0; c < spk.channels; c++) {
                    *dst++ = (u8)(v & 0xff);
                    *dst++ = (u8)((v >> 8) & 0xff);
                }
            }
        }
        DCFlushRange(iso_sizes, 32);
        DCFlushRange(iso_buf, ISO_PACKETS * 512);
        ret = USB_WriteIsoMsg(fd, (u8)spk.ep, ISO_PACKETS, iso_sizes, iso_buf);
        if (!logged) {
            dbg("ua: first iso write = %d", (int)ret);
            logged = 1;
        }
        if (ret < 0)
            return (int)ret;
        done += frame * ISO_PACKETS;
    }
    return 0;
}
