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
#   make install-skill    - Install the /gsurf agent skill (~/.agents, ~/.claude, ~/.grok)
#   make clean            - Clean build artifacts
#   make DEBUG=1          - Build with debug symbols
#   make MCP=1            - Build with the MCP server module
#   make GTK_BACKEND=gtk4 - Build against WebKitGTK 6.0 / GTK4

.DEFAULT_GOAL := all

# Cleaning and building cannot share a dependency graph: clean can delete
# files after make has already decided they are up to date. Dispatch mixed
# invocations in two phases, reparsing the normal graph after cleaning.
# Recursive make preserves command-line options and the parallel jobserver.
_clean_goals := $(filter clean clean-all,$(MAKECMDGOALS))
_other_goals := $(filter-out clean clean-all,$(MAKECMDGOALS))
ifneq ($(and $(_clean_goals),$(_other_goals)),)
.PHONY: $(MAKECMDGOALS) _clean_then_build
$(MAKECMDGOALS): _clean_then_build ;
_clean_then_build:
	$(MAKE) $(if $(filter clean-all,$(_clean_goals)),clean-all,clean)
	$(MAKE) $(_other_goals)
else

.PHONY: all lib gsurf gsurfctl gir modules test test-build test-gui appimage adblock-lists deps check-deps

# Include configuration
include config.mk

# Check dependencies before anything else (skip for targets that don't need them)
SKIP_DEP_CHECK_TARGETS := install-deps help check-deps show-config clean clean-all adblock-lists \
	install-skill install-skills uninstall-skill uninstall-skills test-skill test-install-skill
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
	$(wildcard src/util/*.c) \
	$(wildcard src/control/*.c)
LIB_SRCS := $(filter-out src/main.c,$(LIB_SRCS))

# Backend sources (only the selected backend compiles)
LIB_SRCS += $(wildcard src/protocol/*.c) $(wildcard src/backend/webkit/*.c)
ifeq ($(GTK_BACKEND),gtk4)
LIB_SRCS += $(wildcard src/backend/gtk4/*.c)
else
LIB_SRCS += $(wildcard src/backend/gtk3/*.c)
endif

# libregnum (LRG) backend: additive to the GTK backend (its WebKitGTK engine
# reuses the already-linked WebKit; selected at runtime). Compiled only when
# LRG_BACKEND=1 and the LRG deps were found (see config.mk). Exactly one engine
# .c is built — the WebKitGTK engine when a GTK backend is present, else WPE.
ifeq ($(LRG_BACKEND),1)
ifeq ($(LRG_AVAILABLE),1)
LIB_SRCS += src/backend/lrg/gsurf-lrg-backend.c \
	src/backend/lrg/gsurf-lrg-view.c \
	src/backend/lrg/gsurf-lrg-window.c \
	src/backend/lrg/gsurf-lrg-engine-$(LRG_ENGINE).c
ifeq ($(LRG_3D),1)
LIB_SRCS += src/backend/lrg/gsurf-lrg-scene.c
endif

# Standalone convenience: build the bundled (or pointed-at) graylib so its
# graylib/raylib archives -- and its GENERATED headers (grl-version.h, which
# graylib.h includes) -- exist before anything that includes graylib.h
# compiles or links.  Only graylib is built, not all of libregnum: the LRG
# backend uses nothing but graylib, and the full libregnum build drags in
# MMO/OpenSSL, cad-glib, ai-glib, ... that can fail independently of gsurf.
#
# The archives have NO prerequisites, so make runs the recipe only when one is
# MISSING -- never to "update" an embedder's prebuilt copy.  cmacs builds its
# libregnum itself and overrides LIBREGNUM_DIR, so the archives are already
# present and the recipe never fires there.  `&:' (grouped target) runs the
# recipe ONCE for both archives; with plain `a b:' a parallel make ran two
# concurrent graylib builds over the same tree.
#
# Command-line variables propagate to sub-makes via MAKEFLAGS and override the
# sub-make's own assignments, so graylib's BUILDDIR is pinned (a gsurf
# `BUILDDIR=build-lrg' would otherwise move graylib's archives away from
# $(LRG_GRAYLIB_LIB)), and its GIR is skipped (gsurf's `BUILD_GIR=1' is about
# Gsurf's GIR; graylib's would only add a g-ir-scanner dependency).
#
# A 3D build (LRG_3D=1) links liblibregnum.a instead, and libregnum's own build
# produces graylib at the same paths -- so ONE grouped rule builds all three,
# rather than two rules racing over the same graylib tree.
ifeq ($(LRG_3D),1)
# The BUNDLED copy also rebuilds when its config.mk (which carries the
# version, and which a submodule checkout rewrites) is newer than the
# archive: otherwise a checkout that built an older libregnum keeps linking
# it after a submodule bump and fails with undefined lrg_* symbols.  An
# embedder's copy (any other LIBREGNUM_DIR) is still never rebuilt here.
ifeq ($(abspath $(LIBREGNUM_DIR)),$(abspath $(CURDIR)/deps/libregnum))
LRG_LIBREGNUM_STAMP := $(LIBREGNUM_DIR)/config.mk
endif
# gsurf's own BUILDDIR (if given on the command line) must not reach
# libregnum or its nested deps: drop it from this recipe's MAKEOVERRIDES.
$(LRG_LIBREGNUM_LIB) $(LRG_GRAYLIB_LIB) $(LRG_RAYLIB_LIB): \
	MAKEOVERRIDES := $(filter-out BUILDDIR=%,$(MAKEOVERRIDES))
$(LRG_LIBREGNUM_LIB) $(LRG_GRAYLIB_LIB) $(LRG_RAYLIB_LIB) &: $(LRG_LIBREGNUM_STAMP)
	$(MAKE) -C $(LIBREGNUM_DIR) all $(LRG_LIBREGNUM_MAKEFLAGS)
$(OUTDIR)/$(LIB_STATIC) $(OUTDIR)/$(LIB_SHARED_FULL): \
	$(LRG_LIBREGNUM_LIB) $(LRG_GRAYLIB_LIB) $(LRG_RAYLIB_LIB)
else
$(LRG_GRAYLIB_LIB) $(LRG_RAYLIB_LIB) &:
	$(MAKE) -C $(GRAYLIB_DIR) BUILDDIR=$(abspath $(GRAYLIB_DIR))/build BUILD_GIR=0
$(OUTDIR)/$(LIB_STATIC) $(OUTDIR)/$(LIB_SHARED_FULL): \
	$(LRG_GRAYLIB_LIB) $(LRG_RAYLIB_LIB)
endif
# The LRG sources include graylib.h, whose grl-version.h exists only once
# graylib has been built: order-only, so a rebuilt archive doesn't recompile.
$(patsubst src/%.c,$(OBJDIR)/%.o,$(filter src/backend/lrg/%,$(LIB_SRCS))): \
	| $(LRG_GRAYLIB_LIB)
endif
endif

# Public headers (for GIR scanner and installation)
LIB_HDRS := $(wildcard src/*.h) \
	$(wildcard src/boxed/*.h) \
	$(wildcard src/core/*.h) \
	$(wildcard src/window/*.h) \
	$(wildcard src/config/*.h) \
	$(wildcard src/module/*.h) \
	$(wildcard src/interfaces/*.h) \
	$(wildcard src/util/*.h) \
	$(filter-out %-private.h,$(wildcard src/control/*.h))

# yaml-glib sources (vendored dependency, compiled into libgsurf)
YAMLGLIB_SRCS := $(wildcard $(YAMLGLIB_DIR)/src/*.c)

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
# Keyed on YAMLGLIB_DIR so an overridden (absolute) path still lands
# its objects in the same place under OBJDIR.
YAMLGLIB_OBJS := $(patsubst $(YAMLGLIB_DIR)/%.c,$(OBJDIR)/deps/yaml-glib/%.o,$(YAMLGLIB_SRCS))
CRISPY_OBJS := $(patsubst deps/%.c,$(OBJDIR)/deps/%.o,$(CRISPY_SRCS))
MAIN_OBJ := $(OBJDIR)/main.o
# gsurfctl, the control-socket client (tools/gsurfctl, linked with libgsurf).
GSURFCTL_OBJ := $(OBJDIR)/tools/gsurfctl/gsurfctl.o
TEST_OBJS := $(patsubst tests/%.c,$(OBJDIR)/tests/%.o,$(TEST_SRCS))
TEST_BINS := $(patsubst tests/%.c,$(OUTDIR)/%,$(TEST_SRCS))

# Build-configuration stamp.  Object paths don't encode the configuration, so
# toggling LRG_BACKEND (or GTK_BACKEND, LIBREGNUM_DIR, any CFLAGS change) between
# two builds of one BUILDDIR used to leave objects compiled for the OTHER
# configuration -- e.g. gsurf-backend.o still referencing the LRG backend after
# LRG_BACKEND=1 -> 0, an undefined-reference link failure -- unless the user
# remembered `make clean'.  The stamp holds every flag that shapes an object;
# it is rewritten (at parse time, so the comparison wins any race with the
# rules below) only when that text changes, and every compiled artifact
# depends on it, so a configuration change rebuilds exactly once.
GSURF_FLAGS_STAMP := $(OBJDIR)/.build-flags
GSURF_FLAGS_NOW := CC=$(CC) GTK_BACKEND=$(GTK_BACKEND) LRG_BACKEND=$(LRG_BACKEND) \
	LRG_ENGINE=$(LRG_ENGINE) LRG_3D=$(LRG_3D) LIBREGNUM_DIR=$(LIBREGNUM_DIR) \
	STATIC=$(STATIC) \
	CFLAGS=$(CFLAGS) TEST_CFLAGS=$(TEST_CFLAGS) \
	LDFLAGS=$(LDFLAGS)
ifeq ($(filter clean clean-all,$(MAKECMDGOALS)),)
ifneq ($(file <$(GSURF_FLAGS_STAMP)),$(GSURF_FLAGS_NOW))
$(shell mkdir -p $(OBJDIR))
$(file >$(GSURF_FLAGS_STAMP),$(GSURF_FLAGS_NOW))
endif
endif
$(LIB_OBJS) $(YAMLGLIB_OBJS) $(MAIN_OBJ) $(GSURFCTL_OBJ) $(TEST_OBJS): $(GSURF_FLAGS_STAMP)
$(OUTDIR)/$(LIB_STATIC) $(OUTDIR)/$(LIB_SHARED_FULL): $(GSURF_FLAGS_STAMP)

# Include build rules
include rules.mk

# Default target
all: deps lib gsurf gsurfctl
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
# BUILDDIR is pinned for the sub-make: a command-line BUILDDIR (e.g. a
# separate GTK4 tree, `make GTK_BACKEND=gtk4 BUILDDIR=build-gtk4`) would
# otherwise propagate and move crispy's archive away from $(CRISPY_LIB).
crispy-lib:
	$(MAKE) -C $(CRISPY_DIR) BUILDDIR=build lib

$(CRISPY_LIB): crispy-lib

deps: $(YAMLGLIB_OBJS) $(CRISPY_LIB)

# Build the library
lib: src/gsurf-version.h $(OUTDIR)/$(LIB_STATIC) $(OUTDIR)/$(LIB_SHARED_FULL) $(OUTDIR)/gsurf.pc

# Build the executable
gsurf: lib $(OUTDIR)/gsurf

# Build the control-socket client
gsurfctl: lib $(OUTDIR)/gsurfctl

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

# Build all modules. A module that fails to compile fails the build
# (`|| exit`); otherwise the loop would carry on and a broken module would
# silently be missing from $(OUTDIR)/modules.
#
# GSURF_HDRS: every libgsurf header and the shared module headers. A
# module's .so depends on them (and on its own *.h), so a header change (an
# enum growing, a struct field) rebuilds the modules instead of leaving
# them compiled against the old layout.
GSURF_HDRS := $(abspath $(shell find src modules/include -name '*.h'))
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
				GTK_BACKEND=$(GTK_BACKEND) \
				PKG_CONFIG="$(PKG_CONFIG)" \
				GSURF_HDRS="$(GSURF_HDRS)" \
				$(STATIC_MODULE_FLAGS) || exit $$?; \
		fi \
	done

# Build and run tests (modules needed for the module-loading test)
test: gsurf gsurfctl modules $(TEST_BINS)
	@echo "Running tests..."
	@failed=0; \
	echo "  Running test-build-config.sh..."; \
	if tests/test-build-config.sh "$(MAKE)" "$(BUILDDIR)"; then \
		echo "    PASS"; \
	else \
		echo "    FAIL"; \
		failed=$$((failed + 1)); \
	fi; \
	for script in tests/test-skill.sh tests/test-install-skill.sh; do \
		echo "  Running $$(basename $$script)..."; \
		if $$script; then \
			echo "    PASS"; \
		else \
			echo "    FAIL"; \
			failed=$$((failed + 1)); \
		fi \
	done; \
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

# Build-system regression tests only (no compile; see the script's header).
test-build:
	tests/test-build-config.sh "$(MAKE)" "$(BUILDDIR)"

# The /gsurf agent skill: its structure (tests/test-skill.sh, the checks of
# agent-skills' validate-skills.sh) and its installer in a temporary HOME
# (tests/test-install-skill.sh). Neither needs a build and `make test' runs
# both; test-skill-examples checks the skill's examples against the built
# library, modules and gsurfctl.
.PHONY: test-skill test-install-skill
test-skill:
	tests/test-skill.sh
test-install-skill:
	tests/test-install-skill.sh

# test-skill-examples builds the skill's module template the way the
# `modules' target builds a module, so it needs the module flags.
$(OUTDIR)/test-skill-examples: $(OUTDIR)/module-cflags.txt
$(OUTDIR)/module-cflags.txt: $(GSURF_FLAGS_STAMP) | $(OUTDIR)
	$(file >$@,$(MODULE_CFLAGS))

# test-tab-tree compiles the tabs module's tree model in directly.
$(OBJDIR)/tests/test-tab-tree.o: modules/tabs/gsurf-tab-tree.c modules/tabs/gsurf-tab-tree.h

# The wmux model tests compile the module's toolkit-neutral core in
# (layout, command language, keys, binding tables).
WMUX_MODEL_FILES := $(foreach part,layout command keys bindings,\
	modules/wmux/gsurf-wmux-$(part).c modules/wmux/gsurf-wmux-$(part).h)
$(OBJDIR)/tests/test-wmux-layout.o $(OBJDIR)/tests/test-wmux-command.o \
$(OBJDIR)/tests/test-wmux-keys.o $(OBJDIR)/tests/test-wmux-bindings.o: $(WMUX_MODEL_FILES)
# The module tests read the module's struct (private header); the GUI test
# also compiles the layout model in to inspect the panes.
$(OBJDIR)/tests/test-wmux.o $(OBJDIR)/tests/test-wmux-webkit.o \
$(OBJDIR)/tests/test-wmux-docs.o: $(WMUX_MODEL_FILES) $(wildcard modules/wmux/*.h)

# The WebKit GUI suites share the header-only tests/webkit-test-util.h
# (GTK init, its log writer, bounded waits): a change there rebuilds every
# suite that includes it.
WEBKIT_TEST_UTIL_OBJS := $(patsubst tests/%.c,$(OBJDIR)/tests/%.o,\
	$(shell grep -l '"webkit-test-util.h"' tests/*.c))
$(WEBKIT_TEST_UTIL_OBJS): tests/webkit-test-util.h

$(OUTDIR)/test-%: $(OBJDIR)/tests/test-%.o $(OUTDIR)/$(LIB_SHARED_FULL)
	$(CC) -o $@ $< $(TEST_LDFLAGS)

# test-hardening compiles the hardening module's shared decision logic
# (#include of modules/hardening/gsurf-hardening-rules.c).
$(OBJDIR)/tests/test-hardening.o: modules/hardening/gsurf-hardening-rules.c \
	modules/hardening/gsurf-hardening-rules.h

.PHONY: tests
tests: test

# Headless GUI smoke test (needs a display + web process): launch the
# browser under Xvfb on about:blank and confirm it stays alive briefly.
# Gated on xvfb-run being available so `make test` stays pure-headless.
# Forces GTK onto Xvfb's X display, so running it from a Wayland session
# never opens test windows on the user's desktop.
.PHONY: test-gui
test-gui: gsurf gsurfctl modules $(OUTDIR)/test-backend-settings $(OUTDIR)/test-protocol-webkit $(OUTDIR)/test-adblock-webkit $(OUTDIR)/test-keynav-webkit $(OUTDIR)/test-chrome-lifecycle $(OUTDIR)/test-containers-webkit $(OUTDIR)/test-dark-mode-webkit $(OUTDIR)/test-form-autofill-webkit $(OUTDIR)/test-containers-module-webkit $(OUTDIR)/test-session $(OUTDIR)/test-hardening-webkit $(OUTDIR)/test-tabs-webkit $(OUTDIR)/test-window-layout-webkit $(OUTDIR)/test-wmux-webkit $(OUTDIR)/test-temp-storage-webkit $(OUTDIR)/test-modal-webkit $(OUTDIR)/test-mcp-webkit $(OUTDIR)/test-control-webkit $(OUTDIR)/test-gsurfctl \
	$(OUTDIR)/test-backend-lrg
	@if ! command -v xvfb-run >/dev/null 2>&1; then \
		echo "test-gui: xvfb-run not found; skipping"; exit 0; fi; \
	unset WAYLAND_DISPLAY; export GDK_BACKEND=x11; \
	echo "Running the libregnum backend tests (the live --lrg=3d scene)..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-backend-lrg || exit $$?; \
	echo "Running native settings regression tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-backend-settings || exit $$?; \
	echo "Running native protocol regression tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-protocol-webkit || exit $$?; \
	echo "Running adblock WebKit enforcement tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-adblock-webkit || exit $$?; \
	echo "Running keyboard focus/navigation tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-keynav-webkit || exit $$?; \
	echo "Running chrome module lifetime/key tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-chrome-lifecycle || exit $$?; \
	echo "Running container isolation / script world tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-containers-webkit || exit $$?; \
	echo "Running dark mode WebKit tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-dark-mode-webkit || exit $$?; \
	echo "Running form autofill WebKit tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-form-autofill-webkit || exit $$?; \
	echo "Running containers module (host assignment, actions) tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-containers-module-webkit || exit $$?; \
	echo "Running session restore tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-session || exit $$?; \
	echo "Running hardening WebKit enforcement tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-hardening-webkit || exit $$?; \
	echo "Running tabs sidebar layout tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-tabs-webkit || exit $$?; \
	echo "Running window view routing / layout tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-window-layout-webkit || exit $$?; \
	echo "Running wmux pane multiplexer tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-wmux-webkit || exit $$?; \
	echo "Running temporary-container storage (IndexedDB blobs) tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-temp-storage-webkit || exit $$?; \
	echo "Running modal (vim modes, typing in fields) tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-modal-webkit || exit $$?; \
	echo "Running MCP tools (views, tree, containers, wmux, modes) tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-mcp-webkit || exit $$?; \
	echo "Running control socket (core methods, events) tests..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-control-webkit || exit $$?; \
	echo "Running gsurfctl against a real gsurf..."; \
	GSURF_TEST_GUI=1 LD_LIBRARY_PATH="$(abspath $(OUTDIR))" \
	 xvfb-run -a $(OUTDIR)/test-gsurfctl || exit $$?; \
	echo "Running headless GUI smoke test..."; \
	GSURF_MODULE_PATH="$(abspath $(OUTDIR)/modules)" \
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
	printf '#!/bin/sh\nHERE=$$(dirname "$$(readlink -f "$$0")")\nexport LD_LIBRARY_PATH="$$HERE/usr/lib:$$LD_LIBRARY_PATH"\nexport GSURF_MODULE_PATH="$$HERE/usr/lib/gsurf/modules"\ncase "$$(basename "$${ARGV0:-$$0}")" in gsurfctl*) exec "$$HERE/usr/bin/gsurfctl" "$$@" ;; esac\nif [ "$$1" = ctl ]; then shift; exec "$$HERE/usr/bin/gsurfctl" "$$@"; fi\nexec "$$HERE/usr/bin/gsurf" "$$@"\n' > $(APPDIR)/AppRun
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

# Fetch uBlock Origin / EasyList filter lists (plus their !#include files)
# for the adblock module's `filter_lists:`. The module parses the lists
# natively, so this is only a download. Override:
#   ADBLOCK_DIR=...                 output dir (default ~/.config/gsurf/adblock)
#   ADBLOCK_LISTS="name=url ..."    lists to fetch (default EasyList,
#                                   EasyPrivacy and uBO's own lists)
ADBLOCK_DIR ?= $(HOME)/.config/gsurf/adblock
ADBLOCK_LISTS ?=

.PHONY: adblock-lists
adblock-lists:
	@modules/adblock/fetch-lists.sh -d "$(ADBLOCK_DIR)" $(ADBLOCK_LISTS)

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
	@echo "  gsurfctl   - Build the gsurfctl control client"
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
	@echo "  appimage         - Build a self-contained AppImage"
	@echo "  install-skill    - Install the /gsurf agent skill into ~/.agents/skills"
	@echo "                     and link it from ~/.claude and ~/.grok (SKILLS_LINK=1"
	@echo "                     links this tree instead of copying; also install-skills)"
	@echo "  uninstall-skill  - Remove the /gsurf agent skill"
	@echo "  test-skill       - Check the skill's structure (part of test)"
	@echo "  adblock-lists    - Fetch EasyList + uBlock Origin filter lists"
	@echo "                     JSON for the adblock content_filters: option"
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
# Which copy the vendored objects under OBJDIR were built from.
#
# Object paths are fixed regardless of YAMLGLIB_DIR, so switching copies leaves
# objects -- and .d files -- describing the other one.  A stale .d naming
# a source that no longer exists makes make fail during graph
# construction with "No rule to make target", before any recipe could
# have cleaned it up.  Hence a parse-time check rather than a rule:
# `-include' of the .d files happens at parse time too, and this has to
# win that race.
_yaml-glib_stamp := $(OBJDIR)/deps/.yaml-glib-dir
_yaml-glib_prev := $(shell cat $(_yaml-glib_stamp) 2>/dev/null)
ifneq ($(_yaml-glib_prev),$(YAMLGLIB_DIR))
$(shell rm -rf $(OBJDIR)/deps/yaml-glib)
$(shell mkdir -p $(dir $(_yaml-glib_stamp)) && printf '%s' '$(YAMLGLIB_DIR)' > $(_yaml-glib_stamp))
endif

-include $(wildcard $(LIB_OBJS:.o=.d))
-include $(wildcard $(MAIN_OBJ:.o=.d))
endif

endif # mixed clean/build invocation
