/*
 * gsurf-mousebind.h - GSURF Mousebind Boxed Type
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Associates a pointer button (button + modifiers) and an input mode
 * with a browser action and an optional string argument. The mouse
 * analogue of #GsurfKeybind.
 */

#ifndef GSURF_MOUSEBIND_H
#define GSURF_MOUSEBIND_H

#include <glib-object.h>
#include "../gsurf-types.h"
#include "../gsurf-enums.h"

G_BEGIN_DECLS

#define GSURF_TYPE_MOUSEBIND (gsurf_mousebind_get_type())

/**
 * GsurfMousebind:
 * @button: the pointer button the binding triggers on (see #GsurfMousebutton)
 * @modifiers: required modifier flags (see #GsurfKeyMod)
 * @mode: the input mode the binding applies in
 * @action: the browser action to perform
 * @arg: (nullable): optional string argument for the action
 *
 * A single mouse binding mapping a button/mode to a #GsurfAction.
 */
struct _GsurfMousebind {
	guint		button;		/* Pointer button */
	guint		modifiers;	/* Modifier flags */
	GsurfModePolicy	mode;		/* Mode the binding applies in */
	GsurfAction	action;		/* Action to perform */
	gchar		*arg;		/* Optional action argument */
};

/**
 * gsurf_mousebind_get_type:
 *
 * Gets the GType for the GsurfMousebind boxed type.
 *
 * Returns: the GType
 */
GType gsurf_mousebind_get_type(void) G_GNUC_CONST;

/**
 * gsurf_mousebind_new:
 * @button: the pointer button
 * @modifiers: required modifier flags
 * @mode: the input mode the binding applies in
 * @action: the browser action to perform
 * @arg: (nullable): optional string argument (deep-copied)
 *
 * Creates a new mouse binding.
 *
 * Returns: (transfer full): a new #GsurfMousebind
 */
GsurfMousebind *gsurf_mousebind_new(
	guint		button,
	guint		modifiers,
	GsurfModePolicy	mode,
	GsurfAction	action,
	const gchar	*arg
);

/**
 * gsurf_mousebind_copy:
 * @mousebind: a #GsurfMousebind
 *
 * Creates a deep copy of the mouse binding.
 *
 * Returns: (transfer full): a copy of @mousebind
 */
GsurfMousebind *gsurf_mousebind_copy(const GsurfMousebind *mousebind);

/**
 * gsurf_mousebind_free:
 * @mousebind: a #GsurfMousebind
 *
 * Frees the memory allocated for the mouse binding.
 */
void gsurf_mousebind_free(GsurfMousebind *mousebind);

/**
 * gsurf_mousebind_get_button:
 * @mousebind: a #GsurfMousebind
 *
 * Returns: the pointer button
 */
guint gsurf_mousebind_get_button(const GsurfMousebind *mousebind);

/**
 * gsurf_mousebind_set_button:
 * @mousebind: a #GsurfMousebind
 * @button: the pointer button
 *
 * Sets the pointer button.
 */
void gsurf_mousebind_set_button(GsurfMousebind *mousebind, guint button);

/**
 * gsurf_mousebind_get_modifiers:
 * @mousebind: a #GsurfMousebind
 *
 * Returns: the modifier flags
 */
guint gsurf_mousebind_get_modifiers(const GsurfMousebind *mousebind);

/**
 * gsurf_mousebind_set_modifiers:
 * @mousebind: a #GsurfMousebind
 * @modifiers: the modifier flags
 *
 * Sets the modifier flags.
 */
void gsurf_mousebind_set_modifiers(GsurfMousebind *mousebind, guint modifiers);

/**
 * gsurf_mousebind_get_mode:
 * @mousebind: a #GsurfMousebind
 *
 * Returns: the input mode the binding applies in
 */
GsurfModePolicy gsurf_mousebind_get_mode(const GsurfMousebind *mousebind);

/**
 * gsurf_mousebind_set_mode:
 * @mousebind: a #GsurfMousebind
 * @mode: the input mode
 *
 * Sets the input mode the binding applies in.
 */
void gsurf_mousebind_set_mode(GsurfMousebind *mousebind, GsurfModePolicy mode);

/**
 * gsurf_mousebind_get_action:
 * @mousebind: a #GsurfMousebind
 *
 * Returns: the bound action
 */
GsurfAction gsurf_mousebind_get_action(const GsurfMousebind *mousebind);

/**
 * gsurf_mousebind_set_action:
 * @mousebind: a #GsurfMousebind
 * @action: the action to perform
 *
 * Sets the bound action.
 */
void gsurf_mousebind_set_action(GsurfMousebind *mousebind, GsurfAction action);

/**
 * gsurf_mousebind_get_arg:
 * @mousebind: a #GsurfMousebind
 *
 * Returns: (nullable) (transfer none): the action argument, or %NULL
 */
const gchar *gsurf_mousebind_get_arg(const GsurfMousebind *mousebind);

/**
 * gsurf_mousebind_set_arg:
 * @mousebind: a #GsurfMousebind
 * @arg: (nullable): the action argument (deep-copied)
 *
 * Sets the action argument, freeing any previous value.
 */
void gsurf_mousebind_set_arg(GsurfMousebind *mousebind, const gchar *arg);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfMousebind, gsurf_mousebind_free)

G_END_DECLS

#endif /* GSURF_MOUSEBIND_H */
