# Wii build. Run it in the devkitPPC container (see build.sh).
include wolfssl.mk
include $(DEVKITPPC)/base_tools

MACHDEP := -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float
CFLAGS  := -O2 -Wall $(MACHDEP) $(WOLF_CFLAGS) -I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include
LDFLAGS := $(MACHDEP) -Wl,-Map,build/sonoran.map -L$(DEVKITPRO)/libogc/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib
LIBS    := -lpng16 -lz -lfat -lwiiuse -lbte -logc -lm

APP_SRC := source/main_wii.c source/map.c source/https.c source/api.c source/net_wii.c
SRC := $(APP_SRC) $(CJSON_SRC) $(WOLF_SRC)
OBJ := $(patsubst %.c,build/obj/%.o,$(SRC))

build/boot.dol: build/sonoran.elf
	elf2dol $< $@

build/sonoran.elf: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

build/obj/%.o: %.c include/user_settings.h source/cacerts.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build
.PHONY: clean
