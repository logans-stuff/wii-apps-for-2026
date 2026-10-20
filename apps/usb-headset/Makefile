# USB headset driver + Opus test -> build/usb-headset.dol
ROOT := ../..
APP  := usb-headset
APP_SRC  := source/headset.c source/usbaudio.c
APP_CFLAGS := -I$(DEVKITPRO)/portlibs/ppc/include/opus -DSONORAN_DEBUG
APP_LIBS := -lopus -lfat -lwiiuse -lbte -logc -lm
USE_NET := 1
USE_OGC_USB := 1
USE_WOLFSSL := 1
include $(ROOT)/common.mk
