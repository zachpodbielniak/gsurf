/*
 * gsurf-render-overlay.c - Module hook: overlay rendering
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-render-overlay.h"

G_DEFINE_INTERFACE(GsurfRenderOverlay, gsurf_render_overlay, G_TYPE_OBJECT)

static void
gsurf_render_overlay_default_init(GsurfRenderOverlayInterface *iface)
{
}

void
gsurf_render_overlay_render(GsurfRenderOverlay *self, GsurfView *view, gpointer draw_target)
{
	GsurfRenderOverlayInterface *iface;

	g_return_if_fail(GSURF_IS_RENDER_OVERLAY(self));

	iface = GSURF_RENDER_OVERLAY_GET_IFACE(self);
	if (iface->render != NULL)
		iface->render(self, view, draw_target);
}
