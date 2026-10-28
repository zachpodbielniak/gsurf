# Module Makefile - builds adblock.so and its web-process extension
#
# Invoked by the top-level `modules` target with OUTDIR, LIBDIR, CFLAGS
# (module cflags), LDFLAGS (-shared -fPIC) and GTK_BACKEND passed in.
#
# Outputs:
#   $(OUTDIR)/adblock.so                                 the module
#   $(OUTDIR)/adblock-webext/gsurf-adblock-webext.so     loaded by WebKit
#                                                        into web processes
# The module finds the extension relative to its own location, so the two
# must be installed side by side. js/*.js are embedded into adblock.so.

MODULE_NAME := $(notdir $(CURDIR))
SRCS := $(wildcard *.c)
HDRS := $(wildcard *.h)
JS := js/scriptlets.js js/cosmetic.js
CC ?= gcc
PKG_CONFIG ?= pkg-config
GTK_BACKEND ?= gtk3

# libgsurf link flags. Empty for STATIC=1 builds, where the binary is
# linked --export-dynamic and modules resolve gsurf_* from the executable.
LIBGSURF_LINK ?= -L$(LIBDIR) -lgsurf

ifeq ($(GTK_BACKEND),gtk4)
WEBEXT_PKG := webkitgtk-web-process-extension-6.0
else
WEBEXT_PKG := webkit2gtk-web-extension-4.1
endif
WEBEXT_AVAILABLE := $(shell $(PKG_CONFIG) --exists $(WEBEXT_PKG) libsoup-3.0 && echo 1 || echo 0)
SOUP_CFLAGS := $(shell $(PKG_CONFIG) --cflags libsoup-3.0 2>/dev/null)
SOUP_LIBS := $(shell $(PKG_CONFIG) --libs libsoup-3.0 2>/dev/null)

GENDIR := $(OUTDIR)/adblock-gen
JS_HDR := $(GENDIR)/gsurf-adblock-js.h
WEBEXT_DIR := $(OUTDIR)/adblock-webext
WEBEXT_SO := $(WEBEXT_DIR)/gsurf-adblock-webext.so

all: $(OUTDIR)/$(MODULE_NAME).so
ifeq ($(WEBEXT_AVAILABLE),1)
all: $(WEBEXT_SO)
endif

# Embed the JS assets as C string constants (the same sed idiom rules.mk
# uses for the default config).
$(JS_HDR): $(JS)
	@mkdir -p $(GENDIR)
	@echo "  GEN     $@"
	@echo "/* Generated from modules/adblock/js/ - do not edit. */" > $@
	@echo "static const gchar gsurf_adblock_scriptlets_js[] =" >> $@
	@sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' js/scriptlets.js >> $@
	@echo ";" >> $@
	@echo "static const gchar gsurf_adblock_cosmetic_js[] =" >> $@
	@sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' js/cosmetic.js >> $@
	@echo ";" >> $@

$(OUTDIR)/$(MODULE_NAME).so: $(SRCS) $(HDRS) $(JS_HDR)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $(SOUP_CFLAGS) -I$(GENDIR) $(LDFLAGS) -o $@ $(SRCS) $(LIBGSURF_LINK) $(SOUP_LIBS)

# The web-process extension links WebKit's web-process API only (never
# libgsurf: it runs inside WebKit's web process, not in gsurf).
$(WEBEXT_SO): webext/gsurf-adblock-webext.c gsurf-adblock-match.c gsurf-adblock-match.h
	@mkdir -p $(WEBEXT_DIR)
	$(CC) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags $(WEBEXT_PKG)) $(LDFLAGS) -o $@ \
		webext/gsurf-adblock-webext.c gsurf-adblock-match.c \
		$(shell $(PKG_CONFIG) --libs $(WEBEXT_PKG) libsoup-3.0)

clean:
	rm -f $(OUTDIR)/$(MODULE_NAME).so
	rm -rf $(GENDIR) $(WEBEXT_DIR)

.PHONY: all clean
