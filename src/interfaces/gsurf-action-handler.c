/*
 * gsurf-action-handler.c - Module hook: keybind actions
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-action-handler.h"

G_DEFINE_INTERFACE(GsurfActionHandler, gsurf_action_handler, G_TYPE_OBJECT)

static void
gsurf_action_handler_default_init(GsurfActionHandlerInterface *iface)
{
}

gboolean
gsurf_action_handler_handle_action(GsurfActionHandler *self, GsurfView *view,
                                   GsurfAction action)
{
	GsurfActionHandlerInterface *iface;

	g_return_val_if_fail(GSURF_IS_ACTION_HANDLER(self), FALSE);

	iface = GSURF_ACTION_HANDLER_GET_IFACE(self);
	if (iface->handle_action != NULL)
		return iface->handle_action(self, view, action);
	return FALSE;
}
