# C configuration (`config.c`)

Sources: `docs/c-config.org`, `data/default-config.c`, `data/sensible-config.c`, `src/config/gsurf-config.h`, `src/config/gsurf-config-compiler.c`.

## How it runs

- Path: `--c-config PATH`, else `$GSURF_CONFIG_C`, else `$XDG_CONFIG_HOME/gsurf/config.c`, else `/etc/gsurf/config.c`. `--no-c-config` skips it.
- gsurf compiles it with the vendored crispy into `$XDG_CACHE_HOME/gsurf/cconfig/<sha256>.so` (recompiled only when the source changes), opens it, and calls `gboolean gsurf_config_init(void)`.
- It runs **after** the built-in and user YAML, before modules are loaded and configured, so it overrides both.
- Failure (no gcc, a compile error, `gsurf_config_init` returning `FALSE`) logs `C config '<path>': <error>` and gsurf continues with the YAML configuration.
- `#define CRISPY_PARAMS "..."` (a real define, not in a comment) adds compiler and linker flags. gsurf adds the include paths of its *development* tree; an installed gsurf whose headers live in `/usr/include/gsurf` needs `#define CRISPY_PARAMS "-I/usr/include/gsurf"` (the headers include each other as `"config/gsurf-config.h"`).

## The API a config uses

`GsurfConfig *config = gsurf_config_get_default();` then assign fields. Free a string before replacing it.

| Field | Type | YAML equivalent |
|---|---|---|
| `homepage`, `new_view_uri` | `gchar *` | `browser.homepage`, `browser.new_view_uri` |
| `user_agent` | `gchar *` | `user_agent` (also call `gsurf_settings_set_user_agent(config->settings, ua)`) |
| `window_title`, `window_width`, `window_height` | `gchar *`, `gint` | `window.title`, `window.geometry` |
| `default_zoom`, `smooth_scroll`, `fullscreen_on_start` | `gdouble`, `gboolean` | `browser.*` |
| `cookie_jar`, `data_dir`, `cache_dir`, `enable_disk_cache` | `gchar *`, `gboolean` | `storage.*` |
| `proxy_mode` (`default`/`none`/`custom`), `proxy_uri`, `do_not_track`, `tls_strict` | | `network.*`, `tls.strict` |
| `settings->javascript`, `images`, `webgl`, `webaudio`, `media`, `media_stream`, `webrtc`, `plugins`, `smooth_scrolling`, `caret_browsing`, `dns_prefetch`, `hyperlink_auditing`, `site_quirks`, `developer_extras`, `js_can_open_windows`, `js_can_access_clipboard` | `gboolean` | `webkit.*` (note the `js_` prefix for the two `javascript_can_*` keys) |
| `settings->default_font_size`, `default_monospace_font_size` | `guint` | `webkit.*` |
| charset | `gsurf_settings_set_default_charset(settings, "UTF-8")` | `webkit.default_charset` |

Bindings: `gsurf_config_set_keybind(config, "Ctrl+r", GSURF_ACTION_RELOAD)` and `gsurf_config_set_mousebind(config, "Button8", GSURF_ACTION_BACK)` normalise the key string like the YAML loader; `GSURF_ACTION_NONE` removes a binding. Do not insert into `config->keybinds` directly: unnormalised keys never match. Action enums: `GSURF_ACTION_` + the action name in upper case with `_` ([keybindings-and-modes.md](keybindings-and-modes.md)).

## Module options: the embedded-YAML pattern

Modules have no C struct; each reads `modules.<name>` from the YAML mapping. A C config embeds that YAML as a string and loads it:

```c
static const char *MODULES_YAML =
"modules:\n"
"  modal:\n"
"    enabled: true\n"
"    hint_font_size: 15\n"
"  tabs:\n"
"    enabled: true\n"
"    strip: false\n";

/* ... at the end of gsurf_config_init(): */
return gsurf_config_load_from_data(config, MODULES_YAML, -1, NULL);
```

Rules:

- `gsurf_config_load_from_data()` **replaces the whole `modules:` mapping**. Every module the user wants must be in the string with `enabled: true`; anything left out turns off. Start from `data/sensible-config.c` or the template, which list a full set.
- Return its result, so a YAML syntax error fails the C config visibly instead of leaving a half-configured browser.
- The string is C: escape `"` as `\"` and every backslash once more. Inside YAML single quotes a backslash is literal, so the YAML pattern `'(^|\.)example\.com$'` is written `"          - '(^|\\.)example\\.com$'\n"` in C.
- Indentation is YAML indentation inside the string: two spaces per level, every line ends with `\n`.
- A comment between string literals (`/* why */`) is fine: adjacent literals concatenate.
- Core sections can go in the same string (`"keybinds:\n  \"Ctrl+q\": stop\n"`), but struct assignments are clearer for scalars.

## Editing a user's `config.c`

1. Read the whole file first. Find the embedded `modules:` string and the struct assignments; follow the file's existing style (tabs, `/* */` comments, section banners).
2. Module changes go into the string, at the module's existing section; add a new module as a new section with `enabled: true`.
3. Do not move settings between the YAML file and `config.c` unless asked; say which file now controls what.
4. Compile it without starting the browser (the same flags gsurf uses in a source tree):
   ```bash
   gcc -std=gnu11 -fsyntax-only -Wall -Wextra \
       -I"$GSURF_SRC/build/include" -I"$GSURF_SRC/build/include/gsurf" \
       -I"$GSURF_SRC/deps/yaml-glib/src" $(pkg-config --cflags glib-2.0 gio-2.0 gmodule-2.0) \
       ~/.config/gsurf/config.c
   ```
   With an installed gsurf use `-I/usr/include/gsurf -I/usr/include` instead. Then restart gsurf and check stderr for `C config` and module warnings.
5. Never add personal data (domains, tokens, card data) that the user did not ask for.

## Worked example

[../templates/config.c.template](../templates/config.c.template) is a complete config: engine defaults, TLS strictness, key and mouse bindings, and a full modules block (modal, chromebar, search_engines, tabs with the tree sidebar, containers, history, find_bar, downloads, cookie_policy, toggles, homepage, site_styles, notifications). Copy it to `~/.config/gsurf/config.c`, then trim.

## Gotchas

- `ignore_yaml: true` in `config.yaml` skips that YAML file, not the built-in defaults; the C config still runs. Use it to make `config.c` the only user config.
- The cached `.so` is keyed by the source hash: editing the file is enough; nothing to clean.
- gsurf runs `gsurf_config_init` before modules exist: do not call module or window functions there.
- A C config is trusted code running in the browser process. Only install one the user wrote or reviewed.
