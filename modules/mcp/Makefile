# MCP module Makefile - builds mcp.so (requires MCP=1)
#
# The parent passes CFLAGS/LDFLAGS plus MCP_CFLAGS/MCP_LDFLAGS. MCP_LDFLAGS
# embeds the vendored mcp-glib *static* archive (libmcp-glib-1.0.a) directly
# into the module, so there is no external libmcp-glib-1.0.so to find at
# runtime and hence no rpath is needed.

MODULE_NAME := mcp
SRCS := $(wildcard *.c)
CC ?= gcc

# libgsurf link flags. Empty for STATIC=1 builds, where the binary is
# linked --export-dynamic and modules resolve gsurf_* from the executable.
LIBGSURF_LINK ?= -L$(LIBDIR) -lgsurf

all: $(OUTDIR)/$(MODULE_NAME).so

$(OUTDIR)/$(MODULE_NAME).so: $(SRCS)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $(MCP_CFLAGS) $(LDFLAGS) -o $@ $(SRCS) \
		$(LIBGSURF_LINK) $(MCP_LDFLAGS)

clean:
	rm -f $(OUTDIR)/$(MODULE_NAME).so
