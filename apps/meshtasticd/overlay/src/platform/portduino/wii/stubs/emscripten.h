// Wii build: the firmware's ARCH_PORTDUINO_WASM paths (single-threaded loop,
// continuous RX, no YAML) are exactly what the Wii needs, and the only
// emscripten call they make is emscripten_sleep(). Provide that on libogc.
#pragma once
#include <unistd.h>

static inline void emscripten_sleep(unsigned int ms)
{
    if (ms)
        usleep((useconds_t)ms * 1000);
}
