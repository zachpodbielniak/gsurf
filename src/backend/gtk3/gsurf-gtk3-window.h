/*
 * gsurf-gtk3-window.h - GTK3 browser window backend
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Concrete #GsurfWindow implementation using a GtkWindow + GtkStack of
 * view widgets. Compiled only when GTK_BACKEND=gtk3.
 */

#ifndef GSURF_GTK3_WINDOW_H
#define GSURF_GTK3_WINDOW_H

#include "window/gsurf-window.h"

G_BEGIN_DECLS

#define GSURF_TYPE_GTK3_WINDOW (gsurf_gtk3_window_get_type())

G_DECLARE_FINAL_TYPE(GsurfGtk3Window, gsurf_gtk3_window, GSURF, GTK3_WINDOW, GsurfWindow)

/**
 * gsurf_gtk3_window_new:
 *
 * Creates a new GTK3 browser window.
 *
 * Returns: (transfer full): a new #GsurfWindow
 */
GsurfWindow *gsurf_gtk3_window_new(void);

G_END_DECLS

#endif /* GSURF_GTK3_WINDOW_H */
