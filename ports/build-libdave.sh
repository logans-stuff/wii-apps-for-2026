#!/bin/sh
# Cross-build libdave v1.2.1 (C++) for the Wii, into ports/out.
set -e
OUT=/w/ports/out
cmake -S /w/third_party/libdave/cpp -B /w/ports/build-libdave \
  -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Wii.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$OUT \
  -DCMAKE_PREFIX_PATH=$OUT -DOPENSSL_ROOT_DIR=$OUT -DOPENSSL_USE_STATIC_LIBS=ON \
  -DTESTING=OFF -DPERSISTENT_KEYS=OFF -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_FIND_ROOT_PATH="$OUT;$DEVKITPRO/portlibs/ppc;$DEVKITPRO/libogc"
cmake --build /w/ports/build-libdave -j"$(nproc)"
cmake --install /w/ports/build-libdave
ls -la $OUT/lib | grep -i dave
