/*
 * gsurf-navigation-hook.h - Module hook: navigation lifecycle
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfNavigationHook observe and police
 * navigation: vetoing requests (popup blockers), recording history,
 * reacting to load state. Mirrors gst's output/escape hook style.
 */

#ifndef GSURF_NAVIGATION_HOOK_H
#define GSURF_NAVIGATION_HOOK_H

#include <glib-object.h>

#include "gsurf-enums.h"
#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_NAVIGATION_HOOK (gsurf_navigation_hook_get_type())

G_DECLARE_INTERFACE(GsurfNavigationHook, gsurf_navigation_hook, GSURF, NAVIGATION_HOOK, GObject)

/**
 * GsurfNavigationHookInterface:
 * @parent_iface: the parent interface
 * @before_navigate: decide whether a navigation may proceed
 * @after_navigate: notified once a navigation is committed
 * @load_changed: notified on load-state transitions
 */
struct _GsurfNavigationHookInterface
{
	GTypeInterface parent_iface;

	GsurfPolicyDecision (*before_navigate)(GsurfNavigationHook *self, GsurfView *view, const gchar *uri);
	void                (*after_navigate) (GsurfNavigationHook *self, GsurfView *view, const gchar *uri);
	void                (*load_changed)   (GsurfNavigationHook *self, GsurfView *view, GsurfLoadEvent event);
};

GsurfPolicyDecision gsurf_navigation_hook_before_navigate(GsurfNavigationHook *self, GsurfView *view, const gchar *uri);
void                gsurf_navigation_hook_after_navigate(GsurfNavigationHook *self, GsurfView *view, const gchar *uri);
void                gsurf_navigation_hook_load_changed(GsurfNavigationHook *self, GsurfView *view, GsurfLoadEvent event);

G_END_DECLS

#endif /* GSURF_NAVIGATION_HOOK_H */
