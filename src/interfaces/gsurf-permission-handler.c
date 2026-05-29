/*
 * gsurf-permission-handler.c - Module hook: permission requests
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-permission-handler.h"

G_DEFINE_INTERFACE(GsurfPermissionHandler, gsurf_permission_handler, G_TYPE_OBJECT)

static void
gsurf_permission_handler_default_init(GsurfPermissionHandlerInterface *iface)
{
}

GsurfPermissionVerdict
gsurf_permission_handler_decide(GsurfPermissionHandler *self, GsurfView *view,
                                const gchar *type, const gchar *origin)
{
	GsurfPermissionHandlerInterface *iface;

	g_return_val_if_fail(GSURF_IS_PERMISSION_HANDLER(self), GSURF_PERMISSION_PROMPT);

	iface = GSURF_PERMISSION_HANDLER_GET_IFACE(self);
	if (iface->decide != NULL)
		return iface->decide(self, view, type, origin);
	return GSURF_PERMISSION_PROMPT;
}
