/*
 * gsurf-gtk4-window.h - GTK4 browser window backend
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Concrete #GsurfWindow for GTK4 / WebKitGTK 6.0. Compiled only when
 * GTK_BACKEND=gtk4 (requires webkitgtk-6.0).
 */

#ifndef GSURF_GTK4_WINDOW_H
#define GSURF_GTK4_WINDOW_H

#include "window/gsurf-window.h"

G_BEGIN_DECLS

#define GSURF_TYPE_GTK4_WINDOW (gsurf_gtk4_window_get_type())

G_DECLARE_FINAL_TYPE(GsurfGtk4Window, gsurf_gtk4_window, GSURF, GTK4_WINDOW, GsurfWindow)

GsurfWindow *gsurf_gtk4_window_new(void);

G_END_DECLS

#endif /* GSURF_GTK4_WINDOW_H */
