/* USB probe: list the USB devices and send their raw descriptors to the
 * debug log on bigboy (TCP 4301). Used to plan the headset driver. */
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/usb.h>
#include <wiiuse/wpad.h>

#include "net.h"

static void *xfb;

static void video_start(void)
{
    GXRModeObj *rmode;

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
}

static void dump(const char *what, const u8 *p, int n)
{
    char line[200];
    int i, off;

    for (off = 0; off < n; off += 64) {
        int k = 0, end = off + 64 < n ? off + 64 : n;
        for (i = off; i < end; i++)
            k += snprintf(line + k, sizeof(line) - k, "%02x", p[i]);
        dbg("%s +%d: %s", what, off, line);
    }
}

static void probe_device(const usb_device_entry *e)
{
    s32 fd = -1, ret;
    u8 *buf = memalign(32, 1024);
    int total;
    char what[48];

    printf("  device id %08x  VID %04x  PID %04x\n", (unsigned)e->device_id, e->vid, e->pid);
    dbg("usb device id=%08x vid=%04x pid=%04x", (unsigned)e->device_id, e->vid, e->pid);
    ret = USB_OpenDevice(e->device_id, e->vid, e->pid, &fd);
    if (ret < 0) {
        printf("    open failed (%d)\n", (int)ret);
        dbg("usb open failed %d", (int)ret);
        free(buf);
        return;
    }
    memset(buf, 0, 1024);
    ret = USB_WriteCtrlMsg(fd, USB_CTRLTYPE_DIR_DEVICE2HOST, USB_REQ_GETDESCRIPTOR,
                           USB_DT_DEVICE << 8, 0, 18, buf);
    snprintf(what, sizeof(what), "%04x:%04x dev", e->vid, e->pid);
    dbg("usb get device desc ret=%d", (int)ret);
    if (ret > 0)
        dump(what, buf, ret);

    memset(buf, 0, 1024);
    ret = USB_WriteCtrlMsg(fd, USB_CTRLTYPE_DIR_DEVICE2HOST, USB_REQ_GETDESCRIPTOR,
                           USB_DT_CONFIG << 8, 0, 9, buf);
    total = ret >= 4 ? (buf[2] | (buf[3] << 8)) : 0;
    dbg("usb config head ret=%d total=%d", (int)ret, total);
    if (total > 1024)
        total = 1024;
    if (total > 0) {
        memset(buf, 0, 1024);
        ret = USB_WriteCtrlMsg(fd, USB_CTRLTYPE_DIR_DEVICE2HOST, USB_REQ_GETDESCRIPTOR,
                               USB_DT_CONFIG << 8, 0, (u16)total, buf);
        snprintf(what, sizeof(what), "%04x:%04x cfg", e->vid, e->pid);
        dbg("usb config ret=%d", (int)ret);
        if (ret > 0)
            dump(what, buf, ret);
        printf("    config descriptor: %d bytes (sent to the log)\n", (int)ret);
    }
    USB_CloseDevice(&fd);
    free(buf);
}

int main(void)
{
    static usb_device_entry list[16];
    u8 cnt = 0;
    char err[200];
    s32 ret;
    int cls, i;

    video_start();
    WPAD_Init();
    printf("\x1b[2;1HSonoran USB probe\n\n");
    if (net_start(err, sizeof(err)) != 0)
        printf("network: %s (no log)\n", err);
    dbg("usbprobe: start");
    ret = USB_Initialize();
    dbg("usb init %d", (int)ret);

    /* Class 0 should list all devices. Class 1 is audio. */
    for (cls = 0; cls <= 1; cls++) {
        cnt = 0;
        ret = USB_GetDeviceList(list, 16, (u8)cls, &cnt);
        printf("Device list, class %d: ret %d, %u devices\n", cls, (int)ret, cnt);
        dbg("usb list class=%d ret=%d cnt=%u", cls, (int)ret, cnt);
        for (i = 0; i < cnt; i++)
            probe_device(&list[i]);
        if (cnt > 0)
            break;
    }
    dbg("usbprobe: done");
    printf("\nDone. Push Home to exit.\n");
    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            break;
        VIDEO_WaitVSync();
    }
    return 0;
}
