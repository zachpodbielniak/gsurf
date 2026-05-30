# Makefile - GSURF (GObject Surf)
# A GLib/GObject web browser, port of suckless surf, with a modular
# extension system, YAML + C configuration, and optional MCP server.
#
# Usage:
#   make                  - Build all (lib, gsurf, modules)
#   make lib              - Build static and shared libraries
#   make gsurf            - Build the gsurf executable
#   make gir              - Generate GIR/typelib for introspection
#   make modules          - Build all modules
#   make test             - Run the test suite
#   make install          - Install to PREFIX
#   make clean            - Clean build artifacts
#   make DEBUG=1          - Build with debug symbols
#   make MCP=1            - Build with the MCP server module
#   make GTK_BACKEND=gtk4 - Build against WebKitGTK 6.0 / GTK4

.DEFAULT_GOAL := all
.PHONY: all lib gsurf gir modules test test-gui appimage deps check-deps

# Include configuration
include config.mk

# Check dependencies before anything else (skip for targets that don't need them)
SKIP_DEP_CHECK_TARGETS := install-deps help check-deps show-config clean clean-all
ifeq ($(filter $(SKIP_DEP_CHECK_TARGETS),$(MAKECMDGOALS)),)
$(foreach dep,$(DEPS_REQUIRED),$(call check_dep,$(dep)))
endif

# Source files - Library (auto-discovered; main.c is the binary, not the lib)
LIB_SRCS := $(wildcard src/*.c) \
	$(wildcard src/boxed/*.c) \
	$(wildcard src/core/*.c) \
	$(wildcard src/window/*.c) \
	$(wildcard src/config/*.c) \
	$(wildcard src/module/*.c) \
	$(wildcard src/interfaces/*.c) \
	$(wildcard src/util/*.c)
LIB_SRCS := $(filter-out src/main.c,$(LIB_SRCS))

# Backend sources (only the selected backend compiles)
ifeq ($(GTK_BACKEND),gtk4)
LIB_SRCS += $(wildcard src/backend/gtk4/*.c)
else
LIB_SRCS += $(wildcard src/backend/gtk3/*.c)
endif

# Public headers (for GIR scanner and installation)
LIB_HDRS := $(wildcard src/*.h) \
	$(wildcard src/boxed/*.h) \
	$(wildcard src/core/*.h) \
	$(wildcard src/window/*.h) \
	$(wildcard src/config/*.h) \
	$(wildcard src/module/*.h) \
	$(wildcard src/interfaces/*.h) \
	$(wildcard src/util/*.h)

# yaml-glib sources (vendored dependency, compiled into libgsurf)
YAMLGLIB_SRCS := $(wildcard deps/yaml-glib/src/*.c)

# crispy sources (vendored dependency for C config; wired in Phase 3)
# Minimal config-compilation subset to avoid pulling in the full CLI/tooling.
CRISPY_SRCS :=

# Test sources
TEST_SRCS := $(wildcard tests/test-*.c)

# Module directories
MODULE_DIRS := $(wildcard modules/*)
ifneq ($(MCP_AVAILABLE),1)
MODULE_DIRS := $(filter-out modules/mcp,$(MODULE_DIRS))
endif

# Object files
LIB_OBJS := $(patsubst src/%.c,$(OBJDIR)/%.o,$(LIB_SRCS))
YAMLGLIB_OBJS := $(patsubst deps/%.c,$(OBJDIR)/deps/%.o,$(YAMLGLIB_SRCS))
CRISPY_OBJS := $(patsubst deps/%.c,$(OBJDIR)/deps/%.o,$(CRISPY_SRCS))
MAIN_OBJ := $(OBJDIR)/main.o
TEST_OBJS := $(patsubst tests/%.c,$(OBJDIR)/tests/%.o,$(TEST_SRCS))
TEST_BINS := $(patsubst tests/%.c,$(OUTDIR)/%,$(TEST_SRCS))

# Include build rules
include rules.mk

# Default target
all: deps lib gsurf
ifeq ($(BUILD_MODULES),1)
all: modules
endif
ifeq ($(BUILD_GIR),1)
all: gir
endif
# The standalone gsurf-mcp relay is built only if its source tree exists.
ifeq ($(MCP_AVAILABLE),1)
ifneq ($(wildcard tools/gsurf-mcp/Makefile),)
all: gsurf-mcp
endif
endif

# Build vendored dependencies (yaml-glib objects + crispy static archive)
.PHONY: crispy-lib
crispy-lib:
	$(MAKE) -C $(CRISPY_DIR) lib

$(CRISPY_LIB): crispy-lib

deps: $(YAMLGLIB_OBJS) $(CRISPY_LIB)

# Build the library
lib: src/gsurf-version.h $(OUTDIR)/$(LIB_STATIC) $(OUTDIR)/$(LIB_SHARED_FULL) $(OUTDIR)/gsurf.pc

# Build the executable
gsurf: lib $(OUTDIR)/gsurf

# Build GIR/typelib
gir: $(OUTDIR)/$(GIR_FILE) $(OUTDIR)/$(TYPELIB_FILE)

# Build mcp-glib dependency (only if MCP=1 and deps available)
ifeq ($(MCP_AVAILABLE),1)
.PHONY: mcp-glib
mcp-glib:
	$(MAKE) -C deps/mcp-glib

# Build gsurf-mcp relay binary (only if MCP=1)
.PHONY: gsurf-mcp
gsurf-mcp: lib
	$(MAKE) -C tools/gsurf-mcp OUTDIR=$(abspath $(OUTDIR))
endif

# Build all modules
ifeq ($(MCP_AVAILABLE),1)
modules: mcp-glib
endif
modules: lib $(OUTDIR)/modules
	@for dir in $(MODULE_DIRS); do \
		if [ -d "$$dir" ] && [ -f "$$dir/Makefile" ]; then \
			echo "Building module: $$(basename $$dir)"; \
			$(MAKE) -C "$$dir" \
				OUTDIR=$(abspath $(OUTDIR)/modules) \
				LIBDIR=$(abspath $(OUTDIR)) \
				CFLAGS="$(MODULE_CFLAGS)" \
				LDFLAGS="$(MODULE_LDFLAGS)" \
				MCP_CFLAGS="$(MCP_CFLAGS)" \
				MCP_LDFLAGS="$(MCP_LDFLAGS)" \
				$(STATIC_MODULE_FLAGS); \
		fi \
	done

# Build and run tests (modules needed for the module-loading test)
test: lib modules $(TEST_BINS)
	@echo "Running tests..."
	@failed=0; \
	for test in $(TEST_BINS); do \
		echo "  Running $$(basename $$test)..."; \
		if LD_LIBRARY_PATH=$(OUTDIR):$(CURDIR)/deps/mcp-glib/build $$test; then \
			echo "    PASS"; \
		else \
			echo "    FAIL"; \
			failed=$$((failed + 1)); \
		fi \
	done; \
	if [ $$failed -gt 0 ]; then \
		echo "$$failed test(s) failed"; \
		exit 1; \
	else \
		echo "All tests passed"; \
	fi

$(OUTDIR)/test-%: $(OBJDIR)/tests/test-%.o $(OUTDIR)/$(LIB_SHARED_FULL)
	$(CC) -o $@ $< $(TEST_LDFLAGS)

# Headless GUI smoke test (needs a display + web process): launch the
# browser under Xvfb on about:blank and confirm it stays alive briefly.
# Gated on xvfb-run being available so `make test` stays pure-headless.
.PHONY: test-gui
test-gui: gsurf modules
	@if ! command -v xvfb-run >/dev/null 2>&1; then \
		echo "test-gui: xvfb-run not found; skipping"; exit 0; fi
	@echo "Running headless GUI smoke test..."
	@GSURF_MODULE_PATH="$(abspath $(OUTDIR)/modules)" \
	 LD_LIBRARY_PATH="$(abspath $(OUTDIR)):$(CURDIR)/deps/mcp-glib/build" \
	 timeout 15 xvfb-run -a $(OUTDIR)/gsurf about:blank >/dev/null 2>&1; \
	 st=$$?; \
	 if [ $$st -eq 124 ]; then \
		echo "  PASS (ran until timeout without crashing)"; \
	 elif [ $$st -eq 0 ]; then \
		echo "  PASS (exited cleanly)"; \
	 else \
		echo "  FAIL (exit $$st)"; exit 1; \
	 fi

# Build an AppImage. Installs into a self-contained AppDir, drops an AppRun
# that points LD_LIBRARY_PATH/GSURF_MODULE_PATH at the bundle, then runs
# appimagetool if present (otherwise leaves the populated AppDir in place).
.PHONY: appimage
appimage: all
	rm -rf $(APPDIR)
	$(MAKE) install DESTDIR=$(abspath $(APPDIR)) PREFIX=/usr LIBDIR=/usr/lib \
		MODULEDIR=/usr/lib/gsurf/modules BUILD_MODULES=1 $(if $(filter 1,$(MCP_AVAILABLE)),MCP=1)
	printf '#!/bin/sh\nHERE=$$(dirname "$$(readlink -f "$$0")")\nexport LD_LIBRARY_PATH="$$HERE/usr/lib:$$LD_LIBRARY_PATH"\nexport GSURF_MODULE_PATH="$$HERE/usr/lib/gsurf/modules"\nexec "$$HERE/usr/bin/gsurf" "$$@"\n' > $(APPDIR)/AppRun
	chmod +x $(APPDIR)/AppRun
	cp data/gsurf.desktop $(APPDIR)/gsurf.desktop
	cp data/logo-256.png $(APPDIR)/gsurf.png
	@if command -v $(APPIMAGETOOL) >/dev/null 2>&1; then \
		$(APPIMAGETOOL) $(APPDIR) $(OUTDIR)/gsurf-$(VERSION)-x86_64.AppImage && \
		echo "Wrote $(OUTDIR)/gsurf-$(VERSION)-x86_64.AppImage"; \
	else \
		echo "$(APPIMAGETOOL) not found; populated AppDir at $(APPDIR)"; \
		echo "Install appimagetool and re-run, or: APPIMAGETOOL=/path/to/appimagetool make appimage"; \
	fi

# Check dependencies
check-deps:
	@echo "Checking dependencies (GTK_BACKEND=$(GTK_BACKEND))..."
	@for dep in $(DEPS_REQUIRED); do \
		if $(PKG_CONFIG) --exists $$dep; then \
			echo "  $$dep: OK ($$($(PKG_CONFIG) --modversion $$dep 2>/dev/null))"; \
		else \
			echo "  $$dep: MISSING"; \
		fi \
	done

# Help target
.PHONY: help
help:
	@echo "GSURF - GObject Surf (web browser)"
	@echo ""
	@echo "Build targets:"
	@echo "  all        - Build library, executable, and modules (default)"
	@echo "  lib        - Build static and shared libraries"
	@echo "  gsurf      - Build the gsurf executable"
	@echo "  gir        - Generate GObject Introspection data"
	@echo "  modules    - Build all modules"
	@echo "  test       - Build and run the test suite"
	@echo "  install    - Install to PREFIX ($(PREFIX))"
	@echo "  uninstall  - Remove installed files"
	@echo "  clean      - Remove build artifacts"
	@echo "  clean-all  - Remove all build directories"
	@echo ""
	@echo "Build options (set on command line):"
	@echo "  DEBUG=1            - Enable debug build"
	@echo "  ASAN=1             - Enable AddressSanitizer"
	@echo "  STATIC=1           - Link libgsurf statically into the binary"
	@echo "  PREFIX=path        - Set installation prefix"
	@echo "  GTK_BACKEND=gtk3   - WebKit2GTK 4.1 / GTK3 (default)"
	@echo "  GTK_BACKEND=gtk4   - WebKitGTK 6.0 / GTK4"
	@echo "  MCP=1              - Build the MCP server module + relay"
	@echo "  BUILD_GIR=1        - Generate GObject Introspection data"
	@echo "  BUILD_TESTS=0      - Disable test building"
	@echo ""
	@echo "Utility targets:"
	@echo "  install-deps     - Install build dependencies (auto-detects distro)"
	@echo "  check-deps       - Check for required dependencies"
	@echo "  show-config      - Show current build configuration"
	@echo "  help             - Show this help message"

# Print any make variable: `make print-CFLAGS`
print-%:
	@echo '$($*)'

# Generate compile_commands.json for clangd from the real build flags
.PHONY: compile-commands
compile-commands:
	@printf '[\n' > compile_commands.json
	@first=1; for src in $(LIB_SRCS) src/main.c; do \
		if [ $$first -eq 0 ]; then printf ',\n' >> compile_commands.json; fi; \
		first=0; \
		printf '  {"directory": "%s", "file": "%s", "command": "%s %s -c %s"}' \
			"$(CURDIR)" "$(CURDIR)/$$src" "$(CC)" "$(CFLAGS)" "$$src" >> compile_commands.json; \
	done; \
	for src in $(TEST_SRCS); do \
		printf ',\n  {"directory": "%s", "file": "%s", "command": "%s %s -c %s"}' \
			"$(CURDIR)" "$(CURDIR)/$$src" "$(CC)" "$(TEST_CFLAGS)" "$$src" >> compile_commands.json; \
	done
	@printf '\n]\n' >> compile_commands.json
	@echo "Wrote compile_commands.json ($$(grep -c '\"file\"' compile_commands.json) entries)"

# Dependency tracking (optional, for incremental builds).
# Only include .d files that already exist, so make never tries to
# pre-build them via -MM before the generated headers (gsurf-version.h,
# crispy-version.h) are in place on a fresh tree.
ifeq ($(filter clean clean-all,$(MAKECMDGOALS)),)
-include $(wildcard $(LIB_OBJS:.o=.d))
-include $(wildcard $(MAIN_OBJ:.o=.d))
endif
