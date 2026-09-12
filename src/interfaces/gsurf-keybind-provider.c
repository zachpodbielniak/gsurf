/*
 * gsurf-keybind-provider.c - Module hook: advertise live keybindings
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-keybind-provider.h"

G_DEFINE_INTERFACE(GsurfKeybindProvider, gsurf_keybind_provider, G_TYPE_OBJECT)

static void
gsurf_keybind_provider_default_init(GsurfKeybindProviderInterface *iface)
{
}

void
gsurf_keybind_provider_list_keybinds(GsurfKeybindProvider *self, GPtrArray *entries)
{
	GsurfKeybindProviderInterface *iface;

	g_return_if_fail(GSURF_IS_KEYBIND_PROVIDER(self));
	g_return_if_fail(entries != NULL);

	iface = GSURF_KEYBIND_PROVIDER_GET_IFACE(self);
	if (iface->list_keybinds != NULL)
		iface->list_keybinds(self, entries);
}
