# Keybindings, actions and vim modes

Source: `docs/keybindings.org`. Key strings are used by the core `keybinds:` / `mousebinds:` tables and by every module `key_*` option. wmux has its own tmux notation ([wmux.md](wmux.md)).

## Key syntax

- `[Mods+]Key`, where `Key` is a GDK keyval name (`r`, `slash`, `Return`, `Page_Up`, `F11`, `question`) and the modifiers are `Ctrl` (`Control`), `Alt` (`Mod1`), `Shift`, `Super` (`Logo`, `Mod4`). Order and case of modifiers are normalised.
- A letter's case is Shift: `N`, `Shift+n` and `Shift+N` are one key; `o` never fires for Shift+o.
- Shifted punctuation is its shifted keysym: with `Shift`, `period`/`.` becomes `greater`, `bracketleft`/`[` becomes `braceleft`, `slash` → `question`, `semicolon` → `colon`, `apostrophe` → `quotedbl`, `bracketright` → `braceright`, `minus` → `underscore`, `equal` → `plus`, `grave` → `asciitilde`, `backslash` → `bar`, `comma` → `less`. So `Ctrl+Shift+.` = `Ctrl+Shift+period` = `Ctrl+Shift+greater`.
- `+` separates modifiers: write a literal plus as `plus` (`Ctrl+plus`).
- Shift+Tab (`ISO_Left_Tab`) matches `Shift+Tab`.
- Mouse: `[Mods+]ButtonN` (`Button2` middle, `Button8`/`Button9` thumb buttons, `Ctrl+Button1`).

## Core table

```yaml
keybinds:
  "Ctrl+r": reload
  "Ctrl+Shift+r": reload-nocache
  "Escape": stop
  "Alt+Left": back
  "Alt+Right": forward
  "Ctrl+plus": zoom-in
  "Ctrl+minus": zoom-out
  "Ctrl+0": zoom-reset
  "Ctrl+f": find
  "Ctrl+g": find-next
  "Ctrl+Shift+g": find-prev
  "F11": toggle-fullscreen
  "Ctrl+q": quit
  "question": show-keybinds
  "o": open-prompt
  "Ctrl+l": open-prompt
  "Ctrl+e": scroll-down
  "Ctrl+y": scroll-up
mousebinds:
  "Button8": back
  "Button9": forward
  "Ctrl+Button1": open-new-view-background
  "Button2": open-new-view-background
```

The YAML `keybinds:` map is merged over the defaults; there is no YAML way to delete a default binding except rebinding the key. **An unknown action name (or `none`) is skipped without a warning**: check spelling against `gsurfctl actions` or the table below. In C, `gsurf_config_set_keybind(config, "Ctrl+q", GSURF_ACTION_NONE)` removes it.

## Actions

Action names accept `-` or `_` (`reload-nocache` = `reload_nocache`). The C enum is `GSURF_ACTION_` + upper case (`GSURF_ACTION_RELOAD_NOCACHE`).

| Group | Actions |
|---|---|
| Navigation | `back`, `forward`, `reload`, `reload-nocache`, `stop`, `home`, `open-prompt`, `open-new-view`, `open-new-view-background` |
| Scrolling | `scroll-up`, `scroll-down`, `scroll-left`, `scroll-right`, `scroll-top`, `scroll-bottom`, `page-up`, `page-down`, `half-page-up`, `half-page-down` |
| Zoom, find | `zoom-in`, `zoom-out`, `zoom-reset`, `find`, `find-next`, `find-prev` |
| Tabs (tabs module) | `tab-new`, `tab-close`, `tab-next`, `tab-prev`, `tab-reopen`, `tab-sidebar-toggle`, `tab-tree-collapse`, `tab-move-up`, `tab-move-down` |
| Modes (modal) | `enter-normal-mode`, `enter-insert-mode`, `follow-hints`, `follow-hints-new-view` |
| Modules | `container-new-temp`, `container-reopen-next` (containers), `dark-mode-toggle`, `dark-mode-toggle-site` (dark_mode), `form-autofill` (form_autofill), `pane-split-right`, `pane-split-down` (wmux; no default key) |
| Misc | `toggle-fullscreen`, `copy-url`, `paste-url`, `quit`, `show-keybinds` |

Actions with no built-in implementation (`open-prompt`, `find`, `tab-*`, `follow-hints`, the module rows) go to the module that owns the UI through `GsurfActionHandler`. With that module disabled the key does nothing and reaches the page. `gsurfctl actions` lists every name; `gsurfctl action NAME` runs one on the active view.

## Module keys that are on by default

`tabs` (above, plus the tree keys), `find_bar` (`/`, `n`, `N` while a search is up), `toggles` (`Ctrl+Shift+s` JavaScript, `Ctrl+Shift+m` images, `Ctrl+i` caret browsing, `Ctrl+Shift+w` WebGL), `cookie_policy` (`Ctrl+Shift+at`), `homepage` (`Ctrl+Home`), `chromebar` (`Ctrl+l`), `modal` (below). Off-by-default modules add `Ctrl+d` / `Ctrl+Shift+d` (bookmarks), `Ctrl+Alt+d` / `Ctrl+Alt+Shift+d` (dark_mode), `Ctrl+Alt+t` / `Ctrl+Alt+c` (containers), `Ctrl+Alt+f` (form_autofill), `Ctrl+Shift+i` (inspector), `Ctrl+m` (playexternal), `Ctrl+Shift+o` (omnibar), `Ctrl+Shift+p` (proxy_switch), `Ctrl+Shift+s` (session: clashes with toggles' JavaScript key). When two modules bind one key, the one with the lower priority number sees it first.

`?` opens a live overlay of every active binding (core table plus each active module's `GsurfKeybindProvider` list); `j`/`k` move, `q`/`?`/`Escape` close. Use it to check what a key does now.

## The modal module: NORMAL and INSERT

| Mode | Enter with | Keys |
|---|---|---|
| NORMAL (start) | `Escape`, `start_mode: normal` | bare letters are commands, except while a text field or the address bar has focus |
| INSERT | `i` / `a` (`insert_keys`), `gi` (focus the first text field), `enter-insert-mode` | every bare key goes to the page (web apps' own shortcuts) |

NORMAL keys: `h j k l` scroll, `d` / `u` half page, `gg` / `G` top / bottom, `H` / `L` back / forward, `r` reload, `gt` / `gT` next / previous tab, `f` link hints, `F` hint into a new tab, `gi` focus input. `Escape` in INSERT returns to NORMAL and blurs the field. Focus is tracked in every frame and open shadow roots, so typing into a field never needs `i` first. Closed shadow roots are opaque: use `i`.

Link hints: labels from `hint_chars` grow to 2-4 characters as targets increase; typing narrows, a unique match is clicked, `Escape` cancels. Mode indicators (`-- INSERT --`) show in the chromebar and the wmux status line; tune them with `mode_indicator`, `mode_hide_normal` and the three `mode_*_color` keys in each module.

The mode is published on the module manager (`GsurfModuleManager:mode`, `mode-changed`); read it with `gsurfctl mode`, set it with `gsurfctl mode insert`, follow it with `gsurfctl watch mode`.

## Kiosk mode

`gsurf --kiosk URL` refuses URL entry, URL paste, `home`, new views and popups, the context menu and the inspector, whatever is bound. Through gsurfctl a refusal exits with status 4.

## Examples

Rebind the address bar and free `Ctrl+q`, YAML:

```yaml
keybinds:
  "Ctrl+Shift+l": open-prompt
  "Ctrl+q": stop
mousebinds:
  "Button2": open-new-view
```

The same in a C config:

```c
gsurf_config_set_keybind(config, "Ctrl+Shift+l", GSURF_ACTION_OPEN_PROMPT);
gsurf_config_set_keybind(config, "Ctrl+q", GSURF_ACTION_NONE);
gsurf_config_set_mousebind(config, "Button2", GSURF_ACTION_OPEN_NEW_VIEW);
```
