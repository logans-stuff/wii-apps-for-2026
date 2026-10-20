# Shared build for every Wii app in this repo. An app's Makefile sets ROOT,
# APP, APP_SRC (paths under its own source/), APP_LIBS, and optional flags
# USE_NET / USE_OGC_USB / USE_WOLFSSL / USE_CJSON and APP_CFLAGS, then:
#   ROOT := ../..
#   include $(ROOT)/common.mk
# Build it in the pinned container from the repo root, e.g.:
#   ./build.sh apps/meshtoad
include $(DEVKITPPC)/base_tools

TP      := $(ROOT)/third_party
MACHDEP := -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float
CFLAGS  := -O2 -Wall $(MACHDEP) -I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include -I$(ROOT)/common $(APP_CFLAGS)
LDFLAGS := $(MACHDEP) -L$(DEVKITPRO)/libogc/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib

OBJ := $(patsubst source/%.c,build/%.o,$(APP_SRC))

ifeq ($(USE_WOLFSSL),1)
  CFLAGS += -DWOLFSSL_USER_SETTINGS -I$(TP)/wolfssl -I$(TP)/cJSON
  WOLF_SRC := $(addprefix $(TP)/wolfssl/src/, internal.c keys.c ssl.c tls.c tls13.c wolfio.c) \
    $(addprefix $(TP)/wolfssl/wolfcrypt/src/, aes.c asn.c coding.c chacha.c chacha20_poly1305.c \
      curve25519.c ecc.c error.c fe_operations.c hash.c hmac.c kdf.c logging.c memory.c poly1305.c \
      random.c rsa.c sha.c sha256.c sha512.c signature.c sp_int.c sp_c32.c sp_c64.c wc_encrypt.c \
      wc_port.c wolfmath.c md5.c)
  WOLF_OBJ := $(addprefix build/wolf/,$(notdir $(WOLF_SRC:.c=.o)))
  OBJ += $(WOLF_OBJ)
endif
ifeq ($(USE_CJSON),1)
  OBJ += build/cJSON.o
endif
ifeq ($(USE_QRGEN),1)
  CFLAGS += -I$(TP)/qrcodegen/c
  OBJ += build/qrcodegen.o
endif
ifeq ($(USE_NET),1)
  OBJ += build/net_wii.o
endif
ifeq ($(USE_OSK),1)
  OBJ += build/osk.o
endif
ifeq ($(USE_OGC_USB),1)
  OBJ += build/ogc_usb.o
endif

build/$(APP).dol: build/$(APP).elf
	elf2dol $< $@

build/$(APP).elf: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(APP_LIBS)

build/%.o: source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Shared sources live in ../../common; compile them into this app's build dir.
build/net_wii.o: $(ROOT)/common/net_wii.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/osk.o: $(ROOT)/common/osk.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/cJSON.o: $(TP)/cJSON/cJSON.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/qrcodegen.o: $(TP)/qrcodegen/c/qrcodegen.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# ogc_usb.c is libogc's usb.c plus one function; it needs libogc's own headers.
build/ogc_usb.o: $(ROOT)/common/ogc_usb.c
	@mkdir -p build
	$(CC) -O2 $(MACHDEP) -DHW_RVL -I$(TP)/libogc/gc -I$(TP)/libogc/libogc \
	  -I$(TP)/libogc/gc/ogc -I$(TP)/libogc/gc/ogc/machine -c -o $@ $<

build/wolf/%.o: $(TP)/wolfssl/src/%.c
	@mkdir -p build/wolf
	$(CC) $(CFLAGS) -c -o $@ $<
build/wolf/%.o: $(TP)/wolfssl/wolfcrypt/src/%.c
	@mkdir -p build/wolf
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build
.PHONY: clean all
all: build/$(APP).dol
