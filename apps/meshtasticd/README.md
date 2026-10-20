# meshtasticd for the Wii

The real [Meshtastic firmware](https://github.com/meshtastic/firmware) (pinned in
`build.sh`, currently **v2.8.1**) built for the Nintendo Wii, driving a MeshToad
(CH341 + E22/SX1262) on the Wii's USB port. Every firmware behaviour - routing,
reliability, the periodic modules and their intervals, roles, rebroadcast modes,
airtime and congestion safeguards - is upstream code, not a re-implementation.

It is the firmware's portduino build shaped like its `native-wasm` target:
single-threaded `setup()` + `loop()`, the radio through `Ch341Hal` and a
libpinedio backend, the client API in-process.

| path | what |
|---|---|
| `build.sh` | fetch the pinned firmware into `third_party/`, apply `patches/`, copy `overlay/`, build in Docker |
| `docker/` | devkitPPC + PlatformIO build image |
| `platform-wii/` | PlatformIO platform (from meshtastic/platform-wasm, toolchain swapped for devkitPPC) |
| `overlay/variants/native/wii/platformio.ini` | `[env:native-wii]` |
| `overlay/src/platform/portduino/wii/` | Wii glue: entry point, SD/identity/region setup, libogc CH341 backend (`libpinedio_ogc.c`), stubs, optional log streaming |
| `overlay/lib/WiFi/` | framework-portduino's WiFi library on libogc sockets |
| `patches/` | changes to upstream (big-endian wire format) |

```sh
apps/meshtasticd/build.sh                            # -> apps/meshtasticd/build/meshtasticd.dol
WII_DEBUG_HOST=10.0.0.125 apps/meshtasticd/build.sh  # also stream the log to :4302 (tools/fwlog)
```

State lives on the SD card in `sd:/apps/meshtasticd/`. The node number comes
from the console's device ID. A fresh node defaults to region US.

Background and the firmware behaviour notes: `docs/meshtastic-2.8.1-notes.md`.

Licence: GPL-3.0 (the firmware's licence; see `LICENSE`). This directory is
GPL even though the rest of the repository is Unlicense.
