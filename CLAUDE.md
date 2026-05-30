# CLAUDE.md — gsurf

Guidance for Claude Code (and humans) working in this repository.

## What gsurf is

A GLib/GObject web browser — a port of suckless **surf** following the exact
architecture of the sibling **gst** project (a GObject port of `st`). It is
**library-first**: nearly all logic lives in `libgsurf.so`; the `gsurf` binary is
a thin `main.c`. surf "patches" are implemented as runtime-loaded `.so` **modules**.

## Build

Three-file Makefile system (mirrors gst):

- `config.mk` — version, dirs, flags, dependency detection, `install-deps`.
- `rules.mk` — pattern rules, lib/binary/GIR/install rules, embedded-config
  generation, the `build/include/gsurf -> src` dev symlink.
- `Makefile` — driver; auto-discovers `src/**/*.c` via wildcards.

Key flags: `DEBUG=1`, `ASAN=1`, `MCP=1`, `AI=1` (reserved/deferred),
`GTK_BACKEND=gtk3|gtk4`, `BUILD_GIR=1`, `BUILD_TESTS=0`, `STATIC=1`.

`STATIC=1` links `libgsurf.a` into the `gsurf` binary (no runtime
`libgsurf.so` dependency); system libs (GTK/WebKit/…) stay dynamic since
WebKit can't be statically linked. The binary is linked `--export-dynamic`
with the gsurf+yaml-glib objects `--whole-archive`d, and modules drop their
own `libgsurf` link (`LIBGSURF_LINK=`) so they resolve `gsurf_*`/`yaml_*`
from the executable — one library instance, no duplicate GType registration.

```sh
make                 # build lib + binary + modules (GTK3 default)
make MCP=1           # also build the mcp module (needs libsoup-3.0 libdex-1 libpng)
make test            # GLib test suite
make compile-commands # regenerate compile_commands.json for clangd
make show-config     # dump resolved configuration
```

Vendored deps build into `libgsurf`: **yaml-glib** sources are compiled in;
**crispy** is built via its own Makefile and its static archive is merged into
`libgsurf` (used by the C-config compiler). **mcp-glib** is built separately and
linked only by the `mcp` module (`MCP=1`).

On this dev machine: `webkit2gtk-4.1` + `gtk+-3.0` are installed; `webkitgtk-6.0`
(GTK4) is **not** — so `GTK_BACKEND=gtk4` won't build here until it is installed.
`libnotify`/`gcr` are absent; notification/cert features use Gio `GNotification`
and `GTlsCertificate` instead.

## Architecture (mirror these patterns when extending)

- **Abstract derivable bases** with a vfunc table + `gpointer padding[N]` and
  public wrappers that `g_return_if_fail` then call the vfunc:
  `GsurfView` (`src/core/gsurf-view.h`), `GsurfWindow` (`src/window/gsurf-window.h`),
  `GsurfModule` (`src/module/gsurf-module.h`).
- **Backend abstraction**: only `src/backend/gtk3/**` (or `gtk4/**`) touches
  GTK/WebKit. `src/core/gsurf-backend.c` is the single `#ifdef GSURF_BACKEND_*`
  bridge with the `gsurf_view_new()` / `gsurf_window_new()` factories. The
  abstract API never leaks GTK types (`get_native_widget` returns `gpointer`).
- **Module system**: a module is a `GsurfModule` subclass in `modules/<name>/`
  that exports `G_MODULE_EXPORT GType gsurf_module_register(void)` and implements
  one or more hook interfaces from `src/interfaces/`. `GsurfModuleManager` loads
  `.so`s (GModule), reads each module's `enabled:` flag + config from the YAML
  `modules:` map, activates the enabled ones, and dispatches hooks in priority
  order (first `TRUE`/non-`ALLOW` wins for consumable hooks).
- **Config**: `GsurfConfig` (final, public struct) parses YAML via yaml-glib and
  keeps the `modules:` mapping for modules to read their own section
  (`gsurf_config_get_module_node`). `GsurfConfigCompiler` compiles an optional
  `config.c` to a cached `.so` via crispy and calls its `gsurf_config_init()`.
- **Enums** live in `src/gsurf-enums.{h,c}` (registered GTypes). `GsurfAction`
  nicks are the keybind action strings (`gsurf_action_from_string`).

## Adding a module

1. `mkdir modules/<name>` and copy `modules/search_engines/Makefile` (it derives
   the `.so` name from the directory).
2. Write `gsurf-<name>-module.c`: subclass `GSURF_TYPE_MODULE` with
   `G_DEFINE_FINAL_TYPE_WITH_CODE(... G_IMPLEMENT_INTERFACE(...))`, implement the
   relevant hook interface(s), read options in `configure()` via
   `gsurf_config_get_module_node()`, and export `gsurf_module_register()`.
3. Add an `enabled:` block (plus options) under `modules:` in
   `data/default-config.yaml`, and a row in `builtin_modules[]` in `src/main.c`
   (drives `--list-modules`).
4. `make` (modules are auto-discovered) and add a test under `tests/`.

`modules/search_engines/` is the reference implementation; `modules/mcp/` shows a
module that runs its own server lifecycle on `activate()`.

## Conventions

- C: `-std=gnu11`, tabs, GLib style, AGPL-3.0 SPDX headers, `gsurf_`/`Gsurf` prefixes.
- Keep core backend-agnostic; put GTK/WebKit only under `src/backend/<backend>/`.
- clangd false-positives ("glib.h not found") clear after `make compile-commands`.

## Tests / running

```sh
make test                                   # headless GLib tests (45 cases)
make test-gui                               # xvfb-gated GUI smoke test (skips if no xvfb-run)
xvfb-run -a ./build/release/gsurf about:blank   # manual headless GUI smoke test
```

Test layout (`tests/test-*.c`, auto-discovered): `test-version`, `test-enums`
(action<->string, GType registration), `test-keys` (canonicalize/normalize/
match), `test-settings` (defaults/copy/setters), `test-config` (YAML parsing,
keybind lookup, module nodes, bad-input handling), `test-manager` (loading,
enabled gating, priority, input passthrough, default verdicts, and every hook
dispatch through the real module .so files), `test-modules` (search-engines/
history/adblock), `test-boxed` (the boxed value types: new/copy/free, getters,
setters, deep-copy independence). The GTK/WebKit view+window layer needs a
display and web process, so it is exercised by `make test-gui` / ad-hoc Xvfb
harnesses rather than `make test`.

Packaging targets: `make appimage` builds a self-contained AppImage (bundles the
binary, `libgsurf`, all modules, desktop file + SVG icon; needs `appimagetool` on
PATH or `APPIMAGETOOL=`). `make install` also installs `data/gsurf.desktop`, the
`data/gsurf.svg` hicolor icon, and the `data/gsurf.1` man page. CI lives in
`.forgejo/workflows/` (`ci.yaml`: build + `test` + `test-gui`; `latest.yaml`:
AppImage release).

The `/run`, `/verify`, and `/code-review` skills are available for driving and
reviewing the app.
