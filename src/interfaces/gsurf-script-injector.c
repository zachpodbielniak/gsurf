/*
 * gsurf-script-injector.c - Module hook: user script/style injection
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-script-injector.h"

G_DEFINE_INTERFACE(GsurfScriptInjector, gsurf_script_injector, G_TYPE_OBJECT)

static void
gsurf_script_injector_default_init(GsurfScriptInjectorInterface *iface)
{
}

void
gsurf_script_injector_inject(GsurfScriptInjector *self, GsurfView *view, const gchar *uri)
{
	GsurfScriptInjectorInterface *iface;

	g_return_if_fail(GSURF_IS_SCRIPT_INJECTOR(self));

	iface = GSURF_SCRIPT_INJECTOR_GET_IFACE(self);
	if (iface->inject != NULL)
		iface->inject(self, view, uri);
}
