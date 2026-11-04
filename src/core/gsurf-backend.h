/*
 * gsurf-backend.h - Backend factory and initialization
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The single place in the core library that knows which concrete backends were
 * compiled in. Historically a build contained exactly one backend, chosen at
 * compile time. It can now contain several: the two GTK backends (GTK3/WebKit2GTK
 * and GTK4/WebKitGTK) remain mutually exclusive, but the libregnum backend
 * (%GSURF_BACKEND_LRG) is additive and selected at runtime. cmacs, for instance,
 * builds GTK3 + LRG into one libgsurf and uses GTK for pgtk frames and LRG for
 * @samp{--lrg} frames.
 *
 * Everything outside this file uses the abstract #GsurfView / #GsurfWindow API.
 */

#ifndef GSURF_BACKEND_H
#define GSURF_BACKEND_H

#include "gsurf-enums.h"
#include "core/gsurf-view.h"
#include "window/gsurf-window.h"

G_BEGIN_DECLS

/**
 * gsurf_backend_is_available:
 * @type: a #GsurfBackendType
 *
 * Returns: %TRUE if @type was compiled into this libgsurf and can be used.
 */
gboolean gsurf_backend_is_available(GsurfBackendType type);

/**
 * gsurf_backend_get_default_type:
 *
 * The backend used by the parameterless factory functions (gsurf_view_new(),
 * gsurf_window_new(), gsurf_backend_init()). Defaults to the compiled-in GTK
 * backend when present, otherwise to %GSURF_BACKEND_LRG. Override with
 * gsurf_backend_set_default_type() before creating any view/window.
 *
 * Returns: the current default #GsurfBackendType.
 */
GsurfBackendType gsurf_backend_get_default_type(void);

/**
 * gsurf_backend_set_default_type:
 * @type: a #GsurfBackendType (must be available)
 *
 * Selects the default backend. A no-op (with a warning) if @type is not
 * available. Intended to be called once at startup (e.g. by the @samp{--lrg}
 * command-line handler, or by cmacs based on the active Emacs display backend).
 *
 * Returns: %TRUE on success.
 */
gboolean gsurf_backend_set_default_type(GsurfBackendType type);

/**
 * gsurf_backend_type_to_name:
 * @type: a #GsurfBackendType
 *
 * Returns: (transfer none): a human-readable backend name (e.g. "gtk3-webkit2",
 *   "lrg-webkitgtk", "lrg-wpe"), or "unknown".
 */
const gchar *gsurf_backend_type_to_name(GsurfBackendType type);

/**
 * gsurf_backend_init_backend:
 * @type: the backend to initialize
 * @argc: (inout) (optional): address of argc from main()
 * @argv: (inout) (array length=argc) (optional): address of argv
 * @error: (out) (optional): location for an error
 *
 * Initializes a specific backend (e.g. gtk_init for the GTK backends, or the
 * EGL/web-engine bootstrap for the LRG backend). Must be called before creating
 * windows/views of that backend. Safe to call more than once per backend.
 *
 * Returns: %TRUE on success.
 */
gboolean gsurf_backend_init_backend(GsurfBackendType type, int *argc,
                                    char ***argv, GError **error);

/**
 * gsurf_view_new_for_backend:
 * @type: the backend to create the view for (must be available)
 *
 * Returns: (transfer full) (nullable): a new #GsurfView of @type, or %NULL if
 *   @type is unavailable.
 */
GsurfView *gsurf_view_new_for_backend(GsurfBackendType type);

/**
 * gsurf_window_new_for_backend:
 * @type: the backend to create the window for (must be available)
 *
 * Returns: (transfer full) (nullable): a new #GsurfWindow of @type, or %NULL if
 *   @type is unavailable.
 */
GsurfWindow *gsurf_window_new_for_backend(GsurfBackendType type);

/* --- Convenience wrappers over the current default backend --- */

/**
 * gsurf_backend_init:
 * @argc: (inout) (optional): address of argc from main()
 * @argv: (inout) (array length=argc) (optional): address of argv
 * @error: (out) (optional): location for an error
 *
 * Initializes the default backend (see gsurf_backend_get_default_type()).
 *
 * Returns: %TRUE on success
 */
gboolean gsurf_backend_init(int *argc, char ***argv, GError **error);

/**
 * gsurf_backend_get_backend_type:
 *
 * Returns: the current default #GsurfBackendType.
 */
GsurfBackendType gsurf_backend_get_backend_type(void);

/**
 * gsurf_backend_get_name:
 *
 * Returns: (transfer none): the current default backend's human-readable name.
 */
const gchar *gsurf_backend_get_name(void);

/**
 * gsurf_view_new:
 *
 * Creates a new view of the current default backend type.
 *
 * Returns: (transfer full): a new #GsurfView
 */
GsurfView *gsurf_view_new(void);

/**
 * gsurf_view_new_full:
 * @opener: (nullable): the view this one is opened from (link, popup, hint)
 * @container_id: (nullable): requested container id, or
 *   #GSURF_CONTAINER_TEMPORARY; the container registry has the final say
 * @initial_uri: (nullable): the URI the caller is about to load; only a
 *   placement hint, it is not loaded
 * @related: %TRUE for an engine popup that must share @opener's web
 *   process and session (the view is then always in @opener's container)
 *
 * Creates a view on the default backend, recording its opener (for the
 * tab tree) and its container. gsurf_view_new() is the same as passing
 * %NULL, %NULL, %NULL and %FALSE.
 *
 * Returns: (transfer full) (nullable): a new #GsurfView
 */
GsurfView *gsurf_view_new_full(GsurfView *opener, const gchar *container_id,
                               const gchar *initial_uri, gboolean related);

/**
 * gsurf_window_new:
 *
 * Creates a new window of the current default backend type.
 *
 * Returns: (transfer full): a new #GsurfWindow
 */
GsurfWindow *gsurf_window_new(void);

/**
 * gsurf_window_reopen_view:
 * @window: the #GsurfWindow holding @view
 * @view: the view to reopen
 * @container_id: (nullable): the container to reopen it in: an id,
 *   #GSURF_CONTAINER_TEMPORARY (a new temporary container) or
 *   #GSURF_CONTAINER_DEFAULT; %NULL means #GSURF_CONTAINER_DEFAULT
 *
 * Reopens @view's page in another container. WebKit fixes a view's
 * session when it is created, so this makes a new view (on the default
 * backend, with the default config's settings) with @view's opener and
 * the requested container, puts it at @view's place in @window, makes
 * it the active view, loads @view's URI in it (its first URI if nothing
 * has loaded yet, else the configured new-view URI) and then removes
 * @view. The container registry's resolver (the containers module) has
 * the final say, as for every new view. The window emits
 * #GsurfWindow::view-added for the new view, then
 * #GsurfWindow::view-replaced, then #GsurfWindow::view-removed for
 * @view.
 *
 * Returns: (transfer none) (nullable): the new view, owned by @window,
 *   or %NULL when @view is not in @window or no view could be created
 */
GsurfView *gsurf_window_reopen_view(GsurfWindow *window, GsurfView *view,
                                    const gchar *container_id);

G_END_DECLS

#endif /* GSURF_BACKEND_H */
