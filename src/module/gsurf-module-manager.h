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
