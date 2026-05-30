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
GsurfApplication  *gsurf_module_manager_get_application(GsurfModuleManager *self);
void               gsurf_module_manager_set_config(GsurfModuleManager *self, GsurfConfig *config);
GsurfConfig       *gsurf_module_manager_get_config(GsurfModuleManager *self);
GsurfView         *gsurf_module_manager_get_active_view(GsurfModuleManager *self);
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
GsurfModule *gsurf_module_manager_load_module(GsurfModuleManager *self, const gchar *path, GError **error);
guint        gsurf_module_manager_load_from_directory(GsurfModuleManager *self, const gchar *dir);
void         gsurf_module_manager_add_module(GsurfModuleManager *self, GsurfModule *module);
GsurfModule *gsurf_module_manager_get_module(GsurfModuleManager *self, const gchar *name);
GPtrArray   *gsurf_module_manager_get_modules(GsurfModuleManager *self);
void         gsurf_module_manager_activate_all(GsurfModuleManager *self);
void         gsurf_module_manager_deactivate_all(GsurfModuleManager *self);

/* --- Hook dispatch --- */
gboolean            gsurf_module_manager_dispatch_key_event(GsurfModuleManager *self, GsurfView *view,
                                                            guint keyval, guint keycode, guint state,
                                                            GsurfModePolicy mode);
gboolean            gsurf_module_manager_dispatch_mouse_event(GsurfModuleManager *self, GsurfView *view,
                                                              guint button, guint state);
gchar              *gsurf_module_manager_dispatch_rewrite_uri(GsurfModuleManager *self, const gchar *input);
GsurfPolicyDecision gsurf_module_manager_dispatch_before_navigate(GsurfModuleManager *self, GsurfView *view, const gchar *uri);
void                gsurf_module_manager_dispatch_after_navigate(GsurfModuleManager *self, GsurfView *view, const gchar *uri);
void                gsurf_module_manager_dispatch_load_changed(GsurfModuleManager *self, GsurfView *view, GsurfLoadEvent event);
void                gsurf_module_manager_dispatch_inject_scripts(GsurfModuleManager *self, GsurfView *view, const gchar *uri);
void                gsurf_module_manager_dispatch_apply_settings(GsurfModuleManager *self, GsurfView *view, gpointer settings, const gchar *uri);
GsurfPermissionVerdict gsurf_module_manager_dispatch_permission(GsurfModuleManager *self, GsurfView *view, const gchar *type, const gchar *origin);
gchar              *gsurf_module_manager_dispatch_decide_destination(GsurfModuleManager *self, const gchar *uri, const gchar *suggested);
GsurfTlsDecision    gsurf_module_manager_dispatch_verify_cert(GsurfModuleManager *self, const gchar *host, guint tls_errors);
GsurfFilterVerdict  gsurf_module_manager_dispatch_filter_request(GsurfModuleManager *self, GsurfView *view, const gchar *uri, gchar **redirect_uri);
void                gsurf_module_manager_dispatch_populate_menu(GsurfModuleManager *self, GsurfHitTest *hit, GPtrArray *items);
gchar              *gsurf_module_manager_dispatch_status_text(GsurfModuleManager *self, GsurfView *view);
void                gsurf_module_manager_dispatch_render_overlay(GsurfModuleManager *self, GsurfView *view, gpointer draw_target);

G_END_DECLS

#endif /* GSURF_MODULE_MANAGER_H */
