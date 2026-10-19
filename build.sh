#!/bin/sh
# Build boot.dol in the pinned devkitPPC container.
set -e
cd "$(dirname "$0")"
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/w -w /w \
    devkitpro/devkitppc:20260503 make -j"$(nproc)" "$@"
