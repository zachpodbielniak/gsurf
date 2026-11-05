# Troubleshooting

Start with evidence: gsurf logs to stderr. `G_MESSAGES_DEBUG=all gsurf … 2>&1 | tee /tmp/gsurf.log` shows debug lines too (wmux restore mismatches, module activation). For a crash: `coredumpctl list gsurf`, `coredumpctl info <pid>` (the `/lql` skill can query coredumps).

## Configuration has no effect

| Symptom | Cause | Fix |
|---|---|---|
| After editing `config.yaml`, vim keys, the address bar, search or history stopped working | the file's `modules:` block **replaced** the built-in one; every module not listed is off | put the full module set in the file (`gsurf --generate-yaml-config` as a base) with `enabled: true` each |
| Same, with a `config.c` | `gsurf_config_load_from_data()` replaced the modules mapping with the C string | list every module in the embedded YAML |
| A key in a module section does nothing | the key is not read (see *unread* in [modules-catalog.md](modules-catalog.md)), misspelt (unknown keys are ignored silently; only `wmux`, `dark_mode` and autofill profiles warn), or the module is off | check the catalog; `gsurfctl modules`; `gsurfctl config modules.NAME` |
| A `keybinds:` entry does nothing | unknown action names are skipped without a warning; or a module consumed the key first | check the name against `gsurfctl actions`; press `?` to see who owns the key |
| `config.c` changes ignored | compile failed (`C config '<path>': …` on stderr), no `gcc`, `--no-c-config`, or headers not found on an installed gsurf | fix the error; add `#define CRISPY_PARAMS "-I/usr/include/gsurf"` for installed headers |
| Root `search_engines:` / `uri_params:` / `site_styles:` / `cert_pins:` ignored | only the `modules.<name>` sections are read | move them under `modules:` |
| `storage.download_dir` ignored | not read by the core | `modules.downloads.dir` |
| Setting applied after a restart only | settings and module options are read at startup | restart; for one run, `gsurfctl module NAME on|off` toggles a module |

## Keys

- **`C-l` does not open the address bar**: wmux binds `C-h/j/k/l` in its root table. Use `o`, or set `root_bindings: {"C-l": ""}` (and move pane navigation to `M-h/j/k/l`).
- **A web editor never sees `Ctrl+h` / `Ctrl+b`**: wmux's root table or prefix takes them. `C-b C-b` sends one `C-b`; unbind the root key.
- **Two modules bind one key**: the defaults collide in a few places: `session.key_save` and `toggles.keys.javascript` are both `Ctrl+Shift+s`; `tabs.key_reopen` and a `toggles` `strict_tls` entry share `Ctrl+Shift+t` (the latter does nothing anyway). The module that runs first (lower priority number) wins. Rebind one.
- **Bare letters typed into a page trigger commands**: the field is inside a closed shadow root that the focus tracker cannot see. Press `i` first (INSERT), `Escape` to leave.
- **`Ctrl+Shift+period` binding never fires**: write it as GDK reports it, or let the normaliser do it: `Ctrl+Shift+.`, `Ctrl+Shift+period` and `Ctrl+Shift+greater` are the same binding. Without `Shift`, `Ctrl+.` is the unshifted key.
- **Kiosk mode refuses an action**: by design (`gsurf --kiosk`): URL entry, new views, popups, the inspector and the context menu are off whatever is bound. gsurfctl exits 4.

## gsurfctl

| Exit / message | Meaning | Check |
|---|---|---|
| 3, no instance | no live gsurf with a control socket for this user | `gsurfctl list`; is gsurf running as this user with the same `$XDG_RUNTIME_DIR`? (`sudo`, distrobox and SSH sessions can differ) |
| 3 with gsurf running | `control.enabled: false`, an embedder that did not start `GsurfControlServer`, or an older gsurf without the control socket (before 0.5.0) | `gsurf --version`; enable `control:`; use `-S PATH` |
| 3 with `-i NAME` | no `<name>.sock` link: the instance has no `control.name` | `gsurfctl list` shows names and pids |
| 2 | usage error | `gsurfctl help CMD` |
| 4 | refused by kiosk mode | expected in `--kiosk` |
| 1 with "enable the wmux module" / "tabs module" / "modal module" | the feature's module is off | enable it in the config and restart |

## Builds

- **GTK4 build: `Package webkitgtk-6.0 was not found`**: `webkitgtk6.0-devel` (and `javascriptcoregtk6.0-devel`) are not in the image. Tell the user to layer them into the image build; do not install on the host.
- **`gdk/gdkkeysyms.h` cannot be included directly** (GTK 4.24): include `<gdk/gdk.h>`.
- **`LRG_BACKEND=1 but graylib was not found`**: `git submodule update --init --recursive`, or `LIBREGNUM_DIR=<a recursively checked-out libregnum>`.
- **`upload-pack: not our ref` during submodule update**: a nested submodule commit was never pushed; tell the user.
- **A warning appears in only one backend**: build both (`BUILDDIR=build-gtk4` for GTK4); zero warnings is required on each.
- **Stale objects after switching flags**: should not happen (`.build-flags` stamp); `make clean-all` if a tree was copied by hand.

## Tests

- **`test-gui: xvfb-run not found; skipping`**: provide an `xvfb-run` (package `xorg-x11-server-Xvfb`) or a wrapper of that name (headless mutter; see [building.md](building.md)). A skip is not a pass.
- **WebKitGTK 6 GUI test aborts with "failed to receive credentials" or a bubblewrap `Not a directory`**: the test binary was run by absolute path; run it relative to the repo root as `make test-gui` does.
- **Rare GTK4 abort "Duplicate call to gdk_frame_timings_presented()" under headless mutter**: environment flake; rerun once, and report it if it repeats.
- **A test fails only under `ASAN=1`**: a real leak or use-after-free; fix it.

## Runtime

- **Logged out of every site at each start**: `modules.containers` is enabled with `new_tab: temporary` (the default when the module is on), so tabs start in throwaway containers. Use `new_tab: default` or assign the site to a persistent container.
- **A tab switched container and its id changed**: a view cannot change session; host assignment and `reopen` create a new view. Re-read ids with `gsurfctl tabs`.
- **A popup or OAuth window is in the "wrong" container**: popups always share the opener's container (WebKit requirement).
- **Blank window and GTK criticals on FreeBSD**: GVfs breaks WebKit there; gsurf sets `GIO_USE_VFS=local` unless `GSURF_GVFS=1`.
- **`1.blob` files in the launch directory**: only gsurf older than the runtime-directory change wrote them; current gsurf runs in `$XDG_RUNTIME_DIR/gsurf/<pid>-XXXXXX`. Upgrade.
- **MCP client cannot connect**: the module needs an `MCP=1` build, `modules.mcp.enabled: true`, the tool enabled under `tools:`, and the socket printed at activation (`$XDG_RUNTIME_DIR/gsurf-mcp-<pid or name>.sock`).
- **`Transport error: Remote end closed connection`**: mcp-glib noise when an MCP client disconnects; harmless.
- **Adblock lists missing**: `make adblock-lists` downloads them into `~/.config/gsurf/adblock/` (network access; ask first). Missing list files warn and are skipped.
- **Card data refused by autofill**: the profile file must be a regular 0600 file owned by the user; the warning never prints the value. Do not open the file.
