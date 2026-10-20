#!/bin/sh
# Build the Meshtastic firmware for the Wii:  apps/meshtasticd/build.sh
#   1. fetch meshtastic/firmware at FW_REF into third_party/meshtastic-firmware
#   2. reset it, apply patches/*.patch, copy overlay/ on top
#   3. build the patched libogc usb.c (common/ogc_usb.c)
#   4. pio run -e native-wii in the devkitPPC + PlatformIO container
# Output: apps/meshtasticd/build/meshtasticd.dol
# WII_DEBUG_HOST=<ip> also streams the firmware log to <ip>:4302 over Wi-Fi.
set -e
cd "$(dirname "$0")/../.."
ROOT=$PWD
APP=apps/meshtasticd
FW=third_party/meshtastic-firmware
FW_REF=v2.8.1.8e6a88d
IMAGE=meshtasticd-wii-build:2
GIT="env GIT_CONFIG_GLOBAL=/dev/null git"

if [ ! -d "$FW/.git" ]; then
    $GIT clone --depth 1 --branch "$FW_REF" https://github.com/meshtastic/firmware "$FW"
    $GIT -C "$FW" submodule update --init --depth 1 protobufs
fi
$GIT -C "$FW" checkout -q -- .
$GIT -C "$FW" clean -qfd -e .pio
for p in "$APP"/patches/*.patch; do
    [ -e "$p" ] && $GIT -C "$FW" apply --whitespace=nowarn "$ROOT/$p"
done
cp -r "$APP"/overlay/. "$FW"/

docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" "$APP"/docker

mkdir -p "$APP"/build
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT":/w -w /w devkitpro/devkitppc:20260503 \
    /opt/devkitpro/devkitPPC/bin/powerpc-eabi-gcc -O2 -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -DHW_RVL \
    -Ithird_party/libogc/gc -Ithird_party/libogc/libogc -Ithird_party/libogc/gc/ogc \
    -Ithird_party/libogc/gc/ogc/machine -c -o "$APP"/build/ogc_usb.o common/ogc_usb.c

docker volume create meshtasticd-wii-pio >/dev/null
EXTRA_FLAGS="$WII_EXTRA_FLAGS" # e.g. "-DRADIOLIB_DEBUG_BASIC=1 -DRADIOLIB_DEBUG_SPI=1"
[ -n "$WII_DEBUG_HOST" ] && EXTRA_FLAGS="$EXTRA_FLAGS -DWII_DEBUG_HOST=\\\"$WII_DEBUG_HOST\\\""
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/pio -e PLATFORMIO_CORE_DIR=/pio/.platformio \
    -e PLATFORMIO_BUILD_FLAGS="$EXTRA_FLAGS" \
    -v meshtasticd-wii-pio:/pio -v "$ROOT":/w -w /w/"$FW" "$IMAGE" pio run -e native-wii "$@"

cp "$FW"/.pio/build/native-wii/meshtasticd.dol "$APP"/build/meshtasticd.dol
echo "built $APP/build/meshtasticd.dol"
