# gsurfctl

`gsurfctl` drives a running gsurf over its control socket (JSON-RPC 2.0, newline-delimited; see [control-protocol.md](control-protocol.md)). The socket is built into every gsurf and on by default; it is user-private (0600 in a 0700 directory, same-uid peers only). `gsurf URL` starts a new browser process; `gsurfctl open URL` adds a tab to a running one.

## Synopsis

```text
gsurfctl [OPTIONS] COMMAND [ARGS…]
```

Options may appear anywhere on the line.

| Option | Meaning |
|---|---|
| `-i`, `--instance PID\|NAME` | target one instance (a NAME comes from `control.name`) |
| `-a`, `--all` | send the command to every instance |
| `-S`, `--socket PATH` | use this socket; overrides instance selection |
| `-j`, `--json` | JSON output (one document; `watch` prints one per line) |
| `-p`, `--pretty` | indented JSON (with `-j`) |
| `-T`, `--timeout MS` | give up waiting for a reply |
| `-q`, `--quiet` | no output on success |
| `-V`, `--version` | print the version |
| `--completion bash` | print the bash completion script |
| `--license` | print the AGPLv3 license |
| `-h`, `--help` | usage with examples |

**Instance selection**: `-S`, else `-i`, else `$GSURF_INSTANCE`, else the only running instance, else the most recently focused one.

**Exit status**: 0 ok, 1 the command failed, 2 usage error, 3 no instance or cannot connect, 4 refused by kiosk mode.

## Commands

| Command | Does |
|---|---|
| `list` | running instances: pid, name, version, backend, windows, views, active URI and title |
| `open URL [--bg] [--container ID] [--child-of VIEW]` | new tab (`--bg`: stay on the current tab; `--container`: an id, `temporary` or `default`; `--child-of`: opener in the tab tree); prints the new view |
| `nav URL` | load URL in the active tab |
| `back`, `forward`, `reload [--hard]`, `stop` | history and loading on the active tab (`--hard` bypasses the cache) |
| `tabs` | views: id, opener, parent, container, title, URI, active, visible, pane |
| `tab VIEW` | activate a view |
| `close VIEW` | close a view (never the last one) |
| `tree` | the tab tree (grouped by wmux window when wmux runs) |
| `move VIEW --parent P \| --before V \| --after V` | re-parent or reorder a subtree (needs `tabs`) |
| `collapse VIEW [on\|off]` | collapse or expand a subtree (needs `tabs`) |
| `containers` | the default container and every registry container with its host rules |
| `reopen VIEW CONTAINER` | reopen a tab in another container, in place; prints the new view (new id) |
| `title`, `uri`, `text` | the active page's title, URI, rendered text |
| `eval JS` | run JavaScript in the active page and print the result (refused in kiosk mode) |
| `screenshot FILE` | PNG of the visible page |
| `actions` | every action name and whether kiosk mode allows it |
| `action NAME` | perform an action as its key would (`zoom-in`, `tab-sidebar-toggle`, `pane-split-right`, …) |
| `mode [normal\|insert]` | print or set the vim mode (needs `modal`) |
| `panes` | panes of the current wmux window: index, id, view, active, zoomed, rectangle |
| `windows` | wmux windows: index, name, current, last |
| `wmux 'COMMAND LINE'` | run one wmux command ([wmux.md](wmux.md)); errors come back as exit 1 |
| `select-window N`, `rename-window N NAME` | wmux window shortcuts |
| `config KEY` | a configuration value as the instance uses it (`browser.homepage`, `modules.wmux.enabled`) |
| `modules` | modules: name, loaded, enabled, active |
| `module NAME on\|off` | activate or deactivate a module for this run (not saved) |
| `watch [TOPIC…]` | stream events until interrupted: `view`, `uri`, `title`, `load`, `mode`, `layout`, `download` |
| `call METHOD [JSON \| K=V…]` | raw JSON-RPC call (`gsurfctl call methods.list`) |
| `help [CMD]`, `help --names` | help; `--names` prints one command name per line |

`VIEW` is a stable view id from `tabs` (positions change as tabs move).

## Recipes

```bash
gsurfctl open --bg https://example.org                     # background tab
gsurfctl -j tabs | yq '.[] | select(.active) | .uri'        # active tab's URI
gsurfctl -j open https://docs.gtk.org/ | yq '.id'           # id of the new tab
gsurfctl text > /tmp/page.txt                               # page text for summarising
gsurfctl wmux 'split-window -h https://github.com'          # a pane on the right
gsurfctl mode insert                                        # let a web app have its keys
gsurfctl -a reload                                          # every instance
GSURF_INSTANCE=work gsurfctl tabs                           # a named instance
gsurfctl watch load uri                                     # follow page loads
```

Wait for a load to finish:

```bash
gsurfctl nav https://example.org
gsurfctl -j watch load | yq -p json 'select(.params.event == "finished") | .params.view' | head -n 1
```

A full session script: [../templates/gsurfctl-session.sh.template](../templates/gsurfctl-session.sh.template).

## Notes

- Commands act on the active view of the chosen instance unless they take a `VIEW`.
- `-j` output is the JSON-RPC `result` object; the human output is for people, do not parse it.
- `watch` keeps the connection open; stop it with `head`, `timeout` or Ctrl+C.
- Bash completion: `gsurfctl --completion bash > ~/.local/share/bash-completion/completions/gsurfctl` (installed by `make install` under `$(DATADIR)/bash-completion/completions/`).
- `module NAME off` and `close` change the user's live session; ask before running them.
