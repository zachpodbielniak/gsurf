# Module Makefile - builds dark_mode.so
#
# Invoked by the top-level `modules` target with OUTDIR, LIBDIR, CFLAGS
# (module cflags), and LDFLAGS (-shared -fPIC) passed in.
#
# The page engine (js/dark-mode.js) and the vendored Dark Reader bundle
# (vendor/darkreader-$(DARKREADER_VERSION).js, see vendor/PROVENANCE.org)
# are compiled into dark_mode.so as a compressed GResource, so the module
# ships as a single file and reads no assets at run time.

MODULE_NAME := $(notdir $(CURDIR))
SRCS := $(wildcard *.c)
CC ?= gcc
PKG_CONFIG ?= pkg-config
GLIB_COMPILE_RESOURCES ?= $(shell $(PKG_CONFIG) --variable=glib_compile_resources gio-2.0 2>/dev/null)
ifeq ($(GLIB_COMPILE_RESOURCES),)
GLIB_COMPILE_RESOURCES := glib-compile-resources
endif

DARKREADER_VERSION := 4.9.133
JS := js/dark-mode.js vendor/darkreader-$(DARKREADER_VERSION).js

# libgsurf link flags. Empty for STATIC=1 builds, where the binary is
# linked --export-dynamic and modules resolve gsurf_* from the executable.
LIBGSURF_LINK ?= -L$(LIBDIR) -lgsurf

GENDIR := $(OUTDIR)/dark_mode-gen
RES_XML := $(GENDIR)/gsurf-dark-mode.gresource.xml
RES_C := $(GENDIR)/gsurf-dark-mode-resources.c
RES_H := $(GENDIR)/gsurf-dark-mode-resources.h

all: $(OUTDIR)/$(MODULE_NAME).so

# The resource manifest is generated so the Dark Reader file name follows
# DARKREADER_VERSION; the bundle is always served as darkreader.js.
$(RES_XML): Makefile
	@mkdir -p $(GENDIR)
	@echo "  GEN     $@"
	@printf '%s\n' \
		'<?xml version="1.0" encoding="UTF-8"?>' \
		'<gresources>' \
		'  <gresource prefix="/org/gsurf/dark_mode">' \
		'    <file alias="dark-mode.js" compressed="true">js/dark-mode.js</file>' \
		'    <file alias="darkreader.js" compressed="true">vendor/darkreader-$(DARKREADER_VERSION).js</file>' \
		'  </gresource>' \
		'</gresources>' > $@

$(RES_C): $(RES_XML) $(JS)
	@echo "  GEN     $@"
	@$(GLIB_COMPILE_RESOURCES) --sourcedir=. --target=$@ --generate-source \
		--internal --manual-register --c-name gsurf_dark_mode_res $(RES_XML)

$(RES_H): $(RES_XML) $(JS)
	@echo "  GEN     $@"
	@$(GLIB_COMPILE_RESOURCES) --sourcedir=. --target=$@ --generate-header \
		--internal --manual-register --c-name gsurf_dark_mode_res $(RES_XML)

$(OUTDIR)/$(MODULE_NAME).so: $(SRCS) $(RES_C) $(RES_H) $(wildcard *.h) $(GSURF_HDRS)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -I$(GENDIR) $(LDFLAGS) -o $@ $(SRCS) $(RES_C) $(LIBGSURF_LINK)

clean:
	rm -f $(OUTDIR)/$(MODULE_NAME).so
	rm -rf $(GENDIR)

.PHONY: all clean
