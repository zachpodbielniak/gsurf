# config.mk - GSURF Configuration
# GObject Surf - a GLib/GObject web browser (port of suckless surf)
#
# This file contains all configurable build options.
# Override any variable on the command line:
#   make DEBUG=1
#   make PREFIX=/usr/local
#   make GTK_BACKEND=gtk4
#   make MCP=1

# Version
VERSION_MAJOR := 0
VERSION_MINOR := 1
VERSION_MICRO := 0
VERSION := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_MICRO)

# Installation directories
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

# Auto-detect lib vs lib64 for 64-bit systems (Fedora, RHEL, SUSE, etc.)
# Override with: make LIBDIR=/usr/local/lib
LIBDIR_SUFFIX := $(shell if [ -d /usr/lib64 ]; then echo lib64; else echo lib; fi)
LIBDIR ?= $(PREFIX)/$(LIBDIR_SUFFIX)
INCLUDEDIR ?= $(PREFIX)/include
DATADIR ?= $(PREFIX)/share
MANDIR ?= $(DATADIR)/man
ICONDIR ?= $(DATADIR)/icons/hicolor
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
GIRDIR ?= $(DATADIR)/gir-1.0
TYPELIBDIR ?= $(LIBDIR)/girepository-1.0
MODULEDIR ?= $(LIBDIR)/gsurf/modules
SYSCONFDIR ?= /etc

# Build directories
BUILDDIR := build
OBJDIR_DEBUG := $(BUILDDIR)/debug/obj
OBJDIR_RELEASE := $(BUILDDIR)/release/obj
BINDIR_DEBUG := $(BUILDDIR)/debug
BINDIR_RELEASE := $(BUILDDIR)/release

# Build options (0 or 1)
DEBUG ?= 0
ASAN ?= 0
BUILD_GIR ?= 0
BUILD_TESTS ?= 1
BUILD_MODULES ?= 1
MCP ?= 0
AI ?= 0

# WebKit/GTK backend selection: gtk3 (WebKit2GTK 4.1) or gtk4 (WebKitGTK 6.0)
GTK_BACKEND ?= gtk3

# Select build directories based on DEBUG
ifeq ($(DEBUG),1)
    OBJDIR := $(OBJDIR_DEBUG)
    OUTDIR := $(BINDIR_DEBUG)
    BUILD_TYPE := debug
else
    OBJDIR := $(OBJDIR_RELEASE)
    OUTDIR := $(BINDIR_RELEASE)
    BUILD_TYPE := release
endif

# Compiler and tools
CC := gcc
AR := ar
PKG_CONFIG ?= pkg-config
GIR_SCANNER ?= g-ir-scanner
GIR_COMPILER ?= g-ir-compiler
INSTALL := install
INSTALL_PROGRAM := $(INSTALL) -m 755
INSTALL_DATA := $(INSTALL) -m 644
MKDIR_P := mkdir -p

# AppImage tool (override: make APPIMAGETOOL=/path/to/appimagetool appimage)
APPIMAGETOOL ?= appimagetool-x86_64.AppImage
APPDIR := $(BUILDDIR)/AppDir

# C standard and dialect
CSTD := -std=gnu11

# Base compiler flags
CFLAGS_BASE := $(CSTD) -Wall -Wextra -Wno-unused-parameter
CFLAGS_BASE += -fPIC
CFLAGS_BASE += -DGSURF_VERSION=\"$(VERSION)\"
CFLAGS_BASE += -DGSURF_VERSION_MAJOR=$(VERSION_MAJOR)
CFLAGS_BASE += -DGSURF_VERSION_MINOR=$(VERSION_MINOR)
CFLAGS_BASE += -DGSURF_VERSION_MICRO=$(VERSION_MICRO)
CFLAGS_BASE += -DGSURF_MODULEDIR=\"$(MODULEDIR)\"
CFLAGS_BASE += -DGSURF_SYSCONFDIR=\"$(SYSCONFDIR)\"
CFLAGS_BASE += -DGSURF_DATADIR=\"$(DATADIR)\"
CFLAGS_BASE += -DGSURF_DEV_INCLUDE_DIR=\"$(CURDIR)/$(BUILDDIR)/include\"

# WebKit/GTK backend dependencies + feature macro
ifeq ($(GTK_BACKEND),gtk4)
    DEPS_BACKEND := webkitgtk-6.0 gtk4 javascriptcoregtk-6.0
    CFLAGS_BASE += -DGSURF_BACKEND_GTK4=1
    BACKEND_NAME := gtk4
else
    DEPS_BACKEND := webkit2gtk-4.1 gtk+-3.0 javascriptcoregtk-4.1
    CFLAGS_BASE += -DGSURF_BACKEND_GTK3=1
    BACKEND_NAME := gtk3
endif
BACKEND_AVAILABLE := $(shell $(PKG_CONFIG) --exists $(DEPS_BACKEND) 2>/dev/null && echo 1 || echo 0)

# Debug/Release flags
ifeq ($(DEBUG),1)
    CFLAGS_BUILD := -g -O0 -DDEBUG
    ifeq ($(ASAN),1)
        CFLAGS_BUILD += -fsanitize=address -fsanitize=undefined
        LDFLAGS_ASAN := -fsanitize=address -fsanitize=undefined
    endif
else
    CFLAGS_BUILD := -O2 -DNDEBUG
endif

# Required dependencies (core library + selected backend)
DEPS_REQUIRED := glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0
DEPS_REQUIRED += yaml-0.1 json-glib-1.0
DEPS_REQUIRED += $(DEPS_BACKEND)

# Optional MCP dependencies (requires mcp-glib submodule at deps/mcp-glib)
ifeq ($(MCP),1)
DEPS_MCP := libsoup-3.0 libdex-1 libpng
MCP_AVAILABLE := $(shell $(PKG_CONFIG) --exists $(DEPS_MCP) 2>/dev/null && echo 1 || echo 0)
else
MCP_AVAILABLE := 0
endif

# Optional AI dependencies (ai-glib integration; DEFERRED - reserved for future)
ifeq ($(AI),1)
DEPS_AI := libsoup-3.0 json-glib-1.0
AI_AVAILABLE := $(shell $(PKG_CONFIG) --exists $(DEPS_AI) 2>/dev/null && echo 1 || echo 0)
else
AI_AVAILABLE := 0
endif

# Check for required dependencies
define check_dep
$(if $(shell $(PKG_CONFIG) --exists $(1) && echo yes),,$(error Missing dependency: $(1)))
endef

# Get flags from pkg-config
CFLAGS_DEPS := $(shell $(PKG_CONFIG) --cflags $(DEPS_REQUIRED) 2>/dev/null)
LDFLAGS_DEPS := $(shell $(PKG_CONFIG) --libs $(DEPS_REQUIRED) 2>/dev/null)

# crispy (C-config compiler) is built via its own Makefile and its static
# archive is linked into libgsurf. Only referenced objects are pulled in.
# The crispy-lib target runs `make -C deps/crispy`, and DEBUG propagates to
# that sub-make via MAKEFLAGS — so crispy lands in build/debug when we build
# DEBUG=1 and build/release otherwise. Track the same path here, or the
# libgsurf.a/binary link looks for the archive in the wrong directory.
CRISPY_DIR := deps/crispy
ifeq ($(DEBUG),1)
CRISPY_BUILD := $(CRISPY_DIR)/build/debug
else
CRISPY_BUILD := $(CRISPY_DIR)/build/release
endif
CRISPY_LIB := $(CRISPY_BUILD)/libcrispy.a

# Include paths
CFLAGS_INC := -I. -Isrc -Ideps/yaml-glib/src -Ideps/crispy/src -I$(OUTDIR)

# Combine all CFLAGS
CFLAGS := $(CFLAGS_BASE) $(CFLAGS_BUILD) $(CFLAGS_INC) $(CFLAGS_DEPS)

# Linker flags
LDFLAGS := $(LDFLAGS_DEPS) $(LDFLAGS_ASAN)
LDFLAGS_SHARED := -shared -Wl,-soname,libgsurf.so.$(VERSION_MAJOR)

# Library names
LIB_NAME := gsurf
LIB_STATIC := lib$(LIB_NAME).a
LIB_SHARED := lib$(LIB_NAME).so
LIB_SHARED_FULL := lib$(LIB_NAME).so.$(VERSION)
LIB_SHARED_MAJOR := lib$(LIB_NAME).so.$(VERSION_MAJOR)

# Static build (STATIC=1): link libgsurf.a directly into the gsurf binary
# instead of libgsurf.so, so the binary has no runtime dependency on the
# shared library. System libraries (GTK/WebKit/GLib/...) stay dynamic —
# WebKit cannot be statically linked, so a fully-static binary is not
# possible. Modules remain runtime .so plugins; the binary is linked with
# --export-dynamic and the modules link no libgsurf of their own, so they
# resolve gsurf_* symbols from the executable (one library instance, no
# duplicate GType registrations). See rules.mk for the link recipes.
STATIC ?= 0
ifeq ($(STATIC),1)
    STATIC_MODULE_FLAGS := LIBGSURF_LINK=
else
    STATIC_MODULE_FLAGS :=
endif

# GIR settings
GIR_NAMESPACE := Gsurf
GIR_VERSION := $(VERSION_MAJOR).$(VERSION_MINOR)
GIR_FILE := $(GIR_NAMESPACE)-$(GIR_VERSION).gir
TYPELIB_FILE := $(GIR_NAMESPACE)-$(GIR_VERSION).typelib

# Test framework
TEST_CFLAGS := $(CFLAGS) $(shell $(PKG_CONFIG) --cflags glib-2.0)
TEST_LDFLAGS := $(LDFLAGS) -L$(OUTDIR) -lgsurf -Wl,-rpath,$(OUTDIR)

# Module flags (absolute include paths for out-of-tree compilation).
# The build/include/gsurf symlink lets modules use <gsurf/gsurf.h> exactly
# as an installed consumer would.
MODULE_CFLAGS_INC := -I$(CURDIR)/$(BUILDDIR)/include -I$(CURDIR) -I$(CURDIR)/src -I$(CURDIR)/deps/yaml-glib/src -I$(CURDIR)/deps/crispy/src
MODULE_CFLAGS := $(CFLAGS_BASE) $(CFLAGS_BUILD) $(MODULE_CFLAGS_INC) $(CFLAGS_DEPS)
MODULE_LDFLAGS := -shared -fPIC

# MCP module flags (extra includes/libs for mcp-glib).
#
# The vendored mcp-glib archive (built -fPIC) is embedded directly into the
# mcp module in BOTH shared and static builds, so the module never depends
# on an external libmcp-glib-1.0.so — we ship our pinned version, not a
# system one that could deviate. This matches how the other vendored deps/
# are handled: yaml-glib is compiled straight into libgsurf and crispy is
# linked from libcrispy.a, so neither is an external runtime dependency
# either. Only true system libraries (libsoup/libdex/libpng/json-glib/glib)
# stay dynamic.
ifeq ($(MCP_AVAILABLE),1)
MCP_CFLAGS := -I$(CURDIR)/deps/mcp-glib/src $(shell $(PKG_CONFIG) --cflags $(DEPS_MCP) json-glib-1.0 2>/dev/null)
MCP_LDFLAGS := $(CURDIR)/deps/mcp-glib/build/libmcp-glib-1.0.a $(shell $(PKG_CONFIG) --libs $(DEPS_MCP) json-glib-1.0 2>/dev/null)
endif

# Print configuration (for debugging)
.PHONY: show-config
show-config:
	@echo "GSURF Build Configuration"
	@echo "========================="
	@echo "Version:           $(VERSION)"
	@echo "Build type:        $(BUILD_TYPE)"
	@echo "Compiler:          $(CC)"
	@echo "GTK_BACKEND:       $(GTK_BACKEND)"
	@echo "BACKEND_AVAILABLE: $(BACKEND_AVAILABLE)"
	@echo "CFLAGS:            $(CFLAGS)"
	@echo "LDFLAGS:           $(LDFLAGS)"
	@echo "PREFIX:            $(PREFIX)"
	@echo "MODULEDIR:         $(MODULEDIR)"
	@echo "DEBUG:             $(DEBUG)"
	@echo "ASAN:              $(ASAN)"
	@echo "STATIC:            $(STATIC)"
	@echo "BUILD_GIR:         $(BUILD_GIR)"
	@echo "BUILD_TESTS:       $(BUILD_TESTS)"
	@echo "BUILD_MODULES:     $(BUILD_MODULES)"
	@echo "MCP:               $(MCP)"
	@echo "MCP_AVAILABLE:     $(MCP_AVAILABLE)"
	@echo "AI:                $(AI)"

# Distro auto-detection via /etc/os-release
# Override with: make DISTRO=fedora install-deps
DISTRO_ID := $(shell . /etc/os-release 2>/dev/null && echo $$ID)
DISTRO_ID_LIKE := $(shell . /etc/os-release 2>/dev/null && echo $$ID_LIKE)

ifeq ($(DISTRO),)
    ifneq ($(filter fedora,$(DISTRO_ID)),)
        DISTRO := fedora
    else ifneq ($(filter rhel centos,$(DISTRO_ID)),)
        DISTRO := fedora
    else ifneq ($(filter debian ubuntu,$(DISTRO_ID)),)
        DISTRO := debian
    else ifneq ($(filter arch,$(DISTRO_ID)),)
        DISTRO := arch
    else ifneq ($(filter fedora rhel centos,$(DISTRO_ID_LIKE)),)
        DISTRO := fedora
    else ifneq ($(filter debian ubuntu,$(DISTRO_ID_LIKE)),)
        DISTRO := debian
    else ifneq ($(filter arch,$(DISTRO_ID_LIKE)),)
        DISTRO := arch
    endif
endif

# Fedora / RHEL / CentOS (dnf)
FEDORA_DEPS_TOOLS := gcc make pkgconf-pkg-config
FEDORA_DEPS_REQUIRED := glib2-devel libyaml-devel json-glib-devel
FEDORA_DEPS_GTK3 := gtk3-devel webkit2gtk4.1-devel
FEDORA_DEPS_GTK4 := gtk4-devel webkitgtk6.0-devel
FEDORA_DEPS_GIR := gobject-introspection-devel
FEDORA_DEPS_MCP := libsoup3-devel libdex-devel json-glib-devel libpng-devel

# Debian / Ubuntu (apt)
DEBIAN_DEPS_TOOLS := gcc make pkg-config
DEBIAN_DEPS_REQUIRED := libglib2.0-dev libyaml-dev libjson-glib-dev
DEBIAN_DEPS_GTK3 := libgtk-3-dev libwebkit2gtk-4.1-dev
DEBIAN_DEPS_GTK4 := libgtk-4-dev libwebkitgtk-6.0-dev
DEBIAN_DEPS_GIR := gobject-introspection libgirepository1.0-dev
DEBIAN_DEPS_MCP := libsoup-3.0-dev libjson-glib-dev libpng-dev

# Arch Linux (pacman)
ARCH_DEPS_TOOLS := gcc make pkgconf
ARCH_DEPS_REQUIRED := glib2 libyaml json-glib
ARCH_DEPS_GTK3 := gtk3 webkit2gtk-4.1
ARCH_DEPS_GTK4 := gtk4 webkitgtk-6.0
ARCH_DEPS_GIR := gobject-introspection
ARCH_DEPS_MCP := libsoup3 libdex json-glib libpng

# Install build dependencies (auto-detects distro)
.PHONY: install-deps
install-deps:
ifeq ($(DISTRO),fedora)
	sudo dnf install -y $(FEDORA_DEPS_TOOLS) $(FEDORA_DEPS_REQUIRED) \
		$(if $(filter gtk4,$(GTK_BACKEND)),$(FEDORA_DEPS_GTK4),$(FEDORA_DEPS_GTK3)) \
		$(if $(filter 1,$(BUILD_GIR)),$(FEDORA_DEPS_GIR)) \
		$(if $(filter 1,$(MCP)),$(FEDORA_DEPS_MCP))
else ifeq ($(DISTRO),debian)
	sudo apt-get update
	sudo apt-get install -y $(DEBIAN_DEPS_TOOLS) $(DEBIAN_DEPS_REQUIRED) \
		$(if $(filter gtk4,$(GTK_BACKEND)),$(DEBIAN_DEPS_GTK4),$(DEBIAN_DEPS_GTK3)) \
		$(if $(filter 1,$(BUILD_GIR)),$(DEBIAN_DEPS_GIR)) \
		$(if $(filter 1,$(MCP)),$(DEBIAN_DEPS_MCP))
else ifeq ($(DISTRO),arch)
	sudo pacman -S --needed --noconfirm $(ARCH_DEPS_TOOLS) $(ARCH_DEPS_REQUIRED) \
		$(if $(filter gtk4,$(GTK_BACKEND)),$(ARCH_DEPS_GTK4),$(ARCH_DEPS_GTK3)) \
		$(if $(filter 1,$(BUILD_GIR)),$(ARCH_DEPS_GIR)) \
		$(if $(filter 1,$(MCP)),$(ARCH_DEPS_MCP))
else
	$(error Unsupported distro "$(DISTRO_ID)". Override with: make DISTRO=fedora|debian|arch install-deps)
endif
