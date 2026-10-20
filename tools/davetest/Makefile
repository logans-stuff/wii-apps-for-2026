# DAVE (Discord end-to-end voice) link test -> build/davetest.dol
# Needs the ports built first: run ../../ports/build-*.sh to produce ports/out.
ROOT := ../..
include $(DEVKITPPC)/base_tools

TP      := $(ROOT)/third_party
PORTS   := $(ROOT)/ports/out
MACHDEP := -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float
CFLAGS  := -O2 -Wall $(MACHDEP) -DWOLFSSL_USER_SETTINGS -I$(ROOT)/common -I$(TP)/wolfssl \
  -I$(DEVKITPRO)/libogc/include -I$(PORTS)/include
LDFLAGS := $(MACHDEP) -L$(DEVKITPRO)/libogc/lib/wii -L$(PORTS)/lib
DAVE_LIBS := -ldave -lmlspp -lmls_ds -lhpke -ltls_syntax -lbytes -lcrypto -lstdc++
LIBS := -lfat -lwiiuse -lbte -logc -lm

WOLF_SRC := $(addprefix $(TP)/wolfssl/wolfcrypt/src/, aes.c hash.c sha.c sha256.c sha512.c \
  random.c memory.c wc_port.c error.c logging.c coding.c)
OBJ := build/davetest.o build/posix_stubs.o build/net_wii.o $(addprefix build/wolf/,$(notdir $(WOLF_SRC:.c=.o)))

build/davetest.dol: build/davetest.elf
	elf2dol $< $@
build/davetest.elf: $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $(OBJ) $(DAVE_LIBS) $(LIBS)
build/%.o: source/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<
build/net_wii.o: $(ROOT)/common/net_wii.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<
build/wolf/%.o: $(TP)/wolfssl/wolfcrypt/src/%.c
	@mkdir -p build/wolf
	$(CC) $(CFLAGS) -c -o $@ $<
clean:
	rm -rf build
.PHONY: clean
