# One-shot Meshtoad SD installer -> build/meshtoad-installer.dol
.DEFAULT_GOAL := all
ROOT := ../..
APP  := meshtoad-installer
APP_SRC  := source/installer.c
APP_LIBS := -lfat -lwiiuse -lbte -logc -lm

# install_data.h is generated rather than checked in.  Include both the HBC
# app files and the complete offline tile tree in the one-shot installer.
INSTALL_APP_FILES := $(sort $(wildcard $(ROOT)/dist/apps/meshtoad/*))
INSTALL_TILE_FILES := $(sort $(shell find $(ROOT)/tools/maptiles/tiles -type f -name '*.png' 2>/dev/null))

source/install_data.h: make_install_data.py $(INSTALL_APP_FILES) $(INSTALL_TILE_FILES)
	python3 $< --app-dir $(ROOT)/dist/apps/meshtoad \
	  --tiles-dir $(ROOT)/tools/maptiles/tiles --output $@

build/installer.o: source/install_data.h

include $(ROOT)/common.mk
