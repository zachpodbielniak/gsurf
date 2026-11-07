# The control protocol

Every gsurf since 0.5 opens a control socket: newline-delimited JSON-RPC 2.0 over a Unix socket. `gsurfctl` is its command-line client ([gsurfctl.md](gsurfctl.md)); scripts, editors and agents can speak it directly. It is built into every gsurf and does not need `MCP=1`. Source of truth: `docs/control.org`; a running instance lists its methods with `gsurfctl methods` (`methods.list`).

## Socket, discovery, config

- The socket is `$XDG_RUNTIME_DIR/gsurf/<pid>-XXXXXX/control.sock` (0600) in the process's private runtime directory (0700, removed at exit). Peers of another uid are disconnected.
- An instance is running while its directory's `lock` is held (`flock`) and it has a `control.sock`; a live directory without one (an embedder without a server, `control.enabled: false`) is not an instance.
- With `control.name: NAME` the server keeps `$XDG_RUNTIME_DIR/gsurf/NAME.sock` linked to its socket (`gsurfctl -i NAME`); a name a live instance holds is not taken (warning).

```yaml
control:
  enabled: true      # false: no socket
  name: work         # letters, digits, '-', '_', '.'
```

In a C config: `config->control_enabled`, `config->control_name`. The `gsurf` binary starts the server after modules activate and stops it before teardown.

## Framing

One JSON value per line each way (UTF-8, up to 16 MiB per line). `params` is an object or absent. A request without `id` is a notification (no answer, not even an error). Answers may come out of order (`page.eval` waits for the page): match by `id`. A batch (array) gets one array back.

```text
→ {"jsonrpc":"2.0","id":1,"method":"open","params":{"uri":"https://example.org","background":true}}
← {"jsonrpc":"2.0","id":1,"result":{"index":1,"id":4,"uri":"https://example.org/",...}}
→ {"jsonrpc":"2.0","id":2,"method":"no.such"}
← {"jsonrpc":"2.0","id":2,"error":{"code":-32601,"message":"unknown method 'no.such' (methods.list gives them)"}}
```

From a shell without gsurfctl:

```bash
sock=$(gsurfctl -j info | yq -p json '.socket')
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"views.list"}' | socat -t 2 - "UNIX-CONNECT:${sock}"
```

## Errors

| Code | `GsurfControlError` | Meaning |
|---|---|---|
| -32700 | `PARSE` | not JSON |
| -32600 | `INVALID_REQUEST` | no string `method`; empty batch |
| -32601 | `METHOD_NOT_FOUND` | no such method |
| -32602 | `INVALID_PARAMS` | missing or mistyped parameter |
| -32000 | `FAILED` | the method failed (message says why) |
| -32001 | `NOT_FOUND` | no such view, window, pane, container, module or key |
| -32002 | `UNSUPPORTED` | needs wmux, the tabs module, modal, or a `--lrg=3d` window |
| -32003 | `PERMISSION_DENIED` | refused by kiosk mode (gsurfctl exit 4) |
| -32004, -32005 | `TIMEOUT`, `CLOSED` | client side: no reply in time, connection closed |

## Methods

Views are addressed by `view` (stable id) or, where noted, `index`; without either a method acts on the active view of the active window.

| Method | Params | Result |
|---|---|---|
| `methods.list` | | `[{name, description, params, provider}]` |
| `events.subscribe` / `events.unsubscribe` | `topics?: [string]` (default all) | the connection receives events |
| `instance.info` | | `{pid, version, backend, name, socket, kiosk, windows, views, active_view, uri, title, focused_at, now}` |
| `open` | `uri?`, `background?`, `parent?` (view id: a tree child), `container?` (id, `temporary`, `default`) | the new view; URI handlers (search engines) rewrite input; kiosk refuses |
| `navigate` | `uri`, `view?`/`index?` | the view; kiosk refuses |
| `back`, `forward`, `stop` | `view?`/`index?` | the view |
| `reload` | `hard?`, `view?`/`index?` | the view |
| `views.list` | | view objects in window order |
| `views.activate` | `view` or `index` | the view |
| `views.close` | `view?`/`index?` | `{closed: id}`; never the last view |
| `page.uri`, `page.title`, `page.text` | `view?`/`index?` | a string |
| `page.eval` | `script`, `view?`/`index?` | the last value as JSON (`undefined` → null); kiosk refuses |
| `page.screenshot` | `path`, `view?`/`index?` | `{path, bytes}`; relative to gsurf's launch directory |
| `actions.list` | | `[{name, allowed}]` |
| `action.run` | `name`, `view?`/`index?` | `{action, performed}` |
| `mode.get` / `mode.set` | `mode: normal\|insert`, `view?` | `{mode, label, passthrough, modal_active}`; set needs `modal` |
| `tree.get` | | `{has_tree_provider, tree, grouped, groups}` (nodes carry `children`, `collapsed`) |
| `tree.move` | `view`, one of `parent` (0: root at the end), `before`, `after`, `index` | the view; `parent` needs `tabs` |
| `tree.collapse` | `view?`, `collapsed?` (default true) | the view; needs `tabs` |
| `containers.list` | | `[{id, name, color, temporary, default, n_views, rules, rules_active}]` |
| `containers.reopen` | `container`, `view?` | the new view (new id); kiosk refuses |
| `layout.describe` | | `{loaded, enabled, active, kiosk, name, prefix, status, mouse, width, height}` |
| `layout.groups` | | wmux windows with their panes |
| `layout.panes` | `window?` | one window: `index, name, current, last, zoomed, panes` (`index, id, view, uri, title, active, focused, zoomed, visible, rect, fraction`) |
| `layout.command` | `command` | the current window after running a wmux command line |
| `layout.select_group` | `window` | that window |
| `layout.rename_group` | `window?`, `name?` (none: automatic) | that window |
| `layout.show_view_at` | `view`, `pane?`, `window?`, `target?`, `zone?: center\|left\|right\|top\|bottom` | the current window (a tab dropped on a pane) |
| `config.get` | `key?` | no key: core keys as an object; a core key; `modules[.NAME[.KEY…]]` as JSON |
| `modules.list` | | `[{name, description, enabled, active, priority}]` |
| `modules.toggle` | `name`, `enabled?` (none: flip) | the module; kiosk refuses |
| `lrg.state` | | the 3D scene: `{mode, arrangement, environment, reading, history, animating, panels, background_fps, history_depth, pinned: [{view, placement}], bookmarks}` |
| `lrg.arrange` / `lrg.environment` | `arrangement?` / `environment?` (none: the next one) | the state; unknown id -32001 |
| `lrg.reading` / `lrg.history` | `enabled?` (none: toggle) | the state |
| `lrg.focus` | `view?`/`index?` | the state; makes it active and flies to it; -32001 while it has no panel yet |
| `lrg.pin` | `enabled?` (default true), `placement?: "x y z yaw w h"`, `view?`/`index?` | the state; bad placement -32602 |
| `lrg.unpin_all` | | the state |
| `lrg.camera` | `op`: `orbit` {`yaw`, `pitch`}, `zoom` {`factor` > 0}, `reset`, `overview`, `save`/`restore` {`name`} | the state; unknown bookmark -32001, bad params -32602 |
| `lrg.screenshot` | `path` | `{path, bytes}` once the next frame is written (relative to gsurf's launch directory) |

`layout.*` go through the `GsurfWindowLayout` vfuncs: without a layout they fail -32002, in kiosk mode -32003.

`lrg.*` (since 0.6) act on a `gsurf --lrg=3d` window; anywhere else, or in a build without the scene, they fail -32002. Every change also emits an `lrg.<what>` event. `config.get` knows `lrg.arrangement`, `lrg.environment`, `lrg.background-fps`, `lrg.history-depth`, `lrg.reading-single-tab`.

## Events

After `events.subscribe` each event is a notification line:

```text
{"jsonrpc":"2.0","method":"event","params":{"topic":"view","event":"view.added","time":123456789,"data":{"view":4,"uri":"","title":""}}}
```

| Topic | Events | `data` |
|---|---|---|
| `view` | `view.added`, `view.removed`, `view.active`, `view.moved` | `view, uri, title` (+ `from, to`) |
| `uri`, `title` | `uri`, `title` | `view, uri, title` |
| `load` | `load` | `view, uri, title, state` (`started`, `redirected`, `committed`, `finished`) |
| `mode` | `mode` | `mode, label, passthrough` |
| `layout` | `layout.groups`, `layout.visible` | `what` |
| `download` | `download.started` only (no progress or completion) | `uri, suggested, destination` |
| `lrg` | `lrg.arrangement`, `lrg.environment`, `lrg.reading`, `lrg.history`, `lrg.focus`, `lrg.pin`, `lrg.camera` (from keys, the pointer, methods) | `what, arrangement, environment, reading, history` |

`time` is the monotonic clock. Events follow signal order: a foreground tab gives `view.added`, `view.active`, then `uri`, `load`, `title`.

## C client and embedding

Control headers are not in `<gsurf/gsurf.h>`: include `<gsurf/control/gsurf-control.h>` (they need json-glib; `pkg-config gsurf` requires `json-glib-1.0`; a crispy-compiled `config.c` cannot use them).

```c
g_autoptr(GPtrArray) list = gsurf_control_list_instances(NULL);
g_autoptr(GsurfControlClient) client = gsurf_control_client_new();
g_autoptr(JsonNode) tabs = NULL;

gsurf_control_client_connect(client,
	gsurf_control_instance_get_socket_path(g_ptr_array_index(list, 0)), &error);
tabs = gsurf_control_client_call(client, "views.list", NULL, &error);
```

`gsurf_control_find_instance(NULL, "work")` resolves a pid, name or directory id; `_call_async()` for hosts that must not block; `_subscribe()` + `_read_event()` for events. An embedder starts a server itself: `gsurf_control_server_new(app)`, `_set_name()`, `_start(server, NULL, &error)` after its modules are active; `_stop()` removes the socket; `gsurf_control_server_dispatch()` runs a request in-process.

## Methods from a module

Implement `GsurfControlProvider` (`register_methods`) and call `gsurf_control_server_add_method(server, "mymod.status", "description", "{}", handler, provider, NULL)`. The handler answers its `GsurfControlInvocation` exactly once (`gsurf_control_invocation_return_value()` / `_return_error()`), now or later. Providers are re-asked at start, after `modules.toggle` and on `gsurf_control_server_refresh_providers()`. Name methods `<module>.<verb>`; `gsurf_control_server_emit_event()` emits a module's own topics.

## Security

Anything that can connect can run JavaScript in the user's pages, read them and open URLs: the user's own shell trust. Never forward the socket over SSH without the user asking. Kiosk mode refuses opening, reopening and navigating views, `page.eval`, module toggles and the actions kiosk forbids.
