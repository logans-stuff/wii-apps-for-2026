#!/bin/sh
# Cross-build OpenSSL 3.0.22 (static, no asm, no threads, no sockets) for
# the Wii, into ports/out. Run inside devkitpro/devkitppc:20260503.
set -e
SRC=/w/third_party/openssl
OUT=/w/ports/out
BUILD=/w/ports/build-openssl
rm -rf "$BUILD"; mkdir -p "$BUILD" "$OUT"
cp -a "$SRC/." "$BUILD/"
cd "$BUILD"
export PATH=$DEVKITPPC/bin:$PATH
./Configure linux-generic32 --cross-compile-prefix=powerpc-eabi- \
  --prefix="$OUT" --openssldir="$OUT/ssl" \
  no-asm no-threads no-shared no-dso no-sock no-dgram no-tests \
  no-ui-console no-async no-engine no-afalgeng no-secure-memory no-filenames \
  no-deprecated no-legacy no-ktls no-cmp no-ocsp no-ts no-srp no-ct \
  --with-rand-seed=none \
  -DB_ENDIAN -DOPENSSL_NO_POSIX_IO -DNO_SYSLOG -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 \
  -I$DEVKITPRO/libogc/include
make -j"$(nproc)" build_libs
make install_dev
ls -la "$OUT/lib"
