#!/bin/bash
# Log in Sonoran for Wii from this computer.
# On the Wii, push 2 first. Then run: ./pair.sh <wii-ip>
# The password does not show and is not saved on this computer.
# ./pair.sh <wii-ip> --cookie   sends an authelia_session value instead.
set -eu
wii="${1:?usage: ./pair.sh <wii-ip> [--cookie]}"
if [ "${2:-}" = "--cookie" ]; then
    read -rsp "Paste the authelia_session value, then push Enter: " value
    echo
    payload=$(printf 'COOKIE\n%s\n' "$value")
    unset value
else
    read -rp "Username: " user
    read -rsp "Password: " pass
    echo
    payload=$(printf 'LOGIN\n%s\n%s\n' "$user" "$pass")
    unset pass
fi
exec 3<>"/dev/tcp/$wii/4300"
printf '%s\n' "$payload" >&3
exec 3>&-
unset payload
echo "Sent. Look at the Wii screen."
