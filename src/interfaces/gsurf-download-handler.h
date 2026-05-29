/*
 * gsurf-download-handler.h - Module hook: download destinations
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfDownloadHandler choose where a download is
 * saved (downloads module). The first handler to return a non-%NULL path
 * wins.
 */

#ifndef GSURF_DOWNLOAD_HANDLER_H
#define GSURF_DOWNLOAD_HANDLER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GSURF_TYPE_DOWNLOAD_HANDLER (gsurf_download_handler_get_type())

G_DECLARE_INTERFACE(GsurfDownloadHandler, gsurf_download_handler, GSURF, DOWNLOAD_HANDLER, GObject)

struct _GsurfDownloadHandlerInterface
{
	GTypeInterface parent_iface;

	gchar *(*decide_destination)(GsurfDownloadHandler *self, const gchar *uri,
	                             const gchar *suggested_filename);
};

gchar *gsurf_download_handler_decide_destination(GsurfDownloadHandler *self, const gchar *uri,
                                                 const gchar *suggested_filename);

G_END_DECLS

#endif /* GSURF_DOWNLOAD_HANDLER_H */
