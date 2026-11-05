# Embedding libgsurf

Sources: `docs/embedding.org`, `docs/architecture.org`, the GIR-annotated headers under `src/` (installed as `/usr/include/gsurf/`). cmacs embeds gsurf this way (GTK frames, and `emacs --lrg` frames through the LRG backend).

## Build flags

```bash
pkg-config --cflags --libs gsurf      # -I${includedir}/gsurf -lgsurf + glib/gobject/gio/gmodule
```

Include `<gsurf/gsurf.h>` (add `-I${includedir}` too) and the backend's own headers only if you touch GTK/WebKit directly. `make BUILD_GIR=1 gir` produces `Gsurf-<MAJOR>.<MINOR>.gir` / `.typelib` for introspected languages; the abstract API exposes no GTK or WebKit types.

## Minimal host

```c
#include <gsurf/gsurf.h>

int
main(
	int	argc,
	char	**argv
){
	g_autoptr(GError) error = NULL;
	GsurfConfig *config;
	GsurfApplication *app;
	GsurfWindow *window;
	GsurfView *view;

	gsurf_init(&argc, &argv);
	if (!gsurf_backend_init(&argc, &argv, &error)) {
		g_printerr("gsurf: %s\n", error->message);
		return 1;
	}
	config = gsurf_config_new();
	gsurf_config_set_default(config);
	app = gsurf_application_new(config);
	window = gsurf_window_new();
	view = gsurf_view_new();
	gsurf_window_add_view(window, view);
	gsurf_application_add_window(app, window);
	gsurf_view_load_uri(view, "https://example.org");
	gsurf_window_present(window);
	gsurf_application_run(app);     /* or drive your own GLib main loop */
	return 0;
}
```

The library uses the default GLib main context; it makes no GTK-main assumption, so a host loop integrates it.

## Things the library never does by itself

| Concern | The host does |
|---|---|
| Load modules | `gsurf_module_manager_set_application()`, `_set_config()`, `_add_search_path(dir)` (or `_set_search_paths`), `_load_modules()`, then `_activate_all()` after the window and view exist. `GSURF_MODULE_PATH` and the system module directory are the `gsurf` binary's policy, never consulted by the library. `gsurf_module_manager_get_system_module_dir()` returns the system directory for hosts that opt in. |
| Control socket (`gsurfctl`) | `gsurf_control_server_new()` and `_start()` after modules are active; stop it before teardown. No server, no `gsurfctl` access. See [control-protocol.md](control-protocol.md). |
| Change the working directory | `gsurf_runtime_dir_enter()` before the first view if you want WebKit's relative writes kept out of your directory. |
| Kiosk | set `config->kiosk = TRUE` before creating windows and views; enforce your own action policy with `gsurf_kiosk_allows_action()`. |
| Actions from keys | register an executor with `gsurf_module_manager_set_action_executor()` so `execute_action` (MCP `run_action`, `gsurfctl action`) can perform built-in actions. |

## Views and windows

- Identity: `gsurf_view_get_id()` (stable, never reused) and `gsurf_view_lookup(id)`; `gsurf_view_get_opener()` / `_get_opener_id()`; `gsurf_view_get_container_id()` with `gsurf_container_registry_lookup()` for name and colour.
- Create views with `gsurf_view_new_full(opener, container_id, initial_uri, related)`: `initial_uri` is a placement hint for the containers policy (not loaded); `related = TRUE` for engine popups, which must share the opener's session.
- Signals on `GsurfView`: `load-changed`, `uri-changed`, `title-changed`, `progress-changed`, `hovered-uri-changed`, `favicon-changed`, `web-process-terminated`, `script-message::<name>` (from `gsurf_view_register_script_message()`), `new-view-requested (uri, background) -> gboolean`, `create-view (uri) -> GsurfView*` (return a new *unrealized* related view, or `NULL` to block and open the URI yourself).
- Native widgets: `gsurf_view_get_native_widget()` / `gsurf_window_get_native_widget()` return the `GtkWidget *` as `gpointer`; on GTK backends the view's widget is the `WebKitWebView`. The LRG view returns `NULL` there and exposes a texture plus input injection (`gsurf-lrg-view.h`).
- Moving a page to another container: `gsurf_window_reopen_view()` (new view, same place, old one removed; emits `view-replaced`).
- Membership is by identity: adding a view or window twice is a no-op; removing the active view selects the next one and emits `active-view-changed` (possibly with `NULL`).

## Window layouts (panes)

To show several views at once, implement `GsurfWindowLayout` and install it with `gsurf_window_set_layout(window, layout)` (returns `FALSE` in kiosk mode and on the LRG backend). The vfuncs a minimal layout implements: `get_root`, `detach` (and optionally `attach`), `add_view` (must not focus), `remove_view`, `activate_view` (must focus), `is_view_visible`. Call `gsurf_window_layout_notify_visible(window)` when the visible set changes. Optional: `list_groups` / `select_group` / `rename_group` (+ `gsurf_window_layout_notify_groups()`), `show_view_at` and `begin_view_drag` / `end_view_drag` (drops of `GSURF_LAYOUT_VIEW_MIME_TYPE`), `serialize` / `restore` (session), and `list_panes` / `run_command` / `describe` (what `gsurfctl panes`, `gsurfctl wmux` and MCP use). `docs/embedding.org` has a complete two-pane GTK4 layout; wmux is the full implementation.

## Configuration in a host

Build a `GsurfConfig` with `gsurf_config_new()` (built-in defaults only), then `gsurf_config_load_from_file()` / `_load_from_data()` for YAML (the `modules:` mapping is replaced, not merged), and `gsurf_config_set_default()` so modules and C configs find it. A host may compile a user `config.c` with `gsurf_config_compiler_new()` + `gsurf_config_compiler_compile_and_load()`.

## Embedding gotchas

- Only one WebKit family can be linked per process: GTK3 builds link webkit2gtk-4.1, GTK4 builds webkitgtk-6.0. The host must use the same GTK major version.
- LRG (`LRG_BACKEND=1`) is additive: `gsurf_backend_set_default_type(GSURF_BACKEND_LRG)` before `gsurf_backend_init()`. An embedder with its own libregnum builds gsurf with `LIBREGNUM_DIR=<its copy>` so only one graylib/raylib is linked.
- `create-view` handlers must not return a realized or parented view.
- Downloads and permission requests are answered synchronously: decide from a policy or keep a reference and answer later; never run a nested modal loop in the callback.
