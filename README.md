# Sonoran for Wii

A Wii homebrew app that shows view.azmsh.net chat and nodes.
It does TLS itself (wolfSSL), so the Wii's old SSL and old root
certificates do not matter. It reads the Sonoran JSON API as it is.
No change to Sonoran is necessary.

## Pinned inputs

| Input | Version |
|---|---|
| devkitPPC image | `devkitpro/devkitppc:20260503` |
| wolfSSL | `v5.9.4-stable` |
| cJSON | `v1.7.19` |
| Root certificates | host `/etc/ssl/certs/ca-certificates.crt`, through `tools/mkcerts.py` |

## Build

    mkdir -p third_party
    git clone --depth 1 -b v5.9.4-stable https://github.com/wolfSSL/wolfssl.git third_party/wolfssl
    git clone --depth 1 -b v1.7.19 https://github.com/DaveGamble/cJSON.git third_party/cJSON
    python3 -I tools/mkcerts.py
    ./build.sh                     # makes build/boot.dol

Linux test of the same TLS and API code:

    docker run --rm -v "$PWD":/w -w /w gcc:14 make -f Makefile.host
    ./build-host/hosttest "/api/chat?limit=3"    # SONORAN_TOKEN=... for a login

## Install

Copy `dist/apps/sonoran/` and `build/boot.dol` to `sd:/apps/sonoran/`.
Or send it with wiiload while the Homebrew Channel is open:

    WIILOAD=tcp:<wii-ip> wiiload build/boot.dol

## Login token

The login proxy (AuthCrunch in front of Keycloak) accepts its own JWT in an
`Authorization: Bearer` header. The app reads that JWT from
`sd:/apps/sonoran/token.txt`. See `dist/apps/sonoran/README.txt`.

## Known limits

- The random seed comes from timer jitter. The Wii has no random device
  that the app can read.
- The Wii clock must be correct for the certificate date check.
- Text out of ASCII (for example emoji) shows as `?`.
