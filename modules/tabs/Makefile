# Module Makefile - builds <dirname>.so
#
# Invoked by the top-level `modules` target with OUTDIR, LIBDIR, CFLAGS
# (module cflags), and LDFLAGS (-shared -fPIC) passed in.
#
# The tabs module is three sources built into one tabs.so: the
# toolkit-neutral tree model (gsurf-tab-tree.c), the module core
# (gsurf-tabs-module.c) and the strip/sidebar UI (gsurf-tabs-ui.c). The UI
# builds on GTK3 and GTK4 through the shared chrome compat header.

MODULE_NAME := $(notdir $(CURDIR))
SRCS := $(wildcard *.c)
# Local headers plus the shared, header-only GTK3/GTK4 chrome compat
# layer: a change to any of them must rebuild this module.
HDRS := $(wildcard *.h) $(wildcard ../include/*.h)
CC ?= gcc

# libgsurf link flags. Empty for STATIC=1 builds, where the binary is
# linked --export-dynamic and modules resolve gsurf_* from the executable.
LIBGSURF_LINK ?= -L$(LIBDIR) -lgsurf

all: $(OUTDIR)/$(MODULE_NAME).so

$(OUTDIR)/$(MODULE_NAME).so: $(SRCS) $(HDRS) $(wildcard *.h) $(GSURF_HDRS)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBGSURF_LINK)

clean:
	rm -f $(OUTDIR)/$(MODULE_NAME).so
