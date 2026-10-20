# Meshtoad LoRa node -> build/meshtoad.dol
ROOT := ../..
APP  := meshtoad
APP_SRC  := source/loratest.c source/ch341.c source/sx1262.c source/meshtastic.c source/nodedb.c source/config.c
APP_LIBS := -lfat -lwiiuse -lbte -logc -lm
USE_NET := 1
USE_GRRLIB := 1
USE_OGC_USB := 1
USE_WOLFSSL := 1
include $(ROOT)/common.mk
