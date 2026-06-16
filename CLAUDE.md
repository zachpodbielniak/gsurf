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
`GTK_BACKEND=gtk3|gtk4`, `BUILD_GIR=1`, `BUILD_TESTS=0`, `STATIC=1`,
`LRG_BACKEND=1`.

`LRG_BACKEND=1` is **additive** to the GTK backend: it compiles
`src/backend/lrg/**` (the libregnum/raylib backend) alongside the GTK one into
a single `libgsurf`, runtime-selected by `gsurf_backend_set_default_type()`.
The standalone binary picks it with `gsurf --lrg[=2d]` (only 2D today; 3d/3dvr
reserved); cmacs uses it for `emacs --lrg` frames.  The page is rendered to a
GrlTexture and composited by the host (the standalone raylib window, or cmacs's
lrgterm), so under `--lrg` gsurf needs no GTK widget tree.  It links graylib +
raylib from the sibling `../libregnum` checkout (or, in cmacs, the canonical
`liblibregnum.a`).  **Toggling `LRG_BACKEND` between builds needs `make clean`**
(the shared object paths don't see the flag change).

`STATIC=1` links `libgsurf.a` into the `gsurf` binary (no runtime
`libgsurf.so` dependency); system libs (GTK/WebKit/…) stay dynamic since
WebKit can't be statically linked. The binary is linked `--export-dynamic`
with the gsurf+yaml-glib objects `--whole-archive`d, and modules drop their
own `libgsurf` link (`LIBGSURF_LINK=`) so they resolve `gsurf_*`/`yaml_*`
from the executable — one library instance, no duplicate GType registration.
The vendored `deps/` are statically embedded in **every** build (not just
`STATIC=1`): yaml-glib is compiled into `libgsurf`, crispy is linked from
`libcrispy.a`, and mcp-glib is linked from `libmcp-glib-1.0.a` (built
`-fPIC`) into the `mcp` module — so we always ship our pinned versions and
never load an external `libmcp-glib-1.0.so`/`libcrispy.so`. `STATIC=1`
additionally folds `libgsurf` itself into the binary; only true system
libraries (GTK/WebKit/GLib, plus libyaml/libsoup/libdex/libpng) ever stay
dynamic.

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
its `-fPIC` static archive (`libmcp-glib-1.0.a`) is linked into the `mcp` module
(`MCP=1`). All three are statically embedded, so no built artifact depends on a
vendored `deps/` shared object at runtime — the pinned versions ship with us.

On this dev machine: `webkit2gtk-4.1` + `gtk+-3.0` are installed; `webkitgtk-6.0`
(GTK4) is **not** — so `GTK_BACKEND=gtk4` won't build here until it is installed.
`libnotify`/`gcr` are absent; notification/cert features use Gio `GNotification`
and `GTlsCertificate` instead.

## Architecture (mirror these patterns when extending)

- **Abstract derivable bases** with a vfunc table + `gpointer padding[N]` and
  public wrappers that `g_return_if_fail` then call the vfunc:
  `GsurfView` (`src/core/gsurf-view.h`), `GsurfWindow` (`src/window/gsurf-window.h`),
  `GsurfModule` (`src/module/gsurf-module.h`).
- **Backend abstraction**: `src/backend/gtk3/**` (or `gtk4/**`) is the GTK/WebKit
  backend; `src/backend/lrg/**` is the GTK-free libregnum backend. The factory
  `src/core/gsurf-backend.c` was changed from a build-time `#ifdef` to a
  **runtime** factory: `gsurf_view_new_for_backend(type)` /
  `gsurf_window_new_for_backend(type)` / `gsurf_backend_is_available(type)` /
  `gsurf_backend_set_default_type(type)`, with `gsurf_view_new()` etc. as thin
  wrappers over the current default. GTK (gtk3⊕gtk4, mutually exclusive) and LRG
  are *additive* in one `libgsurf`. The abstract API never leaks GTK types
  (`get_native_widget` returns `gpointer`; the LRG view returns `NULL` there and
  exposes a texture + input-injection API instead — `gsurf-lrg-view.h`).
- **LRG backend internals** (`src/backend/lrg/`): `GsurfLrgView` (the shared
  `GsurfView` subclass — renders the page to a `GrlTexture`, injects input) and
  `GsurfLrgWindow` (standalone raylib window + render loop + chrome) sit over a
  build-selected **web-engine seam** (`gsurf-lrg-engine.h`). The engine that
  ships today is **offscreen WebKitGTK** (`gsurf-lrg-engine-webkitgtk.c`): it
  reuses the *same* WebKit already linked by the GTK backend (the WebKit family
  shares `webkit_*` symbols, so only one may be linked per process) by hosting
  the WebView in a `GtkOffscreenWindow` with HW-accel off and reading the page
  back from its cairo surface. `gsurf-lrg-engine-wpe.c` (WPE WebKit, GTK-free
  build) is the reserved seam for a future no-GTK build.
- **Module system**: a module is a `GsurfModule` subclass in `modules/<name>/`
  that exports `G_MODULE_EXPORT GType gsurf_module_register(void)` and implements
  one or more hook interfaces from `src/interfaces/`. `GsurfModuleManager` loads
  `.so`s (GModule), reads each module's `enabled:` flag + config from the YAML
  `modules:` map, activates the enabled ones, and dispatches hooks in priority
  order (first `TRUE`/non-`ALLOW` wins for consumable hooks). The manager never
  loads modules implicitly: a host sets directories via
  `gsurf_module_manager_add_search_path()` / `_set_search_paths()` and calls
  `_load_modules()` (first-match-by-filename across paths, like `$PATH`). The
  `gsurf` binary's `find_module_dir()` env→exe-dir→system fallback is *its* policy,
  not the library's, so embedders (cmacs) load only the modules they point at —
  see `docs/embedding.org`.
- **Config**: `GsurfConfig` (final, public struct) parses YAML via yaml-glib and
  keeps the `modules:` mapping for modules to read their own section
  (`gsurf_config_get_module_node`). A root `ignore_yaml: true` key makes a
  document skip itself (use built-in defaults; same as `--no-yaml-config`).
  `GsurfConfigCompiler` compiles an optional `config.c` to a cached `.so` via
  crispy and calls its `gsurf_config_init()` (overrides YAML). C configs set
  scalar fields directly, bind keys/mouse via `gsurf_config_set_keybind()` /
  `_set_mousebind()` (which normalize), and may layer module/YAML-mapping
  config via `gsurf_config_load_from_data()`. `data/{default,sensible}-config.{yaml,c}`
  are the worked examples.
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
make test                                   # headless GLib tests (53 cases)
make test-gui                               # xvfb-gated GUI smoke test (skips if no xvfb-run)
xvfb-run -a ./build/release/gsurf about:blank   # manual headless GUI smoke test
```

Test layout (`tests/test-*.c`, auto-discovered): `test-version`, `test-enums`
(action<->string, GType registration), `test-keys` (canonicalize/normalize/
match), `test-settings` (defaults/copy/setters), `test-config` (YAML parsing,
keybind lookup, module nodes, `ignore_yaml`, the bind setters with
normalization/NULL/empty, bad-input handling), `test-cconfig` (the crispy
C-config compile-and-load pipeline, incl. the commented-`CRISPY_PARAMS`
regression and bad-source handling; skips with no compiler), `test-manager`
(loading, search paths, enabled gating, priority, input passthrough, default
verdicts for every hook — including the request-filter/status/context-menu/
render-overlay ones — and dispatch through the real module .so files),
`test-modules` (search-engines, history, adblock host blocking + whitelist +
edge URIs + the `content_filters` WebKit-JSON load path), `test-boxed` (boxed
value types: new/copy/free, getters, setters, deep-copy independence). The
GTK/WebKit view+window layer (and module UIs like the find bar / tab strip,
the per-view content-filter *application*, modal key dispatch) needs a display
and web process, so it is exercised by `make test-gui` / ad-hoc Xvfb harnesses
rather than `make test`.

Packaging targets: `make appimage` builds a self-contained AppImage (bundles the
binary, `libgsurf`, all modules, desktop file + 256×256 PNG icon; needs
`appimagetool` on PATH or `APPIMAGETOOL=`). `make install` also installs
`data/gsurf.desktop`, the `data/logo-256.png` icon as `hicolor/256x256/apps/gsurf.png`
(mirroring gst; `data/logo.png` is the 1024² source), and the `data/gsurf.1` man
page. CI lives in
`.forgejo/workflows/` (`ci.yaml`: build + `test` + `test-gui`; `latest.yaml`:
AppImage release).

The `/run`, `/verify`, and `/code-review` skills are available for driving and
reviewing the app.
