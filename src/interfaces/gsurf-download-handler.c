/*
 * gsurf-download-handler.c - Module hook: download destinations
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-download-handler.h"

G_DEFINE_INTERFACE(GsurfDownloadHandler, gsurf_download_handler, G_TYPE_OBJECT)

static void
gsurf_download_handler_default_init(GsurfDownloadHandlerInterface *iface)
{
}

gchar *
gsurf_download_handler_decide_destination(GsurfDownloadHandler *self, const gchar *uri,
                                          const gchar *suggested_filename)
{
	GsurfDownloadHandlerInterface *iface;

	g_return_val_if_fail(GSURF_IS_DOWNLOAD_HANDLER(self), NULL);

	iface = GSURF_DOWNLOAD_HANDLER_GET_IFACE(self);
	if (iface->decide_destination != NULL)
		return iface->decide_destination(self, uri, suggested_filename);
	return NULL;
}
