/*
 * gsurf-navigation-hook.c - Module hook: navigation lifecycle
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-navigation-hook.h"

G_DEFINE_INTERFACE(GsurfNavigationHook, gsurf_navigation_hook, G_TYPE_OBJECT)

static void
gsurf_navigation_hook_default_init(GsurfNavigationHookInterface *iface)
{
}

GsurfPolicyDecision
gsurf_navigation_hook_before_navigate(GsurfNavigationHook *self, GsurfView *view, const gchar *uri)
{
	GsurfNavigationHookInterface *iface;

	g_return_val_if_fail(GSURF_IS_NAVIGATION_HOOK(self), GSURF_POLICY_USE);

	iface = GSURF_NAVIGATION_HOOK_GET_IFACE(self);
	if (iface->before_navigate != NULL)
		return iface->before_navigate(self, view, uri);
	return GSURF_POLICY_USE;
}

void
gsurf_navigation_hook_after_navigate(GsurfNavigationHook *self, GsurfView *view, const gchar *uri)
{
	GsurfNavigationHookInterface *iface;

	g_return_if_fail(GSURF_IS_NAVIGATION_HOOK(self));

	iface = GSURF_NAVIGATION_HOOK_GET_IFACE(self);
	if (iface->after_navigate != NULL)
		iface->after_navigate(self, view, uri);
}

void
gsurf_navigation_hook_load_changed(GsurfNavigationHook *self, GsurfView *view, GsurfLoadEvent event)
{
	GsurfNavigationHookInterface *iface;

	g_return_if_fail(GSURF_IS_NAVIGATION_HOOK(self));

	iface = GSURF_NAVIGATION_HOOK_GET_IFACE(self);
	if (iface->load_changed != NULL)
		iface->load_changed(self, view, event);
}
