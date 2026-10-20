# USB headset driver for the Wii

A USB Audio Class 1.0 driver for the Wii (libogc, IOS USBV5). It records
from a USB headset microphone and plays to its headphones, at 48 kHz
16-bit, without gaps. Tested with a Razer headset (USB ID 1532:0a49)
on a Wii with the Homebrew Channel.

| Part | Interface | Endpoint | Format |
|---|---|---|---|
| Microphone | 1, alt 1 | 0x84 iso IN, 96 bytes per ms | 48 kHz mono 16-bit |
| Headphones | 2, alt 1 | 0x05 iso OUT, 192 bytes per ms | 48 kHz stereo 16-bit |

The driver reads the USB descriptors, so other USB Audio Class 1.0
headsets with 16-bit streams should work too. Only the Razer is tested.

## API (`source/usbaudio.h`)

```c
int  ua_open(char *info, size_t infolen, char *err, size_t errlen);
int  ua_record(int16_t *out, int max_samples, int (*keep_going)(void)); /* mono */
int  ua_play(const int16_t *in, int samples);                            /* mono to both ears */
void ua_close(void);
```

`ua_record` stops at `max_samples`, or when `keep_going()` returns 0.
Define `UA_LOG(...)` (printf-like) before you compile `usbaudio.c` to get
log lines.

## Build

    git clone --depth 1 -b v3.1.0 https://github.com/devkitPro/libogc.git libogc
    docker run --rm -v "$PWD":/w -w /w devkitpro/devkitppc:20260503 make

This makes `headset.dol`. Hold A to record, release A to hear it.

## What the driver must do on the Wii (found in testing)

1. **IOS shows each USB interface as its own device.** Use the device id of
   the interface that has the stream endpoint. In USBV5 mode the handle is
   the device id, and many ids are negative as `s32` (for example
   `0xe71e0021`), so do not use `< 0` to mean "no handle".
2. **Set the alternate setting through IOS** (USBV5 IOCTL 7 on
   `/dev/usb/ven`, device id at byte 0, setting at byte 8). A normal
   `SET_INTERFACE` control request is not enough: IOS then refuses iso
   transfers with `-4` (`IPC_EINVAL`). libogc keeps its `/dev/usb/ven`
   handle private, so `source/ogc_usb.c` is a copy of libogc v3.1.0
   `usb.c` with `USBV5_SetAlternateSetting()` added.
3. **IOS takes at most 8 iso packets (8 ms) per call, and a call takes
   about 16 ms.** One call at a time gives gaps. The driver keeps 4 async
   calls in a queue.
4. **Switch the headphones on only while they play**, with 64 ms of
   silence before and 32 ms after. A headset that stays on with no data
   buzzes.
5. **Warm-up at open: play, record, play** (with silence). On the Razer,
   the first playback after power-on is slow and broken, and the first
   recording after the headphones were switched on the first time is
   silent.

## Licenses

- `source/ogc_usb.c` is from libogc (zlib-style license; the original
  notice is in the file, and the file is marked as changed).
- The other files are part of Sonoran for Wii.
