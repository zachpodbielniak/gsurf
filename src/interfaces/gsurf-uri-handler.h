/*
 * gsurf-uri-handler.h - Module hook: URI rewriting
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfUriHandler transform the text a user is
 * about to load. The first handler to return a non-%NULL string wins
 * (e.g. "g cats" -> a Google search URL; "b news" -> a bookmark URL).
 */

#ifndef GSURF_URI_HANDLER_H
#define GSURF_URI_HANDLER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GSURF_TYPE_URI_HANDLER (gsurf_uri_handler_get_type())

G_DECLARE_INTERFACE(GsurfUriHandler, gsurf_uri_handler, GSURF, URI_HANDLER, GObject)

/**
 * GsurfUriHandlerInterface:
 * @parent_iface: the parent interface
 * @rewrite_uri: transform input text into a URI, or return %NULL to pass
 */
struct _GsurfUriHandlerInterface
{
	GTypeInterface parent_iface;

	gchar *(*rewrite_uri)(GsurfUriHandler *self, const gchar *input);
};

/**
 * gsurf_uri_handler_rewrite_uri:
 * @self: a #GsurfUriHandler
 * @input: the user-entered text
 *
 * Returns: (transfer full) (nullable): a rewritten URI, or %NULL
 */
gchar *gsurf_uri_handler_rewrite_uri(GsurfUriHandler *self, const gchar *input);

G_END_DECLS

#endif /* GSURF_URI_HANDLER_H */
