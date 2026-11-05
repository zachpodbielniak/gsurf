/*
 * gsurf-mcp-util.c - Result, argument and view helpers of the mcp module
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Shared by every tool file: building results, reading arguments
 * leniently (models send 3, 3.0 and "3" alike), addressing views by
 * their stable id, and describing a view as JSON.
 */

#include <gio/gio.h>
#include <string.h>

#include "gsurf-mcp-private.h"

/* ------------------------------------------------------------------ */
/* Results                                                             */
/* ------------------------------------------------------------------ */

McpToolResult *
gsurf_mcp_text(const gchar *text)
{
	McpToolResult *r = mcp_tool_result_new(FALSE);

	mcp_tool_result_add_text(r, text != NULL ? text : "");
	return r;
}

McpToolResult *
gsurf_mcp_error(const gchar *format, ...)
{
	McpToolResult *r = mcp_tool_result_new(TRUE);
	g_autofree gchar *text = NULL;
	va_list ap;

	va_start(ap, format);
	text = g_strdup_vprintf(format, ap);
	va_end(ap);
	mcp_tool_result_add_text(r, text);
	return r;
}

McpToolResult *
gsurf_mcp_gerror(const GError *error, const gchar *fallback)
{
	return gsurf_mcp_error("%s", error != NULL ? error->message
		: (fallback != NULL ? fallback : "error"));
}

McpToolResult *
gsurf_mcp_json(JsonNode *node)
{
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autofree gchar *text = NULL;

	json_generator_set_root(gen, node);
	text = json_generator_to_data(gen, NULL);
	json_node_unref(node);
	return gsurf_mcp_text(text);
}

/* ------------------------------------------------------------------ */
/* Arguments                                                           */
/* ------------------------------------------------------------------ */

/* The member @name of @args when present and not JSON null. */
static JsonNode *
member(JsonObject *args, const gchar *name)
{
	JsonNode *node;

	if (args == NULL || !json_object_has_member(args, name))
		return NULL;
	node = json_object_get_member(args, name);
	return (node == NULL || JSON_NODE_HOLDS_NULL(node)) ? NULL : node;
}

gboolean
gsurf_mcp_arg_has(JsonObject *args, const gchar *name)
{
	return member(args, name) != NULL;
}

const gchar *
gsurf_mcp_arg_string(JsonObject *args, const gchar *name)
{
	JsonNode *node = member(args, name);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}

gboolean
gsurf_mcp_arg_int(JsonObject *args, const gchar *name, gint64 *out, GError **error)
{
	JsonNode *node = member(args, name);
	GType type;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"'%s' must be an integer", name);
		return FALSE;
	}
	type = json_node_get_value_type(node);

	/* 1. A JSON integer. */
	if (type == G_TYPE_INT64) {
		*out = json_node_get_int(node);
		return TRUE;
	}

	/* 2. A number without a fraction (3.0). */
	if (type == G_TYPE_DOUBLE) {
		gdouble d = json_node_get_double(node);

		if (d == (gdouble)(gint64)d) {
			*out = (gint64)d;
			return TRUE;
		}
	}

	/* 3. A string of digits ("3", "-1"). */
	if (type == G_TYPE_STRING) {
		const gchar *text = json_node_get_string(node);
		gchar *end = NULL;
		gint64 v;

		if (text != NULL && *text != '\0') {
			v = g_ascii_strtoll(text, &end, 10);
			if (end != NULL && *end == '\0') {
				*out = v;
				return TRUE;
			}
		}
	}
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		"'%s' must be an integer", name);
	return FALSE;
}

gboolean
gsurf_mcp_arg_bool(JsonObject *args, const gchar *name, gboolean *out, GError **error)
{
	JsonNode *node = member(args, name);
	GType type;

	if (node == NULL)
		return TRUE;
	if (JSON_NODE_HOLDS_VALUE(node)) {
		type = json_node_get_value_type(node);
		if (type == G_TYPE_BOOLEAN) {
			*out = json_node_get_boolean(node);
			return TRUE;
		}
		if (type == G_TYPE_STRING) {
			const gchar *text = json_node_get_string(node);

			if (g_strcmp0(text, "true") == 0 || g_strcmp0(text, "1") == 0) {
				*out = TRUE;
				return TRUE;
			}
			if (g_strcmp0(text, "false") == 0 || g_strcmp0(text, "0") == 0) {
				*out = FALSE;
				return TRUE;
			}
		}
		if (type == G_TYPE_INT64) {
			*out = json_node_get_int(node) != 0;
			return TRUE;
		}
	}
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		"'%s' must be a boolean", name);
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* Windows and views                                                   */
/* ------------------------------------------------------------------ */

GsurfWindow *
gsurf_mcp_window(GError **error)
{
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());

	if (window == NULL)
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no active window");
	return window;
}

GsurfView *
gsurf_mcp_view_by_id(GsurfWindow *window, gint64 id, GError **error)
{
	GsurfView *view = id > 0 ? gsurf_view_lookup((guint64)id) : NULL;

	if (view == NULL || gsurf_window_get_view_index(window, view) < 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"unknown view %" G_GINT64_FORMAT " (no such view in the active window; "
			"list_views gives the ids)", id);
		return NULL;
	}
	return view;
}

GsurfView *
gsurf_mcp_resolve_view(GsurfWindow *window, JsonObject *args, const gchar *id_name,
                       const gchar *index_name, gboolean default_active, GError **error)
{
	gint64 value;
	guint n;

	/* 1. A stable id. */
	if (gsurf_mcp_arg_has(args, id_name)) {
		if (!gsurf_mcp_arg_int(args, id_name, &value, error))
			return NULL;
		return gsurf_mcp_view_by_id(window, value, error);
	}

	/* 2. A position in the window (the original tools' "index"). */
	if (index_name != NULL && gsurf_mcp_arg_has(args, index_name)) {
		if (!gsurf_mcp_arg_int(args, index_name, &value, error))
			return NULL;
		n = gsurf_window_get_n_views(window);
		if (value < 0 || value >= (gint64)n) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
				"index %" G_GINT64_FORMAT " out of range (the window has %u views)",
				value, n);
			return NULL;
		}
		return gsurf_window_get_nth_view(window, (guint)value);
	}

	/* 3. The active view, when the tool allows it. */
	if (default_active && gsurf_window_get_active_view(window) != NULL)
		return gsurf_window_get_active_view(window);
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "missing '%s'%s%s%s", id_name,
		index_name != NULL ? " (or '" : "", index_name != NULL ? index_name : "",
		index_name != NULL ? "')" : "");
	return NULL;
}

GsurfView *
gsurf_mcp_target_view(JsonObject *args, GError **error)
{
	GsurfWindow *window = gsurf_mcp_window(error);
	GsurfView *view;

	if (window == NULL)
		return NULL;
	view = gsurf_mcp_resolve_view(window, args, "view", "index", TRUE, error);
	if (view == NULL && error != NULL && *error == NULL)
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no active view");
	return view;
}

GsurfWindowLayout *
gsurf_mcp_layout(GsurfWindow *window, GError **error)
{
	GsurfWindowLayout *layout = gsurf_window_get_layout(window);

	if (layout == NULL && gsurf_kiosk_is_enabled())
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			"kiosk mode shows a single view: wmux stays inactive");
	else if (layout == NULL)
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"no window layout is installed: enable the wmux module "
			"(modules.wmux.enabled: true) to get panes and windows");
	return layout;
}

GsurfTabTreeProvider *
gsurf_mcp_tree_provider(void)
{
	GPtrArray *modules = gsurf_module_manager_get_modules(gsurf_module_manager_get_default());
	guint i;

	for (i = 0; modules != NULL && i < modules->len; i++) {
		GsurfModule *module = g_ptr_array_index(modules, i);

		if (gsurf_module_is_active(module) && GSURF_IS_TAB_TREE_PROVIDER(module))
			return GSURF_TAB_TREE_PROVIDER(module);
	}
	return NULL;
}

guint64
gsurf_mcp_tree_parent(GsurfWindow *window, GsurfView *view)
{
	GsurfTabTreeProvider *provider = gsurf_mcp_tree_provider();
	g_autoptr(GArray) stack = NULL;
	guint n;
	guint i;

	if (provider != NULL)
		return gsurf_tab_tree_provider_get_parent(provider, window, view);

	/* Without the tabs module: walk the window order with the path from
	 * a root to the previous tab; a tab whose opener is on that path is
	 * its child, any other tab starts a new root (docs/tabs.org). */
	stack = g_array_new(FALSE, FALSE, sizeof(guint64));
	n = gsurf_window_get_n_views(window);
	for (i = 0; i < n; i++) {
		GsurfView *v = gsurf_window_get_nth_view(window, i);
		guint64 id = gsurf_view_get_id(v);
		guint64 opener = gsurf_view_get_opener_id(v);
		guint64 parent = 0;
		gint j;

		for (j = (gint)stack->len - 1; opener != 0 && j >= 0; j--) {
			if (g_array_index(stack, guint64, j) == opener) {
				parent = opener;
				break;
			}
		}
		if (parent != 0)
			g_array_set_size(stack, (guint)j + 1);
		else
			g_array_set_size(stack, 0);
		g_array_append_val(stack, id);
		if (v == view)
			return parent;
	}
	return 0;
}

gboolean
gsurf_mcp_find_pane(GsurfWindow *window, GsurfView *view, guint *group, guint *pane)
{
	GsurfWindowLayout *layout = gsurf_window_get_layout(window);
	g_autoptr(GPtrArray) groups = NULL;
	guint64 id = gsurf_view_get_id(view);
	guint n_groups;
	guint g;
	guint i;

	if (layout == NULL)
		return FALSE;
	groups = gsurf_window_layout_list_groups(layout, window);
	n_groups = groups != NULL ? groups->len : 1;
	for (g = 0; g < n_groups; g++) {
		guint index = groups != NULL
			? gsurf_layout_group_get_index(g_ptr_array_index(groups, g)) : 0;
		g_autoptr(GPtrArray) panes = gsurf_window_layout_list_panes(layout, window, index);

		for (i = 0; panes != NULL && i < panes->len; i++) {
			GsurfLayoutPane *p = g_ptr_array_index(panes, i);

			if (gsurf_layout_pane_get_view_id(p) == id) {
				if (group != NULL)
					*group = index;
				if (pane != NULL)
					*pane = gsurf_layout_pane_get_index(p);
				return TRUE;
			}
		}
	}
	return FALSE;
}

void
gsurf_mcp_add_view_members(JsonBuilder *b, GsurfWindow *window, GsurfView *view, guint index)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	const gchar *container_id = gsurf_view_get_container_id(view);
	GsurfContainer *container = NULL;
	const gchar *uri = gsurf_view_get_uri(view);
	const gchar *title = gsurf_view_get_title(view);
	guint group;
	guint pane;

	/* 1. Identity and page. */
	json_builder_set_member_name(b, "index");
	json_builder_add_int_value(b, index);
	json_builder_set_member_name(b, "id");
	json_builder_add_int_value(b, (gint64)gsurf_view_get_id(view));
	json_builder_set_member_name(b, "uri");
	json_builder_add_string_value(b, uri != NULL ? uri : "");
	json_builder_set_member_name(b, "title");
	json_builder_add_string_value(b, title != NULL ? title : "");

	/* 2. Focus and visibility: active is the focused view, visible is
	 *    "on screen" (any pane of the current wmux window). */
	json_builder_set_member_name(b, "active");
	json_builder_add_boolean_value(b, view == gsurf_window_get_active_view(window));
	json_builder_set_member_name(b, "visible");
	json_builder_add_boolean_value(b, gsurf_window_get_view_visible(window, view));

	/* 3. Tree: the opener (fixed) and the tree parent (may differ after
	 *    closes, promotions and moves). */
	json_builder_set_member_name(b, "opener_id");
	json_builder_add_int_value(b, (gint64)gsurf_view_get_opener_id(view));
	json_builder_set_member_name(b, "parent_id");
	json_builder_add_int_value(b, (gint64)gsurf_mcp_tree_parent(window, view));

	/* 4. Container (null = default) and its decoration. */
	if (container_id != NULL && registry != NULL)
		container = gsurf_container_registry_lookup(registry, container_id);
	json_builder_set_member_name(b, "container");
	if (container_id != NULL)
		json_builder_add_string_value(b, container_id);
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "container_name");
	if (container != NULL)
		json_builder_add_string_value(b, gsurf_container_get_name(container));
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "container_color");
	if (container != NULL && gsurf_container_get_color(container) != NULL)
		json_builder_add_string_value(b, gsurf_container_get_color(container));
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "container_temporary");
	json_builder_add_boolean_value(b, container != NULL &&
		gsurf_container_get_temporary(container));

	/* 5. Pane (with a window layout): which group and pane show it. */
	json_builder_set_member_name(b, "pane");
	if (gsurf_mcp_find_pane(window, view, &group, &pane)) {
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "window");
		json_builder_add_int_value(b, group);
		json_builder_set_member_name(b, "index");
		json_builder_add_int_value(b, pane);
		json_builder_end_object(b);
	} else {
		json_builder_add_null_value(b);
	}
}
