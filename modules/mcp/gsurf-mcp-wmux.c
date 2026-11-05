/*
 * gsurf-mcp-wmux.c - Window layout (wmux) tools
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything here goes through the #GsurfWindowLayout seams: groups (wmux
 * windows) and panes are read with gsurf_window_layout_list_groups() and
 * _list_panes(); changes are wmux command-language lines run with
 * gsurf_window_layout_run_command() (the same commands the user types at
 * the C-b : prompt), plus _rename_group() and _show_view_at() (the sidebar
 * rename and the drag-and-drop of a tab onto a pane). The module never
 * links against wmux, so any layout implementing the seams works.
 *
 * Mutating tools return the panes of the current window afterwards (the
 * list_panes result), so an agent sees the effect without another call.
 */

#include <gio/gio.h>
#include <string.h>

#include "gsurf-mcp-private.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* The active window and its layout, or FALSE with @error set. */
static gboolean
get_layout(GsurfWindow **window, GsurfWindowLayout **layout, GError **error)
{
	*window = gsurf_mcp_window(error);
	if (*window == NULL)
		return FALSE;
	*layout = gsurf_mcp_layout(*window, error);
	return *layout != NULL;
}

/* The groups of @layout; a layout without groups is one group, index 0. */
static GPtrArray *
get_groups(GsurfWindowLayout *layout, GsurfWindow *window)
{
	GPtrArray *groups = gsurf_window_layout_list_groups(layout, window);

	if (groups == NULL) {
		groups = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_layout_group_unref);
		g_ptr_array_add(groups, gsurf_layout_group_new(0, NULL, TRUE));
	}
	return groups;
}

/* The group with @index, or NULL. */
static GsurfLayoutGroup *
find_group(GPtrArray *groups, guint index)
{
	guint i;

	for (i = 0; i < groups->len; i++)
		if (gsurf_layout_group_get_index(g_ptr_array_index(groups, i)) == index)
			return g_ptr_array_index(groups, i);
	return NULL;
}

/* The index of the current group (0 when none says so). */
static guint
current_group(GsurfWindowLayout *layout, GsurfWindow *window)
{
	g_autoptr(GPtrArray) groups = get_groups(layout, window);
	guint i;

	for (i = 0; i < groups->len; i++)
		if (gsurf_layout_group_get_current(g_ptr_array_index(groups, i)))
			return gsurf_layout_group_get_index(g_ptr_array_index(groups, i));
	return 0;
}

/* A word of the command language, quoted when it needs to be (the same
 * rules as wmux's own printer: double quotes, \" \\ \n \t escapes). */
static gchar *
quote_word(const gchar *word)
{
	GString *out;
	const gchar *p;
	gboolean plain = word != NULL && *word != '\0';

	if (word == NULL)
		word = "";
	for (p = word; plain && *p != '\0'; p++)
		if (strchr(" \t\n\r'\"\\;#", *p) != NULL || (guchar)*p < 0x20)
			plain = FALSE;
	if (plain)
		return g_strdup(word);
	out = g_string_new("\"");
	for (p = word; *p != '\0'; p++) {
		switch (*p) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\n': g_string_append(out, "\\n");  break;
		case '\t': g_string_append(out, "\\t");  break;
		case '\r': g_string_append_c(out, ' ');  break;
		default:
			if ((guchar)*p >= 0x20)
				g_string_append_c(out, *p);
			break;
		}
	}
	g_string_append_c(out, '"');
	return g_string_free(out, FALSE);
}

/* Run one command line on the layout; FALSE with @error set (the layout's
 * own message, prefixed by the command) when it fails. */
static gboolean
run(GsurfWindowLayout *layout, GsurfWindow *window, const gchar *line, GError **error)
{
	g_autoptr(GError) local = NULL;

	if (gsurf_window_layout_run_command(layout, window, line, &local))
		return TRUE;
	g_set_error(error, local->domain, local->code, "%s: %s", line, local->message);
	return FALSE;
}

/* One pane object: index, id, view, uri, title, active, zoomed, visible,
 * rect (pixels) and fraction (0..1 of the pane area). */
static void
add_pane(JsonBuilder *b, GsurfWindow *window, GsurfLayoutPane *pane, gboolean group_current,
         gboolean group_zoomed)
{
	guint64 view_id = gsurf_layout_pane_get_view_id(pane);
	GsurfView *view = view_id != 0 ? gsurf_view_lookup(view_id) : NULL;
	gint x;
	gint y;
	gint w;
	gint h;
	gdouble fx;
	gdouble fy;
	gdouble fw;
	gdouble fh;

	gsurf_layout_pane_get_rect(pane, &x, &y, &w, &h);
	gsurf_layout_pane_get_fraction(pane, &fx, &fy, &fw, &fh);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "index");
	json_builder_add_int_value(b, gsurf_layout_pane_get_index(pane));
	json_builder_set_member_name(b, "id");
	json_builder_add_int_value(b, gsurf_layout_pane_get_id(pane));
	json_builder_set_member_name(b, "view");
	if (view_id != 0)
		json_builder_add_int_value(b, (gint64)view_id);
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "uri");
	json_builder_add_string_value(b, view != NULL && gsurf_view_get_uri(view) != NULL
		? gsurf_view_get_uri(view) : "");
	json_builder_set_member_name(b, "title");
	json_builder_add_string_value(b, view != NULL && gsurf_view_get_title(view) != NULL
		? gsurf_view_get_title(view) : "");
	json_builder_set_member_name(b, "active");
	json_builder_add_boolean_value(b, gsurf_layout_pane_get_active(pane));
	json_builder_set_member_name(b, "focused");
	json_builder_add_boolean_value(b, view != NULL && group_current &&
		gsurf_window_get_active_view(window) == view);
	json_builder_set_member_name(b, "zoomed");
	json_builder_add_boolean_value(b, gsurf_layout_pane_get_zoomed(pane));
	json_builder_set_member_name(b, "visible");
	json_builder_add_boolean_value(b, group_current &&
		(!group_zoomed || gsurf_layout_pane_get_zoomed(pane)));

	json_builder_set_member_name(b, "rect");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "x");
	json_builder_add_int_value(b, x);
	json_builder_set_member_name(b, "y");
	json_builder_add_int_value(b, y);
	json_builder_set_member_name(b, "width");
	json_builder_add_int_value(b, w);
	json_builder_set_member_name(b, "height");
	json_builder_add_int_value(b, h);
	json_builder_end_object(b);

	json_builder_set_member_name(b, "fraction");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "x");
	json_builder_add_double_value(b, fx);
	json_builder_set_member_name(b, "y");
	json_builder_add_double_value(b, fy);
	json_builder_set_member_name(b, "width");
	json_builder_add_double_value(b, fw);
	json_builder_set_member_name(b, "height");
	json_builder_add_double_value(b, fh);
	json_builder_end_object(b);
	json_builder_end_object(b);
}

/* "panes": the panes of @group as an array member of the open object. */
static void
add_panes(JsonBuilder *b, GsurfWindowLayout *layout, GsurfWindow *window,
          GsurfLayoutGroup *group)
{
	guint index = gsurf_layout_group_get_index(group);
	g_autoptr(GPtrArray) panes = gsurf_window_layout_list_panes(layout, window, index);
	guint i;

	json_builder_set_member_name(b, "panes");
	json_builder_begin_array(b);
	for (i = 0; panes != NULL && i < panes->len; i++)
		add_pane(b, window, g_ptr_array_index(panes, i),
			gsurf_layout_group_get_current(group), gsurf_layout_group_get_zoomed(group));
	json_builder_end_array(b);
}

/* {"window": <index>, "name": ..., "panes": [...]} for @index (or the
 * current window for -1); an error result for an unknown window. */
static McpToolResult *
panes_result(GsurfWindowLayout *layout, GsurfWindow *window, gint64 index)
{
	g_autoptr(GPtrArray) groups = get_groups(layout, window);
	g_autoptr(JsonBuilder) b = NULL;
	GsurfLayoutGroup *group;

	if (index < 0)
		index = current_group(layout, window);
	group = find_group(groups, (guint)index);
	if (group == NULL)
		return gsurf_mcp_error("unknown wmux window %" G_GINT64_FORMAT " (there are %u; "
			"list_wmux_windows gives the indices)", index, groups->len);
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "window");
	json_builder_add_int_value(b, gsurf_layout_group_get_index(group));
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b, gsurf_layout_group_get_name(group));
	json_builder_set_member_name(b, "current");
	json_builder_add_boolean_value(b, gsurf_layout_group_get_current(group));
	json_builder_set_member_name(b, "zoomed");
	json_builder_add_boolean_value(b, gsurf_layout_group_get_zoomed(group));
	add_panes(b, layout, window, group);
	json_builder_end_object(b);
	return gsurf_mcp_json(json_builder_get_root(b));
}

/*
 * select_target:
 *
 * Makes the pane a tool addresses the active pane of the current window:
 * "view" (the pane showing that view, in whatever window), or "window"
 * and/or "pane" (a pane index of that window, default the current one).
 * With none of them the active pane stays. Selecting changes the focus,
 * as the same keys would.
 *
 * Returns: FALSE with @error set for an unknown view, window or pane, or a
 *   view no pane shows
 */
static gboolean
select_target(GsurfWindowLayout *layout, GsurfWindow *window, JsonObject *args,
              GError **error)
{
	g_autofree gchar *line = NULL;
	gint64 win = -1;
	gint64 pane = -1;
	GsurfView *view;
	guint g;
	guint p;

	/* 1. A view: the pane that shows it. */
	if (gsurf_mcp_arg_has(args, "view")) {
		view = gsurf_mcp_resolve_view(window, args, "view", NULL, FALSE, error);
		if (view == NULL)
			return FALSE;
		if (!gsurf_mcp_find_pane(window, view, &g, &p)) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
				"view %" G_GUINT64_FORMAT " is not shown in any pane (a background tab; "
				"show_view_in_pane or split_pane can put it in one)",
				gsurf_view_get_id(view));
			return FALSE;
		}
		win = g;
		pane = p;
	}

	/* 2. Or a window and / or a pane index. */
	if (win < 0 && gsurf_mcp_arg_has(args, "window") &&
	    !gsurf_mcp_arg_int(args, "window", &win, error))
		return FALSE;
	if (pane < 0 && gsurf_mcp_arg_has(args, "pane") &&
	    !gsurf_mcp_arg_int(args, "pane", &pane, error))
		return FALSE;
	if (win < -1 || pane < -1) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"'window' and 'pane' must be 0 or more");
		return FALSE;
	}

	/* 3. Select them through the command language (same checks and
	 *    messages as typed commands). */
	if (win >= 0 && (guint)win != current_group(layout, window)) {
		line = g_strdup_printf("select-window -t %" G_GINT64_FORMAT, win);
		if (!run(layout, window, line, error))
			return FALSE;
		g_clear_pointer(&line, g_free);
	}
	if (pane >= 0) {
		line = g_strdup_printf("select-pane -t %" G_GINT64_FORMAT, pane);
		if (!run(layout, window, line, error))
			return FALSE;
	}
	return TRUE;
}

/* The active pane of the current window, or NULL. */
static GsurfLayoutPane *
active_pane(GsurfWindowLayout *layout, GsurfWindow *window)
{
	g_autoptr(GPtrArray) panes = gsurf_window_layout_list_panes(layout, window,
		current_group(layout, window));
	guint i;

	for (i = 0; panes != NULL && i < panes->len; i++)
		if (gsurf_layout_pane_get_active(g_ptr_array_index(panes, i)))
			return gsurf_layout_pane_ref(g_ptr_array_index(panes, i));
	return NULL;
}

/* A direction argument: left/right/up/down (or L/R/U/D); 0 when absent,
 * '?' for anything else. */
static gchar
direction_letter(const gchar *direction)
{
	if (direction == NULL)
		return 0;
	if (g_ascii_strcasecmp(direction, "left") == 0 || g_strcmp0(direction, "L") == 0)
		return 'L';
	if (g_ascii_strcasecmp(direction, "right") == 0 || g_strcmp0(direction, "R") == 0)
		return 'R';
	if (g_ascii_strcasecmp(direction, "up") == 0 || g_strcmp0(direction, "U") == 0)
		return 'U';
	if (g_ascii_strcasecmp(direction, "down") == 0 || g_strcmp0(direction, "D") == 0)
		return 'D';
	return '?';
}

/* ------------------------------------------------------------------ */
/* Status and listing                                                  */
/* ------------------------------------------------------------------ */

static McpToolResult *
tool_wmux_status(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfModule *wmux = gsurf_module_manager_get_module(gsurf_module_manager_get_default(),
		"wmux");
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());
	GsurfWindowLayout *layout = window != NULL ? gsurf_window_get_layout(window) : NULL;
	g_autoptr(GVariant) info = NULL;
	g_autoptr(JsonBuilder) b = json_builder_new();

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "loaded");
	json_builder_add_boolean_value(b, wmux != NULL);
	json_builder_set_member_name(b, "enabled");
	json_builder_add_boolean_value(b, wmux != NULL && gsurf_module_is_active(wmux));
	/* Active: a layout is installed in the active window. */
	json_builder_set_member_name(b, "active");
	json_builder_add_boolean_value(b, layout != NULL);
	json_builder_set_member_name(b, "kiosk");
	json_builder_add_boolean_value(b, gsurf_kiosk_is_enabled());

	/* What the layout says about itself: name, prefix, status, mouse,
	 * width, height, windows, current. */
	if (layout != NULL)
		info = gsurf_window_layout_describe(layout, window);
	if (info != NULL) {
		GVariantIter iter;
		const gchar *key;
		GVariant *value;

		g_variant_iter_init(&iter, info);
		while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
			json_builder_set_member_name(b, key);
			if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
				json_builder_add_string_value(b, g_variant_get_string(value, NULL));
			else if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN))
				json_builder_add_boolean_value(b, g_variant_get_boolean(value));
			else if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32))
				json_builder_add_int_value(b, g_variant_get_int32(value));
			else if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32))
				json_builder_add_int_value(b, g_variant_get_uint32(value));
			else if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT64))
				json_builder_add_int_value(b, g_variant_get_int64(value));
			else if (g_variant_is_of_type(value, G_VARIANT_TYPE_DOUBLE))
				json_builder_add_double_value(b, g_variant_get_double(value));
			else {
				g_autofree gchar *text = g_variant_print(value, FALSE);

				json_builder_add_string_value(b, text);
			}
			g_variant_unref(value);
		}
	}
	json_builder_end_object(b);
	return gsurf_mcp_json(json_builder_get_root(b));
}

static McpToolResult *
tool_list_wmux_windows(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) groups = NULL;
	g_autoptr(JsonBuilder) b = NULL;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	guint i;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	groups = get_groups(layout, window);
	b = json_builder_new();
	json_builder_begin_array(b);
	for (i = 0; i < groups->len; i++) {
		GsurfLayoutGroup *group = g_ptr_array_index(groups, i);
		guint n_ids;

		gsurf_layout_group_get_view_ids(group, &n_ids);
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "index");
		json_builder_add_int_value(b, gsurf_layout_group_get_index(group));
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, gsurf_layout_group_get_name(group));
		json_builder_set_member_name(b, "current");
		json_builder_add_boolean_value(b, gsurf_layout_group_get_current(group));
		json_builder_set_member_name(b, "last");
		json_builder_add_boolean_value(b, gsurf_layout_group_get_last(group));
		json_builder_set_member_name(b, "zoomed");
		json_builder_add_boolean_value(b, gsurf_layout_group_get_zoomed(group));
		add_panes(b, layout, window, group);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	return gsurf_mcp_json(json_builder_get_root(b));
}

static McpToolResult *
tool_list_panes(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	gint64 index = -1;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (gsurf_mcp_arg_has(args, "window") && !gsurf_mcp_arg_int(args, "window", &index, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (gsurf_mcp_arg_has(args, "window") && index < 0)
		return gsurf_mcp_error("'window' must be 0 or more");
	return panes_result(layout, window, index);
}

/* ------------------------------------------------------------------ */
/* The command language                                                */
/* ------------------------------------------------------------------ */

static McpToolResult *
tool_wmux_command(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	const gchar *command = gsurf_mcp_arg_string(args, "command");
	GsurfWindowLayout *layout;
	GsurfWindow *window;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (command == NULL || *command == '\0')
		return gsurf_mcp_error("missing 'command' (a wmux command such as "
			"\"split-window -h\"; docs/wmux.org lists them)");
	if (!gsurf_window_layout_run_command(layout, window, command, &error))
		return gsurf_mcp_gerror(error, NULL);
	return panes_result(layout, window, -1);
}

/* ------------------------------------------------------------------ */
/* Panes                                                               */
/* ------------------------------------------------------------------ */

/*
 * split_pane: split the addressed pane (select_target()) to @direction.
 * A "view" that no pane shows is put in a new pane beside the active
 * pane instead (what dropping its tab on that pane's edge does).
 */
static McpToolResult *
tool_split_pane(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) line = NULL;
	const gchar *uri = gsurf_mcp_arg_string(args, "uri");
	gboolean duplicate = FALSE;
	gboolean background = FALSE;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	GsurfView *view = NULL;
	gdouble ratio = 0.0;
	gchar dir;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (gsurf_kiosk_is_enabled())
		return gsurf_mcp_error("kiosk mode: splitting is not allowed");
	if (!gsurf_mcp_arg_bool(args, "duplicate", &duplicate, &error) ||
	    !gsurf_mcp_arg_bool(args, "background", &background, &error))
		return gsurf_mcp_gerror(error, NULL);

	/* 1. Direction (default right) and size. */
	dir = direction_letter(gsurf_mcp_arg_string(args, "direction"));
	if (dir == '?')
		return gsurf_mcp_error("'direction' must be left, right, up or down");
	if (dir == 0)
		dir = 'R';
	if (gsurf_mcp_arg_has(args, "ratio")) {
		JsonNode *node = json_object_get_member(args, "ratio");

		ratio = JSON_NODE_HOLDS_VALUE(node) &&
		        (json_node_get_value_type(node) == G_TYPE_DOUBLE ||
		         json_node_get_value_type(node) == G_TYPE_INT64)
			? json_node_get_double(node) : -1.0;
		/* A fraction (0.3) or a percentage (30). */
		if (ratio > 0.0 && ratio < 1.0)
			ratio *= 100.0;
		if (ratio < 1.0 || ratio > 99.0)
			return gsurf_mcp_error("'ratio' must be a fraction (0.05..0.95) or a "
				"percentage (5..95) of the split pane for the new pane");
	}

	/* 2. A background tab: show it in a new pane beside the active one. */
	if (gsurf_mcp_arg_has(args, "view")) {
		view = gsurf_mcp_resolve_view(window, args, "view", NULL, FALSE, &error);
		if (view == NULL)
			return gsurf_mcp_gerror(error, NULL);
		if (!gsurf_mcp_find_pane(window, view, NULL, NULL)) {
			GsurfView *target = gsurf_window_get_active_view(window);
			GsurfLayoutDropZone zone = dir == 'L' ? GSURF_LAYOUT_DROP_LEFT
				: dir == 'R' ? GSURF_LAYOUT_DROP_RIGHT
				: dir == 'U' ? GSURF_LAYOUT_DROP_TOP : GSURF_LAYOUT_DROP_BOTTOM;

			if (uri != NULL || duplicate)
				return gsurf_mcp_error("'uri' and 'duplicate' do not apply when 'view' is "
					"a background tab (it is the new pane's content)");
			if (target == NULL ||
			    !gsurf_window_layout_show_view_at(layout, window, view, target, zone))
				return gsurf_mcp_error("could not show view %" G_GUINT64_FORMAT
					" in a new pane", gsurf_view_get_id(view));
			return panes_result(layout, window, -1);
		}
	}

	/* 3. Otherwise split the addressed pane with split-window. */
	if (!select_target(layout, window, args, &error))
		return gsurf_mcp_gerror(error, NULL);
	line = g_string_new("split-window");
	g_string_append(line, (dir == 'L' || dir == 'R') ? " -h" : " -v");
	if (dir == 'L' || dir == 'U')
		g_string_append(line, " -b");
	if (background)
		g_string_append(line, " -d");
	if (duplicate)
		g_string_append(line, " -c");
	if (ratio > 0.0)
		g_string_append_printf(line, " -p %d", (gint)(ratio + 0.5));
	if (uri != NULL && *uri != '\0') {
		g_autofree gchar *quoted = quote_word(uri);

		g_string_append_printf(line, " %s", quoted);
	}
	if (!run(layout, window, line->str, &error))
		return gsurf_mcp_gerror(error, NULL);
	return panes_result(layout, window, -1);
}

static McpToolResult *
tool_select_pane(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *line = NULL;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	gboolean last = FALSE;
	gchar dir;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (!gsurf_mcp_arg_bool(args, "last", &last, &error))
		return gsurf_mcp_gerror(error, NULL);
	dir = direction_letter(gsurf_mcp_arg_string(args, "direction"));
	if (dir == '?')
		return gsurf_mcp_error("'direction' must be left, right, up or down");

	/* A direction or the last pane, else an explicit pane / view. */
	if (last) {
		if (!run(layout, window, "last-pane", &error))
			return gsurf_mcp_gerror(error, NULL);
	} else if (dir != 0) {
		line = g_strdup_printf("select-pane -%c", dir);
		if (!run(layout, window, line, &error))
			return gsurf_mcp_gerror(error, NULL);
	} else {
		if (!gsurf_mcp_arg_has(args, "pane") && !gsurf_mcp_arg_has(args, "view") &&
		    !gsurf_mcp_arg_has(args, "window"))
			return gsurf_mcp_error("give 'pane', 'view', 'direction' or last=true");
		if (!select_target(layout, window, args, &error))
			return gsurf_mcp_gerror(error, NULL);
	}
	return panes_result(layout, window, -1);
}

static McpToolResult *
tool_kill_pane(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindowLayout *layout;
	GsurfWindow *window;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (!select_target(layout, window, args, &error) ||
	    !run(layout, window, "kill-pane", &error))
		return gsurf_mcp_gerror(error, NULL);
	return panes_result(layout, window, -1);
}

static McpToolResult *
tool_zoom_pane(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfLayoutPane) pane = NULL;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	gboolean zoomed;
	gboolean want;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (!select_target(layout, window, args, &error))
		return gsurf_mcp_gerror(error, NULL);
	pane = active_pane(layout, window);
	if (pane == NULL)
		return gsurf_mcp_error("the current wmux window has no active pane");

	/* Toggle, or reach the requested state (resize-pane -Z toggles). */
	zoomed = gsurf_layout_pane_get_zoomed(pane);
	want = !zoomed;
	if (!gsurf_mcp_arg_bool(args, "zoomed", &want, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (want != zoomed && !run(layout, window, "resize-pane -Z", &error))
		return gsurf_mcp_gerror(error, NULL);
	return panes_result(layout, window, -1);
}

static McpToolResult *
tool_select_layout(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *quoted = NULL;
	const gchar *layout_name = gsurf_mcp_arg_string(args, "name");
	GsurfWindowLayout *layout;
	GsurfWindow *window;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (layout_name == NULL || *layout_name == '\0')
		return gsurf_mcp_error("missing 'name': even-horizontal, even-vertical, "
			"main-horizontal, main-vertical, tiled, next or even");
	if (g_strcmp0(layout_name, "next") == 0) {
		line = g_strdup("next-layout");
	} else if (g_strcmp0(layout_name, "even") == 0) {
		line = g_strdup("select-layout -E");
	} else {
		quoted = quote_word(layout_name);
		line = g_strdup_printf("select-layout %s", quoted);
	}
	if (!run(layout, window, line, &error))
		return gsurf_mcp_gerror(error, NULL);
	return panes_result(layout, window, -1);
}

/*
 * show_view_in_pane: the drag-and-drop of a tab onto a pane. "zone"
 * centre shows the view in the pane's place (swapping when it was in
 * another pane); an edge splits the pane and shows it on that side.
 */
static McpToolResult *
tool_show_view_in_pane(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	static const struct {
		const gchar        *name;
		GsurfLayoutDropZone zone;
	} zones[] = {
		{ "center", GSURF_LAYOUT_DROP_CENTER }, { "centre", GSURF_LAYOUT_DROP_CENTER },
		{ "left", GSURF_LAYOUT_DROP_LEFT }, { "right", GSURF_LAYOUT_DROP_RIGHT },
		{ "top", GSURF_LAYOUT_DROP_TOP }, { "bottom", GSURF_LAYOUT_DROP_BOTTOM },
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) panes = NULL;
	g_autofree gchar *line = NULL;
	const gchar *zone_name = gsurf_mcp_arg_string(args, "zone");
	GsurfLayoutDropZone zone = GSURF_LAYOUT_DROP_CENTER;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	GsurfView *view;
	GsurfView *target = NULL;
	gint64 win = -1;
	gint64 pane = -1;
	guint i;
	gboolean known = (zone_name == NULL);

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	view = gsurf_mcp_resolve_view(window, args, "view", NULL, FALSE, &error);
	if (view == NULL)
		return gsurf_mcp_gerror(error, NULL);
	for (i = 0; zone_name != NULL && i < G_N_ELEMENTS(zones); i++) {
		if (g_ascii_strcasecmp(zone_name, zones[i].name) == 0) {
			zone = zones[i].zone;
			known = TRUE;
		}
	}
	if (!known)
		return gsurf_mcp_error("'zone' must be center, left, right, top or bottom");
	if (zone != GSURF_LAYOUT_DROP_CENTER && gsurf_kiosk_is_enabled())
		return gsurf_mcp_error("kiosk mode: splitting is not allowed");

	/* 1. The target: a view on screen, or a pane of the current window
	 *    (selecting "window" first when given). */
	if (gsurf_mcp_arg_has(args, "target")) {
		target = gsurf_mcp_resolve_view(window, args, "target", NULL, FALSE, &error);
		if (target == NULL)
			return gsurf_mcp_gerror(error, NULL);
	} else {
		if ((gsurf_mcp_arg_has(args, "window") &&
		     !gsurf_mcp_arg_int(args, "window", &win, &error)) ||
		    (gsurf_mcp_arg_has(args, "pane") && !gsurf_mcp_arg_int(args, "pane", &pane, &error)))
			return gsurf_mcp_gerror(error, NULL);
		if (pane < 0)
			return gsurf_mcp_error("give 'pane' (a pane index of the current window) or "
				"'target' (a view id on screen)");
		if (win >= 0 && (guint)win != current_group(layout, window)) {
			line = g_strdup_printf("select-window -t %" G_GINT64_FORMAT, win);
			if (!run(layout, window, line, &error))
				return gsurf_mcp_gerror(error, NULL);
		}
		panes = gsurf_window_layout_list_panes(layout, window, current_group(layout, window));
		for (i = 0; panes != NULL && i < panes->len && target == NULL; i++) {
			GsurfLayoutPane *p = g_ptr_array_index(panes, i);

			if ((gint64)gsurf_layout_pane_get_index(p) == pane &&
			    gsurf_layout_pane_get_view_id(p) != 0)
				target = gsurf_view_lookup(gsurf_layout_pane_get_view_id(p));
		}
		if (target == NULL)
			return gsurf_mcp_error("no pane %" G_GINT64_FORMAT " in the current wmux window "
				"(list_panes gives the indices)", pane);
	}
	if (!gsurf_window_get_view_visible(window, target))
		return gsurf_mcp_error("view %" G_GUINT64_FORMAT " is not on screen: the target "
			"must be shown in a pane of the current wmux window", gsurf_view_get_id(target));

	/* 2. The drop. */
	if (!gsurf_window_layout_show_view_at(layout, window, view, target, zone))
		return gsurf_mcp_error("could not show view %" G_GUINT64_FORMAT " at view %"
			G_GUINT64_FORMAT " (a pane's own view only accepts the center zone, and a "
			"split must leave both panes above the minimum size)",
			gsurf_view_get_id(view), gsurf_view_get_id(target));
	return panes_result(layout, window, -1);
}

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

static McpToolResult *
tool_select_window(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *line = NULL;
	const gchar *which = gsurf_mcp_arg_string(args, "which");
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	gint64 index;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (gsurf_mcp_arg_has(args, "index")) {
		if (!gsurf_mcp_arg_int(args, "index", &index, &error))
			return gsurf_mcp_gerror(error, NULL);
		line = g_strdup_printf("select-window -t %" G_GINT64_FORMAT, index);
	} else if (g_strcmp0(which, "next") == 0) {
		line = g_strdup("next-window");
	} else if (g_strcmp0(which, "previous") == 0) {
		line = g_strdup("previous-window");
	} else if (g_strcmp0(which, "last") == 0) {
		line = g_strdup("last-window");
	} else {
		return gsurf_mcp_error("give 'index' or 'which' (next, previous or last)");
	}
	if (!run(layout, window, line, &error))
		return gsurf_mcp_gerror(error, NULL);
	return panes_result(layout, window, -1);
}

static McpToolResult *
tool_new_window(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) line = NULL;
	const gchar *win_name = gsurf_mcp_arg_string(args, "name");
	const gchar *uri = gsurf_mcp_arg_string(args, "uri");
	GsurfWindowLayout *layout;
	GsurfWindow *window;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (gsurf_kiosk_is_enabled())
		return gsurf_mcp_error("kiosk mode: opening views is not allowed");
	line = g_string_new("new-window");
	if (win_name != NULL && *win_name != '\0') {
		g_autofree gchar *quoted = quote_word(win_name);

		g_string_append_printf(line, " -n %s", quoted);
	}
	if (uri != NULL && *uri != '\0') {
		g_autofree gchar *quoted = quote_word(uri);

		g_string_append_printf(line, " %s", quoted);
	}
	if (!run(layout, window, line->str, &error))
		return gsurf_mcp_gerror(error, NULL);
	return panes_result(layout, window, -1);
}

static McpToolResult *
tool_rename_window(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	const gchar *win_name = gsurf_mcp_arg_string(args, "name");
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	gint64 index = -1;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (gsurf_mcp_arg_has(args, "index") && !gsurf_mcp_arg_int(args, "index", &index, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (!gsurf_mcp_arg_has(args, "name"))
		return gsurf_mcp_error("missing 'name' (\"\" restores automatic naming)");
	if (index < 0)
		index = current_group(layout, window);
	if (!gsurf_window_layout_rename_group(layout, window, (guint)index, win_name))
		return gsurf_mcp_error("unknown wmux window %" G_GINT64_FORMAT
			" (list_wmux_windows gives the indices)", index);
	return panes_result(layout, window, index);
}

static McpToolResult *
tool_kill_window(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *line = NULL;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	gint64 index = -1;

	if (!get_layout(&window, &layout, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (gsurf_mcp_arg_has(args, "index")) {
		if (!gsurf_mcp_arg_int(args, "index", &index, &error))
			return gsurf_mcp_gerror(error, NULL);
		line = g_strdup_printf("select-window -t %" G_GINT64_FORMAT, index);
		if (index != (gint64)current_group(layout, window) &&
		    !run(layout, window, line, &error))
			return gsurf_mcp_gerror(error, NULL);
	}
	if (!run(layout, window, "kill-window", &error))
		return gsurf_mcp_gerror(error, NULL);
	return panes_result(layout, window, -1);
}

/* ------------------------------------------------------------------ */
/* Table                                                               */
/* ------------------------------------------------------------------ */

#define PANES_RETURN "Returns the current wmux window's panes afterwards (as list_panes)."
#define PANE_ARGS \
	{ "pane", "integer", "pane index in the wmux window (list_panes 'index'); default: the active pane", FALSE }, \
	{ "view", "integer", "address the pane by the view id it shows (any wmux window)", FALSE }, \
	{ "window", "integer", "wmux window index for 'pane' (default: the current one)", FALSE }

const GsurfMcpToolDef gsurf_mcp_wmux_tools[] = {
	{ "wmux_status",
	  "Whether the wmux pane multiplexer runs and how it is set up: loaded, enabled (module "
	  "active), active (installed in the window), kiosk, and from the layout: name, prefix "
	  "(prefix key, e.g. Ctrl+b), status, mouse, width/height (pane area, px), windows, current.",
	  {{ NULL, NULL, NULL, FALSE }}, tool_wmux_status },
	{ "list_wmux_windows",
	  "List the wmux windows (tmux-style windows, each its own pane arrangement) as a JSON "
	  "array: index, name, current, last (previous window), zoomed, panes (as list_panes).",
	  {{ NULL, NULL, NULL, FALSE }}, tool_list_wmux_windows },
	{ "list_panes",
	  "List the panes of a wmux window: {window, name, current, zoomed, panes: [{index, id, "
	  "view (view id or null), uri, title, active (the window's active pane), focused, zoomed, "
	  "visible (on screen now), rect {x,y,width,height} in px of the pane area, fraction "
	  "{x,y,width,height} in 0..1}]}. Pane index is what 'pane' means in the other tools.",
	  {{ "window", "integer", "wmux window index (default: the current one)", FALSE }},
	  tool_list_panes },
	{ "wmux_command",
	  "Run any wmux command line (tmux syntax, as typed at the C-b : prompt), e.g. "
	  "\"split-window -h -p 30 https://example.org\", \"select-pane -t 1\", \"resize-pane -L 5\", "
	  "\"swap-pane -D\", \"rotate-window\", \"break-pane\", \"select-layout tiled\", "
	  "\"new-window -n docs\", \"rename-window x\", \"kill-window\". Parse or execution errors "
	  "are returned as the error text. " PANES_RETURN,
	  {{ "command", "string", "one wmux command line", TRUE }},
	  tool_wmux_command },
	{ "split_pane",
	  "Split a pane, showing a new view (or a copy of the pane's page with duplicate=true) in "
	  "the new pane, which becomes active unless background=true. Address the pane with 'pane' "
	  "/ 'window' or 'view' (default: the active pane). A 'view' that no pane shows (a "
	  "background tab) is instead put in a new pane beside the active pane. " PANES_RETURN,
	  {{ "direction", "string", "where the new pane goes: right (default), left, down, up", FALSE },
	   { "uri", "string", "what the new pane loads (default: the new-view URI)", FALSE },
	   { "duplicate", "boolean", "the new pane shows a copy of the split pane's page", FALSE },
	   { "ratio", "number", "the new pane's share: fraction 0.05-0.95 or percent 5-95", FALSE },
	   { "background", "boolean", "keep the split pane active (split-window -d)", FALSE },
	   PANE_ARGS },
	  tool_split_pane },
	{ "select_pane",
	  "Make a pane active (focus its view). Give 'pane' (index) / 'window', 'view', a "
	  "'direction' (left/right/up/down neighbour) or last=true (the previous pane). "
	  PANES_RETURN,
	  {{ "direction", "string", "left, right, up or down", FALSE },
	   { "last", "boolean", "select the previously active pane", FALSE },
	   PANE_ARGS },
	  tool_select_pane },
	{ "kill_pane",
	  "Close a pane (default: the active one). Its view closes too unless wmux's "
	  "on_pane_kill is keep_tab; the last pane of a window closes the window. " PANES_RETURN,
	  { PANE_ARGS }, tool_kill_pane },
	{ "zoom_pane",
	  "Zoom a pane to fill its wmux window, or unzoom (resize-pane -Z). Without 'zoomed' it "
	  "toggles. The pane becomes active. " PANES_RETURN,
	  {{ "zoomed", "boolean", "the wanted state (default: toggle)", FALSE }, PANE_ARGS },
	  tool_zoom_pane },
	{ "select_layout",
	  "Rearrange the current wmux window's panes: even-horizontal, even-vertical, "
	  "main-horizontal, main-vertical, tiled, next (cycle) or even (spread the active pane's "
	  "split evenly). " PANES_RETURN,
	  {{ "name", "string", "the layout name", TRUE }},
	  tool_select_layout },
	{ "show_view_in_pane",
	  "Show a view in a pane, like dragging its tab onto the pane: zone center (default) puts "
	  "it in the pane's place (swapping places when it was in another pane, else the old view "
	  "becomes a background tab); left/right/top/bottom split the pane and show it on that "
	  "side. Target with 'pane' (+ 'window') or 'target' (a view id on screen). " PANES_RETURN,
	  {{ "view", "integer", "the view id to show", TRUE },
	   { "pane", "integer", "the target pane index", FALSE },
	   { "window", "integer", "the target pane's wmux window (selected first)", FALSE },
	   { "target", "integer", "the target as the view id it shows (must be on screen)", FALSE },
	   { "zone", "string", "center (default), left, right, top or bottom", FALSE }},
	  tool_show_view_in_pane },
	{ "select_window",
	  "Switch the current wmux window: 'index', or 'which' = next, previous or last. "
	  PANES_RETURN,
	  {{ "index", "integer", "the wmux window index", FALSE },
	   { "which", "string", "next, previous or last", FALSE }},
	  tool_select_window },
	{ "new_window",
	  "Create a wmux window with one pane showing a new view, and switch to it. Refused in "
	  "kiosk mode. " PANES_RETURN,
	  {{ "name", "string", "its name (default: follows its page title)", FALSE },
	   { "uri", "string", "what its pane loads (default: the new-view URI)", FALSE }},
	  tool_new_window },
	{ "rename_window",
	  "Rename a wmux window (status line, sidebar heading, saved session). An empty name "
	  "restores automatic naming. Returns that window's panes (as list_panes).",
	  {{ "index", "integer", "the wmux window index (default: the current one)", FALSE },
	   { "name", "string", "the new name, \"\" for automatic", TRUE }},
	  tool_rename_window },
	{ "kill_window",
	  "Close a wmux window (default: the current one) and its panes' views (unless "
	  "on_pane_kill is keep_tab). The last window is replaced by a fresh one. " PANES_RETURN,
	  {{ "index", "integer", "the wmux window index", FALSE }},
	  tool_kill_window },
	{ NULL, NULL, {{ NULL, NULL, NULL, FALSE }}, NULL }
};
