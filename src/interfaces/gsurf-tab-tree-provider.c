/*
 * gsurf-tab-tree-provider.c - Module hook: a window's tab tree
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The interface and its wrappers; every vfunc is optional and a missing
 * one answers "root", "expanded" or "cannot".
 */

#include "interfaces/gsurf-tab-tree-provider.h"

G_DEFINE_INTERFACE(GsurfTabTreeProvider, gsurf_tab_tree_provider, G_TYPE_OBJECT)

static void
gsurf_tab_tree_provider_default_init(GsurfTabTreeProviderInterface *iface)
{
	(void)iface;
}

guint64
gsurf_tab_tree_provider_get_parent(GsurfTabTreeProvider *self, GsurfWindow *window,
                                   GsurfView *view)
{
	GsurfTabTreeProviderInterface *iface;

	g_return_val_if_fail(GSURF_IS_TAB_TREE_PROVIDER(self), 0);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), 0);
	g_return_val_if_fail(GSURF_IS_VIEW(view), 0);

	iface = GSURF_TAB_TREE_PROVIDER_GET_IFACE(self);
	return iface->get_parent != NULL ? iface->get_parent(self, window, view) : 0;
}

gboolean
gsurf_tab_tree_provider_get_collapsed(GsurfTabTreeProvider *self, GsurfWindow *window,
                                      GsurfView *view)
{
	GsurfTabTreeProviderInterface *iface;

	g_return_val_if_fail(GSURF_IS_TAB_TREE_PROVIDER(self), FALSE);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), FALSE);
	g_return_val_if_fail(GSURF_IS_VIEW(view), FALSE);

	iface = GSURF_TAB_TREE_PROVIDER_GET_IFACE(self);
	return iface->get_collapsed != NULL ? iface->get_collapsed(self, window, view) : FALSE;
}

gboolean
gsurf_tab_tree_provider_set_collapsed(GsurfTabTreeProvider *self, GsurfWindow *window,
                                      GsurfView *view, gboolean collapsed)
{
	GsurfTabTreeProviderInterface *iface;

	g_return_val_if_fail(GSURF_IS_TAB_TREE_PROVIDER(self), FALSE);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), FALSE);
	g_return_val_if_fail(GSURF_IS_VIEW(view), FALSE);

	iface = GSURF_TAB_TREE_PROVIDER_GET_IFACE(self);
	return iface->set_collapsed != NULL
		? iface->set_collapsed(self, window, view, collapsed != FALSE) : FALSE;
}

gboolean
gsurf_tab_tree_provider_move_view(GsurfTabTreeProvider *self, GsurfWindow *window,
                                  GsurfView *view, GsurfView *target,
                                  GsurfTabTreePosition position)
{
	GsurfTabTreeProviderInterface *iface;

	g_return_val_if_fail(GSURF_IS_TAB_TREE_PROVIDER(self), FALSE);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), FALSE);
	g_return_val_if_fail(GSURF_IS_VIEW(view), FALSE);
	g_return_val_if_fail(target == NULL || GSURF_IS_VIEW(target), FALSE);

	iface = GSURF_TAB_TREE_PROVIDER_GET_IFACE(self);
	return iface->move_view != NULL
		? iface->move_view(self, window, view, target, position) : FALSE;
}
