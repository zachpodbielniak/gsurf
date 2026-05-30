/*
 * gsurf-menu-item.c - GSURF Context Menu Item Boxed Type Implementation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of the GsurfMenuItem boxed type describing a single
 * context-menu entry: a label bound to an action with an optional
 * argument, or a separator.
 */

#include "gsurf-menu-item.h"

G_DEFINE_BOXED_TYPE(GsurfMenuItem, gsurf_menu_item, gsurf_menu_item_copy, gsurf_menu_item_free)

/*
 * gsurf_menu_item_new:
 *
 * Creates a new, empty menu item. All string fields are %NULL and
 * @is_separator is %FALSE.
 *
 * Returns: (transfer full): a newly allocated GsurfMenuItem
 */
GsurfMenuItem *
gsurf_menu_item_new(void)
{
	GsurfMenuItem *menu_item;

	menu_item = g_new0(GsurfMenuItem, 1);
	menu_item->label = NULL;
	menu_item->action = NULL;
	menu_item->arg = NULL;
	menu_item->is_separator = FALSE;

	return menu_item;
}

/*
 * gsurf_menu_item_new_separator:
 *
 * Creates a new separator menu item. The @is_separator flag is %TRUE
 * and all string fields are %NULL.
 *
 * Returns: (transfer full): a newly allocated separator GsurfMenuItem
 */
GsurfMenuItem *
gsurf_menu_item_new_separator(void)
{
	GsurfMenuItem *menu_item;

	menu_item = gsurf_menu_item_new();
	menu_item->is_separator = TRUE;

	return menu_item;
}

/*
 * gsurf_menu_item_copy:
 * @menu_item: a GsurfMenuItem to copy
 *
 * Creates a deep copy of the menu item, duplicating all strings.
 *
 * Returns: (transfer full): a copy of @menu_item
 */
GsurfMenuItem *
gsurf_menu_item_copy(const GsurfMenuItem *menu_item)
{
	GsurfMenuItem *copy;

	g_return_val_if_fail(menu_item != NULL, NULL);

	copy = g_new0(GsurfMenuItem, 1);
	copy->label = g_strdup(menu_item->label);
	copy->action = g_strdup(menu_item->action);
	copy->arg = g_strdup(menu_item->arg);
	copy->is_separator = menu_item->is_separator;

	return copy;
}

/*
 * gsurf_menu_item_free:
 * @menu_item: a GsurfMenuItem to free
 *
 * Frees the memory allocated for the menu item, including all
 * duplicated strings.
 */
void
gsurf_menu_item_free(GsurfMenuItem *menu_item)
{
	if (menu_item == NULL) {
		return;
	}

	g_free(menu_item->label);
	g_free(menu_item->action);
	g_free(menu_item->arg);
	g_free(menu_item);
}

/*
 * gsurf_menu_item_get_label:
 * @menu_item: a GsurfMenuItem
 *
 * Returns: (nullable): the visible label, or %NULL
 */
const gchar *
gsurf_menu_item_get_label(const GsurfMenuItem *menu_item)
{
	g_return_val_if_fail(menu_item != NULL, NULL);

	return menu_item->label;
}

/*
 * gsurf_menu_item_get_action:
 * @menu_item: a GsurfMenuItem
 *
 * Returns: (nullable): the action identifier, or %NULL
 */
const gchar *
gsurf_menu_item_get_action(const GsurfMenuItem *menu_item)
{
	g_return_val_if_fail(menu_item != NULL, NULL);

	return menu_item->action;
}

/*
 * gsurf_menu_item_get_arg:
 * @menu_item: a GsurfMenuItem
 *
 * Returns: (nullable): the action argument, or %NULL
 */
const gchar *
gsurf_menu_item_get_arg(const GsurfMenuItem *menu_item)
{
	g_return_val_if_fail(menu_item != NULL, NULL);

	return menu_item->arg;
}

/*
 * gsurf_menu_item_get_is_separator:
 * @menu_item: a GsurfMenuItem
 *
 * Returns: %TRUE if the item is a separator
 */
gboolean
gsurf_menu_item_get_is_separator(const GsurfMenuItem *menu_item)
{
	g_return_val_if_fail(menu_item != NULL, FALSE);

	return menu_item->is_separator;
}

/*
 * gsurf_menu_item_set_label:
 * @menu_item: a GsurfMenuItem
 * @label: (nullable): the visible label, or %NULL
 *
 * Sets the label, freeing any previous value.
 */
void
gsurf_menu_item_set_label(
	GsurfMenuItem   *menu_item,
	const gchar     *label
){
	g_return_if_fail(menu_item != NULL);

	g_free(menu_item->label);
	menu_item->label = g_strdup(label);
}

/*
 * gsurf_menu_item_set_action:
 * @menu_item: a GsurfMenuItem
 * @action: (nullable): the action identifier, or %NULL
 *
 * Sets the action, freeing any previous value.
 */
void
gsurf_menu_item_set_action(
	GsurfMenuItem   *menu_item,
	const gchar     *action
){
	g_return_if_fail(menu_item != NULL);

	g_free(menu_item->action);
	menu_item->action = g_strdup(action);
}

/*
 * gsurf_menu_item_set_arg:
 * @menu_item: a GsurfMenuItem
 * @arg: (nullable): the action argument, or %NULL
 *
 * Sets the argument, freeing any previous value.
 */
void
gsurf_menu_item_set_arg(
	GsurfMenuItem   *menu_item,
	const gchar     *arg
){
	g_return_if_fail(menu_item != NULL);

	g_free(menu_item->arg);
	menu_item->arg = g_strdup(arg);
}

/*
 * gsurf_menu_item_set_is_separator:
 * @menu_item: a GsurfMenuItem
 * @is_separator: whether the item is a separator
 *
 * Sets the separator flag.
 */
void
gsurf_menu_item_set_is_separator(
	GsurfMenuItem   *menu_item,
	gboolean        is_separator
){
	g_return_if_fail(menu_item != NULL);

	menu_item->is_separator = is_separator;
}
