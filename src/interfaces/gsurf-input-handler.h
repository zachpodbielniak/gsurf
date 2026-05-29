/*
 * gsurf-input-handler.h - Module hook: keyboard/mouse input
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfInputHandler get first crack at key/mouse
 * events (before the default keybind table). The first handler to return
 * %TRUE consumes the event. The modal keymap, tabs, and hints modules
 * live here. Mirrors gst's GstInputHandler.
 */

#ifndef GSURF_INPUT_HANDLER_H
#define GSURF_INPUT_HANDLER_H

#include <glib-object.h>

#include "gsurf-enums.h"
#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_INPUT_HANDLER (gsurf_input_handler_get_type())

G_DECLARE_INTERFACE(GsurfInputHandler, gsurf_input_handler, GSURF, INPUT_HANDLER, GObject)

/**
 * GsurfInputHandlerInterface:
 * @parent_iface: the parent interface
 * @handle_key_event: handle a key press; return %TRUE to consume
 * @handle_mouse_event: handle a button press; return %TRUE to consume
 */
struct _GsurfInputHandlerInterface
{
	GTypeInterface parent_iface;

	gboolean (*handle_key_event)   (GsurfInputHandler *self, GsurfView *view,
	                                guint keyval, guint keycode, guint state,
	                                GsurfModePolicy mode);
	gboolean (*handle_mouse_event) (GsurfInputHandler *self, GsurfView *view,
	                                guint button, guint state);
};

gboolean gsurf_input_handler_handle_key_event(GsurfInputHandler *self, GsurfView *view,
                                              guint keyval, guint keycode, guint state,
                                              GsurfModePolicy mode);
gboolean gsurf_input_handler_handle_mouse_event(GsurfInputHandler *self, GsurfView *view,
                                                guint button, guint state);

G_END_DECLS

#endif /* GSURF_INPUT_HANDLER_H */
