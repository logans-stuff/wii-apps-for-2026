# Meshtastic firmware 2.8.1: what a node does, and how to run it on a Wii

Notes from reading the firmware source while bringing Meshtoad (the Wii
LoRa client in `apps/meshtoad`) up to firmware behaviour. Everything here was
read out of **meshtastic/firmware tag `v2.8.1.8e6a88d`** (protobufs submodule
`8f97d66`). Paths are relative to that tree's `src/` unless noted. Re-check them
when moving to a newer release; 2.8 changed a lot of defaults.

Two parts:

1. **What a 2.8.1 node does on the air** - defaults, minimums, and safeguards.
   That's the behaviour Meshtoad has to copy to be a good citizen on the mesh.
2. **Running the real firmware on the Wii** - the plan we switched to, because
   hand-porting part 1 forever drifts behind upstream.

---

## Part 1 - 2.8.1 behaviour reference

### Radio frame

16-byte plaintext header, then AES-CTR(channel key) of a `Data` protobuf.

| bytes | field |
|---|---|
| 0-3 | `to` (LE) - `0xffffffff` broadcast |
| 4-7 | `from` (LE) |
| 8-11 | `id` (LE) |
| 12 | flags: bits 0-2 `hop_limit`, bit 3 `want_ack`, bit 4 `via_mqtt`, bits 5-7 `hop_start` |
| 13 | channel hash |
| 14 | `next_hop` - last byte of the preferred relay, 0 = flood |
| 15 | `relay_node` - last byte of whoever transmitted *this copy* |

- AES-CTR nonce = packet id as 8 bytes LE, then `from` as 4 bytes LE, then 4
  zero bytes (`mesh/CryptoEngine.cpp`).
- **Channel hash** = `xorHash(name) ^ xorHash(psk)` (`mesh/Channels.cpp:27-56`). An
  empty channel name hashes as the preset's display name. Check: `"MediumFast"`
  with the default key gives `0x1f`, the hash our mesh uses. AEAD channels (new in
  2.8) additionally xor `0xAE`.
- **`Data.bitfield` (field 9) is always sent by firmware >= 2.5**: bit 0 =
  `ok_to_mqtt`, bit 1 = `want_response` (`mesh/Router.h:314`,
  `mesh/Router.cpp:1257`). Its presence is how a receiver knows `hop_start` is
  trustworthy.
- **Every packet we transmit carries `relay_node` = our last byte**, and our own
  packets set `hop_start = hop_limit` (`mesh/Router.cpp:525-551`).
- Broadcasts never carry `want_ack` (`Router::send` clears it).

### Receive-side drops ("safeguards")

- **Pre-hop drop** (`mesh/NodeDB.h:216`, `NodeDB.cpp:3533`): `hop_start <
  hop_limit` is dropped outright. After decode, `hop_start == 0` *without* a
  bitfield is treated as pre-2.3 firmware: skipped (no modules, no relay).
  **Meshtoad originally sent no bitfield**, so a hop-limit-0 packet from it
  would have been ignored by 2.8 nodes. Fixed.
- `from == 0xffffffff` dropped; `via_mqtt` dropped when `ignore_mqtt`.
- Decryptable but malformed `Data` is dropped (DECODE_FAILURE). A channel hash
  we don't have is *opaque*: relay-only, never handled.
- **NeighborInfo is dropped unless the NeighborInfo module is enabled**
  (default off), so most 2.8 nodes neither process nor relay it
  (`mesh/Router.cpp:~1590`).
- `rebroadcast_mode = CORE_PORTNUMS_ONLY` drops (and doesn't relay) anything not
  in: text, text-compressed, position, nodeinfo, routing, telemetry, admin,
  alert, key-verification, waypoint, store-forward(++), traceroute.
- `KNOWN_ONLY`: packets from nodes without User info fail decode -> dropped.

Module order matters (`modules/Modules.cpp`): **TrafficManagement is first,
Routing (ACK + relay) is last.** So a module that answers a request suppresses
the plain ACK, and a TMM "STOP" (deduped position) means no ACK and no relay.

### Reliable unicast (ReliableRouter / NextHopRouter)

- Unicast `want_ack` gets **5 attempts** (`NUM_RELIABLE_UNICAST_ATTEMPTS`,
  `mesh/NextHopRouter.h:115`). Broadcast reliable retx is 3.
- Retransmit timer (`RadioInterface::getRetransmissionMsec`):
  `2*airtime + (2^CW(util) + 2*CWmax + 2^((CWmax+CWmin)/2)) * slotTime + 4500 ms`.
  Every pending timer is pushed back by the airtime of each packet sent or
  received meanwhile (we couldn't hear an ACK then).
- On the last attempt next-hop routing falls back to flooding. Out of attempts
  means a local `MAX_RETRANSMIT` NAK.
- **Implicit ACK**: hearing our own packet rebroadcast by a relay stops
  retransmission (`perhapsGenerateImplicitAckForOwnOverheard`).
- **Sending ACKs** (`ReliableRouter::sniffReceived`): only for packets *to us*,
  and only when no module produced a reply:
  - text DMs: ACK that is itself `want_ack` (reliable),
  - other non-reply packets: ACK with `getHopLimitForResponse`,
  - replies/ACKs (`request_id` set): 0-hop ACK only if heard direct.
- Duplicates are re-ACKed **only** when the originator itself retransmitted
  (`hops_away == 0`); relayed duplicates are not
  (`NextHopRouter::shouldFilterReceived`).
- `getHopLimitForResponse` (`modules/RoutingModule.cpp:64`): hops used, if more
  than our limit; 0 if `hop_start == 0`; hops used + 2 if under the limit;
  else our limit.
- Unanswered `want_response` requests to us (or broadcast) get a
  `NO_RESPONSE` NAK unless a module explicitly ignored them (throttled)
  (`mesh/MeshModule.cpp:213`).

### Managed flooding (relay rules)

- `isRebroadcaster` = role != CLIENT_MUTE and mode != NONE.
- Relay when: not to us, not from us, `hop_limit > 0`, `id != 0`, and
  `next_hop` is 0 or our byte. Decrement `hop_limit` (client roles always do),
  set `relay_node` to us. The header is plaintext, so a relay never
  re-encrypts, except NeighborInfo, which rewrites `last_sent_by_id`.
- **Contention delay**: own packets wait `random(0, 2^CW(util)) * slot`; relays
  wait `2*CWmax*slot + random(0, 2^CW(snr)) * slot`, with
  `CW(snr) = map(snr, -20, 10, 3, 8)`. **Weak copies (far away) relay first.**
  Routers skip the `2*CWmax` offset.
- `slotTime` (SX126x) = `2.5 * symbolTime + 7.6 ms`.
  `CW(util) = map(chanutil%, 0, 100, 3, 8)`.
- **Dupe cancel**: hearing someone else relay a packet we have queued cancels
  ours (any client role). An ACK/reply for a DM that isn't for us also cancels
  our queued relay of that DM.
- **Hop-limit upgrade**: a later copy with *more* hops left replaces our queued
  lower-hop copy.
- **Opaque relay** (`NextHopRouter::relayOpaquePacket`): undecodable packets are
  relayed in ALL / ALL_SKIP_DECODING / CORE_PORTNUMS_ONLY. In LOCAL_ONLY /
  KNOWN_ONLY only a PKI-shaped unicast (channel byte 0) with a known party is.
  They use their own small seen-set.
- CLIENT_HIDDEN defaults rebroadcast to LOCAL_ONLY (`NodeDB.cpp:1716`).

### Airtime and the TX gates (`airtime.cpp`)

- Channel utilization: 6 x 10 s buckets (the last 60 s), TX + RX + unparseable
  RX. Plus an EMA with a 1/128 weight per bucket (~21 min) used by hop scaling.
- TX utilization: 60 x 1 min buckets (the last hour).
- `isTxAllowedChannelUtil(polite)`: below **25 %** (polite: metadata) or
  **40 %**.
- `isTxAllowedAirUtil`: hourly TX below **half** the region duty cycle (always
  true at 100 %, i.e. US).
- `Router::send` refuses outright (DUTY_CYCLE_LIMIT) when hourly TX is above the
  duty cycle. EU_866 is 2.5 % for clients, 10 % for routers.
- Time on air: Semtech/RadioLib formula, explicit header, CRC on, LDRO when the
  symbol time is > 16 ms.

### Periodic broadcasts, 2.8.1 defaults

| what | default | minimum / gate |
|---|---|---|
| NodeInfo | every **3 h** (`node_info_broadcast_secs`) | min **1 h**; first one 30 s after boot; CLIENT_HIDDEN never |
| NodeInfo send/reply throttle | never within **30 min** of the last one (scaled by online nodes); 60 s for "shorter timeout" sends | needs channel util below 40 % |
| NodeInfo reply suppression | one reply per requesting node per **12 h** | - |
| First NodeInfo after a channel change | broadcast with `want_response` | - |
| Position | **opt-in in 2.8**: channel `position_precision` defaults to **0 = don't send** | default-PSK channels capped at **15 bits**; interval 1 h, **6 h floor when fixed/stationary**; smart broadcast min 5 min / 100 m; replies max every 3 min |
| Device telemetry | **opt-in in 2.8** (`device_telemetry_enabled=false`) | interval 1 h; min **30 min** on a default channel; polite channel-util gate; replies only to direct (not multi-hop broadcast) requests |
| NeighborInfo | **disabled** by default | interval 6 h, min **4 h**; sent over LoRa only if `transmit_over_lora` **and** (non-default channel **or** non-default frequency slot); max 10 neighbours, built from 0-hop packets, expire after 2 x their interval; replies max every 3 min |
| Hop limit | `HOP_RELIABLE` = 3 | max 7 |

**Congestion scaling** (`mesh/Default.h` `congestionScalingCoefficient`): above
40 online nodes (heard in the last 2 h), intervals multiply by
`1 + (online - 40) * 2^SF / (BW_kHz * 100)`. For MediumFast that's 0.02048 per
extra node. Routers, trackers and sensors are exempt.

**Default frequency slot** = `djb2(channel or preset display name) % numSlots +
1`. US with a 250 kHz bandwidth has 104 slots. MediumFast's default is **45** and
LongFast's is **20**, so our mesh's slot 18 is *non-default*. That matters for
the NeighborInfo-over-LoRa rule.

### Traffic management (on by default, new in 2.8)

`modules/TrafficManagementModule.cpp`:

- **Position dedup**: drops a relayed position from another node when its
  truncated coordinates match the last one within **5 h**
  (`position_min_interval_secs`). Precision is the channel's precision, or 19
  bits (~90 m) when the channel has none. Trackers get 1 h and lost-and-found
  15 min.
- Rate limiting, unknown-packet drops and direct NodeInfo responses all exist
  but default to **0 = off**.

### Hop scaling (on by default, new in 2.8)

`modules/HopScalingModule.cpp`, runs every 5 min:

- Samples each node's hop count. Every hour it walks the hop buckets and
  suggests the smallest hop that reaches **40** nodes (extending by one if that
  stays under a politeness-weighted ceiling of up to 80).
- It is only *applied* while the channel is congested: smoothed util >= **20 %**
  engages and <= **12 %** releases, each after 3 consecutive samples. When not
  congested it hands back one hop per hour.
- Caps the hop limit of our own broadcast Position, Telemetry, NodeInfo and
  NeighborInfo (`Router.cpp:529`).

### What Meshtoad did / didn't do (as of this writing)

Fixed in `apps/meshtoad/source/meshtastic.c`:

- Data `bitfield` is now always sent (ok_to_mqtt / want_response bits).
- `relay_node` is set on our packets; `next_hop`, `relay_node`, `via_mqtt` and
  `hop_start` are parsed; the channel hash is checked (`opaque` packets);
  malformed Data is flagged (`data_ok`).
- Encoders for Position, device Telemetry, NeighborInfo and User (with role).

Started, not wired in:

- `apps/meshtoad/source/airtime.c/.h`: a port of the airtime windows, the gates,
  time-on-air, slot time, contention windows and the retransmit timer. It
  compiles but isn't used yet.
- `apps/meshtoad/source/router.h`: the interface for a router port. There is
  no implementation.

Still missing in Meshtoad (the reason we moved to Part 2): relaying, the
periodic modules and their intervals, the airtime gates, the ACK rules above,
traffic management and hop scaling.

---

## Part 2 - running the real firmware on the Wii

Re-implementing Part 1 by hand covers a subset and drifts from upstream with
every release. The firmware's own **portduino** target (meshtasticd) already
supports a Meshtoad: `bin/config.d/lora-usb-meshtoad-e22.yaml`.

### The template: `native-wasm`

2.8.1 ships `[env:native-wasm]` (`variants/native/portduino/platformio.ini`,
`src/platform/portduino/wasm/`), which runs the whole firmware **single-threaded**
under Emscripten and drives a CH341 Meshtoad over WebUSB. It is the closest
existing thing to a Wii build:

- The radio goes through `Ch341Hal` (`platform/portduino/USBHal.h`) against a
  **12-function `libpinedio-usb` API** (`wasm/include/libpinedio-usb.h`). The wasm
  build implements it in `libpinedio_webusb.c`. A Wii build implements it on
  libogc USB, much like our `apps/meshtoad/source/ch341.c`.
- The entry points are `setup()` then `loop()` once per tick
  (`wasm/portduino_main_wasm.cpp`).
- The phone API runs in-process: `wasm_api_to_radio()` / `wasm_api_from_radio()`
  feed the firmware's own PhoneAPI ToRadio/FromRadio protobufs. A Wii UI (or a
  TCP :4403 bridge for the official apps) can sit on that.
- **The `ARCH_PORTDUINO_WASM` guards are all things the Wii wants too**:
  - cooperative sleep instead of a pthread semaphore (`main.cpp:1580`),
  - continuous RX instead of duty-cycled RX, which would stall a slow USB SPI
    link (`mesh/SX126xInterface.cpp:459`),
  - config from setters instead of YAML (`PortduinoGlue.cpp:396`),
  - no `popen` (`PortduinoGlue.cpp:1467`),
  - no in-process reboot (`Power.cpp:1042`),
  - no raspi HTTP server.

  The *only* emscripten call outside the wasm dir is `emscripten_sleep()` in
  `main.cpp`, so a Wii build can define `ARCH_PORTDUINO_WASM` and supply a
  stub `<emscripten.h>`. The one guard we may want to undo is the exclusion of
  the TCP `ServerAPI` (`mesh/InterfacesTemplates.cpp:11`): the official apps
  could connect to the Wii over Wi-Fi.
- The Arduino layer is `framework-portduino` (`platform-wasm` pins
  `214bf605`). `platform-wasm`'s `builder/frameworks/arduino.py` compiles a
  hand-picked subset: `PortduinoGPIO`, `PortduinoPrint`, `logging`, `Utility`,
  `itoa`, `dtostrf`, `FS/{FS,vfs_api,PortduinoFS}`,
  `linux/{millis,LinuxSerial,LinuxHardwareI2C,LinuxHardwareSPI}`, plus the
  ArduinoCore-API `Common/Stream/Print/String/IPAddress`. A `platform-wii`
  is the same builder with devkitPPC in place of emcc (the toolchain is
  `powerpc-eabi-*`; MACHDEP is `-DGEKKO -mrvl -mcpu=750 -meabi -mhard-float`;
  link libogc/libfat; then `elf2dol`).

### Big-endian (the Wii's Broadway CPU)

The firmware assumes a little-endian CPU in a few places. These are known so
far:

- `mesh/RadioLibInterface.cpp:681-686` reads `to/from/id` by overlaying
  `PacketHeader` on the radio buffer. TX builds the header the same way. Both
  need explicit LE loads and stores.
- `mesh/CryptoEngine.cpp` builds the AES-CTR nonce by `memcpy` of the packet
  id and `from`. That needs explicit LE writes.
- Still to audit: the rweather Crypto lib (Curve25519/SHA for PKI; it has
  endian helpers), RadioLib register access (byte-wise, probably fine) and
  anything else that `memcpy`s integers onto the wire. nanopb is endian-safe.

### Other Wii specifics we already learned the hard way

- **USB**: link the patched `ogc_usb.o`; wait up to ~5 s for the CH341 to
  enumerate. Send multi-packet SPI transfers as one bulk write. Reading packet
  by packet drops a byte at every 32-byte boundary.
- **Wi-Fi**: using the network breaks HBC's Wi-Fi until reopened. Keep
  networking opt-in. Debug logging goes over TCP to `DEBUG_HOST:4301`, and the
  watchdog thread pattern in `loratest.c` finds hangs.
- **Licence**: the firmware, `platform-wasm` and `framework-portduino` are
  GPL-3.0. The Wii port app has to be GPL-3.0 even though the rest of this
  repo is Unlicense.
