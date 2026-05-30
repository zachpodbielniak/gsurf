/*
 * gsurf-request-filter.h - Module hook: resource request filtering
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfRequestFilter inspect outgoing resource
 * requests and decide whether to allow, block, or redirect them
 * (adblock, privacy filters, URL rewriting).
 */

#ifndef GSURF_REQUEST_FILTER_H
#define GSURF_REQUEST_FILTER_H

#include <glib-object.h>

#include "../gsurf-enums.h"
#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_REQUEST_FILTER (gsurf_request_filter_get_type())

G_DECLARE_INTERFACE(GsurfRequestFilter, gsurf_request_filter, GSURF, REQUEST_FILTER, GObject)

/**
 * GsurfRequestFilterInterface:
 * @parent_iface: the parent interface
 * @filter_request: decide whether a resource request may proceed
 */
struct _GsurfRequestFilterInterface
{
	GTypeInterface parent_iface;

	GsurfFilterVerdict (*filter_request)(GsurfRequestFilter *self, GsurfView *view, const gchar *uri, gchar **redirect_uri);
};

GsurfFilterVerdict gsurf_request_filter_filter_request(GsurfRequestFilter *self, GsurfView *view, const gchar *uri, gchar **redirect_uri);

G_END_DECLS

#endif /* GSURF_REQUEST_FILTER_H */
