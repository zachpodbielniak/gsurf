/*
 * gsurf-navigation-action.c - GSURF Navigation Action Boxed Type Implementation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of the GsurfNavigationAction boxed type describing a
 * pending navigation request: target URI, navigation type, originating
 * mouse button and modifiers, and the user-gesture/redirect flags.
 */

#include "gsurf-navigation-action.h"

G_DEFINE_BOXED_TYPE(GsurfNavigationAction, gsurf_navigation_action, gsurf_navigation_action_copy, gsurf_navigation_action_free)

/*
 * gsurf_navigation_action_new:
 *
 * Creates a new, empty navigation action. The URI is %NULL, all scalar
 * fields are 0, and both booleans are %FALSE.
 *
 * Returns: (transfer full): a newly allocated GsurfNavigationAction
 */
GsurfNavigationAction *
gsurf_navigation_action_new(void)
{
	GsurfNavigationAction *action;

	action = g_new0(GsurfNavigationAction, 1);
	action->uri = NULL;
	action->navigation_type = 0;
	action->mouse_button = 0;
	action->modifiers = 0;
	action->is_user_gesture = FALSE;
	action->is_redirect = FALSE;

	return action;
}

/*
 * gsurf_navigation_action_copy:
 * @action: a GsurfNavigationAction to copy
 *
 * Creates a deep copy of the navigation action, duplicating the URI.
 *
 * Returns: (transfer full): a copy of @action
 */
GsurfNavigationAction *
gsurf_navigation_action_copy(const GsurfNavigationAction *action)
{
	GsurfNavigationAction *copy;

	g_return_val_if_fail(action != NULL, NULL);

	copy = g_new0(GsurfNavigationAction, 1);
	copy->uri = g_strdup(action->uri);
	copy->navigation_type = action->navigation_type;
	copy->mouse_button = action->mouse_button;
	copy->modifiers = action->modifiers;
	copy->is_user_gesture = action->is_user_gesture;
	copy->is_redirect = action->is_redirect;

	return copy;
}

/*
 * gsurf_navigation_action_free:
 * @action: a GsurfNavigationAction to free
 *
 * Frees the memory allocated for the navigation action, including the
 * duplicated URI.
 */
void
gsurf_navigation_action_free(GsurfNavigationAction *action)
{
	if (action == NULL) {
		return;
	}

	g_free(action->uri);
	g_free(action);
}

/*
 * gsurf_navigation_action_get_uri:
 * @action: a GsurfNavigationAction
 *
 * Returns: (nullable): the target URI, or %NULL
 */
const gchar *
gsurf_navigation_action_get_uri(const GsurfNavigationAction *action)
{
	g_return_val_if_fail(action != NULL, NULL);

	return action->uri;
}

/*
 * gsurf_navigation_action_get_navigation_type:
 * @action: a GsurfNavigationAction
 *
 * Returns: the navigation type
 */
guint
gsurf_navigation_action_get_navigation_type(const GsurfNavigationAction *action)
{
	g_return_val_if_fail(action != NULL, 0);

	return action->navigation_type;
}

/*
 * gsurf_navigation_action_get_mouse_button:
 * @action: a GsurfNavigationAction
 *
 * Returns: the originating mouse button (0 if none)
 */
guint
gsurf_navigation_action_get_mouse_button(const GsurfNavigationAction *action)
{
	g_return_val_if_fail(action != NULL, 0);

	return action->mouse_button;
}

/*
 * gsurf_navigation_action_get_modifiers:
 * @action: a GsurfNavigationAction
 *
 * Returns: the modifier state bitmask
 */
guint
gsurf_navigation_action_get_modifiers(const GsurfNavigationAction *action)
{
	g_return_val_if_fail(action != NULL, 0);

	return action->modifiers;
}

/*
 * gsurf_navigation_action_get_is_user_gesture:
 * @action: a GsurfNavigationAction
 *
 * Returns: %TRUE if the navigation was triggered by a user gesture
 */
gboolean
gsurf_navigation_action_get_is_user_gesture(const GsurfNavigationAction *action)
{
	g_return_val_if_fail(action != NULL, FALSE);

	return action->is_user_gesture;
}

/*
 * gsurf_navigation_action_get_is_redirect:
 * @action: a GsurfNavigationAction
 *
 * Returns: %TRUE if the navigation is a redirect
 */
gboolean
gsurf_navigation_action_get_is_redirect(const GsurfNavigationAction *action)
{
	g_return_val_if_fail(action != NULL, FALSE);

	return action->is_redirect;
}

/*
 * gsurf_navigation_action_set_uri:
 * @action: a GsurfNavigationAction
 * @uri: (nullable): the target URI, or %NULL
 *
 * Sets the target URI, freeing any previous value.
 */
void
gsurf_navigation_action_set_uri(
	GsurfNavigationAction   *action,
	const gchar             *uri
){
	g_return_if_fail(action != NULL);

	g_free(action->uri);
	action->uri = g_strdup(uri);
}

/*
 * gsurf_navigation_action_set_navigation_type:
 * @action: a GsurfNavigationAction
 * @navigation_type: the navigation type
 *
 * Sets the navigation type.
 */
void
gsurf_navigation_action_set_navigation_type(
	GsurfNavigationAction   *action,
	guint                   navigation_type
){
	g_return_if_fail(action != NULL);

	action->navigation_type = navigation_type;
}

/*
 * gsurf_navigation_action_set_mouse_button:
 * @action: a GsurfNavigationAction
 * @mouse_button: the originating mouse button (0 if none)
 *
 * Sets the originating mouse button.
 */
void
gsurf_navigation_action_set_mouse_button(
	GsurfNavigationAction   *action,
	guint                   mouse_button
){
	g_return_if_fail(action != NULL);

	action->mouse_button = mouse_button;
}

/*
 * gsurf_navigation_action_set_modifiers:
 * @action: a GsurfNavigationAction
 * @modifiers: the modifier state bitmask
 *
 * Sets the modifier state.
 */
void
gsurf_navigation_action_set_modifiers(
	GsurfNavigationAction   *action,
	guint                   modifiers
){
	g_return_if_fail(action != NULL);

	action->modifiers = modifiers;
}

/*
 * gsurf_navigation_action_set_is_user_gesture:
 * @action: a GsurfNavigationAction
 * @is_user_gesture: whether the navigation was a user gesture
 *
 * Sets the user-gesture flag.
 */
void
gsurf_navigation_action_set_is_user_gesture(
	GsurfNavigationAction   *action,
	gboolean                is_user_gesture
){
	g_return_if_fail(action != NULL);

	action->is_user_gesture = is_user_gesture;
}

/*
 * gsurf_navigation_action_set_is_redirect:
 * @action: a GsurfNavigationAction
 * @is_redirect: whether the navigation is a redirect
 *
 * Sets the redirect flag.
 */
void
gsurf_navigation_action_set_is_redirect(
	GsurfNavigationAction   *action,
	gboolean                is_redirect
){
	g_return_if_fail(action != NULL);

	action->is_redirect = is_redirect;
}
