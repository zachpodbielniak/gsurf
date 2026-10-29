/*
 * gsurf-action-handler.h - Module hook: keybind actions
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfActionHandler perform core keybind actions
 * whose UI they own: the address bar answers %GSURF_ACTION_OPEN_PROMPT,
 * the find bar %GSURF_ACTION_FIND, the tab strip the tab actions. The
 * host dispatches an action here when it has no built-in implementation;
 * the first handler (in priority order) to return %TRUE performs it.
 */

#ifndef GSURF_ACTION_HANDLER_H
#define GSURF_ACTION_HANDLER_H

#include <glib-object.h>

#include "gsurf-enums.h"
#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_ACTION_HANDLER (gsurf_action_handler_get_type())

G_DECLARE_INTERFACE(GsurfActionHandler, gsurf_action_handler, GSURF, ACTION_HANDLER, GObject)

/**
 * GsurfActionHandlerInterface:
 * @parent_iface: the parent interface
 * @handle_action: perform @action on @view; return %TRUE if handled
 */
struct _GsurfActionHandlerInterface
{
	GTypeInterface parent_iface;

	gboolean (*handle_action) (GsurfActionHandler *self, GsurfView *view,
	                           GsurfAction action);
};

/**
 * gsurf_action_handler_handle_action:
 * @self: a #GsurfActionHandler
 * @view: (nullable): the active view
 * @action: the keybind action to perform
 *
 * Asks the module to perform @action.
 *
 * Returns: %TRUE if the module performed the action
 */
gboolean gsurf_action_handler_handle_action(GsurfActionHandler *self, GsurfView *view,
                                            GsurfAction action);

G_END_DECLS

#endif /* GSURF_ACTION_HANDLER_H */
