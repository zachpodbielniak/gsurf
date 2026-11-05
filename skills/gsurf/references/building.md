# Building and testing gsurf

Sources: `AGENTS.md`, `docs/quickstart.org`, `config.mk` (flags, `install-deps` package lists), `Makefile`, `rules.mk`.

## Get the tree

```bash
git clone --recursive "$GSURF_REMOTE" gsurf   # ask the user for the remote; most work starts from an existing checkout
git submodule update --init --recursive    # deps/yaml-glib, deps/crispy, deps/mcp-glib, deps/libregnum
```

Feature and fix work happens in a worktree under `trees/`: `git worktree add trees/<name> -b <type>/<name> master`.

## Fedora packages

The user's machines run an immutable Fedora image (Immutablue / Hyacinth Macaw). **List the missing packages so the user can layer them into the image build; never run `dnf install`, `sudo make install-deps` or `bootc usr-overlay` on the host without asking.** `make check-deps` reports what pkg-config cannot find for the chosen backend.

| Build | Fedora packages |
|---|---|
| tools | `gcc`, `make`, `pkgconf-pkg-config` |
| core (always) | `glib2-devel`, `libyaml-devel`, `json-glib-devel` |
| `GTK_BACKEND=gtk3` (default) | `gtk3-devel`, `webkit2gtk4.1-devel` |
| `GTK_BACKEND=gtk4` | `gtk4-devel`, `webkitgtk6.0-devel` (pulls `javascriptcoregtk6.0-devel`) |
| `BUILD_GIR=1` | `gobject-introspection-devel` |
| `MCP=1` | `libsoup3-devel`, `libdex-devel`, `libpng-devel` |
| `LRG_BACKEND=1` | `mesa-libGL-devel`, `libX11-devel`, `libXrandr-devel`, `libXinerama-devel`, `libXcursor-devel`, `libXi-devel`, `fontconfig-devel`, plus `wayland-devel`, `libxkbcommon-devel` for graylib's Wayland support |
| `make test-gui` | an `xvfb-run` on `PATH` (`xorg-x11-server-Xvfb` provides it), or any wrapper with that name that runs a command on a private display |
| `make appimage` | `appimagetool` on `PATH` (or `APPIMAGETOOL=`) |

## Targets and flags

```bash
make                        # lib + gsurf + modules (+ gsurfctl), GTK3, build/release
make test                   # headless GLib tests (+ shell tests: build config, skill, skill install)
make test-gui               # WebKit GUI tests under xvfb-run (skips if it is missing)
make BUILD_GIR=1 gir        # Gsurf-<MAJOR>.<MINOR>.gir / .typelib
make show-config            # resolved flags
make compile-commands       # compile_commands.json for clangd
make install PREFIX=/usr    # lib, headers, gsurf.pc, binaries, modules, desktop file, icon, man page
make install-skill          # this skill into ~/.agents/skills/gsurf (+ ~/.claude, ~/.grok links)
```

| Flag | Effect |
|---|---|
| `DEBUG=1` | `-g -O0`, output in `build/debug` |
| `ASAN=1` | AddressSanitizer + UBSan (with `DEBUG=1`) |
| `GTK_BACKEND=gtk4` | WebKitGTK 6.0 / GTK4 instead of WebKit2GTK 4.1 / GTK3 (the two are mutually exclusive in one build) |
| `BUILDDIR=build-gtk4` | separate tree, so GTK3 and GTK4 builds do not clobber each other |
| `LRG_BACKEND=1` | adds the libregnum/raylib backend (`gsurf --lrg`); needs the recursive `deps/libregnum` checkout or `LIBREGNUM_DIR=<path>`; a missing `graylib.h` is a hard error |
| `MCP=1` | builds `modules/mcp` (vendored mcp-glib, statically linked into the module) |
| `STATIC=1` | links `libgsurf` into the binary; modules resolve symbols from it |
| `BUILD_GIR=1` | also generate GIR in `all` |
| `BUILD_TESTS=0`, `BUILD_MODULES=0` | skip them |

Changing a flag needs no `make clean`: the configuration is recorded in `build/<mode>/obj/.build-flags` and a change rebuilds once. `make clean all -j` cleans first, then builds.

## The release gate (zero warnings, both toolkits)

```bash
make clean all && make test && make test-gui && make BUILD_GIR=1 gir
make GTK_BACKEND=gtk4 BUILDDIR=build-gtk4 clean all
make GTK_BACKEND=gtk4 BUILDDIR=build-gtk4 test test-gui
make BUILD_GIR=1 GTK_BACKEND=gtk4 BUILDDIR=build-gtk4 gir
```

Plus `make MCP=1 all test` and `make LRG_BACKEND=1 all test` when those parts changed, and `make DEBUG=1 ASAN=1 test test-gui` after a release. Any compiler warning is a bug: fix the code, never lower the warning flags. `grep -ci warning` over the build log should print 0.

## Tests

- `tests/test-*.c` are auto-discovered GLib test programs; `make test` runs them all plus `tests/test-build-config.sh`, `tests/test-skill.sh` and `tests/test-install-skill.sh`.
- GUI tests (`test-*-webkit`, `test-backend-settings`, `test-chrome-lifecycle`, `test-session`, …) need `GSURF_TEST_GUI=1` and a display; `make test-gui` sets both and forces `GDK_BACKEND=x11` under `xvfb-run`. Each new GUI test is added by hand to the `test-gui` target and recipe.
- Run one test: `LD_LIBRARY_PATH=build/release build/release/test-config -p /gsurf/config/...`.
- WebKitGTK 6 GUI tests must be started by a *relative* path from the repo root (an absolute argv[0] breaks the bubblewrap sandbox); `make test-gui` does this.
- Every behaviour change: a test and the `docs/*.org` update in the same commit (`AGENTS.md`). New public API: GIR annotations and a test asserting finalization (`g_object_add_weak_pointer`) for lifetime-sensitive objects.

## Headless GUI testing without Xvfb

A wrapper named `xvfb-run` first on `PATH` can run the command under `dbus-run-session` inside `mutter --headless --wayland --no-x11 --virtual-monitor 1280x800 --wayland-display=<unique>`, exporting `WAYLAND_DISPLAY`, `GDK_BACKEND=wayland`, `NO_AT_BRIDGE=1`, `GTK_A11Y=none`. GTK4 runs under it print harmless xdg-desktop-portal screencast errors and, rarely, a flaky "Duplicate call to gdk_frame_timings_presented()" abort that passes on a rerun.

## Running from the tree

```bash
build/release/gsurf https://example.org          # finds build/release/modules by itself
build/release/gsurf --c-config ~/.config/gsurf/config.c about:blank
GSURF_MODULE_PATH=$PWD/build/release/modules build/release/gsurf --no-c-config about:blank
build/release/gsurfctl list
```

## Versioning

The version lives in `config.mk` (`VERSION_MAJOR/MINOR/MICRO`) and the `data/gsurf.1` header; every landing on master that changes the build bumps (micro for fixes and small features, minor for a new subsystem or a breaking change), is tagged `vX.Y.Z` and pushed. Feature branches never bump; only the merge to master does. Never push or tag unless the user asks.
