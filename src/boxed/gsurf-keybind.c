/*
 * gsurf-keybind.c - GSURF Keybind Boxed Type Implementation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of the GsurfKeybind boxed type associating a key and
 * input mode with a browser action and optional argument.
 */

#include "gsurf-keybind.h"

G_DEFINE_BOXED_TYPE(GsurfKeybind, gsurf_keybind, gsurf_keybind_copy, gsurf_keybind_free)

/*
 * gsurf_keybind_new:
 * @keyval: the key symbol
 * @modifiers: required modifier flags
 * @mode: the input mode the binding applies in
 * @action: the browser action to perform
 * @arg: (nullable): optional string argument (deep-copied)
 *
 * Creates a new key binding.
 *
 * Returns: (transfer full): a newly allocated #GsurfKeybind
 */
GsurfKeybind *
gsurf_keybind_new(
	guint		keyval,
	guint		modifiers,
	GsurfModePolicy	mode,
	GsurfAction	action,
	const gchar	*arg
){
	GsurfKeybind *keybind;

	keybind = g_slice_new(GsurfKeybind);
	keybind->keyval = keyval;
	keybind->modifiers = modifiers;
	keybind->mode = mode;
	keybind->action = action;
	keybind->arg = g_strdup(arg);

	return keybind;
}

/*
 * gsurf_keybind_copy:
 * @keybind: a #GsurfKeybind to copy
 *
 * Creates a deep copy of the key binding, duplicating its argument.
 *
 * Returns: (transfer full): a copy of @keybind
 */
GsurfKeybind *
gsurf_keybind_copy(const GsurfKeybind *keybind)
{
	GsurfKeybind *copy;

	g_return_val_if_fail(keybind != NULL, NULL);

	copy = g_slice_new(GsurfKeybind);
	copy->keyval = keybind->keyval;
	copy->modifiers = keybind->modifiers;
	copy->mode = keybind->mode;
	copy->action = keybind->action;
	copy->arg = g_strdup(keybind->arg);

	return copy;
}

/*
 * gsurf_keybind_free:
 * @keybind: a #GsurfKeybind to free
 *
 * Frees the key binding and its argument.
 */
void
gsurf_keybind_free(GsurfKeybind *keybind)
{
	if (keybind != NULL) {
		g_free(keybind->arg);
		g_slice_free(GsurfKeybind, keybind);
	}
}

/*
 * gsurf_keybind_get_keyval:
 * @keybind: a #GsurfKeybind
 *
 * Returns: the key symbol
 */
guint
gsurf_keybind_get_keyval(const GsurfKeybind *keybind)
{
	g_return_val_if_fail(keybind != NULL, 0);

	return keybind->keyval;
}

/*
 * gsurf_keybind_set_keyval:
 * @keybind: a #GsurfKeybind
 * @keyval: the key symbol
 *
 * Sets the key symbol.
 */
void
gsurf_keybind_set_keyval(
	GsurfKeybind	*keybind,
	guint		keyval
){
	g_return_if_fail(keybind != NULL);

	keybind->keyval = keyval;
}

/*
 * gsurf_keybind_get_modifiers:
 * @keybind: a #GsurfKeybind
 *
 * Returns: the modifier flags
 */
guint
gsurf_keybind_get_modifiers(const GsurfKeybind *keybind)
{
	g_return_val_if_fail(keybind != NULL, 0);

	return keybind->modifiers;
}

/*
 * gsurf_keybind_set_modifiers:
 * @keybind: a #GsurfKeybind
 * @modifiers: the modifier flags
 *
 * Sets the modifier flags.
 */
void
gsurf_keybind_set_modifiers(
	GsurfKeybind	*keybind,
	guint		modifiers
){
	g_return_if_fail(keybind != NULL);

	keybind->modifiers = modifiers;
}

/*
 * gsurf_keybind_get_mode:
 * @keybind: a #GsurfKeybind
 *
 * Returns: the input mode the binding applies in
 */
GsurfModePolicy
gsurf_keybind_get_mode(const GsurfKeybind *keybind)
{
	g_return_val_if_fail(keybind != NULL, GSURF_MODE_NORMAL);

	return keybind->mode;
}

/*
 * gsurf_keybind_set_mode:
 * @keybind: a #GsurfKeybind
 * @mode: the input mode
 *
 * Sets the input mode the binding applies in.
 */
void
gsurf_keybind_set_mode(
	GsurfKeybind	*keybind,
	GsurfModePolicy	mode
){
	g_return_if_fail(keybind != NULL);

	keybind->mode = mode;
}

/*
 * gsurf_keybind_get_action:
 * @keybind: a #GsurfKeybind
 *
 * Returns: the bound action
 */
GsurfAction
gsurf_keybind_get_action(const GsurfKeybind *keybind)
{
	g_return_val_if_fail(keybind != NULL, GSURF_ACTION_NONE);

	return keybind->action;
}

/*
 * gsurf_keybind_set_action:
 * @keybind: a #GsurfKeybind
 * @action: the action to perform
 *
 * Sets the bound action.
 */
void
gsurf_keybind_set_action(
	GsurfKeybind	*keybind,
	GsurfAction	action
){
	g_return_if_fail(keybind != NULL);

	keybind->action = action;
}

/*
 * gsurf_keybind_get_arg:
 * @keybind: a #GsurfKeybind
 *
 * Returns: (nullable) (transfer none): the action argument, or %NULL
 */
const gchar *
gsurf_keybind_get_arg(const GsurfKeybind *keybind)
{
	g_return_val_if_fail(keybind != NULL, NULL);

	return keybind->arg;
}

/*
 * gsurf_keybind_set_arg:
 * @keybind: a #GsurfKeybind
 * @arg: (nullable): the action argument (deep-copied)
 *
 * Sets the action argument, freeing any previous value.
 */
void
gsurf_keybind_set_arg(
	GsurfKeybind	*keybind,
	const gchar	*arg
){
	g_return_if_fail(keybind != NULL);

	g_free(keybind->arg);
	keybind->arg = g_strdup(arg);
}
