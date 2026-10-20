"""
Builder for the Meshtastic Wii platform (devkitPPC / libogc).

Mirrors platform-wasm's main.py: let env.Tool("gcc")/("g++") install the
standard SCons command templates, then swap in the powerpc-eabi binaries. The
program links to <progname>.elf and a post action converts it to a .dol the
Homebrew Channel can run.
"""

import os

from SCons.Script import AlwaysBuild, COMMAND_LINE_TARGETS, Default, DefaultEnvironment

env = DefaultEnvironment()

DEVKITPRO = os.environ.get("DEVKITPRO", "/opt/devkitpro")
BIN = os.path.join(DEVKITPRO, "devkitPPC", "bin")
OGC = os.path.join(DEVKITPRO, "libogc")
PORTLIBS = os.path.join(DEVKITPRO, "portlibs", "ppc")


def _tool(name):
    return os.path.join(BIN, "powerpc-eabi-" + name)


for k in ("CC", "CXX"):
    if k in env:
        del env[k]

backup_cflags = env.get("CFLAGS", [])
backup_cxxflags = env.get("CXXFLAGS", [])
env.Tool("gcc")
env.Tool("g++")
if "compiledb" in COMMAND_LINE_TARGETS:
    env.Tool("compilation_db")
env.Append(CFLAGS=backup_cflags, CXXFLAGS=backup_cxxflags)

env.Replace(
    AR=_tool("ar"),
    AS=_tool("gcc"),
    CC=_tool("gcc"),
    CXX=_tool("g++"),
    LINK=_tool("g++"),
    RANLIB=_tool("ranlib"),
    OBJCOPY=_tool("objcopy"),
    PROGSUFFIX=".elf",
    PROGNAME="meshtasticd",
)

MACHDEP = ["-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float"]
env.Append(
    CCFLAGS=MACHDEP + ["-O2", "-ffunction-sections", "-fdata-sections"],
    CPPPATH=[os.path.join(OGC, "include"), os.path.join(PORTLIBS, "include")],
    LINKFLAGS=MACHDEP + ["-Wl,--gc-sections"],
    LIBPATH=[os.path.join(OGC, "lib", "wii"), os.path.join(PORTLIBS, "lib")],
)
# libogc must come after everything that uses it.
env.Append(LIBS=["fat", "wiiuse", "bte", "ogc", "m"])

target_elf = env.BuildProgram()
target_dol = env.Command(
    os.path.join("$BUILD_DIR", "meshtasticd.dol"),
    target_elf,
    env.VerboseAction(os.path.join(DEVKITPRO, "tools", "bin", "elf2dol") + " $SOURCE $TARGET",
                      "Converting $SOURCE to .dol"),
)
AlwaysBuild(env.Alias("dol", target_dol))
Default([target_elf, target_dol])
