# Tabs, the tab tree, containers and sessions

Sources: `docs/tabs.org`, `docs/containers.org`, `docs/configuration.org` (Session). gsurf runs **one window per process**; every tab of a process lives in that window. YAML snippets below are sections of the config's complete `modules:` block, which replaces the built-in one ([configuration-yaml.md](configuration-yaml.md)).

## The tab tree (`modules.tabs`, on by default)

- Every view records its **opener** at creation. A tab opened from another tab (middle click, `Ctrl+click`, "Open Link in New Tab", `target=_blank`, `F` hints, Duplicate) becomes that tab's **last child**; `tab-new` and startup tabs are roots.
- The window's view order is the source of truth: each subtree is one contiguous run of tabs, and the sidebar is the window order with indentation.
- Closing a parent: `on_close_parent: promote` (children move up one level) or `close_children` (the subtree closes too; the last view of the window always stays).
- Collapse hides descendants in the sidebar; they stay open. Activating a hidden tab expands its ancestors.
- `tab-move-up` / `tab-move-down` move the active tab with its subtree past the previous / next sibling; a tab never changes parent this way. Drag and drop in the sidebar re-parents (upper quarter = before, lower quarter = after, middle = child).
- With wmux and `wmux_groups: true` the sidebar is grouped by tmux window; background tabs that descend from no pane tab go under `background_label`.
- Other code reads and changes the tree through `GsurfTabTreeProvider` (`get_parent`, `get_collapsed`, `set_collapsed`, `move_view`); that is what `gsurfctl tree`, `move`, `collapse` and the MCP tree tools use.

### Keys and actions

| Action | Default key | Config key |
|---|---|---|
| `tab-new` | `Ctrl+t` | `key_new` |
| `tab-close` | `Ctrl+w` | `key_close` |
| `tab-next` / `tab-prev` | `Ctrl+Tab` / `Ctrl+Shift+Tab` | `key_next` / `key_prev` |
| `tab-reopen` | `Ctrl+Shift+t` | `key_reopen` |
| `tab-sidebar-toggle` | `Ctrl+Shift+.` (GDK: `Ctrl+Shift+greater`) | `key_sidebar` |
| `tab-tree-collapse` | `Ctrl+Shift+[` (GDK: `Ctrl+Shift+braceleft`) | `key_collapse` |
| `tab-move-up` / `tab-move-down` | `Ctrl+Shift+Page_Up` / `Page_Down` | `key_move_up` / `key_move_down` |

An empty key (`""`) unbinds it; the action stays bindable in the core `keybinds:` table. `modal` adds `gt` / `gT`.

### Options and defaults

| Key | Default | Notes |
|---|---|---|
| `position` | `top` | strip: `top` or `bottom` |
| `strip` | `true` | `false`: no strip; tabs only in the sidebar |
| `show_single` | `false` | show the strip with one tab |
| `bg_color` / `active_color` / `fg_color` / `visible_color` | `#181825` / `#313244` / `#cdd6f4` / `#1e1e2e` | CSS colours; a bad one warns and keeps the default |
| `sidebar` | `false` | sidebar shown at startup |
| `sidebar_position` | `left` | `left` or `right` |
| `sidebar_width` | `240` | 60-2000 px, fixed (no drag resize) |
| `sidebar_hides_strip` | `true` | |
| `indent` / `max_depth` | `14` / `8` | 0-64; `max_depth: 0` draws a flat list |
| `on_close_parent` | `promote` | or `close_children` |
| `wmux_groups` | `true` | group the sidebar by wmux window |
| `background_label` | `Background` | non-empty string |

Each configure starts from the defaults. Layout options apply at the next activation (restart); keys and `on_close_parent` apply at once. The right-click menu offers Reload, Duplicate, Open in Split Right/Below (with wmux), Reopen in Container (with `containers`), Collapse/Expand, Close Children, Close.

## Containers (`modules.containers`, off by default)

A container is a separate browsing identity: cookies, local storage, IndexedDB, cache, HSTS.

| Kind | Id | Data | Lifetime |
|---|---|---|---|
| default | none | engine default + `storage.cookie_jar` | the whole run |
| persistent | e.g. `media` | `$XDG_DATA_HOME/gsurf/containers/<id>/`, cache under `$XDG_CACHE_HOME` | kept between runs |
| temporary | `tmp-1`, `tmp-2`, … | `$XDG_RUNTIME_DIR/gsurf/<pid>-XXXXXX/temp/<id>/` | removed `temp_cleanup_delay` s after its last tab |

```yaml
modules:
  containers:
    enabled: true
    new_tab: temporary          # temporary | default | <container id>
    inherit_opener: true
    temp_cleanup_delay: 30      # seconds; 0 = at once
    temp_color: "#89b4fa"       # "" = none
    containers:
      - name: Media             # id defaults to "media"
        color: "#f9e2af"
        hosts: ['(^|\.)youtube\.com$', '(^|\.)twitch\.tv$']
      - name: Work Stuff        # id "work-stuff"
        hosts: ['(^|\.)corp\.example$']
    key_new_temp: "Ctrl+Alt+t"
    key_reopen_next: "Ctrl+Alt+c"
```

- **Ids**: 1-64 chars of `[A-Za-z0-9_-]`; derived from the name (lowercased, other characters become `-`). Reserved: `temporary`, `default`, anything starting `tmp-`; a container named "Temporary" needs an explicit `id`. Changing an id starts an empty profile.
- **hosts** are case-insensitive regexes over the bare host (no scheme, port, path). Write `(^|\.)example\.com$` for a domain and its subdomains; a bare word matches anywhere in the host. The first matching rule over all containers, in config order, wins.
- **Placement** of a new tab, first match wins: an explicit request (`gsurfctl open --container`, session restore, `container-reopen-next`), a host rule for the first URL, the opener's container (`inherit_opener`), then `new_tab`. With the default `new_tab: temporary` gsurf **starts in a temporary container**.
- **Popups** (`window.open`, `target=_blank` popups) always share the opener's container (WebKit requirement; keeps OAuth popups working).
- **Host assignment**: navigating the main frame to a host assigned to another container cancels the load and reopens it in that container, in the same place in the tree and pane. Typed addresses and bookmarks switch before the request; clicked links switch at the response (one request already went out with the old container's cookies). Iframes and downloads never move.
- A view cannot change container: every container change creates a new view (a **new view id**) and closes the old one.
- Actions: `container-new-temp` (new tab, fresh temporary container), `container-reopen-next` (cycle default → persistent in order → new temporary). Kiosk mode refuses both.
- Errors (bad id, bad regex, unknown `new_tab`) warn and skip only the bad part.
- Removing a container from the config keeps its directories; delete them by hand (`gio trash`) to forget its data.

## Session (`modules.session`, off by default)

```yaml
modules:
  session:
    enabled: true
    auto_restore: true
    file: "~/.local/share/gsurf/session.yaml"
    key_save: "Ctrl+Alt+s"      # default Ctrl+Shift+s collides with toggles' javascript key
```

- Saves after every committed navigation, on `key_save`, and when tabs close, move or the wmux layout changes. The file is YAML, written atomically, mode 0600.
- Per tab: `uri`, `container` (absent = default, an id, or `temporary:<n>` shared by tabs of one temporary container), `parent` (index of the opener). Per window: `active`, and `layout` (wmux state) when `modules.wmux.remember_layout` is on.
- A URI on the command line wins over `auto_restore`. Blank tabs are not saved. A container no longer defined falls back to default with a warning. Closing the last tab does not overwrite the saved session.
- wmux activates before session (priority -10), so the layout is restored onto the restored tabs; a layout naming missing tabs is dropped.

## From the command line

```bash
gsurfctl tabs                          # id, opener, container, active, visible, pane
gsurfctl tree                          # the tree (grouped by wmux window when wmux runs)
gsurfctl open --bg --child-of 3 https://example.org/a
gsurfctl move 7 --parent 3             # or --before V / --after V
gsurfctl collapse 3 on
gsurfctl containers                    # default + registry containers with their rules
gsurfctl reopen 7 media                # new view id is printed
gsurfctl open --container temporary https://example.org/
```
