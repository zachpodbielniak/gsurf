/*
 * gsurf-context-menu-provider.h - Module hook: context-menu population
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfContextMenuProvider append #GsurfMenuItem
 * entries to a view's context menu based on the #GsurfHitTest under the
 * pointer (open-in-new-view, copy-link, save-image, etc.).
 */

#ifndef GSURF_CONTEXT_MENU_PROVIDER_H
#define GSURF_CONTEXT_MENU_PROVIDER_H

#include <glib-object.h>

#include "core/gsurf-view.h"
#include "../boxed/gsurf-hit-test.h"
#include "../boxed/gsurf-menu-item.h"

G_BEGIN_DECLS

#define GSURF_TYPE_CONTEXT_MENU_PROVIDER (gsurf_context_menu_provider_get_type())

G_DECLARE_INTERFACE(GsurfContextMenuProvider, gsurf_context_menu_provider, GSURF, CONTEXT_MENU_PROVIDER, GObject)

/**
 * GsurfContextMenuProviderInterface:
 * @parent_iface: the parent interface
 * @populate: append #GsurfMenuItem entries to @items for the given @hit
 */
struct _GsurfContextMenuProviderInterface
{
	GTypeInterface parent_iface;

	void (*populate)(GsurfContextMenuProvider *self, GsurfHitTest *hit, GPtrArray *items);
};

void gsurf_context_menu_provider_populate(GsurfContextMenuProvider *self, GsurfHitTest *hit, GPtrArray *items);

G_END_DECLS

#endif /* GSURF_CONTEXT_MENU_PROVIDER_H */
