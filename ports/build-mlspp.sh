#!/bin/sh
# Cross-build mlspp (libdave's pinned commit) for the Wii, into ports/out.
set -e
OUT=/w/ports/out
# nlohmann-json: headers only, installed with its CMake config
cmake -S /w/third_party/json -B /w/ports/build-json -DJSON_BuildTests=OFF -DJSON_Install=ON \
  -DCMAKE_INSTALL_PREFIX=$OUT >/dev/null
cmake --install /w/ports/build-json >/dev/null
cmake -S /w/third_party/mlspp -B /w/ports/build-mlspp \
  -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Wii.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$OUT \
  -DCMAKE_PREFIX_PATH=$OUT -DOPENSSL_ROOT_DIR=$OUT -DOPENSSL_USE_STATIC_LIBS=ON \
  -DTESTING=OFF -DDISABLE_GREASE=ON -DMLS_CXX_NAMESPACE=mlspp \
  -DCMAKE_FIND_ROOT_PATH="$OUT;$DEVKITPRO/portlibs/ppc;$DEVKITPRO/libogc"
cmake --build /w/ports/build-mlspp -j"$(nproc)"
cmake --install /w/ports/build-mlspp
ls -la $OUT/lib
