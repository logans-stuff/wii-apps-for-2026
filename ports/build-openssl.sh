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
# Wii: with --with-rand-seed=none OpenSSL 3 has no entropy source at all.
# Take entropy from the app (sonoran_seed in source/net_wii.c: timer
# jitter hashed with SHA-256). Credit only 2 bits for each byte.
python3 - <<'PY'
p = "providers/implementations/rands/seeding/rand_unix.c"
s = open(p).read()
old = """#if defined(OPENSSL_RAND_SEED_NONE)
    return ossl_rand_pool_entropy_available(pool);
#else"""
new = """#if defined(OPENSSL_RAND_SEED_NONE)
    {
        extern int sonoran_seed(unsigned char *out, unsigned int sz);
        size_t bytes_needed = ossl_rand_pool_bytes_needed(pool, 4);
        unsigned char *buffer;

        if (bytes_needed > 0) {
            buffer = ossl_rand_pool_add_begin(pool, bytes_needed);
            if (buffer != NULL && sonoran_seed(buffer, (unsigned int)bytes_needed) == 0)
                ossl_rand_pool_add_end(pool, bytes_needed, 2 * bytes_needed);
        }
    }
    return ossl_rand_pool_entropy_available(pool);
#else"""
assert old in s
open(p, "w").write(s.replace(old, new, 1))
PY
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
