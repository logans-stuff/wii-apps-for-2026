# One-shot meshtasticd SD installer -> build/meshtasticd-installer.dol
# Writes boot.dol, meta.xml and icon.png to sd:/apps/meshtasticd/ (config.txt,
# prefs/ and maps/ there are left alone). Reuses the Meshtoad installer's
# source. Stage dist/apps/meshtasticd/boot.dol first: apps/meshtasticd/install.sh
# does all of it.
.DEFAULT_GOAL := all
ROOT := ../..
APP  := meshtasticd-installer
APP_SRC  := source/installer.c
APP_LIBS := -lfat -lwiiuse -lbte -logc -lm
APP_CFLAGS := -DINSTALL_APP='"meshtasticd"' -DINSTALL_TITLE='"Meshtastic"' \
  -DINSTALL_DATA='"$(CURDIR)/build/install_data.h"'

INSTALL_APP_FILES := $(sort $(wildcard $(ROOT)/dist/apps/meshtasticd/*))

build/install_data.h: $(ROOT)/tools/meshtoad-installer/make_install_data.py $(INSTALL_APP_FILES)
	@mkdir -p build
	python3 $< --app-dir $(ROOT)/dist/apps/meshtasticd --tiles-dir /nonexistent --output $@

build/installer.o: $(ROOT)/tools/meshtoad-installer/source/installer.c build/install_data.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

include $(ROOT)/common.mk
