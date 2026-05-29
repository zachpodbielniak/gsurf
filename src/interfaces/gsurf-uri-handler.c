/*
 * gsurf-uri-handler.c - Module hook: URI rewriting
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-uri-handler.h"

G_DEFINE_INTERFACE(GsurfUriHandler, gsurf_uri_handler, G_TYPE_OBJECT)

static void
gsurf_uri_handler_default_init(GsurfUriHandlerInterface *iface)
{
}

gchar *
gsurf_uri_handler_rewrite_uri(GsurfUriHandler *self, const gchar *input)
{
	GsurfUriHandlerInterface *iface;

	g_return_val_if_fail(GSURF_IS_URI_HANDLER(self), NULL);

	iface = GSURF_URI_HANDLER_GET_IFACE(self);
	if (iface->rewrite_uri != NULL)
		return iface->rewrite_uri(self, input);
	return NULL;
}
