/* USB Audio Class 1.0 headset driver for the Wii.
 *
 * The descriptors of the tested headset (Razer, 1532:0a49):
 *   interface 1 alt 1: microphone, iso IN  0x84, 96 bytes/ms  (48 kHz mono 16-bit)
 *   interface 2 alt 1: headphones, iso OUT 0x05, 192 bytes/ms (48 kHz stereo 16-bit)
 * The driver reads the descriptors and does not assume these numbers. */
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/usb.h>

#include "net.h"
#include "usbaudio.h"

#define ISO_MAX 32

struct stream {
    int iface, alt, ep, maxpkt, channels, bits;
};

/* In USBV5 mode a handle is the IOS device id, and many ids are negative
 * as s32 (for example 0xe71e0021). So "open" is a separate flag. */
static s32 fd, mic_fd, spk_fd;      /* control; one IOS device per interface */
static int fd_open, mic_open, spk_open;
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

/* Set the alternate setting through IOS (USBV5 IOCTL 7). IOS must know
 * the setting, or it refuses iso transfers on the endpoint (IPC_EINVAL).
 * The function is in source/ogc_usb.c, our copy of libogc's usb.c, because
 * it needs libogc's own /dev/usb/ven handle. */
s32 USBV5_SetAlternateSetting(s32 device_id, u8 alternate);

static int set_alt(s32 dev, int alt)
{
    return USBV5_SetAlternateSetting(dev, (u8)alt);
}

/* Interface number of one IOS device entry, or -1. */
int ua_play(const int16_t *in, int samples);

static int iface_of(s32 dev)
{
    usb_devdesc udd;
    int n = -1;

    memset(&udd, 0, sizeof(udd));
    if (USB_GetDescriptors(dev, &udd) >= 0 && udd.configurations != NULL
        && udd.configurations[0].interfaces != NULL)
        n = udd.configurations[0].interfaces[0].bInterfaceNumber;
    USB_FreeDescriptors(&udd);
    return n;
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
    iso_buf = memalign(32, ISO_MAX * 512);
    iso_sizes = memalign(32, 2 * ISO_MAX + 32);
    if (cfg == NULL || iso_buf == NULL || iso_sizes == NULL) {
        snprintf(err, errlen, "out of memory");
        free(cfg);
        return -1;
    }
    for (i = 0; i < cnt && !fd_open; i++) {
        memset(&mic, 0, sizeof(mic));
        memset(&spk, 0, sizeof(spk));
        if (USB_OpenDevice(list[i].device_id, list[i].vid, list[i].pid, &fd) < 0)
            continue;
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
            continue;
        }
        fd_open = 1;
        snprintf(info, infolen, "%04x:%04x mic ep %02x %d ch %d bit, headphones ep %02x %d ch %d bit",
                 list[i].vid, list[i].pid, mic.ep, mic.channels, mic.bits, spk.ep,
                 spk.channels, spk.bits);
    }
    free(cfg);
    if (!fd_open) {
        snprintf(err, errlen, "no USB audio headset found");
        return -1;
    }
    dbg("ua: %s", info);
    /* Find the IOS entry of the mic and of the headphone interfaces. */
    for (i = 0; i < cnt; i++) {
        s32 dev;
        int n;
        if (USB_OpenDevice(list[i].device_id, list[i].vid, list[i].pid, &dev) < 0)
            continue;
        n = iface_of(dev);
        dbg("ua: entry %08x is interface %d", (unsigned)list[i].device_id, n);
        if (n == mic.iface && mic.ep && !mic_open) {
            mic_fd = dev;
            mic_open = 1;
        } else if (n == spk.iface && spk.ep && !spk_open) {
            spk_fd = dev;
            spk_open = 1;
        } else if (dev != fd) {
            USB_CloseDevice(&dev);
        }
    }
    if (!mic_open)
        mic_fd = fd;
    if (!spk_open)
        spk_fd = fd;
    if (mic.ep) {
        ret = set_alt(mic_fd, mic.alt);
        dbg("ua: mic set_alt(%08x, %d) = %d", (unsigned)mic_fd, mic.alt, (int)ret);
    }
    /* The headphones are switched on only while they play (ua_play).
     * A headset that stays on with no data can buzz. The first playback
     * after power-on is slow and broken (measured: 2.4 s of sound took
     * 5.6 s), so play 300 ms of silence once here as a warm-up. */
    if (spk.ep) {
        static int16_t warm[48 * 300];
        unsigned long long t0 = net_ms();
        ua_play(warm, 48 * 300);
        dbg("ua: warm-up took %llu ms", net_ms() - t0);
    }
    return 0;
}

void ua_close(void)
{
    if (!fd_open)
        return;
    if (mic.ep)
        set_alt(mic_fd, 0);
    if (spk.ep)
        set_alt(spk_fd, 0);
    if (mic_open && mic_fd != fd)
        USB_CloseDevice(&mic_fd);
    if (spk_open && spk_fd != fd)
        USB_CloseDevice(&spk_fd);
    USB_CloseDevice(&fd);
    fd_open = mic_open = spk_open = 0;
}

/* ---- streaming with a queue of async iso transfers ----
 * IOS takes at most 8 packets (8 ms) per call, and one call takes about
 * 16 ms. With QDEPTH calls in the queue, IOS always has the next one. */

#define QDEPTH 4
#define QPKTS 8

struct xfer {
    u8 *buf;
    u16 *sizes;
    volatile int busy;
    volatile s32 result;
};

static struct xfer q[QDEPTH];

static s32 xfer_done(s32 result, void *arg)
{
    struct xfer *x = arg;

    x->result = result;
    x->busy = 0;
    return 0;
}

static int queue_ready(void)
{
    int i;

    for (i = 0; i < QDEPTH; i++) {
        if (q[i].buf == NULL)
            q[i].buf = memalign(32, QPKTS * 512);
        if (q[i].sizes == NULL)
            q[i].sizes = memalign(32, 32);
        if (q[i].buf == NULL || q[i].sizes == NULL)
            return -1;
        q[i].busy = 0;
    }
    return 0;
}

static void wait_xfer(struct xfer *x)
{
    unsigned long long t0 = net_ms();

    while (x->busy && net_ms() - t0 < 1000)
        usleep(200);
}

static s32 submit_read(struct xfer *x, int bytes)
{
    int p;

    for (p = 0; p < QPKTS; p++)
        x->sizes[p] = (u16)bytes;
    DCFlushRange(x->sizes, 32);
    DCInvalidateRange(x->buf, QPKTS * 512);
    x->busy = 1;
    return USB_ReadIsoMsgAsync(mic_fd, (u8)mic.ep, QPKTS, x->sizes, x->buf, xfer_done, x);
}

int ua_record(int16_t *out, int max_samples, int (*keep_going)(void))
{
    int frame = UA_RATE / 1000, got = 0, i, k = 0;
    int bytes = frame * (mic.bits / 8) * mic.channels;
    int chunk = frame * QPKTS, stop = 0;

    if (!fd_open || mic.ep == 0 || bytes <= 0 || bytes > 512 || queue_ready() != 0)
        return -1;
    for (i = 0; i < QDEPTH; i++)
        if (submit_read(&q[i], bytes) < 0)
            q[i].busy = 0;
    for (;;) {
        struct xfer *x = &q[k];
        int p, s;

        wait_xfer(x);
        if (x->result >= 0 && got + chunk <= max_samples && !stop) {
            DCInvalidateRange(x->sizes, 32);
            DCInvalidateRange(x->buf, QPKTS * 512);
            for (p = 0; p < QPKTS; p++) {
                const u8 *src = x->buf + p * bytes;
                int n = x->sizes[p] <= bytes ? x->sizes[p] / (2 * mic.channels) : frame;
                for (s = 0; s < frame; s++) {
                    const u8 *v = src + s * 2 * mic.channels;
                    out[got++] = s < n ? (int16_t)(v[0] | (v[1] << 8)) : 0;
                }
            }
        }
        if (got + chunk > max_samples || (keep_going && !keep_going()))
            stop = 1;
        if (!stop && submit_read(x, bytes) < 0)
            stop = 1;
        if (stop) {
            /* let the others finish, then return */
            for (i = 0; i < QDEPTH; i++)
                wait_xfer(&q[i]);
            return got;
        }
        k = (k + 1) % QDEPTH;
    }
}


static int play_raw(const int16_t *in, int samples);

/* Play with the headphones switched on only for this sound: 64 ms of
 * silence first (the first packets after the switch are lost), the sound,
 * 32 ms of silence, then off. */
int ua_play(const int16_t *in, int samples)
{
    static int16_t silence[48 * 64];
    s32 ret;
    int rc;

    if (!fd_open || spk.ep == 0)
        return -1;
    ret = set_alt(spk_fd, spk.alt);
    dbg("ua: spk on = %d", (int)ret);
    play_raw(silence, 48 * 64);
    rc = play_raw(in, samples);
    play_raw(silence, 48 * 32);
    ret = set_alt(spk_fd, 0);
    dbg("ua: spk off = %d", (int)ret);
    return rc;
}

static int play_raw(const int16_t *in, int samples)
{
    int frame = UA_RATE / 1000, done = 0, k = 0, i;
    int bytes = frame * (spk.bits / 8) * spk.channels;
    int chunk = frame * QPKTS;

    if (!fd_open || spk.ep == 0 || bytes <= 0 || bytes > 512 || queue_ready() != 0)
        return -1;
    while (done + chunk <= samples) {
        struct xfer *x = &q[k];
        u8 *dst;
        int p, s, c;

        wait_xfer(x);
        dst = x->buf;
        for (p = 0; p < QPKTS; p++) {
            x->sizes[p] = (u16)bytes;
            for (s = 0; s < frame; s++) {
                int16_t v = in[done + p * frame + s];
                for (c = 0; c < spk.channels; c++) {
                    *dst++ = (u8)(v & 0xff);
                    *dst++ = (u8)((v >> 8) & 0xff);
                }
            }
        }
        DCFlushRange(x->sizes, 32);
        DCFlushRange(x->buf, QPKTS * 512);
        x->busy = 1;
        if (USB_WriteIsoMsgAsync(spk_fd, (u8)spk.ep, QPKTS, x->sizes, x->buf, xfer_done, x) < 0) {
            x->busy = 0;
            return -1;
        }
        done += chunk;
        k = (k + 1) % QDEPTH;
    }
    for (i = 0; i < QDEPTH; i++)
        wait_xfer(&q[i]);
    return 0;
}
