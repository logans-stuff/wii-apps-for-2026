# Wii build. Run it in the devkitPPC container (see build.sh).
include wolfssl.mk
include $(DEVKITPPC)/base_tools

MACHDEP := -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float
CFLAGS  := -O2 -Wall $(MACHDEP) $(WOLF_CFLAGS) -I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include -I$(DEVKITPRO)/portlibs/ppc/include/opus -Ithird_party/qrcodegen/c
# Optional, gitignored local overrides (e.g. CFLAGS += -DDEBUG_HOST='"127.0.0.1"').
-include local.mk
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

# DAVE link test: make build/davetest.dol (needs ports/out from ports/build-*.sh)
PORTS := ports/out
DAVE_LIBS := -L$(PORTS)/lib -ldave -lmlspp -lmls_ds -lhpke -ltls_syntax -lbytes -lcrypto -lstdc++
build/obj/source/davetest.o: source/davetest.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(PORTS)/include -c -o $@ $<
DAVETEST_OBJ := build/obj/source/davetest.o build/obj/source/posix_stubs.o build/obj/source/net_wii.o \
  $(filter build/obj/third_party/wolfssl/%,$(OBJ))
build/davetest.elf: $(DAVETEST_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $(DAVETEST_OBJ) $(DAVE_LIBS) $(LIBS)
build/davetest.dol: build/davetest.elf
	elf2dol $< $@

# Headset test: make build/headset.dol
HEADSET_OBJ := build/obj/source/headset.o build/obj/source/usbaudio.o build/obj/source/ogc_usb.o build/obj/source/net_wii.o \
  $(filter build/obj/third_party/wolfssl/%,$(OBJ))
build/headset.elf: $(HEADSET_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(HEADSET_OBJ) -lopus $(LIBS)
build/headset.dol: build/headset.elf
	elf2dol $< $@

# LoRa test (Meshtoad: SX1262 over a CH341 USB-SPI bridge): make build/lora.dol
# ch341.c must use our patched USBV5 stack (ogc_usb.c), like the headset driver;
# libogc's stock USB bulk read returns zeroed data on this console.
LORA_OBJ := build/obj/source/loratest.o build/obj/source/sx1262.o build/obj/source/ch341.o \
  build/obj/source/meshtastic.o build/obj/source/ogc_usb.o build/obj/source/net_wii.o \
  $(filter build/obj/third_party/wolfssl/%,$(OBJ))
build/lora.elf: $(LORA_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(LORA_OBJ) $(LIBS)
build/lora.dol: build/lora.elf
	elf2dol $< $@

build/sonoran.elf: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

# The headset driver logs to the Sonoran debug log in this repo.
build/obj/source/usbaudio.o: source/usbaudio.c source/usbaudio.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DSONORAN_DEBUG -c -o $@ $<

# Our copy of libogc's usb.c needs libogc's own headers.
LIBOGC_SRC := third_party/libogc
build/obj/source/ogc_usb.o: source/ogc_usb.c
	@mkdir -p $(dir $@)
	$(CC) -O2 $(MACHDEP) -DHW_RVL -I$(LIBOGC_SRC)/gc -I$(LIBOGC_SRC)/libogc -I$(LIBOGC_SRC)/gc/ogc -I$(LIBOGC_SRC)/gc/ogc/machine -c -o $@ $<

build/obj/%.o: %.c include/user_settings.h source/cacerts.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build
.PHONY: clean
