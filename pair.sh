#!/bin/bash
# Send the Authelia session cookie to Sonoran for Wii (pairing mode, port 4300).
# Usage: ./pair.sh <wii-ip>
# The token does not show on the screen and is not saved on this computer.
set -eu
wii="${1:?usage: ./pair.sh <wii-ip>}"
read -rsp "Paste the authelia_session value, then push Enter: " token
echo
if [ "${#token}" -lt 16 ]; then
    echo "That value is too short for a session cookie." >&2
    exit 1
fi
exec 3<>"/dev/tcp/$wii/4300"
printf '%s\n' "$token" >&3
exec 3>&-
unset token
echo "Sent. Look at the Wii screen."
