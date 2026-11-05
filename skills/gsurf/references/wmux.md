# wmux: tmux-style panes

`wmux` (module `modules/wmux/`, off by default) is tmux for the web inside one gsurf window. Source of truth: `docs/wmux.org` (its YAML blocks and command examples are checked by `tests/test-wmux-docs.c`).

| tmux | wmux |
|---|---|
| session | the browser window |
| window | a *tmux window*: one arrangement of panes; `C-b c` makes more |
| pane | a viewport showing one tab |
| program in a pane | the tab (web view) |

Tabs no pane shows are *background tabs*: still open, still in the strip and sidebar. Selecting one (sidebar click, `Ctrl+Tab`, `gt`) shows it in the active pane. Selecting a tab that another tmux window shows switches to that window.

## Enable

```yaml
modules:
  wmux:
    enabled: true
```

Add the section inside the config's existing `modules:` block (a block with only `wmux:` turns every other module off: [configuration-yaml.md](configuration-yaml.md)).

What changes: `C-h/j/k/l` move between panes, so **`C-l` no longer opens the address bar** (use `o`, or unbind `C-l` in `root_bindings`); `Ctrl+t`, links opened in a new tab and popups go into the active pane; `C-b %` opens a tab in a new pane. Kiosk mode and `--lrg` keep wmux inactive (logged once).

## Default keys

Prefix `C-b`, then:

| Key | Command | Key | Command |
|---|---|---|---|
| `%` / `"` | `split-window -h` / `-v` (new tab) | `C-%` / `C-"` | same with `-c` (duplicate page, same container) |
| `o` / `;` | next pane / `last-pane` | arrows (repeatable) | `select-pane -U/-D/-L/-R` |
| `x` | `confirm-before kill-pane` | `z` | `resize-pane -Z` (zoom) |
| `q` | `display-panes` (then a digit) | `{` / `}` | `swap-pane -U` / `-D` |
| `!` | `break-pane` | `C-o` / `M-o` | `rotate-window` / `-D` |
| `C-arrows` / `M-arrows` (repeatable) | `resize-pane` by 1 / 5 % | `Space`, `M-1`…`M-5`, `E` | next preset, a preset, `select-layout -E` |
| `c` `n` `p` `l` | new / next / previous / last window | `0`…`9` | `select-window -t N` |
| `w` | `choose-tree` | `&` | `confirm-before kill-window` |
| `,` | `command-prompt -I #W rename-window` | `:` / `?` | `command-prompt` / `list-keys` |
| `C-b` | `send-prefix` (next `C-b` reaches the page) | | |

Root table (no prefix): `C-h` `C-j` `C-k` `C-l` = `select-pane -L/-D/-U/-R`. wmux runs before `modal` (priority -10) and sets the window's key grab after the prefix, so the key after `C-b` works even while typing in a field or the address bar.

## Command language

One command per line: name, flags (getopt style, `-hb` = `-h -b`), arguments. Single quotes are literal; double quotes allow `\" \\ \n \t`; `#W` expands to the current window name. tmux aliases work (`splitw`, `selectp`, `killp`, `resizep`, `neww`, `selectw`, `renamew`, `lsk`, …). No `\;` sequences, no `{ }` blocks, no copy mode, detach or shell.

| Command | Synopsis |
|---|---|
| split-window | `[-h\|-v] [-b] [-d] [-c] [-p pct] [uri]`: `-b` before, `-d` keep focus, `-c` duplicate page, `-p` 1-99 % |
| select-pane | `-L\|-R\|-U\|-D\|-l\|-t N\|+N\|-N` |
| last-pane, kill-pane, break-pane, display-panes, choose-tree, next-layout, list-keys, send-prefix | no arguments |
| resize-pane | `-L\|-R\|-U\|-D [n]` (percent of the window) or `-Z` |
| swap-pane | `-U\|-D` |
| rotate-window | `[-U\|-D]` |
| select-layout | `even-horizontal\|even-vertical\|main-horizontal\|main-vertical\|tiled` or `-E` |
| new-window | `[-n name] [uri]` |
| next-window, previous-window, last-window | no arguments |
| select-window | `-t N\|+N\|-N` |
| kill-window | no arguments |
| rename-window | `[name]` (empty = automatic naming) |
| command-prompt | `[-p prompt] [-I initial] [template]` (`%%` in the template = the input) |
| confirm-before | `[-p prompt] command` |

A line that does not parse is never run; the error starts with the byte position (`position 0: unknown command "spilt-window"`). Execution errors (`select-pane: can't find pane 7 (the window has 2)`, `no space for a new pane (minimum 10%)`) show in the status line, and `gsurfctl wmux` / MCP `wmux_command` return them as errors. A direction with no neighbour is not an error.

## Key notation

Bindings take tmux notation or gsurf notation; both normalise to tmux's canonical form: modifiers `C-` `M-` (Alt) `S-` in that order; printable keys already shifted (`%` not `S-5`, `A` not `S-a`, `C-%`); named keys `Space Enter Escape Tab BTab BSpace Up Down Left Right Home End PPage NPage IC DC F1…F12 KP0…KP9 KPEnter`. `Ctrl+b`, `^b`, `Alt+1`, `Shift+Up`, `percent`, `quotedbl` are accepted spellings. **Super is not allowed** (the compositor owns it): a config key naming Super is an error.

## Configuration (all under `modules.wmux`)

`prefix` (`C-b`), `prefix_timeout` (0 = wait forever; 0-60000 ms), `repeat_time` (500; 0-10000), `display_panes_time` (1000; 100-60000), `bindings` / `root_bindings` (mapping `key: command`, merged over the defaults; `""` or `null` unbinds), `repeatable` (more repeatable prefix keys), `split_ratio` (50; 1-99), `on_pane_kill` (`close_tab` | `keep_tab`), `on_tab_close` (`close_pane` | `keep_pane`), `confirm_kill` (true), `mouse` (true: drag gaps to resize, drop sidebar tabs on panes), `remember_layout` (true: the session module saves layouts), `min_pane_pct` (10; 0-50), `gap` (2 px), `border_width` (1), `active_border_width` (2), `border_color` (`#45475a`), `active_border_color` (`#89b4fa`), `container_colors` (true), `inactive_opacity` (0.55), `active_brighten` (0.25), `status` (true), `status_position` (`bottom` | `top`), `status_bg` (`#181825`), `status_fg` (`#cdd6f4`), `status_current_fg` (`#89b4fa`), `window_name_max` (20; 1-200), `mode_indicator` (true), `mode_hide_normal` (false), `mode_normal_color`, `mode_insert_color`, `mode_follow_color`.

Every configure starts from the defaults; a bad value warns and keeps the default; a binding that does not parse warns and is skipped. Booleans accept `yes`/`no`, `on`/`off`.

## Cookbook

Alt keys for panes so `C-l` opens the address bar again, vim-style splits and a GitHub split:

```yaml
modules:
  wmux:
    enabled: true
    root_bindings:
      "C-h": ""
      "C-j": ""
      "C-k": ""
      "C-l": ""
      "M-h": "select-pane -L"
      "M-j": "select-pane -D"
      "M-k": "select-pane -U"
      "M-l": "select-pane -R"
    bindings:
      "|": "split-window -h"
      "-": "split-window -v"
      "g": "split-window -h https://github.com"
      "G": "command-prompt -p \"Open right:\" \"split-window -h %%\""
      "H": "resize-pane -L 5"
      "L": "resize-pane -R 5"
    repeatable: ["H", "L"]
```

Screen-style prefix and panes that keep their tabs:

```yaml
modules:
  wmux:
    enabled: true
    prefix: "C-a"
    on_pane_kill: keep_tab
    on_tab_close: keep_pane
```

## Driving wmux from outside

```bash
gsurfctl wmux 'split-window -h https://docs.gtk.org/'   # any command line
gsurfctl panes                       # panes of the current tmux window: index, view id, active, zoomed, rectangle
gsurfctl windows                     # tmux windows: index, name, current, last
gsurfctl select-window 1
gsurfctl rename-window 1 research
```

MCP clients use `wmux_status`, `list_panes`, `wmux_command`, `split_pane`, … ([mcp.md](mcp.md)). Both go through the `GsurfWindowLayout` seams (`list_panes`, `run_command`, `describe`, `list_groups`), so they need no wmux-specific code.

## Gotchas

- Sites that capture modified keys (`C-h` in some web editors) lose them while the root table binds them: unbind or move navigation to `M-h/j/k/l`.
- `C-b` (bold) in a web editor: press `C-b C-b`.
- The session file stores the layout only for the first saved window; a layout that names missing tabs is dropped (one pane).
- Warnings: config problems are `g_warning`, command errors `g_message`, restore mismatches `g_debug` (`G_MESSAGES_DEBUG=all`).
