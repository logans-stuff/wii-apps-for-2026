# Sonoran for Wii

A Wii homebrew app that shows Sonoran (s.azmsh.dev) chat and nodes.
It does TLS itself (wolfSSL), so the Wii's old SSL and old root
certificates do not matter. It reads the Sonoran JSON API as it is.
No change to Sonoran is necessary.

## Pinned inputs

| Input | Version |
|---|---|
| devkitPPC image | `devkitpro/devkitppc:20260503` |
| wolfSSL | `v5.9.4-stable` |
| cJSON | `v1.7.19` |
| QR-Code-generator (Nayuki, MIT) | `v1.8.0` |
| Root certificates | host `/etc/ssl/certs/ca-certificates.crt`, through `tools/mkcerts.py` |

## Build

    mkdir -p third_party
    git clone --depth 1 -b v5.9.4-stable https://github.com/wolfSSL/wolfssl.git third_party/wolfssl
    git clone --depth 1 -b v1.7.19 https://github.com/DaveGamble/cJSON.git third_party/cJSON
    git clone --depth 1 -b v1.8.0 https://github.com/nayuki/QR-Code-generator.git third_party/qrcodegen
    python3 -I tools/mkcerts.py
    ./build.sh                     # makes build/boot.dol

Linux test of the same TLS and API code:

    docker run --rm -v "$PWD":/w -w /w gcc:14 make -f Makefile.host
    ./build-host/hosttest "/api/chat?limit=3"    # SONORAN_COOKIE=authelia_session=... for a login

## Install

Copy `dist/apps/sonoran/` and `build/boot.dol` to `sd:/apps/sonoran/`.
Or send it with wiiload while the Homebrew Channel is open:

    WIILOAD=tcp:<wii-ip> wiiload build/boot.dol      # run once, no install

To install over the network, send a zip with the app folder at the top
(`sonoran/boot.dol`, `sonoran/meta.xml`, `sonoran/icon.png`). The
Homebrew Channel refuses a zip with `apps/sonoran/...` paths ("not a
usable zip file"). It extracts the good layout to `sd:/apps/sonoran/`.

## Login token

s.azmsh.dev is behind Authelia (401 to `auth.azmsh.dev/?rd=...`). The app
sends the `authelia_session` cookie. Button 1 logs in through the existing
Authelia API (`POST https://auth.azmsh.dev/api/firstfactor`) with an
on-screen keyboard, and saves the cookie to `sd:/apps/sonoran/token.txt`.
Button 2 is pairing: it gets the cookie over the LAN (port 4300, `pair.sh`).
No server change is necessary. See `dist/apps/sonoran/README.txt`.

## Known limits

- The random seed comes from timer jitter. The Wii has no random device
  that the app can read.
- The Wii clock must be correct for the certificate date check.
- Text out of ASCII (for example emoji) shows as `?`.

## Discord screen

Discord QR login (remote auth gateway v2), REST API v9 with the user
token. Only requests that the user starts. Discord does not allow
unofficial clients on user accounts; the user accepts that risk.
Test the QR flow up to the code: `./build-host/hosttest --qr`.
