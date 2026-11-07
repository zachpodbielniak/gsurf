/*
 * gsurf-mcp-views.c - View, tab tree, container, mode and action tools
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Views are addressed by their stable id (gsurf_view_get_id(), "view" in
 * every tool); the original tools keep their positional "index". The tab
 * tree comes from the active #GsurfTabTreeProvider (the tabs module) and,
 * without one, from the views' openers. Containers come from the default
 * #GsurfContainerRegistry plus the containers module's configured host
 * rules. Modes and actions go through the module manager
 * (gsurf_module_manager_get_mode(), gsurf_module_manager_execute_action()),
 * so kiosk mode refuses here exactly what it refuses on the keyboard.
 */

#include <gio/gio.h>
#include <yaml-glib.h>

#include "gsurf-mcp-private.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* A view object ({index, id, uri, ...}) for @view of @window. */
static JsonNode *
view_node(GsurfWindow *window, GsurfView *view)
{
	g_autoptr(JsonBuilder) b = json_builder_new();

	json_builder_begin_object(b);
	gsurf_mcp_add_view_members(b, window, view,
		(guint)MAX(gsurf_window_get_view_index(window, view), 0));
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

/* What a new view loads when the caller gives no URI. */
static const gchar *
default_uri(const gchar *uri)
{
	GsurfConfig *config = gsurf_config_get_default();

	if (uri != NULL && *uri != '\0')
		return uri;
	return (config != NULL && config->new_view_uri != NULL) ? config->new_view_uri
		: "about:blank";
}

/* Kiosk mode shows one view: tools that open or reopen views refuse. */
static gboolean
kiosk_refuses(GError **error)
{
	if (!gsurf_kiosk_is_enabled())
		return FALSE;
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
		"kiosk mode: opening or reopening views is not allowed");
	return TRUE;
}

/*
 * open_in_window:
 * @opener: (nullable): the view it is opened from (tree parent)
 * @container: (nullable): a container id, "temporary" or "default"; NULL
 *   lets the containers policy place it
 * @activate: make it the active view
 *
 * A new view like the host's own tab paths: settings applied, added to
 * the window (the host wires it up on view-added; the tabs module places
 * a child after its opener's subtree), activated or left behind, loaded.
 */
static GsurfView *
open_in_window(GsurfWindow *window, GsurfView *opener, const gchar *container,
               const gchar *uri, gboolean activate, GError **error)
{
	GsurfConfig *config = gsurf_config_get_default();
	const gchar *load = default_uri(uri);
	g_autoptr(GsurfView) view = NULL;

	view = gsurf_view_new_full(opener, container, load, FALSE);
	if (view == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "could not create a view");
		return NULL;
	}
	if (config != NULL && config->settings != NULL)
		gsurf_view_apply_settings(view, config->settings);
	gsurf_window_add_view(window, view);
	if (activate)
		gsurf_window_set_active_view(window, view);
	gsurf_view_load_uri(view, load);
	/* The window holds it now. */
	return view;
}

/* A container argument: an id from the registry, "temporary" or
 * "default"; anything else is an error naming list_containers. */
static gboolean
check_container(const gchar *container, GError **error)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();

	if (g_strcmp0(container, GSURF_CONTAINER_TEMPORARY) == 0 ||
	    g_strcmp0(container, GSURF_CONTAINER_DEFAULT) == 0)
		return TRUE;
	if (container != NULL && registry != NULL &&
	    gsurf_container_registry_lookup(registry, container) != NULL)
		return TRUE;
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		"unknown container '%s' (list_containers gives the ids; \"temporary\" and "
		"\"default\" are always accepted)", container != NULL ? container : "");
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* Views                                                               */
/* ------------------------------------------------------------------ */

static McpToolResult *
tool_list_views(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindow *window = gsurf_mcp_window(&error);
	g_autoptr(JsonBuilder) b = NULL;
	guint i;
	guint n;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	b = json_builder_new();
	json_builder_begin_array(b);
	n = gsurf_window_get_n_views(window);
	for (i = 0; i < n; i++) {
		json_builder_begin_object(b);
		gsurf_mcp_add_view_members(b, window, gsurf_window_get_nth_view(window, i), i);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	return gsurf_mcp_json(json_builder_get_root(b));
}

static McpToolResult *
tool_switch_view(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindow *window = gsurf_mcp_window(&error);
	GsurfView *view;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	view = gsurf_mcp_resolve_view(window, args, "view", "index", FALSE, &error);
	if (view == NULL)
		return gsurf_mcp_gerror(error, NULL);
	gsurf_window_set_active_view(window, view);
	return gsurf_mcp_text("ok");
}

static McpToolResult *
tool_open_view(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindow *window = gsurf_mcp_window(&error);
	const gchar *container = gsurf_mcp_arg_string(args, "container");
	g_autofree gchar *msg = NULL;
	GsurfView *view;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	if (kiosk_refuses(&error))
		return gsurf_mcp_gerror(error, NULL);

	/* "container": a container id, "temporary" or "default"; the
	 * registry (and the containers module's policy) has the final say. */
	view = open_in_window(window, NULL, container, gsurf_mcp_arg_string(args, "uri"),
		TRUE, &error);
	if (view == NULL)
		return gsurf_mcp_gerror(error, NULL);
	msg = g_strdup_printf("%d", gsurf_window_get_view_index(window, view));
	return gsurf_mcp_text(msg);
}

static McpToolResult *
tool_close_view(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindow *window = gsurf_mcp_window(&error);
	GsurfView *view;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	view = gsurf_mcp_resolve_view(window, args, "view", "index", FALSE, &error);
	if (view == NULL)
		return gsurf_mcp_gerror(error, NULL);
	if (gsurf_window_get_n_views(window) <= 1)
		return gsurf_mcp_error("cannot close the last view");
	gsurf_window_remove_view(window, view);
	return gsurf_mcp_text("ok");
}

static McpToolResult *
tool_open_child_view(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindow *window = gsurf_mcp_window(&error);
	const gchar *container = gsurf_mcp_arg_string(args, "container");
	gboolean background = FALSE;
	GsurfView *parent;
	GsurfView *view;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	if (kiosk_refuses(&error))
		return gsurf_mcp_gerror(error, NULL);
	parent = gsurf_mcp_resolve_view(window, args, "parent", NULL, TRUE, &error);
	if (parent == NULL || !gsurf_mcp_arg_bool(args, "background", &background, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (container != NULL && !check_container(container, &error))
		return gsurf_mcp_gerror(error, NULL);

	view = open_in_window(window, parent, container, gsurf_mcp_arg_string(args, "uri"),
		!background, &error);
	if (view == NULL)
		return gsurf_mcp_gerror(error, NULL);
	return gsurf_mcp_json(view_node(window, view));
}

/*
 * move_view: exactly one of parent (re-parent as its last child; 0 makes
 * a root at the end), before / after (a sibling of that view) or index
 * (a window position; the tree re-derives from the order). parent,
 * before and after move the whole subtree and need the tabs module for
 * a real tree; before / after and index also work without it.
 */
static McpToolResult *
tool_move_view(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	static const gchar *const places[] = { "parent", "before", "after", "index" };
	g_autoptr(GError) error = NULL;
	GsurfWindow *window = gsurf_mcp_window(&error);
	GsurfTabTreeProvider *provider = gsurf_mcp_tree_provider();
	GsurfView *view;
	GsurfView *target = NULL;
	const gchar *place = NULL;
	gint64 value = 0;
	guint i;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	view = gsurf_mcp_resolve_view(window, args, "view", NULL, FALSE, &error);
	if (view == NULL)
		return gsurf_mcp_gerror(error, NULL);

	/* 1. Exactly one destination. */
	for (i = 0; i < G_N_ELEMENTS(places); i++) {
		if (!gsurf_mcp_arg_has(args, places[i]))
			continue;
		if (place != NULL)
			return gsurf_mcp_error("give only one of 'parent', 'before', 'after' "
				"and 'index' (got '%s' and '%s')", place, places[i]);
		place = places[i];
	}
	if (place == NULL)
		return gsurf_mcp_error("missing destination: give 'parent', 'before', "
			"'after' or 'index'");
	if (!gsurf_mcp_arg_int(args, place, &value, &error))
		return gsurf_mcp_gerror(error, NULL);

	/* 2. A window position: plain reorder. */
	if (g_strcmp0(place, "index") == 0) {
		if (value < 0)
			return gsurf_mcp_error("'index' must be 0 or more");
		gsurf_window_move_view(window, view, (guint)value);
		return gsurf_mcp_json(view_node(window, view));
	}

	/* 3. Relative to another view (parent 0: a root at the end). */
	if (!(g_strcmp0(place, "parent") == 0 && value == 0)) {
		target = gsurf_mcp_view_by_id(window, value, &error);
		if (target == NULL)
			return gsurf_mcp_gerror(error, NULL);
		if (target == view)
			return gsurf_mcp_error("cannot move view %" G_GUINT64_FORMAT
				" relative to itself", gsurf_view_get_id(view));
	}
	if (provider != NULL) {
		GsurfTabTreePosition position = g_strcmp0(place, "before") == 0
			? GSURF_TAB_TREE_POSITION_BEFORE
			: g_strcmp0(place, "after") == 0 ? GSURF_TAB_TREE_POSITION_AFTER
			: GSURF_TAB_TREE_POSITION_CHILD;

		if (!gsurf_tab_tree_provider_move_view(provider, window, view, target, position)) {
			g_autofree gchar *where = target != NULL
				? g_strdup_printf("view %" G_GUINT64_FORMAT, gsurf_view_get_id(target))
				: g_strdup("the end");

			return gsurf_mcp_error("cannot move view %" G_GUINT64_FORMAT " %s %s "
				"(a view cannot move into its own subtree)", gsurf_view_get_id(view),
				g_strcmp0(place, "parent") == 0 ? "under" : place, where);
		}
		return gsurf_mcp_json(view_node(window, view));
	}

	/* 4. No tree: only sibling moves, as window reorders. */
	if (g_strcmp0(place, "parent") == 0)
		return gsurf_mcp_error("re-parenting needs a tab tree: enable the tabs module "
			"(modules.tabs.enabled: true)");
	{
		gint from = gsurf_window_get_view_index(window, view);
		gint to = gsurf_window_get_view_index(window, target);

		if (g_strcmp0(place, "before") == 0)
			to = from < to ? to - 1 : to;
		else
			to = from < to ? to : to + 1;
		gsurf_window_move_view(window, view, (guint)MAX(to, 0));
	}
	return gsurf_mcp_json(view_node(window, view));
}

static McpToolResult *
tool_set_collapsed(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindow *window = gsurf_mcp_window(&error);
	GsurfTabTreeProvider *provider = gsurf_mcp_tree_provider();
	gboolean collapsed = TRUE;
	GsurfView *view;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	view = gsurf_mcp_resolve_view(window, args, "view", NULL, FALSE, &error);
	if (view == NULL || !gsurf_mcp_arg_bool(args, "collapsed", &collapsed, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (provider == NULL)
		return gsurf_mcp_error("collapsing needs a tab tree: enable the tabs module "
			"(modules.tabs.enabled: true)");
	if (!gsurf_tab_tree_provider_set_collapsed(provider, window, view, collapsed))
		return gsurf_mcp_error("view %" G_GUINT64_FORMAT " has no children to %s",
			gsurf_view_get_id(view), collapsed ? "collapse" : "expand");
	return gsurf_mcp_json(view_node(window, view));
}

/* ------------------------------------------------------------------ */
/* Tab tree                                                            */
/* ------------------------------------------------------------------ */

/* The parent of every view of the window, in window order. */
typedef struct {
	GsurfWindow          *window;
	GsurfTabTreeProvider *provider;
	GPtrArray            *views;    /* GsurfView*, borrowed, window order */
	GArray               *parents;  /* guint64, parallel to views */
	GHashTable           *in_pane;  /* view ids shown by a pane (any group) */
} Tree;

static void
tree_clear(Tree *t)
{
	g_clear_pointer(&t->views, g_ptr_array_unref);
	g_clear_pointer(&t->parents, g_array_unref);
	g_clear_pointer(&t->in_pane, g_hash_table_unref);
}

static void
tree_init(Tree *t, GsurfWindow *window)
{
	guint i;
	guint n = gsurf_window_get_n_views(window);

	t->window = window;
	t->provider = gsurf_mcp_tree_provider();
	t->views = g_ptr_array_new();
	t->parents = g_array_new(FALSE, FALSE, sizeof(guint64));
	t->in_pane = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	for (i = 0; i < n; i++) {
		GsurfView *view = gsurf_window_get_nth_view(window, i);
		guint64 parent = gsurf_mcp_tree_parent(window, view);

		g_ptr_array_add(t->views, view);
		g_array_append_val(t->parents, parent);
	}
}

/* Whether a pane shows view @id. */
static gboolean
tree_in_pane(Tree *t, guint64 id)
{
	gint64 key = (gint64)id;

	return g_hash_table_contains(t->in_pane, &key);
}

/*
 * One node: the view's members plus "collapsed" and its children in
 * window order. With @skip_panes, children shown in a pane are left out
 * (each is listed under its own pane in the grouped tree).
 */
static void
tree_add_node(Tree *t, JsonBuilder *b, guint index, gboolean skip_panes, gint pane)
{
	GsurfView *view = g_ptr_array_index(t->views, index);
	guint64 id = gsurf_view_get_id(view);
	guint i;

	json_builder_begin_object(b);
	gsurf_mcp_add_view_members(b, t->window, view, index);
	json_builder_set_member_name(b, "collapsed");
	json_builder_add_boolean_value(b, t->provider != NULL &&
		gsurf_tab_tree_provider_get_collapsed(t->provider, t->window, view));
	if (pane >= 0) {
		json_builder_set_member_name(b, "pane_index");
		json_builder_add_int_value(b, pane);
	}
	json_builder_set_member_name(b, "children");
	json_builder_begin_array(b);
	for (i = 0; i < t->views->len; i++) {
		GsurfView *child = g_ptr_array_index(t->views, i);

		if (g_array_index(t->parents, guint64, i) != id)
			continue;
		if (skip_panes && tree_in_pane(t, gsurf_view_get_id(child)))
			continue;
		tree_add_node(t, b, i, skip_panes, -1);
	}
	json_builder_end_array(b);
	json_builder_end_object(b);
}

/* The index of view @id in the window, or -1. */
static gint
tree_index(Tree *t, guint64 id)
{
	guint i;

	for (i = 0; i < t->views->len; i++)
		if (gsurf_view_get_id(g_ptr_array_index(t->views, i)) == id)
			return (gint)i;
	return -1;
}

/* The tabs module's label for the background group, or "Background". */
static gchar *
background_label(void)
{
	GsurfConfig *config = gsurf_config_get_default();
	YamlNode *node = config != NULL ? gsurf_config_get_module_node(config, "tabs") : NULL;
	YamlMapping *m;

	if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING) {
		m = yaml_node_get_mapping(node);
		if (yaml_mapping_has_member(m, "background_label")) {
			const gchar *label = yaml_mapping_get_string_member(m, "background_label");

			if (label != NULL && *label != '\0')
				return g_strdup(label);
		}
	}
	return g_strdup("Background");
}

static McpToolResult *
tool_get_tab_tree(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindow *window = gsurf_mcp_window(&error);
	GsurfWindowLayout *layout;
	g_autoptr(GPtrArray) groups = NULL;
	g_autoptr(JsonBuilder) b = NULL;
	Tree t = { 0 };
	guint i;
	guint g;
	gboolean any_background = FALSE;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	tree_init(&t, window);
	layout = gsurf_window_get_layout(window);
	groups = layout != NULL ? gsurf_window_layout_list_groups(layout, window) : NULL;

	/* 1. The views each group's panes show. */
	for (g = 0; groups != NULL && g < groups->len; g++) {
		guint n_ids;
		const guint64 *ids = gsurf_layout_group_get_view_ids(g_ptr_array_index(groups, g),
			&n_ids);

		for (i = 0; i < n_ids; i++) {
			gint64 *key = g_new(gint64, 1);

			*key = (gint64)ids[i];
			g_hash_table_add(t.in_pane, key);
		}
	}

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "has_tree_provider");
	json_builder_add_boolean_value(b, t.provider != NULL);

	/* 2. The plain opener tree: the roots in window order. */
	json_builder_set_member_name(b, "tree");
	json_builder_begin_array(b);
	for (i = 0; i < t.views->len; i++)
		if (g_array_index(t.parents, guint64, i) == 0)
			tree_add_node(&t, b, i, FALSE, -1);
	json_builder_end_array(b);

	/* 3. Grouped by layout group (wmux window), as the sidebar shows it:
	 *    each group's pane tabs in pane order with their background
	 *    descendants, then the background tabs under no pane tab. */
	json_builder_set_member_name(b, "grouped");
	json_builder_add_boolean_value(b, groups != NULL && groups->len > 0);
	json_builder_set_member_name(b, "groups");
	if (groups == NULL || groups->len == 0) {
		json_builder_add_null_value(b);
	} else {
		g_autofree gchar *label = background_label();

		json_builder_begin_array(b);
		for (g = 0; g < groups->len; g++) {
			GsurfLayoutGroup *group = g_ptr_array_index(groups, g);
			guint n_ids;
			const guint64 *ids = gsurf_layout_group_get_view_ids(group, &n_ids);

			json_builder_begin_object(b);
			json_builder_set_member_name(b, "index");
			json_builder_add_int_value(b, gsurf_layout_group_get_index(group));
			json_builder_set_member_name(b, "name");
			json_builder_add_string_value(b, gsurf_layout_group_get_name(group));
			json_builder_set_member_name(b, "current");
			json_builder_add_boolean_value(b, gsurf_layout_group_get_current(group));
			json_builder_set_member_name(b, "background");
			json_builder_add_boolean_value(b, FALSE);
			json_builder_set_member_name(b, "tabs");
			json_builder_begin_array(b);
			for (i = 0; i < n_ids; i++) {
				gint index = tree_index(&t, ids[i]);

				if (index >= 0)
					tree_add_node(&t, b, (guint)index, TRUE, (gint)i);
			}
			json_builder_end_array(b);
			json_builder_end_object(b);
		}

		/* The background group: roots no pane shows (their pane-shown
		 * descendants are listed under their panes). */
		for (i = 0; i < t.views->len && !any_background; i++)
			if (g_array_index(t.parents, guint64, i) == 0 &&
			    !tree_in_pane(&t, gsurf_view_get_id(g_ptr_array_index(t.views, i))))
				any_background = TRUE;
		if (any_background) {
			json_builder_begin_object(b);
			json_builder_set_member_name(b, "index");
			json_builder_add_null_value(b);
			json_builder_set_member_name(b, "name");
			json_builder_add_string_value(b, label);
			json_builder_set_member_name(b, "current");
			json_builder_add_boolean_value(b, FALSE);
			json_builder_set_member_name(b, "background");
			json_builder_add_boolean_value(b, TRUE);
			json_builder_set_member_name(b, "tabs");
			json_builder_begin_array(b);
			for (i = 0; i < t.views->len; i++)
				if (g_array_index(t.parents, guint64, i) == 0 &&
				    !tree_in_pane(&t, gsurf_view_get_id(g_ptr_array_index(t.views, i))))
					tree_add_node(&t, b, i, TRUE, -1);
			json_builder_end_array(b);
			json_builder_end_object(b);
		}
		json_builder_end_array(b);
	}
	json_builder_end_object(b);
	tree_clear(&t);
	return gsurf_mcp_json(json_builder_get_root(b));
}

/* ------------------------------------------------------------------ */
/* Containers                                                          */
/* ------------------------------------------------------------------ */

/* The containers module's configured entry for @container (matched by
 * id, or by name for an entry without an id), or NULL. */
static YamlMapping *
container_config(GsurfContainer *container)
{
	GsurfConfig *config = gsurf_config_get_default();
	YamlNode *node = config != NULL ? gsurf_config_get_module_node(config, "containers") : NULL;
	YamlMapping *m;
	YamlSequence *list;
	guint i;

	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return NULL;
	m = yaml_node_get_mapping(node);
	if (!yaml_mapping_has_member(m, "containers"))
		return NULL;
	list = yaml_mapping_get_sequence_member(m, "containers");
	for (i = 0; list != NULL && i < yaml_sequence_get_length(list); i++) {
		YamlNode *item = yaml_sequence_get_element(list, i);
		YamlMapping *entry;
		const gchar *id = NULL;
		const gchar *cname = NULL;

		if (item == NULL || yaml_node_get_node_type(item) != YAML_NODE_MAPPING)
			continue;
		entry = yaml_node_get_mapping(item);
		if (yaml_mapping_has_member(entry, "id"))
			id = yaml_mapping_get_string_member(entry, "id");
		if (yaml_mapping_has_member(entry, "name"))
			cname = yaml_mapping_get_string_member(entry, "name");
		if ((id != NULL && g_strcmp0(id, gsurf_container_get_id(container)) == 0) ||
		    (id == NULL && cname != NULL &&
		     g_strcmp0(cname, gsurf_container_get_name(container)) == 0))
			return entry;
	}
	return NULL;
}

/* "rules": the host patterns of @container's configured entry. */
static void
add_rules(JsonBuilder *b, GsurfContainer *container)
{
	YamlMapping *entry = container != NULL ? container_config(container) : NULL;
	YamlSequence *hosts = NULL;
	guint i;

	json_builder_set_member_name(b, "rules");
	json_builder_begin_array(b);
	if (entry != NULL && yaml_mapping_has_member(entry, "hosts"))
		hosts = yaml_mapping_get_sequence_member(entry, "hosts");
	for (i = 0; hosts != NULL && i < yaml_sequence_get_length(hosts); i++) {
		YamlNode *item = yaml_sequence_get_element(hosts, i);

		if (item != NULL && yaml_node_get_node_type(item) == YAML_NODE_SCALAR)
			json_builder_add_string_value(b, yaml_node_get_string(item));
	}
	json_builder_end_array(b);
}

static McpToolResult *
tool_list_containers(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	GsurfModule *policy = gsurf_module_manager_get_module(gsurf_module_manager_get_default(),
		"containers");
	gboolean rules_active = policy != NULL && gsurf_module_is_active(policy);
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());
	g_autoptr(GPtrArray) list = NULL;
	g_autoptr(JsonBuilder) b = json_builder_new();
	guint n_default = 0;
	guint i;

	/* 1. The default container: no registry entry; its views counted in
	 *    the active window. */
	for (i = 0; window != NULL && i < gsurf_window_get_n_views(window); i++)
		if (gsurf_view_get_container_id(gsurf_window_get_nth_view(window, i)) == NULL)
			n_default++;
	json_builder_begin_array(b);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "id");
	json_builder_add_string_value(b, GSURF_CONTAINER_DEFAULT);
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b, "Default");
	json_builder_set_member_name(b, "color");
	json_builder_add_null_value(b);
	json_builder_set_member_name(b, "temporary");
	json_builder_add_boolean_value(b, FALSE);
	json_builder_set_member_name(b, "default");
	json_builder_add_boolean_value(b, TRUE);
	json_builder_set_member_name(b, "n_views");
	json_builder_add_int_value(b, n_default);
	json_builder_set_member_name(b, "rules");
	json_builder_begin_array(b);
	json_builder_end_array(b);
	json_builder_set_member_name(b, "rules_active");
	json_builder_add_boolean_value(b, rules_active);
	json_builder_end_object(b);

	/* 2. Every registry container (persistent, then temporary). */
	list = registry != NULL ? gsurf_container_registry_list(registry) : NULL;
	for (i = 0; list != NULL && i < list->len; i++) {
		GsurfContainer *c = g_ptr_array_index(list, i);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "id");
		json_builder_add_string_value(b, gsurf_container_get_id(c));
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, gsurf_container_get_name(c));
		json_builder_set_member_name(b, "color");
		if (gsurf_container_get_color(c) != NULL)
			json_builder_add_string_value(b, gsurf_container_get_color(c));
		else
			json_builder_add_null_value(b);
		json_builder_set_member_name(b, "temporary");
		json_builder_add_boolean_value(b, gsurf_container_get_temporary(c));
		json_builder_set_member_name(b, "default");
		json_builder_add_boolean_value(b, FALSE);
		json_builder_set_member_name(b, "n_views");
		json_builder_add_int_value(b, gsurf_container_get_n_views(c));
		add_rules(b, c);
		json_builder_set_member_name(b, "rules_active");
		json_builder_add_boolean_value(b, rules_active);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	return gsurf_mcp_json(json_builder_get_root(b));
}

static McpToolResult *
tool_reopen_in_container(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindow *window = gsurf_mcp_window(&error);
	const gchar *container = gsurf_mcp_arg_string(args, "container");
	GsurfView *view;
	GsurfView *fresh;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	if (kiosk_refuses(&error))
		return gsurf_mcp_gerror(error, NULL);
	view = gsurf_mcp_resolve_view(window, args, "view", NULL, TRUE, &error);
	if (view == NULL)
		return gsurf_mcp_gerror(error, NULL);
	if (container == NULL)
		return gsurf_mcp_error("missing 'container' (a container id, \"temporary\" or "
			"\"default\")");
	if (!check_container(container, &error))
		return gsurf_mcp_gerror(error, NULL);

	/* The new view takes the old one's place, opener, children and pane;
	 * the old view closes (its id is gone afterwards). */
	fresh = gsurf_window_reopen_view(window, view, container);
	if (fresh == NULL)
		return gsurf_mcp_error("could not reopen view %" G_GUINT64_FORMAT,
			gsurf_view_get_id(view));
	return gsurf_mcp_json(view_node(window, fresh));
}

static McpToolResult *
tool_new_temporary_container_view(McpServer *s, const gchar *name, JsonObject *args,
                                  gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindow *window = gsurf_mcp_window(&error);
	gboolean background = FALSE;
	GsurfView *view;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	if (kiosk_refuses(&error) || !gsurf_mcp_arg_bool(args, "background", &background, &error))
		return gsurf_mcp_gerror(error, NULL);
	view = open_in_window(window, NULL, GSURF_CONTAINER_TEMPORARY,
		gsurf_mcp_arg_string(args, "uri"), !background, &error);
	if (view == NULL)
		return gsurf_mcp_gerror(error, NULL);
	return gsurf_mcp_json(view_node(window, view));
}

/* ------------------------------------------------------------------ */
/* Modes and actions                                                   */
/* ------------------------------------------------------------------ */

/* The keyboard-mode owner (the modal module), when it is active. */
static gboolean
modal_active(void)
{
	GsurfModule *modal = gsurf_module_manager_get_module(gsurf_module_manager_get_default(),
		"modal");

	return modal != NULL && gsurf_module_is_active(modal);
}

/* Whether the active window is typing: a focused field in its active
 * view's page, or a focused chrome entry (why NORMAL reads INSERT). */
static gboolean
window_typing(GsurfModuleManager *mgr)
{
	GsurfWindow *window = gsurf_module_manager_get_active_window(mgr);

	return window != NULL && gsurf_window_get_typing(window);
}

/* {"mode": "normal", "label": "NORMAL", "passthrough": false,
 *  "typing": false, "modal_active": true} */
static JsonNode *
mode_node(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfModePolicy mode = gsurf_module_manager_get_mode(mgr);
	g_autoptr(JsonBuilder) b = json_builder_new();
	GEnumClass *klass = g_type_class_ref(GSURF_TYPE_MODE_POLICY);
	GEnumValue *value = g_enum_get_value(klass, mode);

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "mode");
	json_builder_add_string_value(b, value != NULL ? value->value_nick : "normal");
	json_builder_set_member_name(b, "label");
	json_builder_add_string_value(b, gsurf_mode_policy_get_label(mode));
	json_builder_set_member_name(b, "passthrough");
	json_builder_add_boolean_value(b, gsurf_module_manager_get_input_passthrough(mgr));
	/* Why NORMAL reads INSERT: the active window is typing into a page
	 * field or a chrome entry. */
	json_builder_set_member_name(b, "typing");
	json_builder_add_boolean_value(b, window_typing(mgr));
	json_builder_set_member_name(b, "modal_active");
	json_builder_add_boolean_value(b, modal_active());
	json_builder_end_object(b);
	g_type_class_unref(klass);
	return json_builder_get_root(b);
}

static McpToolResult *
tool_get_mode(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	return gsurf_mcp_json(mode_node());
}

static McpToolResult *
tool_set_mode(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	const gchar *mode = gsurf_mcp_arg_string(args, "mode");
	GsurfWindow *window = gsurf_mcp_window(&error);
	GsurfModePolicy want;
	GsurfAction action;
	GsurfView *view;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	if (g_strcmp0(mode, "normal") == 0) {
		want = GSURF_MODE_NORMAL;
		action = GSURF_ACTION_ENTER_NORMAL_MODE;
	} else if (g_strcmp0(mode, "insert") == 0) {
		want = GSURF_MODE_INSERT;
		action = GSURF_ACTION_ENTER_INSERT_MODE;
	} else {
		return gsurf_mcp_error("'mode' must be \"normal\" or \"insert\"");
	}
	if (!modal_active())
		return gsurf_mcp_error("input modes need the modal module "
			"(modules.modal.enabled: true)");
	view = gsurf_mcp_resolve_view(window, args, "view", NULL, TRUE, &error);
	if (view == NULL ||
	    !gsurf_module_manager_execute_action(mgr, window, view, action, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (gsurf_module_manager_get_mode(mgr) != want)
		return gsurf_mcp_error("the mode did not change to %s", mode);
	return gsurf_mcp_json(mode_node());
}

static McpToolResult *
tool_follow_hints(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfWindow *window = gsurf_mcp_window(&error);
	gboolean new_view = FALSE;
	GsurfView *view;

	if (window == NULL || !gsurf_mcp_arg_bool(args, "new_view", &new_view, &error))
		return gsurf_mcp_gerror(error, NULL);
	if (!modal_active())
		return gsurf_mcp_error("link hints need the modal module "
			"(modules.modal.enabled: true)");
	view = gsurf_mcp_resolve_view(window, args, "view", NULL, TRUE, &error);
	if (view == NULL ||
	    !gsurf_module_manager_execute_action(gsurf_module_manager_get_default(), window, view,
			new_view ? GSURF_ACTION_FOLLOW_HINTS_NEW_VIEW : GSURF_ACTION_FOLLOW_HINTS,
			&error))
		return gsurf_mcp_gerror(error, NULL);
	return gsurf_mcp_json(mode_node());
}

static McpToolResult *
tool_run_action(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	const gchar *nick = gsurf_mcp_arg_string(args, "action");
	GsurfWindow *window = gsurf_mcp_window(&error);
	g_autoptr(JsonBuilder) b = NULL;
	GsurfAction action;
	GsurfView *view;

	if (window == NULL)
		return gsurf_mcp_gerror(error, NULL);
	if (nick == NULL)
		return gsurf_mcp_error("missing 'action' (list_actions gives the names)");
	action = gsurf_action_from_string(nick);
	if (action == GSURF_ACTION_NONE)
		return gsurf_mcp_error("unknown action '%s' (list_actions gives the names)", nick);
	view = gsurf_mcp_resolve_view(window, args, "view", NULL, TRUE, &error);
	if (view == NULL ||
	    !gsurf_module_manager_execute_action(gsurf_module_manager_get_default(), window, view,
			action, &error))
		return gsurf_mcp_gerror(error, NULL);

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "action");
	json_builder_add_string_value(b, gsurf_action_to_string(action));
	json_builder_set_member_name(b, "performed");
	json_builder_add_boolean_value(b, TRUE);
	json_builder_end_object(b);
	return gsurf_mcp_json(json_builder_get_root(b));
}

static McpToolResult *
tool_list_actions(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	gboolean kiosk = gsurf_kiosk_is_enabled();
	gint a;

	json_builder_begin_array(b);
	for (a = GSURF_ACTION_NONE + 1; a < GSURF_ACTION_LAST; a++) {
		const gchar *nick = gsurf_action_to_string((GsurfAction)a);

		if (nick == NULL)
			continue;
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, nick);
		json_builder_set_member_name(b, "allowed");
		json_builder_add_boolean_value(b, !kiosk || gsurf_kiosk_allows_action((GsurfAction)a));
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	return gsurf_mcp_json(json_builder_get_root(b));
}

/* ------------------------------------------------------------------ */
/* Table                                                               */
/* ------------------------------------------------------------------ */

#define VIEW_ARG { "view", "integer", "stable view id (list_views 'id'); default: the active view", FALSE }

const GsurfMcpToolDef gsurf_mcp_view_tools[] = {
	{ "list_views",
	  "List the views (tabs) of the active window as a JSON array in window order. Each: index, "
	  "id (stable, use it as 'view' in other tools), uri, title, active (has focus), visible "
	  "(on screen in any pane), opener_id (0 = none), parent_id (tree parent, 0 = root), "
	  "container (id, null = default), container_name, container_color, container_temporary, "
	  "pane ({window, index} of the wmux pane showing it, or null).",
	  {{ NULL, NULL, NULL, FALSE }}, tool_list_views },
	{ "switch_view", "Make a view the active (focused) view. Give 'view' (id) or 'index'.",
	  {{ "view", "integer", "stable view id", FALSE },
	   { "index", "integer", "position in the window (list_views 'index')", FALSE }},
	  tool_switch_view },
	{ "open_view",
	  "Open a new view (tab) as a root of the tab tree and focus it; returns its window index "
	  "as text. Refused in kiosk mode.",
	  {{ "uri", "string", "the URI to load (default: the configured new-view URI)", FALSE },
	   { "container", "string", "container id, \"temporary\" (a new temporary container) or "
	     "\"default\"; without it the containers policy places the view", FALSE }},
	  tool_open_view },
	{ "close_view", "Close a view. Give 'view' (id) or 'index'. The last view cannot close.",
	  {{ "view", "integer", "stable view id", FALSE },
	   { "index", "integer", "position in the window", FALSE }},
	  tool_close_view },
	{ "open_child_view",
	  "Open a URI in a new view that is a child of 'parent' in the tab tree (placed after the "
	  "parent's subtree, in the parent's container unless a host rule or 'container' says "
	  "otherwise). Foreground by default; with background=true it stays behind (with wmux it "
	  "takes no pane). Returns the new view as a list_views object. Refused in kiosk mode.",
	  {{ "parent", "integer", "the parent view id (default: the active view)", FALSE },
	   { "uri", "string", "the URI to load (default: the configured new-view URI)", FALSE },
	   { "background", "boolean", "leave the current view focused (default false)", FALSE },
	   { "container", "string", "container id, \"temporary\" or \"default\"", FALSE }},
	  tool_open_child_view },
	{ "move_view",
	  "Move a view with its subtree in the tab tree. Give exactly one of: 'parent' (become its "
	  "last child; 0 = a root at the end), 'before' / 'after' (become that view's sibling, "
	  "after = after its whole subtree), or 'index' (a plain window position; the tree is "
	  "re-derived). 'parent' needs the tabs module. Returns the moved view.",
	  {{ "view", "integer", "the view id to move", TRUE },
	   { "parent", "integer", "new parent view id, 0 for a root", FALSE },
	   { "before", "integer", "the view id to move in front of", FALSE },
	   { "after", "integer", "the view id to move behind (after its subtree)", FALSE },
	   { "index", "integer", "a window position", FALSE }},
	  tool_move_view },
	{ "set_collapsed",
	  "Collapse (hide the descendants of) or expand a view in the tab sidebar. Needs the tabs "
	  "module; fails for a view without children. Returns the view.",
	  {{ "view", "integer", "the view id", TRUE },
	   { "collapsed", "boolean", "true to collapse (default), false to expand", FALSE }},
	  tool_set_collapsed },
	{ "get_tab_tree",
	  "The tab tree of the active window as JSON: 'tree' (roots in window order, each node a "
	  "list_views object plus 'collapsed' and 'children'), and, when wmux is on, 'grouped': "
	  "true and 'groups' (one per wmux window: index, name, current, 'tabs' = the tabs its panes "
	  "show in pane order, each with 'pane_index' and its background descendants; then a "
	  "group with background: true and index null for background tabs under no pane tab).",
	  {{ NULL, NULL, NULL, FALSE }}, tool_get_tab_tree },
	{ "list_containers",
	  "List the cookie/storage containers as a JSON array: first the default container "
	  "(id \"default\"), then each defined one: id, name, color, temporary, default, n_views, "
	  "rules (the host regexes that assign pages to it) and rules_active (the containers module "
	  "is running).",
	  {{ NULL, NULL, NULL, FALSE }}, tool_list_containers },
	{ "reopen_in_container",
	  "Reopen a view's page in another container, in place (same tree position, children, "
	  "pane and focus). The old view closes and the new one gets a NEW id, returned as a "
	  "list_views object. Refused in kiosk mode.",
	  {{ "view", "integer", "the view id (default: the active view)", FALSE },
	   { "container", "string", "container id, \"temporary\" or \"default\"", TRUE }},
	  tool_reopen_in_container },
	{ "new_temporary_container_view",
	  "Open a URI in a new view in a fresh temporary container (own cookies and storage, "
	  "deleted after its last view closes). Returns the new view. Refused in kiosk mode.",
	  {{ "uri", "string", "the URI to load (default: the configured new-view URI)", FALSE },
	   { "background", "boolean", "leave the current view focused (default false)", FALSE }},
	  tool_new_temporary_container_view },
	{ "get_mode",
	  "The keyboard input mode: {mode: normal|insert|follow|find|passthrough, label, "
	  "passthrough (keys go to the page), modal_active (the modal module owns the modes)}.",
	  {{ NULL, NULL, NULL, FALSE }}, tool_get_mode },
	{ "set_mode",
	  "Switch the vim-style input mode (needs the modal module): \"normal\" (keys are "
	  "commands) or \"insert\" (keys go to the page). Returns the mode as get_mode.",
	  {{ "mode", "string", "\"normal\" or \"insert\"", TRUE }, VIEW_ARG },
	  tool_set_mode },
	{ "follow_hints",
	  "Show link hints on the page (modal module's f / F). The user then types a hint; "
	  "returns the mode (follow).",
	  {{ "new_view", "boolean", "open the chosen link in a new view (F)", FALSE }, VIEW_ARG },
	  tool_follow_hints },
	{ "run_action",
	  "Perform any browser action by name, exactly as its key binding would (e.g. "
	  "\"zoom-in\", \"tab-next\", \"tab-sidebar-toggle\", \"pane-split-right\", "
	  "\"container-reopen-next\", \"toggle-fullscreen\"). list_actions gives every name. Kiosk "
	  "mode refuses the actions it forbids on the keyboard.",
	  {{ "action", "string", "the action name (list_actions)", TRUE }, VIEW_ARG },
	  tool_run_action },
	{ "list_actions",
	  "List every action name run_action accepts, with 'allowed' (false when kiosk mode "
	  "forbids it).",
	  {{ NULL, NULL, NULL, FALSE }}, tool_list_actions },
	{ NULL, NULL, {{ NULL, NULL, NULL, FALSE }}, NULL }
};
