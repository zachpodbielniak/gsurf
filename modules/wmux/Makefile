# Module Makefile - builds <dirname>.so
#
# Invoked by the top-level `modules` target with OUTDIR, LIBDIR, CFLAGS
# (module cflags), LDFLAGS (-shared -fPIC) and GTK_BACKEND passed in.
#
# wmux is the toolkit-neutral core (layout, command, keys, bindings), the
# module (module, session, ui) and one pane grid per toolkit: only the
# grid of the selected GTK_BACKEND is compiled in.

MODULE_NAME := $(notdir $(CURDIR))
GTK_BACKEND ?= gtk3
OTHER_GRID := $(if $(filter gtk4,$(GTK_BACKEND)),gsurf-wmux-grid-gtk3.c,gsurf-wmux-grid-gtk4.c)
SRCS := $(filter-out $(OTHER_GRID),$(wildcard *.c))
# Local headers plus the shared, header-only GTK3/GTK4 chrome compat
# layer: a change to any of them must rebuild this module.
HDRS := $(wildcard *.h) $(wildcard ../include/*.h)
CC ?= gcc

# libgsurf link flags. Empty for STATIC=1 builds, where the binary is
# linked --export-dynamic and modules resolve gsurf_* from the executable.
LIBGSURF_LINK ?= -L$(LIBDIR) -lgsurf

all: $(OUTDIR)/$(MODULE_NAME).so

$(OUTDIR)/$(MODULE_NAME).so: $(SRCS) $(HDRS)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBGSURF_LINK)

clean:
	rm -f $(OUTDIR)/$(MODULE_NAME).so
