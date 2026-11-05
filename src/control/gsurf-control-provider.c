/*
 * gsurf-control-provider.c - Module hook: methods on the control socket
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The interface and its one wrapper; a provider without the vfunc
 * registers nothing.
 */

#include "control/gsurf-control-provider.h"

G_DEFINE_INTERFACE(GsurfControlProvider, gsurf_control_provider, G_TYPE_OBJECT)

static void
gsurf_control_provider_default_init(GsurfControlProviderInterface *iface)
{
	(void)iface;
}

void
gsurf_control_provider_register_methods(GsurfControlProvider *self, GsurfControlServer *server)
{
	GsurfControlProviderInterface *iface;

	g_return_if_fail(GSURF_IS_CONTROL_PROVIDER(self));
	g_return_if_fail(GSURF_IS_CONTROL_SERVER(server));

	iface = GSURF_CONTROL_PROVIDER_GET_IFACE(self);
	if (iface->register_methods != NULL)
		iface->register_methods(self, server);
}
