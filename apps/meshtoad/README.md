# Meshtoad — a Meshtastic node on the Wii

Turns a Nintendo Wii into a two-way [Meshtastic](https://meshtastic.org)
node using a USB "Meshtoad": an **SX1262** LoRa radio driven over a
**CH341** USB-to-SPI bridge. The Wii decrypts the channel (AES-CTR), decodes
packets, keeps a database of every node it hears, and can transmit — it
joins the mesh like any other node.

## Features

- **Receive & decode**: text messages, node info (names), position, telemetry.
- **NodeDB**: a live table of every node heard — name, id, hops, SNR, last
  heard, and position/battery when known. Scroll/select with the D-Pad.
- **Transmit**: announce this node, send a typed message (on-screen keyboard)
  or a quick chat.
- **Configurable** from an SD card — no rebuild needed (see below).

## Controls

| Button | Action |
|---|---|
| D-Pad up/down | scroll the list / move the node selection |
| 1 | toggle Nodes / Messages view |
| A | type a message (on-screen keyboard) and send |
| 2 | send a quick chat |
| B | announce this node |
| Home | exit |

## Config

Copy [`config.example.txt`](config.example.txt) to
`sd:/apps/meshtoad/config.txt` and edit it to set the node name, the channel
(preset + frequency slot, or explicit `freq`/`sf`/`bw`), the channel hash,
and a custom channel key (PSK). Without a config file it runs on US
MediumFast, slot 18, with the public default key.

## Build

    ./build.sh apps/meshtoad      # -> apps/meshtoad/build/meshtoad.dol

Needs `third_party/{wolfssl,libogc}` cloned (see the repo README). Install the
`.dol` to `sd:/apps/meshtoad/boot.dol`, or send it with `wiiload`.

## Hardware

Tested with a USB Meshtoad (CH341 `1a86:5512` + SX1262, E22 module), wired as
the Meshtastic `lora-usb-meshtoad-e22` config: CS D0, RXen D1, Reset D2,
Busy D4, IRQ D6, with DIO2 as the RF switch and DIO3 driving the TCXO.
