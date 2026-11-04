/*
 * gsurf-window-layout.c - Delegate that arranges a window's views
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The interface and its wrappers. The window side (set_layout and the
 * delegation of insert/remove/activate) lives in gsurf-window.c.
 */

#include "window/gsurf-window-layout.h"

G_DEFINE_INTERFACE(GsurfWindowLayout, gsurf_window_layout, G_TYPE_OBJECT)

static void
gsurf_window_layout_default_init(GsurfWindowLayoutInterface *iface)
{
	/* Every vfunc is optional: a missing one is a no-op (or "only the
	 * active view is visible", "cannot serialize/restore"). */
	(void)iface;
}

gpointer
gsurf_window_layout_get_root(GsurfWindowLayout *self)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), NULL);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	return iface->get_root != NULL ? iface->get_root(self) : NULL;
}

void
gsurf_window_layout_attach(GsurfWindowLayout *self, GsurfWindow *window)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->attach != NULL)
		iface->attach(self, window);
}

void
gsurf_window_layout_detach(GsurfWindowLayout *self, GsurfWindow *window)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->detach != NULL)
		iface->detach(self, window);
}

void
gsurf_window_layout_add_view(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));
	g_return_if_fail(GSURF_IS_VIEW(view));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->add_view != NULL)
		iface->add_view(self, window, view);
}

void
gsurf_window_layout_remove_view(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));
	g_return_if_fail(GSURF_IS_VIEW(view));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->remove_view != NULL)
		iface->remove_view(self, window, view);
}

void
gsurf_window_layout_activate_view(GsurfWindowLayout *self, GsurfWindow *window,
                                  GsurfView *view)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));
	g_return_if_fail(GSURF_IS_VIEW(view));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->activate_view != NULL)
		iface->activate_view(self, window, view);
}

gboolean
gsurf_window_layout_is_view_visible(GsurfWindowLayout *self, GsurfWindow *window,
                                    GsurfView *view)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), FALSE);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), FALSE);
	g_return_val_if_fail(GSURF_IS_VIEW(view), FALSE);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->is_view_visible != NULL)
		return iface->is_view_visible(self, window, view);
	/* Default: a layout that shows one view at a time. */
	return gsurf_window_get_active_view(window) == view;
}

gchar *
gsurf_window_layout_serialize(GsurfWindowLayout *self, GsurfWindow *window,
                              GsurfWindowLayoutIndexFunc map, gpointer user_data)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), NULL);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), NULL);
	g_return_val_if_fail(map != NULL, NULL);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	return iface->serialize != NULL ? iface->serialize(self, window, map, user_data) : NULL;
}

gboolean
gsurf_window_layout_restore(GsurfWindowLayout *self, GsurfWindow *window,
                            const gchar *state, GsurfView **views, guint n_views)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), FALSE);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), FALSE);
	g_return_val_if_fail(state != NULL, FALSE);
	g_return_val_if_fail(views != NULL || n_views == 0, FALSE);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	return iface->restore != NULL
		? iface->restore(self, window, state, views, n_views) : FALSE;
}
