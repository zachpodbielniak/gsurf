# The MCP server (`mcp` module)

Source: `docs/mcp.org`. The `mcp` module exposes the browser as an MCP server for AI clients. Prefer `gsurfctl` for shell work and scripts ([gsurfctl.md](gsurfctl.md)): it is built into every gsurf, on by default, and needs no per-tool opt-in. Use MCP when an MCP client (Claude Code, cmacs) should call the browser as tools.

| | control socket (`gsurfctl`) | MCP module |
|---|---|---|
| Build | every build | `make MCP=1` (libsoup-3.0, libdex-1, libpng; vendored mcp-glib) |
| Enabled | by default (`control.enabled`) | `modules.mcp.enabled: true` |
| Access | every method | each tool off until `tools.<name>: true` |
| Socket | `$XDG_RUNTIME_DIR/gsurf/<pid>-XXXXXX/control.sock` | `$XDG_RUNTIME_DIR/gsurf-mcp-<pid>.sock` or `gsurf-mcp-NAME.sock` |

## Enabling

```yaml
modules:
  mcp:
    enabled: true
    transport: unix-socket
    socket_name: browser            # $XDG_RUNTIME_DIR/gsurf-mcp-browser.sock
    tools:
      get_current_uri: true
      get_title: true
      navigate: true
      get_page_text: true
      list_views: true
      open_child_view: true
      get_tab_tree: true
      wmux_status: true
      list_panes: true
      split_pane: true
```

Only `transport: unix-socket` works in-process (other values log a message and start nothing). On activation the module prints the socket path. Reconfiguring applies new tool flags at once. Restart gsurf after editing the YAML.

## Tools

All take an optional `view` (stable id, from `list_views`) where a page is meant; positions (`index`) still work but change as tabs move.

| Group | Tools |
|---|---|
| Page | `navigate {uri}`, `get_current_uri`, `get_title`, `go_back`, `go_forward`, `reload {bypass_cache?}`, `stop`, `get_page_text`, `get_page_html`, `get_links`, `execute_javascript {code}`, `scroll {direction}`, `find_text {text, backward?}`, `click_link {index? \| selector?}`, `screenshot`, `save_screenshot {path}`, `get_history {limit?}`, `get_cookies` |
| Config | `get_config`, `set_config`, `set_setting {key, value}`, `list_modules`, `toggle_module {name, enabled?}` |
| Views | `list_views`, `switch_view`, `open_view {uri?, container?}`, `close_view`, `open_child_view {parent?, uri?, background?, container?}` |
| Tab tree | `get_tab_tree`, `move_view {view, parent \| before \| after \| index}`, `set_collapsed {view, collapsed?}` |
| Containers | `list_containers`, `reopen_in_container {view?, container}`, `new_temporary_container_view {uri?, background?}` |
| Modes, actions | `get_mode`, `set_mode {mode}`, `follow_hints {new_view?}`, `run_action {action}`, `list_actions` |
| wmux | `wmux_status`, `list_wmux_windows`, `list_panes {window?}`, `wmux_command {command}`, `split_pane {direction?, uri?, duplicate?, ratio?, background?}`, `select_pane`, `kill_pane`, `zoom_pane`, `select_layout {name}`, `show_view_in_pane {view, pane, zone?}`, `select_window {index \| which}`, `new_window {name?, uri?}`, `rename_window {name, index?}`, `kill_window {index?}` |

A view object has `index`, `id`, `uri`, `title`, `active`, `visible`, `opener_id`, `parent_id`, `container`, `container_name`, `container_color`, `container_temporary` and `pane` (`{window, index}` or `null`). Tools that change panes return the current window's panes. Numbers and booleans are read leniently (`3`, `3.0`, `"3"`; `true`, `"1"`).

Errors are tool error results with a hint: `unknown view 42 (… list_views gives the ids)`, `no window layout is installed: enable the wmux module …`, `re-parenting needs a tab tree: enable the tabs module …`, `input modes need the modal module …`, `kiosk mode: opening or reopening views is not allowed`.

## Agent workflow for panes

1. `wmux_status`: if not active, the user must enable `modules.wmux`.
2. `list_views`: ids, panes, containers.
3. `split_pane {"uri": "https://docs.example.org/", "ratio": 0.4}`, then work in that pane by its `view` id: `navigate`, `get_page_text`, `select_pane {"view": 15}`.
4. Background tabs: `open_child_view {"parent": 1, "uri": …, "background": true}`, then `show_view_in_pane {"view": ID, "pane": 0, "zone": "bottom"}`.
5. `select_layout {"name": "main-vertical"}`, `wmux_command {"command": "resize-pane -L 10"}`.
6. Clean up: `kill_pane {"view": ID}` closes the tab too unless `on_pane_kill: keep_tab`.

## Security

`execute_javascript` gives full page control; `get_page_text`, `get_page_html`, `get_links`, `get_cookies` expose page data; `run_action` can do anything a key can, `quit` included; `set_config` and `toggle_module` change the browser. Enable only what the task needs and only when the user asks. The socket is user-private under `$XDG_RUNTIME_DIR`. Kiosk mode refuses tools that open, reopen or split views.

## Known issues

- The stdio transport of mcp-glib crashes when the server stops with a client connected; there is no in-process stdio transport anyway.
- mcp-glib logs `Transport error: Remote end closed connection` whenever a client hangs up: harmless.

## Embedders and tests

`mcp.so` exports `gsurf_mcp_module_call_tool(module, name, arguments_json, &is_error, &error)` and `gsurf_mcp_module_list_tools(module)` for in-process calls (find them with `g_module_symbol()`). `tests/test-mcp.c` (headless, `MCP=1`) and `tests/test-mcp-webkit.c` (`make test-gui`) cover every tool.
