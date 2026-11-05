# The control protocol

The control server (`GsurfControlServer`, `src/control/`) is what `gsurfctl` talks to. Use this reference to call it from a program (Python, C, a cmacs package) or to add methods from a module. The authoritative description is `docs/control.org`.

## Socket and discovery

- Each gsurf process owns a runtime directory `$XDG_RUNTIME_DIR/gsurf/<pid>-XXXXXX/` (0700, removed at exit). The socket is `control.sock` in it (0600). Only peers with the same uid are accepted.
- An instance is alive while its directory's `lock` file is held (`flock`); a directory without `control.sock` (an embedder, or `control.enabled: false`) is skipped.
- With `control.name: work` the server also creates `$XDG_RUNTIME_DIR/gsurf/work.sock`, a symlink to its socket (`gsurfctl -i work`).
- Config:
  ```yaml
  control:
    enabled: true
    name: work
  ```
- In C: `gsurf_control_list_instances()` returns the live instances; `GsurfControlClient` connects, calls and subscribes (sync and async variants).

## Framing

Newline-delimited JSON-RPC 2.0, one message per line, UTF-8:

```text
→ {"jsonrpc":"2.0","id":1,"method":"open","params":{"uri":"https://example.org","background":true}}
← {"jsonrpc":"2.0","id":1,"result":{"id":7,"uri":"https://example.org","active":false}}
```

Requests without `id` are notifications (no reply). Errors use the standard `error` object with `code` and `message`. A quick test from a shell:

```bash
sock=$(ls -d "$XDG_RUNTIME_DIR"/gsurf/*/control.sock | head -n 1)
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"instance.info"}' | socat - "UNIX-CONNECT:$sock"
```

## Methods

| Method | Params | Result |
|---|---|---|
| `instance.info` | | pid, version, backend, kiosk, windows, views, active uri and title, `focused_at` |
| `open` | `uri`, `background?`, `parent?`, `container?` | the new view |
| `navigate` | `uri`, `view?` | |
| `back`, `forward`, `reload`, `stop` | `view?` (`reload`: `bypass_cache?`) | |
| `views.list` | | view objects: id, opener, parent, container, uri, title, active, visible, pane |
| `views.activate`, `views.close` | `view` | |
| `page.uri`, `page.title`, `page.text` | `view?` | string |
| `page.eval` | `code`, `view?` | the JavaScript result (refused in kiosk mode) |
| `page.screenshot` | `path`, `view?` | |
| `actions.list` | | `{name, allowed}` |
| `action.run` | `name`, `view?` | |
| `mode.get`, `mode.set` | `mode` (`normal`, `insert`) | the mode |
| `tree.get`, `tree.move`, `tree.collapse` | `view`, `parent` / `before` / `after`; `collapsed` | needs a `GsurfTabTreeProvider` (the `tabs` module) |
| `containers.list`, `containers.reopen` | `view`, `container` | |
| `layout.describe`, `layout.panes`, `layout.command`, `layout.groups`, `layout.select_group`, `layout.rename_group`, `layout.show_view_at` | `command`, `index`, `name`, `view`, `pane`, `zone` | needs a window layout (wmux) |
| `config.get` | `key` | the value |
| `modules.list`, `modules.toggle` | `name`, `enabled` | |
| `methods.list` | | every method with its description and params |
| `events.subscribe` | `topics: [...]` | turns the connection into an event stream |

Ask a running instance for the exact list: `gsurfctl call methods.list`.

## Errors

`GsurfControlError` maps to JSON-RPC codes: invalid params (-32602), method not found (-32601), parse error (-32700), and server errors for not found (view, pane), unsupported (no layout, no tab tree), permission denied (kiosk) and failed. `gsurfctl` turns kiosk refusals into exit status 4 and other errors into 1.

## Events

After `events.subscribe {"topics": ["view", "uri", "load"]}` the server sends notifications, one per line:

```text
← {"jsonrpc":"2.0","method":"event","params":{"topic":"load","view":7,"event":"finished"}}
```

Topics: `view` (added, removed, active), `uri`, `title`, `load`, `mode`, `layout` (groups and visible views changed), `download`.

## Adding methods from a module

Implement the optional `GsurfControlProvider` interface on the module; the server asks active providers to register their methods with `gsurf_control_server_add_method(server, name, description, params_doc, handler, data)`. Name methods `<module>.<verb>`, validate params, and return `GsurfControlError` codes. Add a test that calls the method through a client on a temporary socket (`tests/test-control.c` shows the pattern).

## Embedders

The library never opens the socket by itself. A host that wants `gsurfctl` access calls `gsurf_control_server_new()` and `gsurf_control_server_start()` after its modules are active, and stops the server before teardown.
