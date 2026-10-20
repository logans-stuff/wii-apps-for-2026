# Sonoran chat/nodes client -> build/sonoran.dol
ROOT := ../..
APP  := sonoran
APP_SRC  := source/main_wii.c source/map.c source/https.c source/api.c source/discord.c source/dc_qr.c source/ws.c
APP_LIBS := -lpng16 -lz -lfat -lwiiuse -lbte -logc -lm
USE_NET := 1
USE_WOLFSSL := 1
USE_CJSON := 1
USE_QRGEN := 1
include $(ROOT)/common.mk
