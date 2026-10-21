#!/bin/sh
# Install meshtasticd on the Wii's SD card so it launches from the HBC menu:
#   apps/meshtasticd/install.sh <wii-ip>
# Builds a release firmware (no debug log stream, whatever WII_DEBUG_HOST
# says), stages it as dist/apps/meshtasticd/boot.dol next to meta.xml and
# icon.png, builds tools/meshtasticd-installer around those three files and
# sends it through the wiiload relay. The installer leaves config.txt, prefs/
# and maps/ on the card alone.
set -e
cd "$(dirname "$0")/../.."
WII=${1:?usage: $0 <wii-ip>}
WII_DEBUG_HOST= apps/meshtasticd/build.sh
if grep -aq "debuglog: streaming" apps/meshtasticd/build/meshtasticd.dol; then
    # PlatformIO kept objects from a WII_DEBUG_HOST build: rebuild from clean
    rm -rf third_party/meshtastic-firmware/.pio/build/native-wii
    WII_DEBUG_HOST= apps/meshtasticd/build.sh
fi
if grep -aq "debuglog: streaming" apps/meshtasticd/build/meshtasticd.dol; then
    echo "release build still has the debug log stream" >&2
    exit 1
fi
cp apps/meshtasticd/build/meshtasticd.dol dist/apps/meshtasticd/boot.dol
./build.sh tools/meshtasticd-installer
DOL=tools/meshtasticd-installer/build/meshtasticd-installer.dol apps/meshtasticd/send.sh "$WII"
