/*
 * gsurf-permission-handler.h - Module hook: permission requests
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfPermissionHandler decide geolocation,
 * notification, and media permission requests (geolocation,
 * notifications modules).
 */

#ifndef GSURF_PERMISSION_HANDLER_H
#define GSURF_PERMISSION_HANDLER_H

#include <glib-object.h>

#include "gsurf-enums.h"
#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_PERMISSION_HANDLER (gsurf_permission_handler_get_type())

G_DECLARE_INTERFACE(GsurfPermissionHandler, gsurf_permission_handler, GSURF, PERMISSION_HANDLER, GObject)

struct _GsurfPermissionHandlerInterface
{
	GTypeInterface parent_iface;

	GsurfPermissionVerdict (*decide)(GsurfPermissionHandler *self, GsurfView *view,
	                                 const gchar *type, const gchar *origin);
};

GsurfPermissionVerdict gsurf_permission_handler_decide(GsurfPermissionHandler *self, GsurfView *view,
                                                       const gchar *type, const gchar *origin);

G_END_DECLS

#endif /* GSURF_PERMISSION_HANDLER_H */
