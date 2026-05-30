/*
 * gsurf-mousebind.c - GSURF Mousebind Boxed Type Implementation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of the GsurfMousebind boxed type associating a pointer
 * button and input mode with a browser action and optional argument.
 */

#include "gsurf-mousebind.h"

G_DEFINE_BOXED_TYPE(GsurfMousebind, gsurf_mousebind, gsurf_mousebind_copy, gsurf_mousebind_free)

/*
 * gsurf_mousebind_new:
 * @button: the pointer button
 * @modifiers: required modifier flags
 * @mode: the input mode the binding applies in
 * @action: the browser action to perform
 * @arg: (nullable): optional string argument (deep-copied)
 *
 * Creates a new mouse binding.
 *
 * Returns: (transfer full): a newly allocated #GsurfMousebind
 */
GsurfMousebind *
gsurf_mousebind_new(
	guint		button,
	guint		modifiers,
	GsurfModePolicy	mode,
	GsurfAction	action,
	const gchar	*arg
){
	GsurfMousebind *mousebind;

	mousebind = g_slice_new(GsurfMousebind);
	mousebind->button = button;
	mousebind->modifiers = modifiers;
	mousebind->mode = mode;
	mousebind->action = action;
	mousebind->arg = g_strdup(arg);

	return mousebind;
}

/*
 * gsurf_mousebind_copy:
 * @mousebind: a #GsurfMousebind to copy
 *
 * Creates a deep copy of the mouse binding, duplicating its argument.
 *
 * Returns: (transfer full): a copy of @mousebind
 */
GsurfMousebind *
gsurf_mousebind_copy(const GsurfMousebind *mousebind)
{
	GsurfMousebind *copy;

	g_return_val_if_fail(mousebind != NULL, NULL);

	copy = g_slice_new(GsurfMousebind);
	copy->button = mousebind->button;
	copy->modifiers = mousebind->modifiers;
	copy->mode = mousebind->mode;
	copy->action = mousebind->action;
	copy->arg = g_strdup(mousebind->arg);

	return copy;
}

/*
 * gsurf_mousebind_free:
 * @mousebind: a #GsurfMousebind to free
 *
 * Frees the mouse binding and its argument.
 */
void
gsurf_mousebind_free(GsurfMousebind *mousebind)
{
	if (mousebind != NULL) {
		g_free(mousebind->arg);
		g_slice_free(GsurfMousebind, mousebind);
	}
}

/*
 * gsurf_mousebind_get_button:
 * @mousebind: a #GsurfMousebind
 *
 * Returns: the pointer button
 */
guint
gsurf_mousebind_get_button(const GsurfMousebind *mousebind)
{
	g_return_val_if_fail(mousebind != NULL, 0);

	return mousebind->button;
}

/*
 * gsurf_mousebind_set_button:
 * @mousebind: a #GsurfMousebind
 * @button: the pointer button
 *
 * Sets the pointer button.
 */
void
gsurf_mousebind_set_button(
	GsurfMousebind	*mousebind,
	guint		button
){
	g_return_if_fail(mousebind != NULL);

	mousebind->button = button;
}

/*
 * gsurf_mousebind_get_modifiers:
 * @mousebind: a #GsurfMousebind
 *
 * Returns: the modifier flags
 */
guint
gsurf_mousebind_get_modifiers(const GsurfMousebind *mousebind)
{
	g_return_val_if_fail(mousebind != NULL, 0);

	return mousebind->modifiers;
}

/*
 * gsurf_mousebind_set_modifiers:
 * @mousebind: a #GsurfMousebind
 * @modifiers: the modifier flags
 *
 * Sets the modifier flags.
 */
void
gsurf_mousebind_set_modifiers(
	GsurfMousebind	*mousebind,
	guint		modifiers
){
	g_return_if_fail(mousebind != NULL);

	mousebind->modifiers = modifiers;
}

/*
 * gsurf_mousebind_get_mode:
 * @mousebind: a #GsurfMousebind
 *
 * Returns: the input mode the binding applies in
 */
GsurfModePolicy
gsurf_mousebind_get_mode(const GsurfMousebind *mousebind)
{
	g_return_val_if_fail(mousebind != NULL, GSURF_MODE_NORMAL);

	return mousebind->mode;
}

/*
 * gsurf_mousebind_set_mode:
 * @mousebind: a #GsurfMousebind
 * @mode: the input mode
 *
 * Sets the input mode the binding applies in.
 */
void
gsurf_mousebind_set_mode(
	GsurfMousebind	*mousebind,
	GsurfModePolicy	mode
){
	g_return_if_fail(mousebind != NULL);

	mousebind->mode = mode;
}

/*
 * gsurf_mousebind_get_action:
 * @mousebind: a #GsurfMousebind
 *
 * Returns: the bound action
 */
GsurfAction
gsurf_mousebind_get_action(const GsurfMousebind *mousebind)
{
	g_return_val_if_fail(mousebind != NULL, GSURF_ACTION_NONE);

	return mousebind->action;
}

/*
 * gsurf_mousebind_set_action:
 * @mousebind: a #GsurfMousebind
 * @action: the action to perform
 *
 * Sets the bound action.
 */
void
gsurf_mousebind_set_action(
	GsurfMousebind	*mousebind,
	GsurfAction	action
){
	g_return_if_fail(mousebind != NULL);

	mousebind->action = action;
}

/*
 * gsurf_mousebind_get_arg:
 * @mousebind: a #GsurfMousebind
 *
 * Returns: (nullable) (transfer none): the action argument, or %NULL
 */
const gchar *
gsurf_mousebind_get_arg(const GsurfMousebind *mousebind)
{
	g_return_val_if_fail(mousebind != NULL, NULL);

	return mousebind->arg;
}

/*
 * gsurf_mousebind_set_arg:
 * @mousebind: a #GsurfMousebind
 * @arg: (nullable): the action argument (deep-copied)
 *
 * Sets the action argument, freeing any previous value.
 */
void
gsurf_mousebind_set_arg(
	GsurfMousebind	*mousebind,
	const gchar	*arg
){
	g_return_if_fail(mousebind != NULL);

	g_free(mousebind->arg);
	mousebind->arg = g_strdup(arg);
}
