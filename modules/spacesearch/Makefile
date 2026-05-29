# Module Makefile - builds <dirname>.so
#
# Invoked by the top-level `modules` target with OUTDIR, LIBDIR, CFLAGS
# (module cflags), and LDFLAGS (-shared -fPIC) passed in.

MODULE_NAME := $(notdir $(CURDIR))
SRCS := $(wildcard *.c)
CC ?= gcc

all: $(OUTDIR)/$(MODULE_NAME).so

$(OUTDIR)/$(MODULE_NAME).so: $(SRCS)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) -L$(LIBDIR) -lgsurf

clean:
	rm -f $(OUTDIR)/$(MODULE_NAME).so
