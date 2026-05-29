/*
 * gsurf-backend.h - Backend factory and initialization
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The single place in the core library that knows which concrete
 * backend (GTK3/WebKit2GTK or GTK4/WebKitGTK) was compiled in. Selected
 * at build time via the GTK_BACKEND make flag / GSURF_BACKEND_* macros.
 * Everything else uses the abstract #GsurfView / #GsurfWindow API.
 */

#ifndef GSURF_BACKEND_H
#define GSURF_BACKEND_H

#include "gsurf-enums.h"
#include "core/gsurf-view.h"
#include "window/gsurf-window.h"

G_BEGIN_DECLS

/**
 * gsurf_backend_init:
 * @argc: (inout) (optional): address of argc from main()
 * @argv: (inout) (array length=argc) (optional): address of argv
 * @error: (out) (optional): location for an error
 *
 * Initializes the windowing backend (e.g. gtk_init). Must be called
 * before creating windows/views. Safe to call more than once.
 *
 * Returns: %TRUE on success
 */
gboolean gsurf_backend_init(int *argc, char ***argv, GError **error);

/**
 * gsurf_backend_get_backend_type:
 *
 * Returns: the compiled-in #GsurfBackendType.
 */
GsurfBackendType gsurf_backend_get_backend_type(void);

/**
 * gsurf_backend_get_name:
 *
 * Returns: (transfer none): a human-readable backend name.
 */
const gchar *gsurf_backend_get_name(void);

/**
 * gsurf_view_new:
 *
 * Creates a new view of the compiled-in backend type.
 *
 * Returns: (transfer full): a new #GsurfView
 */
GsurfView *gsurf_view_new(void);

/**
 * gsurf_window_new:
 *
 * Creates a new window of the compiled-in backend type.
 *
 * Returns: (transfer full): a new #GsurfWindow
 */
GsurfWindow *gsurf_window_new(void);

G_END_DECLS

#endif /* GSURF_BACKEND_H */
