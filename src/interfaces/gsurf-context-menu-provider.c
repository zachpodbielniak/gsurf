/*
 * gsurf-context-menu-provider.c - Module hook: context-menu population
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-context-menu-provider.h"

G_DEFINE_INTERFACE(GsurfContextMenuProvider, gsurf_context_menu_provider, G_TYPE_OBJECT)

static void
gsurf_context_menu_provider_default_init(GsurfContextMenuProviderInterface *iface)
{
}

void
gsurf_context_menu_provider_populate(GsurfContextMenuProvider *self, GsurfHitTest *hit, GPtrArray *items)
{
	GsurfContextMenuProviderInterface *iface;

	g_return_if_fail(GSURF_IS_CONTEXT_MENU_PROVIDER(self));

	iface = GSURF_CONTEXT_MENU_PROVIDER_GET_IFACE(self);
	if (iface->populate != NULL)
		iface->populate(self, hit, items);
}
