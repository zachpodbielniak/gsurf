# gsurfctl

`gsurfctl` drives a running gsurf (0.5 or later) over its control socket: newline-delimited JSON-RPC 2.0 ([control-protocol.md](control-protocol.md)). The socket is part of every gsurf build and on by default (`control.enabled`); it is user-private (0600 in a 0700 directory, same-uid peers only). `gsurf URL` starts a new browser process; `gsurfctl open URL` adds a tab to a running one. Source of truth: `docs/gsurfctl.org`, `gsurfctl --help`, `gsurfctl help COMMAND`.

## Synopsis

```text
gsurfctl [OPTION...] COMMAND [ARGUMENTS...]
```

| Option | Meaning |
|---|---|
| `-i`, `--instance ID` | a pid, a `control.name`, or a runtime directory id (`<pid>-XXXXXX`); default `$GSURF_INSTANCE` |
| `-S`, `--socket PATH` | this socket instead of an instance |
| `-a`, `--all` | every running instance (output prefixed `[NAME]`, or one `{"instance":..,"result":..}` JSON line each; exit status is the last failure) |
| `-j`, `--json` | results as one-line JSON (`watch`: one JSON line per event) |
| `-p`, `--pretty` | indent JSON |
| `-T`, `--timeout S` | seconds to wait for a reply (default 30; 0 waits forever) |
| `-q`, `--quiet` | print nothing on success |
| `--completion bash` | print the bash completion script |
| `-V`, `--version`, `--license`, `-h`, `--help` | version, AGPLv3 text, help with examples |

**Word order.** gsurfctl's options may come before or after the command; command flags (`--bg`, `--view`, `--hard`, `--window`, …) come after the command word. After `wmux`, `eval` and `call` every remaining word belongs to the command (`gsurfctl wmux split-window -h` works), so put gsurfctl's options before those three. `--` ends option parsing.

**Instance selection**, in order: `-S`; `-i` (else `$GSURF_INSTANCE`; an unknown one exits 3); the only running instance; else the most recently focused one (the last window focus, key or button press).

**Exit status**: 0 ok; 1 the command failed (reason on stderr; with `-j` also an `error` object on stdout); 2 usage error; 3 no instance, or it could not be reached or closed the connection; 4 refused by kiosk mode.

## Commands

`VIEW` is a view id from `gsurfctl tabs` (stable; positions change as tabs move). Commands that act on a page take `--view VIEW` and default to the active view.

| Command | Method | Does |
|---|---|---|
| `list [--names]` | (local) | running instances: pid, name, views, active page |
| `info` | `instance.info` | pid, version, backend, name, socket, kiosk, windows, views, active view, focus time |
| `open [URL] [--bg] [--container ID] [--child-of VIEW]` | `open` | new tab (default URL: the new-view page); a file path opens as `file://` relative to your shell; `--container` takes an id, `temporary` or `default`; `--child-of` makes it a child in the tab tree; prints the new view |
| `nav URL` | `navigate` | load URL in the view |
| `back`, `forward`, `stop`, `reload [--hard]` | `back`, … | history and loading (`--hard` bypasses the cache) |
| `tabs` | `views.list` | `ID TITLE URI` per view; `*` active, `+` on screen |
| `tab VIEW` | `views.activate` | switch to a view |
| `close [VIEW]` | `views.close` | close a view (default the active one; never the last) |
| `tree` | `tree.get` | the tab tree, indented (grouped per wmux window with wmux) |
| `move VIEW --parent P\|--before V\|--after V\|--index N` | `tree.move` | move a view with its subtree; `--parent 0` = a root at the end (needs `tabs`) |
| `collapse VIEW [on\|off]` | `tree.collapse` | fold a subtree in the sidebar (needs `tabs`) |
| `containers` | `containers.list` | `ID NAME N views` |
| `reopen VIEW CONTAINER` | `containers.reopen` | reopen in a container (id, `temporary`, `default`), in place; prints the new view (new id) |
| `title`, `uri`, `text` | `page.*` | the page's title, URI, rendered text |
| `eval JS` | `page.eval` | run JavaScript; strings print as they are, other values as JSON (refused in kiosk) |
| `screenshot FILE` | `page.screenshot` | PNG of the visible page (0600), relative to your shell |
| `3d` | `lrg.state` | the `--lrg=3d` scene: `3d  ARRANGEMENT  ENVIRONMENT  [reading]  [moving]  (N panels)`; `-j` adds `pinned` (`{view, placement}`) and camera `bookmarks` |
| `3d-arrange [ID]` | `lrg.arrange` | `carousel`, `grid`, `free`, `single-panel`, `stack`; no ID: the next one |
| `3d-env [ID]` | `lrg.environment` | `studio`, `sky`, `workshop`, `cockpit`, `void`; no ID: the next one |
| `3d-reading [on\|off]`, `3d-history [on\|off]` | `lrg.reading`, `lrg.history` | reading mode (active tab head-on, then pixel-exact) / history depth (back/forward pages as a stack); no word: toggle |
| `3d-focus [VIEW]` | `lrg.focus` | make VIEW active and fly the camera to it |
| `3d-pin [on\|off] [--placement "X Y Z YAW W H"] [--view VIEW]`, `3d-unpin-all` | `lrg.pin`, `lrg.unpin_all` | pin a tab's panel where it is or at a placement; unpin one or all |
| `3d-camera orbit YAW [PITCH]\|zoom FACTOR\|reset\|overview\|save NAME\|restore NAME` | `lrg.camera` | move the camera (degrees; FACTOR <1 closer) or use a camera bookmark |
| `3d-shot FILE` | `lrg.screenshot` | PNG of the next 3D frame, scene and chrome as shown (relative to your shell) |
| `actions [--names]` | `actions.list` | every action name |
| `action NAME` | `action.run` | run an action as its key would (`zoom-in`, `tab-next`, `pane-split-right`, `quit`) |
| `mode [normal\|insert]` | `mode.get` / `mode.set` | print or set the vim mode (needs `modal`) |
| `panes [--window N]` | `layout.panes` | panes of the current (or Nth) wmux window |
| `windows` | `layout.groups` | wmux windows and their panes |
| `wmux 'COMMAND'` | `layout.command` | any wmux command line ([wmux.md](wmux.md)) |
| `select-window N` | `layout.select_group` | show wmux window N |
| `rename-window N [NAME]` | `layout.rename_group` | rename it (no NAME: automatic naming) |
| `config [KEY]` | `config.get` | a value (`browser.homepage`, `control.name`, `modules.wmux.prefix`); no KEY: the core keys |
| `modules [--names]` | `modules.list` | loaded modules: name, state (`active`/`off`), description |
| `module NAME [on\|off]` | `modules.toggle` | switch a module for this run (no on/off: flip; not saved) |
| `methods [--names]` | `methods.list` | every socket method |
| `watch [TOPIC...]` | `events.subscribe` | print events until interrupted: `view`, `uri`, `title`, `load`, `mode`, `layout`, `download`, `lrg` (default all) |
| `call METHOD [JSON\|K=V...]` | any | raw call; `K=V` values are JSON when they parse (`n=3`, `b=true`), else strings; a `{...}` argument merges in |
| `help [COMMAND\|--names]` | (local) | help, one command's usage, or the command names one per line (no instance needed) |

## Output

Human output is for people. Parse `-j`: `tabs` gives an array of view objects (`index`, `id`, `uri`, `title`, `active`, `visible`, `opener_id`, `parent_id`, `container`, `container_name`, `container_color`, `container_temporary`, `pane` = `{window, index}` or null); `open`, `tab`, `nav`, `reopen` give one view object; `config KEY` gives the JSON value (human output prints booleans as `yes`/`no`); `watch` gives one line per event: `{"topic":"load","event":"load","time":…,"data":{"view":4,"uri":…,"title":…,"state":"finished"}}`.

## Recipes

```bash
gsurfctl open https://example.org --bg                         # background tab
gsurfctl -j tabs | yq -p json '.[] | select(.active) | .uri'    # active tab's URI
id=$(gsurfctl -j open https://docs.gtk.org/ | yq -p json '.id') # new tab's id
gsurfctl text --view "$id" > /tmp/page.txt                      # page text for summarising
gsurfctl tab 4 && gsurfctl title
gsurfctl wmux 'split-window -h https://github.com'              # a pane on the right
gsurfctl -j config modules.wmux.enabled                        # true / false
gsurfctl mode insert                                            # let a web app have its keys
gsurfctl -a reload                                              # every instance
gsurfctl -i work open https://mail.example.org                  # a named instance
gsurfctl call open uri=https://example.org background=true      # raw method call
source <(gsurfctl --completion bash)
```

Wait until a page has loaded (start the watch before navigating):

```bash
coproc WATCH { timeout 30 gsurfctl -j watch load; }
gsurfctl nav https://example.org
grep -m 1 '"state":"finished"' <&"${WATCH[0]}" > /dev/null && gsurfctl title
```

The 3D tab space (only in a gsurf started with `--lrg=3d`; elsewhere the `3d*` commands exit 1 with `this window is not a 3D scene`):

```bash
gsurfctl 3d                                             # 3d  carousel  studio  (4 panels)
gsurfctl 3d-arrange grid && gsurfctl 3d-env sky
gsurfctl 3d-focus 7 && gsurfctl 3d-reading on           # read tab 7 flat
gsurfctl 3d-camera orbit 30 -10                         # degrees right, down
gsurfctl -j 3d | yq -p json '.pinned[] | (.view | tostring) + " " + .placement'
gsurfctl 3d-pin --view 7 --placement "1.5 0 -0.4 -20 4.2 3.1"
gsurfctl 3d-shot /tmp/space.png
```

A full session script: [../templates/gsurfctl-session.sh.template](../templates/gsurfctl-session.sh.template).

## Notes

- `close`, `module NAME off`, `action quit` and wmux kill commands change the user's live session; ask first.
- `watch` keeps the connection open and ends quietly when gsurf quits; bound it with `timeout`.
- `make install` installs the completion to `$(DATADIR)/bash-completion/completions/gsurfctl`. The AppImage runs it as `gsurf-x86_64.AppImage ctl COMMAND`.
