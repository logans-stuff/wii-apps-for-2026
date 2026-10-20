# USB headset driver for the Wii, with an example. Needs devkitPPC + libogc.
#   make            -> headset.dol
# Or in Docker:
#   docker run --rm -v "$PWD":/w -w /w devkitpro/devkitppc:20260503 make
include $(DEVKITPPC)/base_tools

LIBOGC_SRC ?= libogc
MACHDEP := -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float
CFLAGS  := -O2 -Wall $(MACHDEP) -I$(DEVKITPRO)/libogc/include
LDFLAGS := $(MACHDEP) -L$(DEVKITPRO)/libogc/lib/wii
LIBS    := -lwiiuse -lbte -logc -lm

headset.dol: headset.elf
	elf2dol $< $@

headset.elf: build/main.o build/usbaudio.o build/ogc_usb.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

build/%.o: source/%.c source/usbaudio.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# ogc_usb.c is libogc v3.1.0 usb.c plus one function; it needs the libogc
# source headers: git clone --depth 1 -b v3.1.0 https://github.com/devkitPro/libogc.git
build/ogc_usb.o: source/ogc_usb.c
	@mkdir -p build
	@test -d $(LIBOGC_SRC)/gc || { echo "Clone libogc v3.1.0 into ./libogc first (see README)"; exit 1; }
	$(CC) -O2 $(MACHDEP) -DHW_RVL -I$(LIBOGC_SRC)/gc -I$(LIBOGC_SRC)/libogc -I$(LIBOGC_SRC)/gc/ogc \
	  -I$(LIBOGC_SRC)/gc/ogc/machine -c -o $@ $<

clean:
	rm -rf build headset.elf headset.dol
.PHONY: clean
