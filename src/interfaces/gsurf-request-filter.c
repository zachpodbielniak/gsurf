/*
 * gsurf-request-filter.c - Module hook: resource request filtering
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-request-filter.h"

G_DEFINE_INTERFACE(GsurfRequestFilter, gsurf_request_filter, G_TYPE_OBJECT)

static void
gsurf_request_filter_default_init(GsurfRequestFilterInterface *iface)
{
}

GsurfFilterVerdict
gsurf_request_filter_filter_request(GsurfRequestFilter *self, GsurfView *view, const gchar *uri, gchar **redirect_uri)
{
	GsurfRequestFilterInterface *iface;

	g_return_val_if_fail(GSURF_IS_REQUEST_FILTER(self), GSURF_FILTER_ALLOW);

	iface = GSURF_REQUEST_FILTER_GET_IFACE(self);
	if (iface->filter_request != NULL)
		return iface->filter_request(self, view, uri, redirect_uri);
	return GSURF_FILTER_ALLOW;
}
