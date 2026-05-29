/*
 * gsurf-script-injector.h - Module hook: user script/style injection
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfScriptInjector add user scripts/stylesheets
 * to a view at creation (userscripts, site-styles, dark-mode). They call
 * gsurf_view_add_user_script() / gsurf_view_add_user_style().
 */

#ifndef GSURF_SCRIPT_INJECTOR_H
#define GSURF_SCRIPT_INJECTOR_H

#include <glib-object.h>

#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_SCRIPT_INJECTOR (gsurf_script_injector_get_type())

G_DECLARE_INTERFACE(GsurfScriptInjector, gsurf_script_injector, GSURF, SCRIPT_INJECTOR, GObject)

struct _GsurfScriptInjectorInterface
{
	GTypeInterface parent_iface;

	void (*inject)(GsurfScriptInjector *self, GsurfView *view, const gchar *uri);
};

void gsurf_script_injector_inject(GsurfScriptInjector *self, GsurfView *view, const gchar *uri);

G_END_DECLS

#endif /* GSURF_SCRIPT_INJECTOR_H */
