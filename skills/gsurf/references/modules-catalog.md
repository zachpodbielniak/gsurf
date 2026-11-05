# Modules catalog

All 33 modules, what they do, and every key each one reads under `modules.<name>`. "On" means the shipped default config sets `enabled: true`; a module with no `enabled: true` in the merged config stays off. `gsurf --list-modules` prints the same names. Keys are read at startup (each `configure()` call); restart after editing.

**Unread keys.** `data/default-config.yaml` lists some keys that no code reads. They are marked *unread* below: setting them changes nothing. Do not suggest them as fixes.

Key strings use the syntax in [keybindings-and-modes.md](keybindings-and-modes.md). `""` unbinds a module key where noted. Host patterns are case-insensitive regexes over the bare host: `(^|\.)example\.com$`.

## Navigation and input

- **search_engines** (on). Prefix searches in the address bar; bare words go to the default engine.
  - `default` (engine name), `default_is_search` (`true`: words that are not a URI search the default), `engines`: `NAME: { prefix: "g ", url: "https://…?q=%s" }`.
  - The longest matching prefix wins; prefixes are literal and case-sensitive (keep the trailing space). A `url` without `%s` gets the query appended. `engines` replaces the previous set on each configure.
  - The top-level `search_engines:` section (outside `modules:`) is not read; use `modules.search_engines`.
- **chromebar** (on). The address bar; non-URI input goes to search. Performs `open-prompt`.
  - `key_focus` (`Ctrl+l`), `mode_indicator` (`true`), `mode_hide_normal` (`false`), `mode_normal_color` (`#89b4fa`), `mode_insert_color` (`#a6e3a1`), `mode_follow_color` (`#f9e2af`).
  - *Unread:* `position`, `bg_color`, `fg_color`, `accent_color`.
- **modal** (on). Vim NORMAL/INSERT modes and vimium link hints; claims the highest priority.
  - `start_mode` (`normal` | `insert`), `insert_keys` (`[i, a]`; `[]` = only the action), `scroll_step` (`40` px), `hint_key` (`f`), `hint_key_newview` (`F`), `hint_chars` (`asdfghjkl`), `hint_bg` (`#ffd700`), `hint_fg` (`#000000`), `hint_font_size` (`11`, positive integer, CSS px).
  - *Unread:* `hint_alpha`.
- **find_bar** (on). Find in page; performs `find`.
  - `key_open` (`slash`), `key_next` (`n`), `key_prev` (`N`), `match_case` (`false`). *Unread:* `wrap_around`, `highlight_color`.
- **toggles** (on). Keys that flip engine settings at runtime.
  - `notify_on_toggle` (`true`: log "gsurf: NAME on/off"), `keys`: `SETTING: "KEY"`.
  - Working setting names: `javascript`, `images`, `webgl`, `webaudio`, `media_stream`, `webrtc`, `plugins`, `smooth_scrolling`, `caret_browsing`, `site_quirks`, `developer_extras`, and `dark_mode` (forwarded to the dark_mode toggle).
  - The default config's `strict_tls` and `geolocation` entries are not settings: their keys pass through unchanged.
- **homepage** (on). `home` action. `uri` (`about:blank`), `key_home` (`Ctrl+Home`). *Unread:* `new_tab_uri` (use core `browser.new_view_uri`).
- **spacesearch** (off). A leading space in the bar forces a search. `url` (`https://duckduckgo.com/?q=%s`). *Unread:* `engine`.
- **omnibar** (off). History completion popup; reads the `history` module's `file`. `key` (`Ctrl+Shift+o`). *Unread:* `max_results`, `rank`, `match`.
- **bookmarks** (off). `key_add` (`Ctrl+d`), `key_open` (`Ctrl+Shift+d`), `file` (`$XDG_DATA_HOME/gsurf/bookmarks`), `dmenu_cmd` (`dmenu -l 20 -i`). *Unread:* `prefix`.
- **history** (on). Appends visits (URI, tab, title) to a text log. `file` (`$XDG_DATA_HOME/gsurf/history`; `""` disables writing), `log_titles` (`true`). *Unread:* `max_entries`, `dmenu_cmd`.

## Tabs, panes, sessions

- **tabs** (on). Tab strip, tree sidebar, tab keys and context menu. Keys: `position`, `strip`, `show_single`, the colours `bg_color`, `active_color`, `fg_color`, `visible_color`, `sidebar`, `sidebar_position`, `sidebar_width`, `sidebar_hides_strip`, `indent`, `max_depth`, `on_close_parent`, `wmux_groups`, `background_label`, `key_new`, `key_close`, `key_next`, `key_prev`, `key_reopen`, `key_sidebar`, `key_collapse`, `key_move_up`, `key_move_down`. Defaults and rules: [tabs-and-containers.md](tabs-and-containers.md).
- **wmux** (off). tmux-style panes, windows and a status line. 33 keys besides `enabled` (prefix, bindings, borders, status line, mode indicator): [wmux.md](wmux.md).
- **containers** (off). Separate cookie jars per tab: `new_tab`, `inherit_opener`, `temp_cleanup_delay`, `temp_color`, `containers` (`name`, `id`, `color`, `hosts`), `key_new_temp`, `key_reopen_next`: [tabs-and-containers.md](tabs-and-containers.md).
- **session** (off). Saves open tabs (with containers, tree parents and the wmux layout) to YAML and restores them. `auto_restore` (`false`), `file` (`$XDG_DATA_HOME/gsurf/session.yaml`), `key_save` (`Ctrl+Shift+s`, which also is `toggles.keys.javascript`: rebind one of them). See [tabs-and-containers.md](tabs-and-containers.md).
- **status_bar** (off). Bottom bar with URI, load progress, TLS and mode. Reads no keys; *unread:* `position`, `show_progress`, `show_tls`, `show_mode`.

## Privacy and content

- **adblock** (off). uBlock Origin / Adblock Plus lists parsed natively, plus a web-process extension.
  - `filter_lists` (paths, or `{ path: …, trusted: true }` for lists allowed `trusted-*` scriptlets), `filters` (inline rules, always trusted), `cosmetic`, `scriptlets`, `redirects` (all `true`), `host_files` and `block_lists` (hosts-format; also veto top-level navigations), `content_filters` (WebKit content-blocker JSON), `whitelist` (hosts never filtered), `web_process_extension` (path override).
  - Fetch lists with `make adblock-lists` (writes `~/.config/gsurf/adblock/`; needs network). Lists are recompiled only when their content changes.
- **hardening** (off). LibreWolf-style network privacy.
  - `https_only` (`true`), `https_only_local` (`false`), `strip_params` (globs, default `[utm_*, fbclid, gclid, dclid, mc_eid, igshid, _hsenc, yclid, msclkid]`), `referer` (`off` | `trim_cross_origin` | `strip_cross_origin`), `gpc` (`true`), `dnt` (default: core `network.do_not_track`), `no_prefetch` (`true`), `third_party_cookies` (`allow` | `block` | `block_all`), `itp` (`true`), `webrtc` (`false` forces WebRTC off), `exempt` (host rules with `skip: [feature, …]`), `web_process_extension`.
  - Core `network.do_not_track` only takes effect while this module is enabled.
- **cookie_policy** (on). `policy` (`always` | `never` | `no-third-party`, default `no-third-party`), `key_toggle` (`Ctrl+Shift+at`, cycles the policy). *Unread:* `per_site`.
- **cert_manager** (off). TLS exceptions: `pins` is a mapping whose **keys** are hosts allowed past certificate errors (values are ignored). *Unread:* `store`, `strict`, `key_show`, `allow_exceptions`.
- **uri_params** (off). Per-URI engine settings (surf `uriparams[]`): `rules: [{ regex: "…", settings: { javascript: false } }]`; settings names as in toggles minus `dark_mode`. A given `rules` list replaces the old one; `[]` clears.
- **useragent** (off). `default` (UA string), `presets` (`NAME: "UA"`), `rules: [{ regex: "…", ua: PRESET_OR_STRING }]` (first match wins). *Unread:* `key_cycle`.
- **proxy_switch** (off; not in the default config). Cycles proxies at runtime: `key_cycle` (`Ctrl+Shift+p`), `presets` (`NAME: "socks5://127.0.0.1:9050"`; `""` = no proxy). Core `network.proxy` sets the startup proxy.
- **geolocation** (off). `default` (`deny` | `allow` | `ask`; default `deny`). *Unread:* `per_site`.
- **notifications** (on). `allow_web_notifications` (`allow` | `deny` | `ask`, default `ask`). *Unread:* `urgency`, `timeout`, `notify_downloads`.
- **form_autofill** (off). Fills forms from profiles in an isolated script world.
  - `file` (`$XDG_CONFIG_HOME/gsurf/autofill.yaml`, must be 0600; the only place card data is accepted), `suggest_on_focus` (`true`), `key_fill` (`Ctrl+Alt+f`; must include Ctrl, Alt or Super), `exclude` (host rules), `profiles` (inline: `label`, `given_name`, `family_name`, `email`, `tel`, `organization`, `street`, `city`, `region`, `postal_code`, `country`).
  - Never write card data inline or read the user's autofill file.

## Appearance and pages

- **dark_mode** (off). `mode` (`off` | `native` | `filter` | `dynamic` (Dark Reader); legacy `auto`, `light`, `dark` + `force_dark_css`), `brightness`, `contrast` (0-200), `sepia`, `grayscale` (0-100), `key_toggle` (`Ctrl+Alt+d`), `key_toggle_site` (`Ctrl+Alt+Shift+d`, saved in `$XDG_DATA_HOME/gsurf/dark_mode/sites.yaml`), `sites` (rules: `match` plus any of the keys above). Unknown keys warn.
- **site_styles** (on). Per-site CSS (surf `styles[]`): `default` (a CSS file for every page), `rules: [{ match: "REGEX", file: "path.css" }]` or `{ match: …, css: "body{…}" }`.
- **userscripts** (off). `global` (`~/.config/gsurf/script.js`), `dir` (every script in it), `inject_time` (`start` | `end`, default `end`), `rules` (`"URI-REGEX": "path.js"` mapping).
- **inspector** (off). `key` (`Ctrl+Shift+i`) opens the web inspector (it turns developer extras on itself; kiosk mode disables it). *Unread:* `detached`.

## External programs

- **downloads** (on). `dir` (`~/Downloads`; relative paths resolve against the launch directory). Strips directory parts from suggested names. *Unread:* `ask_destination`, `key_list`, `notify_complete`, `open_when_done`.
- **externalpipe** (off). `commands: [{ key: "Ctrl+Shift+l", command: "sh pipeline", input: html|text|uri|selection }]` (default input `html`); runs `sh -c`.
- **playexternal** (off). `command` (`mpv --`, the URI is appended), `key` (`Ctrl+m`). *Unread:* `also_links`, `context_menu_entry`.

## Automation

- **mcp** (off; built only with `make MCP=1`). MCP server on a Unix socket: `transport` (only `unix-socket` works in-process), `socket_name` (`$XDG_RUNTIME_DIR/gsurf-mcp-NAME.sock`; default `gsurf-mcp-<pid>.sock`), `tools` (one boolean per tool, all `false`). *Unread:* `port`, `host`. See [mcp.md](mcp.md).

## Checking a module from a shell

```bash
gsurf --list-modules                                   # the registry, alphabetical
ls "${GSURF_MODULE_PATH:-/usr/local/lib64/gsurf/modules}"   # the .so files found
gsurfctl modules                                       # loaded modules: active or off (running gsurf)
```

A module that is listed but not loaded is missing its `.so` (check `GSURF_MODULE_PATH`, or the binary's `modules/` directory in a source tree). `mcp` exists only in `MCP=1` builds.
