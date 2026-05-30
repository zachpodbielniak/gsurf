/*
 * gsurf-navigation-action.h - GSURF Navigation Action Boxed Type
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Represents a pending navigation request: the target URI, the kind of
 * navigation, the originating mouse button and modifier state, and
 * whether it was a user gesture or a redirect.
 */

#ifndef GSURF_NAVIGATION_ACTION_H
#define GSURF_NAVIGATION_ACTION_H

#include <glib-object.h>
#include "../gsurf-types.h"

G_BEGIN_DECLS

#define GSURF_TYPE_NAVIGATION_ACTION (gsurf_navigation_action_get_type())

/**
 * GsurfNavigationAction:
 * @uri: Target URI of the navigation, or %NULL
 * @navigation_type: Kind of navigation (link click, form submit, etc.)
 * @mouse_button: Originating mouse button (0 if none)
 * @modifiers: Keyboard modifier state bitmask
 * @is_user_gesture: Whether the navigation was triggered by a user gesture
 * @is_redirect: Whether the navigation is a redirect
 *
 * Describes a navigation request as it is about to be performed.
 */
struct _GsurfNavigationAction {
	gchar     *uri;             /* Target URI, or NULL */
	guint      navigation_type; /* Navigation type */
	guint      mouse_button;    /* Mouse button (0 if none) */
	guint      modifiers;       /* Modifier state bitmask */
	gboolean   is_user_gesture; /* User-gesture initiated */
	gboolean   is_redirect;     /* Redirect navigation */
};

/**
 * gsurf_navigation_action_get_type:
 *
 * Gets the GType for the GsurfNavigationAction boxed type.
 *
 * Returns: the GType
 */
GType gsurf_navigation_action_get_type(void) G_GNUC_CONST;

/**
 * gsurf_navigation_action_new:
 *
 * Creates a new, empty navigation action with a %NULL URI, zeroed
 * scalar fields, and both booleans %FALSE.
 *
 * Returns: (transfer full): a new GsurfNavigationAction
 */
GsurfNavigationAction *gsurf_navigation_action_new(void);

/**
 * gsurf_navigation_action_copy:
 * @action: a GsurfNavigationAction
 *
 * Creates a deep copy of the navigation action, duplicating the URI.
 *
 * Returns: (transfer full): a copy of @action
 */
GsurfNavigationAction *gsurf_navigation_action_copy(const GsurfNavigationAction *action);

/**
 * gsurf_navigation_action_free:
 * @action: a GsurfNavigationAction
 *
 * Frees the memory allocated for the navigation action.
 */
void gsurf_navigation_action_free(GsurfNavigationAction *action);

/* Getters */
const gchar *gsurf_navigation_action_get_uri(const GsurfNavigationAction *action);
guint        gsurf_navigation_action_get_navigation_type(const GsurfNavigationAction *action);
guint        gsurf_navigation_action_get_mouse_button(const GsurfNavigationAction *action);
guint        gsurf_navigation_action_get_modifiers(const GsurfNavigationAction *action);
gboolean     gsurf_navigation_action_get_is_user_gesture(const GsurfNavigationAction *action);
gboolean     gsurf_navigation_action_get_is_redirect(const GsurfNavigationAction *action);

/* Setters */
void gsurf_navigation_action_set_uri(GsurfNavigationAction *action, const gchar *uri);
void gsurf_navigation_action_set_navigation_type(GsurfNavigationAction *action, guint navigation_type);
void gsurf_navigation_action_set_mouse_button(GsurfNavigationAction *action, guint mouse_button);
void gsurf_navigation_action_set_modifiers(GsurfNavigationAction *action, guint modifiers);
void gsurf_navigation_action_set_is_user_gesture(GsurfNavigationAction *action, gboolean is_user_gesture);
void gsurf_navigation_action_set_is_redirect(GsurfNavigationAction *action, gboolean is_redirect);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfNavigationAction, gsurf_navigation_action_free)

G_END_DECLS

#endif /* GSURF_NAVIGATION_ACTION_H */
