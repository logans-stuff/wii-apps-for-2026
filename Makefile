# Wii build. Run it in the devkitPPC container (see build.sh).
include wolfssl.mk
include $(DEVKITPPC)/base_tools

MACHDEP := -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float
CFLAGS  := -O2 -Wall $(MACHDEP) $(WOLF_CFLAGS) -I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include -Ithird_party/qrcodegen/c
LDFLAGS := $(MACHDEP) -Wl,-Map,build/sonoran.map -L$(DEVKITPRO)/libogc/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib
LIBS    := -lpng16 -lz -lfat -lwiiuse -lbte -logc -lm

APP_SRC := source/main_wii.c source/map.c source/https.c source/api.c source/discord.c source/dc_qr.c source/ws.c source/net_wii.c third_party/qrcodegen/c/qrcodegen.c
SRC := $(APP_SRC) $(CJSON_SRC) $(WOLF_SRC)
OBJ := $(patsubst %.c,build/obj/%.o,$(SRC))

build/boot.dol: build/sonoran.elf
	elf2dol $< $@

# USB probe for the headset driver: make build/probe.dol
PROBE_OBJ := build/obj/source/usbprobe.o build/obj/source/net_wii.o \
  $(filter build/obj/third_party/wolfssl/%,$(OBJ))
build/probe.elf: $(PROBE_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(PROBE_OBJ) $(LIBS)
build/probe.dol: build/probe.elf
	elf2dol $< $@

# Headset test: make build/headset.dol
HEADSET_OBJ := build/obj/source/headset.o build/obj/source/usbaudio.o build/obj/source/net_wii.o \
  $(filter build/obj/third_party/wolfssl/%,$(OBJ))
build/headset.elf: $(HEADSET_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(HEADSET_OBJ) $(LIBS)
build/headset.dol: build/headset.elf
	elf2dol $< $@

build/sonoran.elf: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

build/obj/%.o: %.c include/user_settings.h source/cacerts.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build
.PHONY: clean
