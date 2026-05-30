/*
 * gsurf-keybind.h - GSURF Keybind Boxed Type
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Associates a key (keyval + modifiers) and an input mode with a
 * browser action and an optional string argument. Used to express
 * the modal vim-style keymap.
 */

#ifndef GSURF_KEYBIND_H
#define GSURF_KEYBIND_H

#include <glib-object.h>
#include "../gsurf-types.h"
#include "../gsurf-enums.h"

G_BEGIN_DECLS

#define GSURF_TYPE_KEYBIND (gsurf_keybind_get_type())

/**
 * GsurfKeybind:
 * @keyval: the key symbol (backend keyval) the binding triggers on
 * @modifiers: required modifier flags (see #GsurfKeyMod)
 * @mode: the input mode the binding applies in
 * @action: the browser action to perform
 * @arg: (nullable): optional string argument for the action
 *
 * A single key binding mapping a key/mode to a #GsurfAction.
 */
struct _GsurfKeybind {
	guint		keyval;		/* Key symbol */
	guint		modifiers;	/* Modifier flags */
	GsurfModePolicy	mode;		/* Mode the binding applies in */
	GsurfAction	action;		/* Action to perform */
	gchar		*arg;		/* Optional action argument */
};

/**
 * gsurf_keybind_get_type:
 *
 * Gets the GType for the GsurfKeybind boxed type.
 *
 * Returns: the GType
 */
GType gsurf_keybind_get_type(void) G_GNUC_CONST;

/**
 * gsurf_keybind_new:
 * @keyval: the key symbol
 * @modifiers: required modifier flags
 * @mode: the input mode the binding applies in
 * @action: the browser action to perform
 * @arg: (nullable): optional string argument (deep-copied)
 *
 * Creates a new key binding.
 *
 * Returns: (transfer full): a new #GsurfKeybind
 */
GsurfKeybind *gsurf_keybind_new(
	guint		keyval,
	guint		modifiers,
	GsurfModePolicy	mode,
	GsurfAction	action,
	const gchar	*arg
);

/**
 * gsurf_keybind_copy:
 * @keybind: a #GsurfKeybind
 *
 * Creates a deep copy of the key binding.
 *
 * Returns: (transfer full): a copy of @keybind
 */
GsurfKeybind *gsurf_keybind_copy(const GsurfKeybind *keybind);

/**
 * gsurf_keybind_free:
 * @keybind: a #GsurfKeybind
 *
 * Frees the memory allocated for the key binding.
 */
void gsurf_keybind_free(GsurfKeybind *keybind);

/**
 * gsurf_keybind_get_keyval:
 * @keybind: a #GsurfKeybind
 *
 * Returns: the key symbol
 */
guint gsurf_keybind_get_keyval(const GsurfKeybind *keybind);

/**
 * gsurf_keybind_set_keyval:
 * @keybind: a #GsurfKeybind
 * @keyval: the key symbol
 *
 * Sets the key symbol.
 */
void gsurf_keybind_set_keyval(GsurfKeybind *keybind, guint keyval);

/**
 * gsurf_keybind_get_modifiers:
 * @keybind: a #GsurfKeybind
 *
 * Returns: the modifier flags
 */
guint gsurf_keybind_get_modifiers(const GsurfKeybind *keybind);

/**
 * gsurf_keybind_set_modifiers:
 * @keybind: a #GsurfKeybind
 * @modifiers: the modifier flags
 *
 * Sets the modifier flags.
 */
void gsurf_keybind_set_modifiers(GsurfKeybind *keybind, guint modifiers);

/**
 * gsurf_keybind_get_mode:
 * @keybind: a #GsurfKeybind
 *
 * Returns: the input mode the binding applies in
 */
GsurfModePolicy gsurf_keybind_get_mode(const GsurfKeybind *keybind);

/**
 * gsurf_keybind_set_mode:
 * @keybind: a #GsurfKeybind
 * @mode: the input mode
 *
 * Sets the input mode the binding applies in.
 */
void gsurf_keybind_set_mode(GsurfKeybind *keybind, GsurfModePolicy mode);

/**
 * gsurf_keybind_get_action:
 * @keybind: a #GsurfKeybind
 *
 * Returns: the bound action
 */
GsurfAction gsurf_keybind_get_action(const GsurfKeybind *keybind);

/**
 * gsurf_keybind_set_action:
 * @keybind: a #GsurfKeybind
 * @action: the action to perform
 *
 * Sets the bound action.
 */
void gsurf_keybind_set_action(GsurfKeybind *keybind, GsurfAction action);

/**
 * gsurf_keybind_get_arg:
 * @keybind: a #GsurfKeybind
 *
 * Returns: (nullable) (transfer none): the action argument, or %NULL
 */
const gchar *gsurf_keybind_get_arg(const GsurfKeybind *keybind);

/**
 * gsurf_keybind_set_arg:
 * @keybind: a #GsurfKeybind
 * @arg: (nullable): the action argument (deep-copied)
 *
 * Sets the action argument, freeing any previous value.
 */
void gsurf_keybind_set_arg(GsurfKeybind *keybind, const gchar *arg);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfKeybind, gsurf_keybind_free)

G_END_DECLS

#endif /* GSURF_KEYBIND_H */
