# Module Makefile - builds hardening.so and its web-process extension
#
# Invoked by the top-level `modules` target with OUTDIR, LIBDIR, CFLAGS
# (module cflags), LDFLAGS (-shared -fPIC) and GTK_BACKEND passed in.
#
# Outputs:
#   $(OUTDIR)/hardening.so                                   the module
#   $(OUTDIR)/hardening-webext/gsurf-hardening-webext.so     loaded by WebKit
#                                                            into web processes
# The module finds the extension relative to its own location, so the two
# must be installed side by side. gsurf-hardening-rules.c is compiled into
# both (and into tests/test-hardening.c).

MODULE_NAME := $(notdir $(CURDIR))
SRCS := $(wildcard *.c)
HDRS := $(wildcard *.h)
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

WEBEXT_DIR := $(OUTDIR)/hardening-webext
WEBEXT_SO := $(WEBEXT_DIR)/gsurf-hardening-webext.so

all: $(OUTDIR)/$(MODULE_NAME).so
ifeq ($(WEBEXT_AVAILABLE),1)
all: $(WEBEXT_SO)
endif

$(OUTDIR)/$(MODULE_NAME).so: $(SRCS) $(HDRS)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBGSURF_LINK)

# The web-process extension links WebKit's web-process API only (never
# libgsurf: it runs inside WebKit's web process, not in gsurf).
$(WEBEXT_SO): webext/gsurf-hardening-webext.c gsurf-hardening-rules.c gsurf-hardening-rules.h
	@mkdir -p $(WEBEXT_DIR)
	$(CC) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags $(WEBEXT_PKG) libsoup-3.0) $(LDFLAGS) -o $@ \
		webext/gsurf-hardening-webext.c gsurf-hardening-rules.c \
		$(shell $(PKG_CONFIG) --libs $(WEBEXT_PKG) libsoup-3.0)

clean:
	rm -f $(OUTDIR)/$(MODULE_NAME).so
	rm -rf $(WEBEXT_DIR)

.PHONY: all clean
