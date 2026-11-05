# Writing a module

Sources: `docs/module-system.org`, `docs/modules.org`, `AGENTS.md` ("Adding a module"), `src/module/gsurf-module.h`, `src/interfaces/*.h`. Reference implementations: `modules/search_engines/` (small, `GsurfUriHandler`), `modules/mcp/` (runs a server from `activate()`), `modules/adblock/` and `modules/hardening/` (ship a WebKit web-process extension), `modules/tabs/` and `modules/wmux/` (GTK chrome on both toolkits through `modules/include/gsurf-chrome-compat.h`).

## What a module is

A `.so` in a module directory exporting `G_MODULE_EXPORT GType gsurf_module_register(void)`, which returns a `GsurfModule` subclass. The subclass implements one or more hook interfaces; the manager dispatches each hook to the active modules in priority order.

Lifecycle: load (`gsurf_module_register`) → `configure(config)` → the manager reads `modules.<name>.enabled` (default **off**) → `activate()` for enabled modules → hooks while `gsurf_module_is_active()` → `deactivate()` at shutdown. `configure()` can run again (reconfigure): start from the defaults each time so it replaces the old state, and keep a failed parse all-or-nothing (load into a fresh object, swap on success).

`GsurfModuleClass` vfuncs: `get_name` (required: the `modules.<name>` key), `get_description`, `configure(self, gpointer config)` (a `GsurfConfig *`), `activate` (return `TRUE` on success), `deactivate`. Priority: `gsurf_module_set_priority()`; **lower runs first** (`GSURF_PRIORITY_HIGHEST` 0 … `GSURF_PRIORITY_LOWEST` 100; wmux uses -10, modal claims `HIGHEST` in `activate()`).

## Hook interfaces (`src/interfaces/`)

| Interface | vfunc | Dispatch |
|---|---|---|
| `GsurfInputHandler` | `handle_key_event(view, keyval, keycode, state, mode)`, `handle_mouse_event(view, button, state)` | first `TRUE` consumes the event |
| `GsurfActionHandler` | `handle_action(view, action)` | core actions with no built-in implementation; first `TRUE` wins |
| `GsurfKeybindProvider` | `list_keybinds(entries)`: append `gsurf_keybind_help_append(entries, key, description, "name", action)` with the *current* keys | additive (`?` overlay) |
| `GsurfUriHandler` | `rewrite_uri(input)` → new string or `NULL` | first non-`NULL` wins |
| `GsurfNavigationHook` | `before_navigate`, `decide_main_response` (return a `GsurfPolicyDecision`), `after_navigate`, `load_changed` | `before_*`: veto; others: all |
| `GsurfRequestFilter` | `filter_request(view, uri, &redirect_uri)` → `GsurfFilterVerdict` | first non-allow wins |
| `GsurfScriptInjector` | `inject(view, uri)`: once per view at construction (`uri` is `NULL`); user scripts added then persist across loads | all |
| `GsurfSettingProvider` | `apply_settings(view, settings, uri)` | all, in order |
| `GsurfPermissionHandler` | `decide(view, type, origin)` → `GsurfPermissionVerdict` (synchronous; never block) | first non-prompt wins |
| `GsurfDownloadHandler` | `decide_destination(uri, suggested)` → path | first non-`NULL` |
| `GsurfCertHandler` | `verify(host, tls_errors)` → `GsurfTlsDecision` | first non-prompt wins |
| `GsurfStatusProvider` | `status_text(view)` | first non-`NULL` |
| `GsurfContextMenuProvider` | `populate(hit, items)` | additive |
| `GsurfRenderOverlay` | `render(view, draw_target)` | all |
| `GsurfTabTreeProvider` | `get_parent`, `get_collapsed`, `set_collapsed`, `move_view` | not dispatched: looked up by consumers (mcp, control) |

Window chrome or panes: `gsurf_window_add_side_widget()`, `GsurfWindowLayout` (see [embedding.md](embedding.md)), `gsurf_window_set_key_grab()`. Modes: `gsurf_module_manager_set_mode()` / `get_mode()`. Perform any action like its key: `gsurf_module_manager_execute_action(mgr, window, view, action, &error)` (kiosk refusals come back as `G_IO_ERROR_PERMISSION_DENIED`). Control methods for `gsurfctl`: implement the optional `GsurfControlProvider` interface (see [control-protocol.md](control-protocol.md)).

## Shared helpers (`src/util/`)

- `GsurfHostRules`: ordered case-insensitive host regexes with optional per-rule YAML data. `gsurf_host_rules_load(rules, node, "match", &error)`, `_find`, `_lookup`, `_lookup_all`, `_to_json` (for page scripts), `_to_variant` (for web-process extensions); `gsurf_host_from_uri()`.
- Private files: `gsurf_private_mkdir()` (0700), `gsurf_private_write()` (atomic, 0600), `gsurf_private_file_is_private()`, `gsurf_expand_home()`. Use them for anything that holds user data.
- `gsurf_keys_normalize()` / `gsurf_keys_match(keyval, state, key)` for configurable keys.
- Web-process work: `gsurf_web_process_extension_register("name", path, init_data)` from `configure()`, before the first view exists.

## Steps (in the gsurf tree)

1. Work in a worktree: `git worktree add trees/feat-<name> -b feat/<name> master`.
2. `mkdir modules/<name>`; copy [../templates/module.mk.template](../templates/module.mk.template) to `modules/<name>/Makefile` (the `.so` takes the directory name) and [../templates/module.c.template](../templates/module.c.template) to `modules/<name>/gsurf-<name>-module.c`; rename `quicklinks` / `Quicklinks` / `QUICKLINKS`.
3. Pick the interfaces; add each with `G_IMPLEMENT_INTERFACE` in `G_DEFINE_FINAL_TYPE_WITH_CODE`. Read options in `configure()` with `gsurf_config_get_module_node(config, "<name>")` and the `yaml_mapping_*` getters. Warn (`g_warning`) on bad values naming the key, and keep the default.
4. Add `modules.<name>` with `enabled:` and every option to `data/default-config.yaml`, and a row to `builtin_modules[]` in `src/main.c` (kept sorted; `--list-modules` prints it).
5. Write `tests/test-<name>.c` (headless; auto-discovered): load the real `.so` like `tests/test-manager.c` (`mgr_with()`), and cover the defaults, every key, a second `configure()` returning to defaults, and every bad input (bad regex, missing file, wrong type). Anything that touches pages or the network also gets `tests/test-<name>-webkit.c`, gated on `GSURF_TEST_GUI=1` with a libsoup loopback server (`tests/test-adblock-webkit.c`), added by hand to the `test-gui` target and recipe in `Makefile`.
6. Write `docs/<name>.org` (org-mode, no hard wrap) and add a row to `docs/modules.org`; link it from `docs/index.org`; update this skill's [modules-catalog.md](modules-catalog.md).
7. Build and test on both toolkits with zero warnings ([building.md](building.md)).
8. GTK code: use `gsurf-chrome-compat.h` helpers so one source builds on GTK3 and GTK4; disconnect every signal and detach widgets in `deactivate()` and `dispose`, both idempotent.

## Out of the tree

Build against a source tree's flags and point gsurf at the output directory:

```bash
make -C ~/source/projects/gsurf                      # builds build/release and build/include
cd ~/src/quicklinks                                  # Makefile + gsurf-quicklinks-module.c
make OUTDIR="$PWD/out" LIBDIR="$HOME/source/projects/gsurf/build/release" \
     CFLAGS="$(make -s -C ~/source/projects/gsurf print-MODULE_CFLAGS)" LDFLAGS="-shared -fPIC"
GSURF_MODULE_PATH="$PWD/out" gsurf about:blank
```

`GSURF_MODULE_PATH` is **one directory** and replaces the default module directory for the `gsurf` binary, so copy or link the built-in modules into it too, or install the module next to them (`MODULEDIR`, default `$(PREFIX)/lib64/gsurf/modules` on Fedora). Embedders choose module directories themselves (`gsurf_module_manager_add_search_path()`).

## Checklist

- `get_name()` matches the directory name and the YAML key.
- No GTK or WebKit outside `#ifdef GSURF_BACKEND_GTK3/4` or the compat header; core stays backend-agnostic.
- Every string you keep is `g_strdup`ed; `finalize` frees it; `g_autoptr`/`g_autofree` for locals.
- GIR-style comments on anything public; `/* */` comments; tabs; `gsurf_<name>_` / `Gsurf<Name>` prefixes.
- Kiosk: never open views in kiosk mode (`gsurf_kiosk_is_enabled()`).
