/*
 * gsurf-webkit2-view.h - WebKit2GTK 4.1 / GTK3 web view backend
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Concrete #GsurfView implementation wrapping a WebKitWebView from
 * WebKit2GTK 4.1 (GTK3). Compiled only when GTK_BACKEND=gtk3.
 */

#ifndef GSURF_WEBKIT2_VIEW_H
#define GSURF_WEBKIT2_VIEW_H

#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_WEBKIT2_VIEW (gsurf_webkit2_view_get_type())

G_DECLARE_FINAL_TYPE(GsurfWebkit2View, gsurf_webkit2_view, GSURF, WEBKIT2_VIEW, GsurfView)

/**
 * gsurf_webkit2_view_new:
 *
 * Creates a new GTK3/WebKit2GTK web view.
 *
 * Returns: (transfer full): a new #GsurfView
 */
GsurfView *gsurf_webkit2_view_new(void);

G_END_DECLS

#endif /* GSURF_WEBKIT2_VIEW_H */
