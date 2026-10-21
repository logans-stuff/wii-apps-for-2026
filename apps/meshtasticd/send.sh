#!/bin/sh
# Send the built firmware to a Wii sitting in the Homebrew Channel:
#   apps/meshtasticd/send.sh <wii-ip> [key=value ...]
# Goes through tools/wiiload_relay.py (536-byte TCP segments): the Wii's Wi-Fi
# drops plain wiiload uploads of this size. Extra key=value arguments seed
# config.txt on first boot.
set -e
cd "$(dirname "$0")/../.."
WII=${1:?usage: $0 <wii-ip> [key=value ...]}
shift
python3 tools/wiiload_relay.py "$WII" &
RELAY=$!
sleep 1
docker run --rm --network host -e WIILOAD=tcp:127.0.0.1 -v "$PWD":/w -w /w devkitpro/devkitppc:20260503 \
    /opt/devkitpro/tools/bin/wiiload apps/meshtasticd/build/meshtasticd.dol "$@"
wait "$RELAY" 2>/dev/null || true
