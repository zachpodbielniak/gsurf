# Module Makefile - builds <dirname>.so
#
# Invoked by the top-level `modules` target with OUTDIR, LIBDIR, CFLAGS
# (module cflags), and LDFLAGS (-shared -fPIC) passed in.

MODULE_NAME := $(notdir $(CURDIR))
SRCS := $(wildcard *.c)
CC ?= gcc

# libgsurf link flags. Empty for STATIC=1 builds, where the binary is
# linked --export-dynamic and modules resolve gsurf_* from the executable.
LIBGSURF_LINK ?= -L$(LIBDIR) -lgsurf

# The tab strip still uses GTK3-only API (GtkContainer, GdkScreen CSS,
# GtkStyleContext, ...) and is being rewritten separately. Until that
# lands, a GTK_BACKEND=gtk4 build skips this module entirely (no tabs.so),
# so the rest of the GTK4 build and its tests can run. Tests that need
# tabs.so skip when it is missing.
ifeq ($(GTK_BACKEND),gtk4)
all:
	@echo "  tabs: GTK3-only for now (being rewritten); skipped on GTK_BACKEND=gtk4"
else
all: $(OUTDIR)/$(MODULE_NAME).so
endif

$(OUTDIR)/$(MODULE_NAME).so: $(SRCS)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBGSURF_LINK)

clean:
	rm -f $(OUTDIR)/$(MODULE_NAME).so
