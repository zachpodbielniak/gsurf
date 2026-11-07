/*
 * gsurf-module-manager.h - Module registry, loader, and hook dispatch
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads .so modules from the module directory (via GModule), holds them
 * in priority order, and dispatches hooks to the modules that implement
 * the matching interface. Modules reach browser state (active view,
 * window, config) through this manager. Mirrors gst's GstModuleManager.
 */

#ifndef GSURF_MODULE_MANAGER_H
#define GSURF_MODULE_MANAGER_H

#include <glib-object.h>

#include "gsurf-enums.h"
#include "module/gsurf-module.h"
#include "core/gsurf-view.h"
#include "core/gsurf-application.h"
#include "config/gsurf-config.h"

G_BEGIN_DECLS

#define GSURF_TYPE_MODULE_MANAGER (gsurf_module_manager_get_type())

G_DECLARE_FINAL_TYPE(GsurfModuleManager, gsurf_module_manager, GSURF, MODULE_MANAGER, GObject)

/**
 * gsurf_module_manager_get_default:
 *
 * Returns: (transfer none): the process-wide module manager (created on
 *   first use).
 */
GsurfModuleManager *gsurf_module_manager_get_default(void);

GsurfModuleManager *gsurf_module_manager_new(void);

/* --- Context (set by the host so modules can reach browser state) --- */
void               gsurf_module_manager_set_application(GsurfModuleManager *self, GsurfApplication *app);
/**
 * gsurf_module_manager_get_application:
 * @self: a #GsurfModuleManager
 *
 * Returns: (transfer none) (nullable): the host application
 */
GsurfApplication  *gsurf_module_manager_get_application(GsurfModuleManager *self);
void               gsurf_module_manager_set_config(GsurfModuleManager *self, GsurfConfig *config);
/**
 * gsurf_module_manager_get_config:
 * @self: a #GsurfModuleManager
 *
 * Returns: (transfer none) (nullable): the configuration
 */
GsurfConfig       *gsurf_module_manager_get_config(GsurfModuleManager *self);
/**
 * gsurf_module_manager_get_active_view:
 * @self: a #GsurfModuleManager
 *
 * Returns: (transfer none) (nullable): the active view
 */
GsurfView         *gsurf_module_manager_get_active_view(GsurfModuleManager *self);
/**
 * gsurf_module_manager_get_active_window:
 * @self: a #GsurfModuleManager
 *
 * Returns: (transfer none) (nullable): the active window
 */
GsurfWindow       *gsurf_module_manager_get_active_window(GsurfModuleManager *self);

/**
 * gsurf_module_manager_set_input_passthrough:
 * @self: a #GsurfModuleManager
 * @passthrough: whether bare keys should bypass input-handler modules
 *
 * Set by the modal module when it enters a full-passthrough mode (INSERT)
 * so the host suppresses bare-key dispatch to all modules and lets the
 * page receive the keys. Modified keys and Escape are unaffected.
 */
void               gsurf_module_manager_set_input_passthrough(GsurfModuleManager *self, gboolean passthrough);

/**
 * gsurf_module_manager_get_input_passthrough:
 * @self: a #GsurfModuleManager
 *
 * Returns: whether input passthrough is currently active.
 */
gboolean           gsurf_module_manager_get_input_passthrough(GsurfModuleManager *self);

/**
 * gsurf_module_manager_set_mode:
 * @self: a #GsurfModuleManager
 * @mode: the input mode now in effect
 *
 * Publishes the input mode of the module that owns the keyboard modes
 * (the modal module: NORMAL, INSERT, FOLLOW while hints are up), so chrome
 * can show it (the chromebar's and wmux's "-- INSERT --" indicators).
 * The mode in effect (gsurf_module_manager_get_mode()) is this mode,
 * except that NORMAL reads INSERT while the user is typing into a text
 * field of the active window. Emits #GsurfModuleManager::mode-changed
 * and notifies #GsurfModuleManager:mode when the mode in effect changes;
 * setting the current mode again does nothing. It does not change the
 * input passthrough (gsurf_module_manager_set_input_passthrough()),
 * which stays the modal module's separate decision.
 *
 * Since: 0.4.2
 */
void               gsurf_module_manager_set_mode(GsurfModuleManager *self, GsurfModePolicy mode);

/**
 * gsurf_module_manager_get_mode:
 * @self: a #GsurfModuleManager
 *
 * The input mode in effect: what a bare key does right now. It is the
 * mode last published with gsurf_module_manager_set_mode()
 * (%GSURF_MODE_NORMAL until then), except that NORMAL reads
 * %GSURF_MODE_INSERT while the active window is typing
 * (gsurf_window_get_typing(): a focused field in the active view's page,
 * or a focused chrome entry such as the address bar), since those keys
 * go to the field as text. It follows focus, the active view and the
 * active window as they change; the mode indicators and the control
 * protocol's mode report show it.
 *
 * Returns: the input mode in effect
 *
 * Since: 0.4.2
 */
GsurfModePolicy    gsurf_module_manager_get_mode(GsurfModuleManager *self);

/**
 * gsurf_module_manager_is_typing:
 * @self: a #GsurfModuleManager
 * @view: (nullable): the view a key press is for (the window's active view)
 *
 * Whether a bare key pressed for @view is text for the page rather than a
 * command, which is how the gsurf binary routes keys: the mode owner passes
 * keys through (INSERT; gsurf_module_manager_set_input_passthrough()), or
 * @view has a focused page field (gsurf_view_get_editing()) while the mode
 * is NORMAL. A mode that owns the keys itself, such as FOLLOW while link
 * hints are up, keeps them even over a focused field. Modified keys,
 * Escape and key grabs are the host's own decision.
 *
 * Returns: %TRUE if bare keys should go to the page
 *
 * Since: 0.5.9
 */
gboolean           gsurf_module_manager_is_typing(GsurfModuleManager *self, GsurfView *view);

/* --- Module search paths (for embedders / non-CLI hosts) ---
 *
 * The library never loads modules on its own: a host configures one or more
 * search directories and then calls gsurf_module_manager_load_modules().
 * An embedding application (e.g. cmacs) therefore loads ONLY the modules it
 * points the manager at — it will not pick up a system-installed gsurf's
 * modules, and the GSURF_MODULE_PATH environment variable and the built-in
 * system directory are *not* consulted automatically (that fallback is the
 * gsurf command-line binary's own policy, not the library's). An embedder
 * that explicitly wants the system modules can add
 * gsurf_module_manager_get_system_module_dir() to the search paths.
 */

/**
 * gsurf_module_manager_add_search_path:
 * @self: a #GsurfModuleManager
 * @dir: a directory to append to the module search path
 *
 * Appends @dir to the ordered list of directories searched by
 * gsurf_module_manager_load_modules(). Duplicate paths are ignored.
 */
void         gsurf_module_manager_add_search_path(GsurfModuleManager *self, const gchar *dir);

/**
 * gsurf_module_manager_set_search_paths:
 * @self: a #GsurfModuleManager
 * @dirs: (array zero-terminated=1) (nullable): the directories, or %NULL to clear
 *
 * Replaces the module search path with @dirs (in order).
 */
void         gsurf_module_manager_set_search_paths(GsurfModuleManager *self, const gchar *const *dirs);

/**
 * gsurf_module_manager_clear_search_paths:
 * @self: a #GsurfModuleManager
 *
 * Removes all configured module search paths.
 */
void         gsurf_module_manager_clear_search_paths(GsurfModuleManager *self);

/**
 * gsurf_module_manager_get_search_paths:
 * @self: a #GsurfModuleManager
 *
 * Returns: (transfer none) (element-type utf8): the ordered search paths.
 */
GPtrArray   *gsurf_module_manager_get_search_paths(GsurfModuleManager *self);

/**
 * gsurf_module_manager_load_modules:
 * @self: a #GsurfModuleManager
 *
 * Loads every `*.so` module from the configured search paths, in path
 * order. A module file name seen in an earlier path wins: the same
 * basename in a later path is skipped (first-match, like $PATH).
 *
 * Returns: the number of modules loaded.
 */
guint        gsurf_module_manager_load_modules(GsurfModuleManager *self);

/**
 * gsurf_module_manager_get_system_module_dir:
 *
 * Returns: (transfer none): the compile-time system module directory
 *   (`GSURF_MODULEDIR`). Never added to the search path automatically;
 *   an embedder may add it explicitly to opt in to system modules.
 */
const gchar *gsurf_module_manager_get_system_module_dir(void);

/* --- Loading / registration --- */
/**
 * gsurf_module_manager_load_module:
 * @self: a #GsurfModuleManager
 * @path: module shared object path
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer none) (nullable): the module owned by the manager, or %NULL on error
 */
GsurfModule *gsurf_module_manager_load_module(GsurfModuleManager *self, const gchar *path, GError **error);
guint        gsurf_module_manager_load_from_directory(GsurfModuleManager *self, const gchar *dir);
void         gsurf_module_manager_add_module(GsurfModuleManager *self, GsurfModule *module);
/**
 * gsurf_module_manager_get_module:
 * @self: a #GsurfModuleManager
 * @name: registered module name
 *
 * Returns: (transfer none) (nullable): the named module
 */
GsurfModule *gsurf_module_manager_get_module(GsurfModuleManager *self, const gchar *name);
/**
 * gsurf_module_manager_get_modules:
 * @self: a #GsurfModuleManager
 *
 * Returns: (transfer none) (element-type GsurfModule): the manager-owned modules
 */
GPtrArray   *gsurf_module_manager_get_modules(GsurfModuleManager *self);
void         gsurf_module_manager_activate_all(GsurfModuleManager *self);
void         gsurf_module_manager_deactivate_all(GsurfModuleManager *self);

/* --- Action execution (since 0.4.4) --- */

/**
 * GsurfActionExecutor:
 * @window: the window to act in
 * @view: (nullable): the view to act on (normally @window's active view)
 * @action: the action to perform
 * @user_data: (closure): data passed to
 *   gsurf_module_manager_set_action_executor()
 *
 * Performs @action the way the host's keybindings do: the host's
 * built-in actions (back, zoom, scroll, ...) and, for the rest,
 * gsurf_module_manager_dispatch_action().
 *
 * Returns: %TRUE when @action was performed
 *
 * Since: 0.4.4
 */
typedef gboolean (*GsurfActionExecutor) (GsurfWindow *window, GsurfView *view,
                                         GsurfAction action, gpointer user_data);

/**
 * gsurf_module_manager_set_action_executor:
 * @self: a #GsurfModuleManager
 * @executor: (nullable) (scope notified) (closure user_data) (destroy destroy):
 *   the host's action executor, or %NULL to remove it
 * @user_data: data for @executor
 * @destroy: (nullable): frees @user_data when the executor is replaced or
 *   the manager is disposed
 *
 * Registers the host's implementation of the keybind actions, so modules
 * (the MCP server's run_action) can perform any #GsurfAction exactly as
 * a key press would. The gsurf binary registers its own; an embedder may
 * register one or leave it unset (then only module actions run).
 *
 * Since: 0.4.4
 */
void                gsurf_module_manager_set_action_executor(GsurfModuleManager *self,
                                                             GsurfActionExecutor executor,
                                                             gpointer user_data,
                                                             GDestroyNotify destroy);

/**
 * gsurf_module_manager_execute_action:
 * @self: a #GsurfModuleManager
 * @window: (nullable): the window to act in; %NULL for the active window
 * @view: (nullable): the view to act on; %NULL for @window's active view
 * @action: the action to perform
 * @error: return location for a #GError
 *
 * Performs @action through the host's executor (see
 * gsurf_module_manager_set_action_executor()), or, without one (or
 * without a window to act in), through
 * gsurf_module_manager_dispatch_action(). In kiosk mode
 * (gsurf_kiosk_is_enabled()) an action gsurf_kiosk_allows_action()
 * refuses fails with %G_IO_ERROR_PERMISSION_DENIED and nothing runs.
 * %GSURF_ACTION_NONE and out-of-range values fail with
 * %G_IO_ERROR_INVALID_ARGUMENT; an action nothing performed (no host
 * implementation and no module claiming it) fails with
 * %G_IO_ERROR_NOT_SUPPORTED.
 *
 * Returns: %TRUE when @action was performed
 *
 * Since: 0.4.4
 */
gboolean            gsurf_module_manager_execute_action(GsurfModuleManager *self,
                                                        GsurfWindow *window, GsurfView *view,
                                                        GsurfAction action, GError **error);

/* --- Hook dispatch --- */
gboolean            gsurf_module_manager_dispatch_key_event(GsurfModuleManager *self, GsurfView *view,
                                                            guint keyval, guint keycode, guint state,
                                                            GsurfModePolicy mode);
/**
 * gsurf_module_manager_dispatch_action:
 * @self: a #GsurfModuleManager
 * @view: (nullable): the active view
 * @action: a keybind action the host has no built-in implementation for
 *
 * Offers @action to every active #GsurfActionHandler module in priority
 * order; the first to return %TRUE performs it.
 *
 * Returns: %TRUE if a module performed the action, %FALSE otherwise
 */
gboolean            gsurf_module_manager_dispatch_action(GsurfModuleManager *self, GsurfView *view,
                                                         GsurfAction action);
gboolean            gsurf_module_manager_dispatch_mouse_event(GsurfModuleManager *self, GsurfView *view,
                                                              guint button, guint state);
gchar              *gsurf_module_manager_dispatch_rewrite_uri(GsurfModuleManager *self, const gchar *input);
GsurfPolicyDecision gsurf_module_manager_dispatch_before_navigate(GsurfModuleManager *self, GsurfView *view, const gchar *uri);
/**
 * gsurf_module_manager_dispatch_main_response:
 * @self: a #GsurfModuleManager
 * @view: the view whose main frame received a displayable response
 * @uri: the response URI
 *
 * Asks active #GsurfNavigationHook modules, in priority order, whether
 * the main frame may display the response; the first non-USE verdict
 * wins. Called by the WebKit backends from the response policy decision.
 *
 * Returns: the verdict
 */
GsurfPolicyDecision gsurf_module_manager_dispatch_main_response(GsurfModuleManager *self, GsurfView *view, const gchar *uri);
void                gsurf_module_manager_dispatch_after_navigate(GsurfModuleManager *self, GsurfView *view, const gchar *uri);
void                gsurf_module_manager_dispatch_load_changed(GsurfModuleManager *self, GsurfView *view, GsurfLoadEvent event);
void                gsurf_module_manager_dispatch_inject_scripts(GsurfModuleManager *self, GsurfView *view, const gchar *uri);
void                gsurf_module_manager_dispatch_apply_settings(GsurfModuleManager *self, GsurfView *view, gpointer settings, const gchar *uri);
GsurfPermissionVerdict gsurf_module_manager_dispatch_permission(GsurfModuleManager *self, GsurfView *view, const gchar *type, const gchar *origin);
gchar              *gsurf_module_manager_dispatch_decide_destination(GsurfModuleManager *self, const gchar *uri, const gchar *suggested);
GsurfTlsDecision    gsurf_module_manager_dispatch_verify_cert(GsurfModuleManager *self, const gchar *host, guint tls_errors);
GsurfFilterVerdict  gsurf_module_manager_dispatch_filter_request(GsurfModuleManager *self, GsurfView *view, const gchar *uri, gchar **redirect_uri);
/**
 * gsurf_module_manager_dispatch_populate_menu:
 * @self: a #GsurfModuleManager
 * @hit: hit-test context
 * @items: (element-type GsurfMenuItem): caller-owned array receiving menu entries
 *
 * Appends module-provided menu items to @items.
 */
void                gsurf_module_manager_dispatch_populate_menu(GsurfModuleManager *self, GsurfHitTest *hit, GPtrArray *items);
gchar              *gsurf_module_manager_dispatch_status_text(GsurfModuleManager *self, GsurfView *view);
void                gsurf_module_manager_dispatch_render_overlay(GsurfModuleManager *self, GsurfView *view, gpointer draw_target);

/**
 * gsurf_module_manager_collect_keybinds:
 * @self: a #GsurfModuleManager
 *
 * Builds the current keybinding help list: every core `keybinds:` entry
 * plus each *active* module that implements #GsurfKeybindProvider. The
 * result is sorted with core rows first, then by module name and key.
 * Re-run this at display time; do not cache it across config or module
 * changes.
 *
 * Returns: (transfer full) (element-type GsurfKeybindHelp): the rows
 */
GPtrArray          *gsurf_module_manager_collect_keybinds(GsurfModuleManager *self);

G_END_DECLS

#endif /* GSURF_MODULE_MANAGER_H */
