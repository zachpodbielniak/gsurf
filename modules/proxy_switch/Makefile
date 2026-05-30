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

all: $(OUTDIR)/$(MODULE_NAME).so

$(OUTDIR)/$(MODULE_NAME).so: $(SRCS)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBGSURF_LINK)

clean:
	rm -f $(OUTDIR)/$(MODULE_NAME).so
