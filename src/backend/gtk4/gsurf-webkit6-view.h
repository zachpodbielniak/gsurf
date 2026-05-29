/*
 * gsurf-webkit6-view.h - WebKitGTK 6.0 / GTK4 web view backend
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Concrete #GsurfView for WebKitGTK 6.0 (GTK4). Compiled only when
 * GTK_BACKEND=gtk4 (requires webkitgtk-6.0).
 */

#ifndef GSURF_WEBKIT6_VIEW_H
#define GSURF_WEBKIT6_VIEW_H

#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_WEBKIT6_VIEW (gsurf_webkit6_view_get_type())

G_DECLARE_FINAL_TYPE(GsurfWebkit6View, gsurf_webkit6_view, GSURF, WEBKIT6_VIEW, GsurfView)

GsurfView *gsurf_webkit6_view_new(void);

G_END_DECLS

#endif /* GSURF_WEBKIT6_VIEW_H */
