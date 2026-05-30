/*
 * gsurf-render-overlay.h - Module hook: overlay rendering
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfRenderOverlay draw on top of a view's
 * content (link hints, status badges, scrollbar indicators). The draw
 * target is kept opaque to avoid leaking GTK/cairo into the abstract API.
 */

#ifndef GSURF_RENDER_OVERLAY_H
#define GSURF_RENDER_OVERLAY_H

#include <glib-object.h>

#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_RENDER_OVERLAY (gsurf_render_overlay_get_type())

G_DECLARE_INTERFACE(GsurfRenderOverlay, gsurf_render_overlay, GSURF, RENDER_OVERLAY, GObject)

/**
 * GsurfRenderOverlayInterface:
 * @parent_iface: the parent interface
 * @render: draw the overlay for @view onto @draw_target
 */
struct _GsurfRenderOverlayInterface
{
	GTypeInterface parent_iface;

	void (*render)(GsurfRenderOverlay *self, GsurfView *view, gpointer draw_target);
};

void gsurf_render_overlay_render(GsurfRenderOverlay *self, GsurfView *view, gpointer draw_target);

G_END_DECLS

#endif /* GSURF_RENDER_OVERLAY_H */
