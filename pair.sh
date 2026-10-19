#!/bin/bash
# Send the login token to Sonoran for Wii (pairing mode, port 4300).
# Usage: ./pair.sh <wii-ip>
# The token does not show on the screen and is not saved on this computer.
set -eu
wii="${1:?usage: ./pair.sh <wii-ip>}"
read -rsp "Paste the access_token value, then push Enter: " token
echo
case "$token" in
    eyJ*) ;;
    *) echo "That is not a token. It must start with eyJ." >&2; exit 1 ;;
esac
exec 3<>"/dev/tcp/$wii/4300"
printf '%s\n' "$token" >&3
exec 3>&-
unset token
echo "Sent. Look at the Wii screen."
