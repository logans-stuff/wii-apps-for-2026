# Copyright 2014-present PlatformIO <contact@platformio.org>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Arduino (Wii / libogc variant, copied from platform-wasm)

Same portduino Arduino core as platform-native, but compiled to WebAssembly.
The difference is the core source set: the host (Linux/macOS) native build can
compile the *whole* `cores/` tree, but several files don't belong in a wasm
image — the core's own `main.cpp` (the firmware provides its own entry point via
src/platform/portduino/wasm/), `linux/LinuxCommon.cpp` (libusb / bluetooth /
process spawning, whose symbols the wasm glue stubs out), `AsyncUDP.cpp`,
`linux/gpio/` (libgpiod) and `simulated/`. So instead of `+<*>` we hand-pick the
emcc-safe subset. The rest mirrors platform-native.
"""

import os

from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()
platform = env.PioPlatform()
board = env.BoardConfig()

FRAMEWORK_DIR = platform.get_package_dir("framework-portduino")
assert os.path.isdir(FRAMEWORK_DIR)

env.Append(
    CPPDEFINES=[
        ("ARDUINO", 4403),
        "PORTDUINO",
    ],
    CCFLAGS=[
        "-w",
        # The ArduinoCore-API headers (`ArduinoAPI.h`, `String.h`, ...) must be
        # reachable, but on a case-insensitive filesystem (macOS APFS) putting
        # their dir on a normal `-I` makes Arduino's `String.h` satisfy
        # `#include <string.h>`, which breaks libc++'s `<cstring>` (unresolved
        # `using ::memmove`). `-idirafter` adds the dir as a *last resort*: real
        # `<string.h>` still wins, but quoted Arduino includes resolve. (The
        # native platform instead routes <string.h> through a generated shim;
        # this is the same trick the standalone emcc build used, and it's
        # portable across Linux/macOS.)
        "-idirafter",
        os.path.join(FRAMEWORK_DIR, "ArduinoCore-API", "api"),
    ],
    CPPPATH=[
        os.path.join(FRAMEWORK_DIR, "cores", "portduino"),
        os.path.join(FRAMEWORK_DIR, "cores", "portduino", "FS"),
        os.path.join(FRAMEWORK_DIR, "ArduinoCore-API"),
    ],
    LIBSOURCE_DIRS=[
        os.path.join(FRAMEWORK_DIR, "libraries"),
    ],
)

#
# Target: Build Core Library
#

libs = []

# Variant (the firmware also adds -I variants/native/portduino itself; this only
# builds variant *sources* if the bundled variant dir happens to have any).
if "build.variant" in env.BoardConfig():
    variants_dir = (
        os.path.join("$PROJECT_DIR", board.get("build.variants_dir"))
        if board.get("build.variants_dir", "")
        else os.path.join(FRAMEWORK_DIR, "variants")
    )
    variant_path = os.path.join(variants_dir, board.get("build.variant"))
    env.Append(CPPPATH=[variant_path])
    resolved_variant = env.subst(variant_path)
    has_variant_sources = False
    if os.path.isdir(resolved_variant):
        for _, _, files in os.walk(resolved_variant):
            if any(
                f.endswith((".c", ".cpp", ".cc", ".cxx", ".S", ".s")) for f in files
            ):
                has_variant_sources = True
                break
    if has_variant_sources:
        libs.append(
            env.BuildLibrary(
                os.path.join("$BUILD_DIR", "FrameworkArduinoVariant"),
                variant_path,
            )
        )

# The Arduino Wiring API layer (ArduinoCore-API/api/*.cpp). Built from the real
# directory rather than via the `cores/arduino/api` symlink so SCons globbing is
# unambiguous. Only the pieces the firmware actually links.
libs.append(
    env.BuildLibrary(
        os.path.join("$BUILD_DIR", "FrameworkArduinoAPI"),
        os.path.join(FRAMEWORK_DIR, "ArduinoCore-API", "api"),
        src_filter=[
            "-<*>",
            "+<Common.cpp>",
            "+<Stream.cpp>",
            "+<Print.cpp>",
            "+<String.cpp>",
            "+<IPAddress.cpp>",
        ],
    )
)

# The portduino core — emcc-safe subset only (see module docstring for what's
# excluded and why). LinuxHardwareI2C/SPI + LinuxSerial compile under emcc and
# provide the Wire/SPI/Serial globals; the FS layer runs against MEMFS/IDBFS.
libs.append(
    env.BuildLibrary(
        os.path.join("$BUILD_DIR", "FrameworkArduino"),
        os.path.join(FRAMEWORK_DIR, "cores"),
        src_filter=[
            "-<*>",
            "+<portduino/PortduinoGPIO.cpp>",
            "+<portduino/PortduinoPrint.cpp>",
            "+<portduino/logging.cpp>",
            "+<portduino/Utility.cpp>",
            "+<portduino/itoa.cpp>",
            "+<portduino/dtostrf.c>",
            "+<portduino/FS/FS.cpp>",
            "+<portduino/FS/vfs_api.cpp>",
            "+<portduino/FS/PortduinoFS.cpp>",
            "+<portduino/linux/millis.cpp>",
            "+<portduino/linux/LinuxSerial.cpp>",
            "+<portduino/linux/LinuxHardwareI2C.cpp>",
            "+<portduino/linux/LinuxHardwareSPI.cpp>",
        ],
    )
)

env.Prepend(LIBS=libs)
