# YAML configuration

Sources: `docs/configuration.org`, `data/default-config.yaml` (the built-in defaults, printed by `gsurf --generate-yaml-config`), `data/sensible-config.yaml` (a curated daily-driver setup), `src/config/gsurf-config.c` (what the core reads).

## Files and order

1. Built-in defaults: the embedded `data/default-config.yaml`.
2. The user file: `-c PATH`, else `$XDG_CONFIG_HOME/gsurf/config.yaml`, else `/etc/gsurf/config.yaml` (the first one found; only one is read).
3. `$XDG_CONFIG_HOME/gsurf/config.c` (or `--c-config PATH`, `$GSURF_CONFIG_C`), compiled and run: see [configuration-c.md](configuration-c.md).

`--no-yaml-config`, or `ignore_yaml: true` at the root of a document, skips that YAML. Note that `--no-yaml-config` also skips the built-in default YAML, so every module is off unless a C config enables it.

## The rule that bites: `modules:` is replaced, not merged

Core sections (`browser`, `webkit`, `keybinds`, …) merge key by key over the defaults. **The `modules:` mapping does not**: a document that has a `modules:` key replaces the whole mapping. A user `config.yaml` with only

```yaml
modules:
  tabs:
    enabled: true
    sidebar: true
```

leaves every other module without `enabled: true`, so modal, chromebar, search_engines, history, find_bar and the rest are **off**. Therefore:

- Start from the full defaults and edit them: `gsurf --generate-yaml-config > ~/.config/gsurf/config.yaml`, or copy [../templates/config.yaml.template](../templates/config.yaml.template).
- When you add a module setting to an existing file, add it inside the file's existing `modules:` block, and keep every module the user relies on in that block with `enabled: true`.
- Snippets in this skill that start with `modules:` show one section; merge them into the full block.

## Core sections (read by libgsurf)

| Section | Keys |
|---|---|
| `browser` | `homepage` (startup URI when none is given), `new_view_uri` (new tabs), `default_zoom` (1.0), `smooth_scroll`, `fullscreen_on_start`, `languages` (`[en-US, de]`: navigator.language + Accept-Language; empty = the locale's, en-US under `C`) |
| `user_agent` | root string; `""` = the engine default |
| `webkit` | `javascript`, `images`, `webgl` (false), `webaudio`, `media`, `media_stream` (false), `webrtc` (false), `plugins`, `smooth_scrolling`, `caret_browsing`, `dns_prefetch` (false), `hyperlink_auditing` (false), `site_quirks`, `developer_extras` (false), `default_font_size` (16), `default_monospace_font_size` (13), `default_charset` (`UTF-8`), `javascript_can_open_windows` (false), `javascript_can_access_clipboard` (false) |
| `tls` | `strict` (true: reject invalid certificates) |
| `storage` | `cache_dir`, `data_dir`, `cookie_jar` (`~/.local/share/gsurf/cookies.sqlite`), `enable_disk_cache`; `~/` is expanded |
| `network` | `proxy: { mode: default\|none\|custom, uri: "socks5://127.0.0.1:9050" }`, `do_not_track` (enforced only by the hardening module) |
| `window` | `title`, `geometry` (`WIDTHxHEIGHT`, both > 0) |
| `control` | `enabled` (true: open the control socket `gsurfctl` uses), `name` (an instance name for `gsurfctl -i NAME`): see [control-protocol.md](control-protocol.md) |
| `lrg` | `arrangement`, `environment`, `background-fps` (4), `history-depth` (8), `reading-single-tab` (true): the `--lrg=3d` scene, see [keybindings-and-modes.md](keybindings-and-modes.md) |
| `keybinds`, `mousebinds` | `"Mods+Key": action`: see [keybindings-and-modes.md](keybindings-and-modes.md) |
| `modules` | one mapping per module: see [modules-catalog.md](modules-catalog.md) |
| `ignore_yaml` | root boolean: skip this document |

`webkit.webrtc: true` also enables media streams (WebKit needs both). Relative paths resolve against the directory gsurf was launched from, not its runtime working directory.

**Not read by the core** although the default file lists them: `storage.download_dir` (use `modules.downloads.dir`), `window.show_in_title`, and the root sections `search_engines:`, `uri_params:`, `site_styles:`, `cert_pins:` (use `modules.search_engines`, `modules.uri_params`, `modules.site_styles`, `modules.cert_manager.pins`). Unknown keys are ignored without a warning, so a typo silently does nothing.

## Editing workflow

1. Read the current file (`~/.config/gsurf/config.yaml`) and check for `config.c` next to it: if it exists, the C config runs last and its `modules:` block wins; edit that instead.
2. Make the smallest change inside the existing structure. Keep comments.
3. Check that it parses and the modules do not warn:
   ```bash
   yq '.' ~/.config/gsurf/config.yaml > /dev/null
   timeout 10 gsurf --config ~/.config/gsurf/config.yaml about:blank 2>&1 | grep -iE 'warn|crit'
   ```
   The second command opens a window for a moment; skip it if the user is mid-session and say so.
4. Restart gsurf. Most settings are read once; `gsurfctl config KEY` on a running instance shows the value it uses.

## Common tasks

Search engines (keep `default` pointing at an engine that has a `url`):

```yaml
modules:
  search_engines:
    enabled: true
    default: ddg
    default_is_search: true
    engines:
      ddg:  { prefix: "d ", url: "https://duckduckgo.com/?q=%s" }
      wiki: { prefix: "w ", url: "https://en.wikipedia.org/w/index.php?search=%s" }
      gl:   { prefix: "gl ", url: "https://gitlab.com/search?search=%s" }
```

Privacy baseline (adblock lists from `make adblock-lists`, hardening, a stricter cookie policy):

```yaml
network:
  do_not_track: true
modules:
  adblock:
    enabled: true
    filter_lists:
      - "~/.config/gsurf/adblock/easylist.txt"
      - "~/.config/gsurf/adblock/easyprivacy.txt"
      - { path: "~/.config/gsurf/adblock/ublock-filters.txt", trusted: true }
    whitelist: []
  hardening:
    enabled: true
    https_only: true
    third_party_cookies: block
    exempt:
      - match: '^(localhost|127\.0\.0\.1)$'
        skip: [https_only]
  cookie_policy:
    enabled: true
    policy: no-third-party
```

Dark mode with per-site rules:

```yaml
modules:
  dark_mode:
    enabled: true
    mode: filter
    brightness: 95
    sites:
      - match: '(^|\.)github\.com$'
        mode: native
      - match: '(^|\.)example\.org$'
        mode: "off"
```

Quote `"off"`, `"on"`, `"yes"` and `"no"` when they are string values; YAML would read them as booleans.

## Generated starting points

```bash
gsurf --generate-yaml-config > ~/.config/gsurf/config.yaml   # defaults, all comments
gsurf --generate-c-config    > ~/.config/gsurf/config.c      # C skeleton
```

`data/sensible-config.yaml` in the source tree is a privacy-leaning full config (modal, tree tabs, adblock hosts file, dark mode, history, downloads) to copy from.
