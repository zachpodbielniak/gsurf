# Module Makefile - builds form_autofill.so
#
# Invoked by the top-level `modules` target with OUTDIR, LIBDIR, CFLAGS
# (module cflags) and LDFLAGS (-shared -fPIC) passed in.
#
# js/form-autofill.js (the page side, run in an isolated script world) is
# embedded into the module as a C string constant.

MODULE_NAME := $(notdir $(CURDIR))
SRCS := $(wildcard *.c)
JS := js/form-autofill.js
CC ?= gcc
PKG_CONFIG ?= pkg-config

# libgsurf link flags. Empty for STATIC=1 builds, where the binary is
# linked --export-dynamic and modules resolve gsurf_* from the executable.
LIBGSURF_LINK ?= -L$(LIBDIR) -lgsurf
JSON_LIBS := $(shell $(PKG_CONFIG) --libs json-glib-1.0 2>/dev/null)

GENDIR := $(OUTDIR)/form_autofill-gen
JS_HDR := $(GENDIR)/gsurf-form-autofill-js.h

all: $(OUTDIR)/$(MODULE_NAME).so

# Embed the JS asset (the same sed idiom rules.mk and adblock use).
$(JS_HDR): $(JS)
	@mkdir -p $(GENDIR)
	@echo "  GEN     $@"
	@echo "/* Generated from modules/form_autofill/js/ - do not edit. */" > $@
	@echo "static const gchar gsurf_form_autofill_js[] =" >> $@
	@sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' $(JS) >> $@
	@echo ";" >> $@

$(OUTDIR)/$(MODULE_NAME).so: $(SRCS) $(JS_HDR) $(wildcard *.h) $(GSURF_HDRS)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -I$(GENDIR) $(LDFLAGS) -o $@ $(SRCS) $(LIBGSURF_LINK) $(JSON_LIBS)

clean:
	rm -f $(OUTDIR)/$(MODULE_NAME).so
	rm -rf $(GENDIR)

.PHONY: all clean
