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
 * @decide_main_response: decide whether the main frame may display the
 *   response it received for @uri (see
 *   gsurf_navigation_hook_decide_main_response())
 */
struct _GsurfNavigationHookInterface
{
	GTypeInterface parent_iface;

	GsurfPolicyDecision (*before_navigate)(GsurfNavigationHook *self, GsurfView *view, const gchar *uri);
	void                (*after_navigate) (GsurfNavigationHook *self, GsurfView *view, const gchar *uri);
	void                (*load_changed)   (GsurfNavigationHook *self, GsurfView *view, GsurfLoadEvent event);
	GsurfPolicyDecision (*decide_main_response)(GsurfNavigationHook *self, GsurfView *view,
	                                            const gchar *uri);
};

GsurfPolicyDecision gsurf_navigation_hook_before_navigate(GsurfNavigationHook *self, GsurfView *view, const gchar *uri);
void                gsurf_navigation_hook_after_navigate(GsurfNavigationHook *self, GsurfView *view, const gchar *uri);
void                gsurf_navigation_hook_load_changed(GsurfNavigationHook *self, GsurfView *view, GsurfLoadEvent event);

/**
 * gsurf_navigation_hook_decide_main_response:
 * @self: a #GsurfNavigationHook
 * @view: the view whose main frame received a response
 * @uri: the URI of the response (after any redirects)
 *
 * Called when the *main frame's* main resource response arrives, before
 * the engine displays it, and only for content the engine can display
 * (downloads are not reported). Unlike before_navigate(), which the
 * engine also calls for sub-frame (iframe) navigations and cannot tell
 * them apart, this is known to be a top-level navigation. The request
 * has already been sent at this point. Returning %GSURF_POLICY_IGNORE
 * cancels the load; the view keeps its previous page.
 *
 * Returns: the decision; %GSURF_POLICY_USE when not implemented
 */
GsurfPolicyDecision gsurf_navigation_hook_decide_main_response(GsurfNavigationHook *self,
                                                               GsurfView *view,
                                                               const gchar *uri);

G_END_DECLS

#endif /* GSURF_NAVIGATION_HOOK_H */
