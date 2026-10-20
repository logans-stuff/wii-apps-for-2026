#!/bin/sh
# Build one app in the pinned devkitPPC container, e.g.:
#   ./build.sh apps/meshtoad      -> apps/meshtoad/build/meshtoad.dol
#   ./build.sh apps/usb-headset
#   ./build.sh apps/sonoran
set -e
cd "$(dirname "$0")"
dir="$1"; shift
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/w -w /w \
    devkitpro/devkitppc:20260503 make -C "$dir" -j"$(nproc)" "$@"
