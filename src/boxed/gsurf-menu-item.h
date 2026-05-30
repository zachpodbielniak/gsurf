/*
 * gsurf-menu-item.h - GSURF Context Menu Item Boxed Type
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Represents a single entry in a context menu: a visible label bound to
 * an action (with an optional argument), or a separator with no label.
 */

#ifndef GSURF_MENU_ITEM_H
#define GSURF_MENU_ITEM_H

#include <glib-object.h>
#include "../gsurf-types.h"

G_BEGIN_DECLS

#define GSURF_TYPE_MENU_ITEM (gsurf_menu_item_get_type())

/**
 * GsurfMenuItem:
 * @label: Visible menu label, or %NULL
 * @action: Action identifier to invoke, or %NULL
 * @arg: Optional argument string for the action, or %NULL
 * @is_separator: Whether this item is a separator
 *
 * Describes one entry in a context menu.
 */
struct _GsurfMenuItem {
	gchar     *label;           /* Visible label, or NULL */
	gchar     *action;          /* Action identifier, or NULL */
	gchar     *arg;             /* Action argument, or NULL */
	gboolean   is_separator;    /* Separator flag */
};

/**
 * gsurf_menu_item_get_type:
 *
 * Gets the GType for the GsurfMenuItem boxed type.
 *
 * Returns: the GType
 */
GType gsurf_menu_item_get_type(void) G_GNUC_CONST;

/**
 * gsurf_menu_item_new:
 *
 * Creates a new, empty menu item with all strings %NULL and
 * @is_separator %FALSE.
 *
 * Returns: (transfer full): a new GsurfMenuItem
 */
GsurfMenuItem *gsurf_menu_item_new(void);

/**
 * gsurf_menu_item_new_separator:
 *
 * Creates a new separator menu item (@is_separator set to %TRUE,
 * all strings %NULL).
 *
 * Returns: (transfer full): a new separator GsurfMenuItem
 */
GsurfMenuItem *gsurf_menu_item_new_separator(void);

/**
 * gsurf_menu_item_copy:
 * @menu_item: a GsurfMenuItem
 *
 * Creates a deep copy of the menu item, duplicating all strings.
 *
 * Returns: (transfer full): a copy of @menu_item
 */
GsurfMenuItem *gsurf_menu_item_copy(const GsurfMenuItem *menu_item);

/**
 * gsurf_menu_item_free:
 * @menu_item: a GsurfMenuItem
 *
 * Frees the memory allocated for the menu item.
 */
void gsurf_menu_item_free(GsurfMenuItem *menu_item);

/* Getters */
const gchar *gsurf_menu_item_get_label(const GsurfMenuItem *menu_item);
const gchar *gsurf_menu_item_get_action(const GsurfMenuItem *menu_item);
const gchar *gsurf_menu_item_get_arg(const GsurfMenuItem *menu_item);
gboolean     gsurf_menu_item_get_is_separator(const GsurfMenuItem *menu_item);

/* Setters */
void gsurf_menu_item_set_label(GsurfMenuItem *menu_item, const gchar *label);
void gsurf_menu_item_set_action(GsurfMenuItem *menu_item, const gchar *action);
void gsurf_menu_item_set_arg(GsurfMenuItem *menu_item, const gchar *arg);
void gsurf_menu_item_set_is_separator(GsurfMenuItem *menu_item, gboolean is_separator);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfMenuItem, gsurf_menu_item_free)

G_END_DECLS

#endif /* GSURF_MENU_ITEM_H */
