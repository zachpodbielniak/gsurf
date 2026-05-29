/*
 * gsurf-input-handler.c - Module hook: keyboard/mouse input
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-input-handler.h"

G_DEFINE_INTERFACE(GsurfInputHandler, gsurf_input_handler, G_TYPE_OBJECT)

static void
gsurf_input_handler_default_init(GsurfInputHandlerInterface *iface)
{
}

gboolean
gsurf_input_handler_handle_key_event(GsurfInputHandler *self, GsurfView *view,
                                     guint keyval, guint keycode, guint state,
                                     GsurfModePolicy mode)
{
	GsurfInputHandlerInterface *iface;

	g_return_val_if_fail(GSURF_IS_INPUT_HANDLER(self), FALSE);

	iface = GSURF_INPUT_HANDLER_GET_IFACE(self);
	if (iface->handle_key_event != NULL)
		return iface->handle_key_event(self, view, keyval, keycode, state, mode);
	return FALSE;
}

gboolean
gsurf_input_handler_handle_mouse_event(GsurfInputHandler *self, GsurfView *view,
                                       guint button, guint state)
{
	GsurfInputHandlerInterface *iface;

	g_return_val_if_fail(GSURF_IS_INPUT_HANDLER(self), FALSE);

	iface = GSURF_INPUT_HANDLER_GET_IFACE(self);
	if (iface->handle_mouse_event != NULL)
		return iface->handle_mouse_event(self, view, button, state);
	return FALSE;
}
