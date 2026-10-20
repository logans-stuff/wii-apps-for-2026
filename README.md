# Wii apps

Homebrew apps for the Nintendo Wii (devkitPPC + libogc, IOS USBV5).
Each app builds to a `.dol` you can run from the Homebrew Channel.

## Apps

| App | Build | What it is |
|---|---|---|
| **Meshtoad LoRa node** | `build/lora.dol` | A two-way [Meshtastic](https://meshtastic.org) node: an SX1262 radio over a CH341 USB-to-SPI bridge (a USB "Meshtoad"). Listens, decrypts the channel (AES-CTR) and decodes packets (text, node info), and transmits — the Wii joins the mesh. |
| **USB headset driver** | `build/headset.dol` | USB Audio Class 1.0 record/playback (48 kHz), tested with a Razer BlackShark V2 Pro. See `source/usbaudio.h`. |
| **Sonoran** | `build/boot.dol` | A chat/nodes client that does its own TLS (wolfSSL), so the Wii's old roots don't matter. |
| **USB probe** | `build/probe.dol` | Dumps USB descriptors to a debug log — used to bring up the USB drivers. |
| **DAVE link test** | `build/davetest.dol` | Ports OpenSSL + libdave (Discord end-to-end voice) to the Wii. |

## Build

Everything builds in the pinned devkitPPC container via `build.sh`:

    ./build.sh build/lora.dol        # the Meshtoad node
    ./build.sh build/headset.dol     # the headset driver
    ./build.sh build/boot.dol        # Sonoran

Some apps need third-party sources cloned into `third_party/` first
(wolfSSL, cJSON, QR-Code-generator, libogc) — see the top of the
`Makefile` and `build.sh`. Install a `.dol` by copying it to
`sd:/apps/<name>/boot.dol`, or send it over the network with `wiiload`.

Linux-side tests of the portable code (TLS, Meshtastic decode/encode):

    docker run --rm -v "$PWD":/w -w /w gcc:14 make -f Makefile.host
    ./build-host/hosttest --mesh      # Meshtastic AES-CTR + protobuf round-trip

## Notes

- The debug-log host is **not** hardcoded: it defaults to `127.0.0.1` and
  is overridden at build time via a gitignored `local.mk`
  (`CFLAGS += -DDEBUG_HOST='"<your-host>"'`).
- The Meshtoad node uses the public default Meshtastic channel key.

## License

Public domain (Unlicense) — see `LICENSE`.
