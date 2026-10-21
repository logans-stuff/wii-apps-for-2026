# Wii apps

Homebrew apps for the Nintendo Wii (devkitPPC + libogc, IOS USBV5).
Each app lives under `apps/` (or `tools/`), shares the plumbing in
`common/`, and builds to a `.dol` you can run from the Homebrew Channel.

## Apps

| App | Build | What it is |
|---|---|---|
| **Meshtoad LoRa node** | `apps/meshtoad` | A two-way [Meshtastic](https://meshtastic.org) node: an SX1262 radio over a CH341 USB-to-SPI bridge (a USB "Meshtoad"). Listens, decrypts the channel (AES-CTR) and decodes packets (text, node info, position), and transmits — the Wii joins the mesh. |
| **Meshtastic firmware** | *moved* | The real Meshtastic firmware (2.8.1) with MeshtasticUI, running natively on the Wii with a USB Meshtoad. It started here as `apps/meshtasticd` and now lives in **[logans-stuff/wii-meshtastic](https://github.com/logans-stuff/wii-meshtastic)**, along with the firmware notes (`docs/meshtastic-2.8.1-notes.md`). |
| **USB headset driver** | `apps/usb-headset` | USB Audio Class 1.0 record/playback (48 kHz) with an Opus round-trip test, tested with a Razer BlackShark V2 Pro. See `apps/usb-headset/source/usbaudio.h`. |
| **Sonoran** | `apps/sonoran` | A chat/nodes client that does its own TLS (wolfSSL), so the Wii's old roots don't matter. |
| **USB probe** | `tools/usbprobe` | Dumps USB descriptors to a debug log — used to bring up the USB drivers. |
| **DAVE link test** | `tools/davetest` | Ports OpenSSL + libdave (Discord end-to-end voice) to the Wii. Needs `ports/` built first. |

## Layout

    common/      shared code: USB stack (ogc_usb), networking (net), wolfSSL config, cacerts
    common.mk    shared build; each app's Makefile sets its sources and `include $(ROOT)/common.mk`
    apps/<name>/ one app: its own source/, Makefile, meta.xml
    tools/<name>/ dev tools and tests
    third_party/ vendored deps (wolfSSL, cJSON, QR-Code-generator, libogc) — cloned, gitignored

## Build

Everything builds in the pinned devkitPPC container via `build.sh <app-dir>`:

    ./build.sh apps/meshtoad        # -> apps/meshtoad/build/meshtoad.dol
    ./build.sh apps/usb-headset
    ./build.sh apps/sonoran

First clone the third-party sources into `third_party/` (wolfSSL, cJSON,
QR-Code-generator, libogc). Install a `.dol` by copying it to
`sd:/apps/<name>/boot.dol`, or send it over the network with `wiiload`.

Linux-side tests of the portable code (TLS, Meshtastic decode/encode):

    docker run --rm -v "$PWD":/w -w /w gcc:14 make -f Makefile.host
    ./build-host/hosttest --mesh      # Meshtastic AES-CTR + protobuf round-trip
    ./build-host/hosttest --meshtx    # text + nodeinfo encode round-trip

## Notes

- The debug-log host is **not** hardcoded: it defaults to `127.0.0.1` and
  is overridden at build time via a gitignored `local.mk`
  (`CFLAGS += -DDEBUG_HOST='"<your-host>"'`).
- The Meshtoad node uses the public default Meshtastic channel key.

## License

Public domain (Unlicense) — see `LICENSE`.
