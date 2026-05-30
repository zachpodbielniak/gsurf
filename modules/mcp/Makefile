# MCP module Makefile - builds mcp.so (requires MCP=1)
#
# The parent passes CFLAGS/LDFLAGS plus MCP_CFLAGS/MCP_LDFLAGS (mcp-glib
# include/link flags). An rpath to the mcp-glib build dir lets the loaded
# module find libmcp-glib-1.0.so at runtime in a development tree.

MODULE_NAME := mcp
SRCS := $(wildcard *.c)
CC ?= gcc

# libgsurf link flags. Empty for STATIC=1 builds, where the binary is
# linked --export-dynamic and modules resolve gsurf_* from the executable.
LIBGSURF_LINK ?= -L$(LIBDIR) -lgsurf

MCP_RPATH := -Wl,-rpath,$(abspath $(LIBDIR)/../../deps/mcp-glib/build)

all: $(OUTDIR)/$(MODULE_NAME).so

$(OUTDIR)/$(MODULE_NAME).so: $(SRCS)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $(MCP_CFLAGS) $(LDFLAGS) -o $@ $(SRCS) \
		$(LIBGSURF_LINK) $(MCP_LDFLAGS) $(MCP_RPATH)

clean:
	rm -f $(OUTDIR)/$(MODULE_NAME).so
