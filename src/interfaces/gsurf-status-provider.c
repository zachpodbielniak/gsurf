/*
 * gsurf-status-provider.c - Module hook: status-bar text contribution
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-status-provider.h"

G_DEFINE_INTERFACE(GsurfStatusProvider, gsurf_status_provider, G_TYPE_OBJECT)

static void
gsurf_status_provider_default_init(GsurfStatusProviderInterface *iface)
{
}

gchar *
gsurf_status_provider_get_status_text(GsurfStatusProvider *self, GsurfView *view)
{
	GsurfStatusProviderInterface *iface;

	g_return_val_if_fail(GSURF_IS_STATUS_PROVIDER(self), NULL);

	iface = GSURF_STATUS_PROVIDER_GET_IFACE(self);
	if (iface->status_text != NULL)
		return iface->status_text(self, view);
	return NULL;
}
