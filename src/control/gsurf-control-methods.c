/*
 * gsurf-control-methods.c - The core methods of the control socket
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every method goes through public core API only: the application's
 * active window and its views (stable ids, gsurf_view_lookup()), the
 * GsurfWindowLayout vfuncs (wmux needs no code of its own here), the
 * active GsurfTabTreeProvider (the tabs module), the container registry,
 * the module manager (mode, actions through the host's executor, module
 * activation) and the config. Kiosk mode refuses here what it refuses on
 * the keyboard: opening, reopening and navigating views, page.eval,
 * module toggles, and the actions gsurf_kiosk_allows_action() forbids.
 *
 * Views are addressed by "view" (the stable id gsurf_view_get_id()) or,
 * where it says so, "index" (a position in the window); without either
 * a method acts on the active view.
 */

#include "control/gsurf-control-private.h"

#include <math.h>
#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include <yaml-glib.h>

#ifdef GSURF_HAVE_LRG_BACKEND
#include "backend/lrg/gsurf-lrg-window.h"
#endif
#ifdef GSURF_HAVE_LRG_3D
#include "backend/lrg/gsurf-lrg-scene.h"
#endif

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

/* The member @name of @params when present and not null. */
static JsonNode *
member(JsonObject *params, const gchar *name)
{
	JsonNode *node;

	if (params == NULL || !json_object_has_member(params, name))
		return NULL;
	node = json_object_get_member(params, name);
	return (node == NULL || JSON_NODE_HOLDS_NULL(node)) ? NULL : node;
}

static gboolean
arg_has(JsonObject *params, const gchar *name)
{
	return member(params, name) != NULL;
}

/* A string member, or NULL; @error set when it has another type. */
static gboolean
arg_string(JsonObject *params, const gchar *name, const gchar **out, GError **error)
{
	JsonNode *node = member(params, name);

	*out = NULL;
	if (node == NULL)
		return TRUE;
	if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) {
		g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_INVALID_PARAMS,
			"'%s' must be a string", name);
		return FALSE;
	}
	*out = json_node_get_string(node);
	return TRUE;
}

/* An integer member: a JSON integer, an integral number or a string of
 * digits. @out keeps its value when absent. */
static gboolean
arg_int(JsonObject *params, const gchar *name, gint64 *out, GError **error)
{
	JsonNode *node = member(params, name);
	GType type;

	if (node == NULL)
		return TRUE;
	if (JSON_NODE_HOLDS_VALUE(node)) {
		type = json_node_get_value_type(node);
		if (type == G_TYPE_INT64) {
			*out = json_node_get_int(node);
			return TRUE;
		}
		if (type == G_TYPE_DOUBLE) {
			gdouble d = json_node_get_double(node);

			if (d == (gdouble)(gint64)d) {
				*out = (gint64)d;
				return TRUE;
			}
		}
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
	}
	g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_INVALID_PARAMS,
		"'%s' must be an integer", name);
	return FALSE;
}

/* A boolean member: a JSON boolean, or true/false/on/off/yes/no/1/0.
 * @out keeps its value when absent. */
static gboolean
arg_bool(JsonObject *params, const gchar *name, gboolean *out, GError **error)
{
	static const gchar *const yes[] = { "true", "1", "on", "yes", NULL };
	static const gchar *const no[] = { "false", "0", "off", "no", NULL };
	JsonNode *node = member(params, name);
	GType type;

	if (node == NULL)
		return TRUE;
	if (JSON_NODE_HOLDS_VALUE(node)) {
		type = json_node_get_value_type(node);
		if (type == G_TYPE_BOOLEAN) {
			*out = json_node_get_boolean(node);
			return TRUE;
		}
		if (type == G_TYPE_INT64) {
			*out = json_node_get_int(node) != 0;
			return TRUE;
		}
		if (type == G_TYPE_STRING) {
			const gchar *text = json_node_get_string(node);

			if (g_strv_contains(yes, text)) {
				*out = TRUE;
				return TRUE;
			}
			if (g_strv_contains(no, text)) {
				*out = FALSE;
				return TRUE;
			}
		}
	}
	g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_INVALID_PARAMS,
		"'%s' must be a boolean", name);
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* Windows and views                                                   */
/* ------------------------------------------------------------------ */

static GsurfWindow *
get_window(GsurfControlServer *server, GError **error)
{
	GsurfWindow *window = gsurf_control_server_get_window(server);

	if (window == NULL)
		g_set_error_literal(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_NOT_FOUND,
			"no window");
	return window;
}

static GsurfView *
view_by_id(GsurfWindow *window, gint64 id, GError **error)
{
	GsurfView *view = id > 0 ? gsurf_view_lookup((guint64)id) : NULL;

	if (view == NULL || gsurf_window_get_view_index(window, view) < 0) {
		g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_NOT_FOUND,
			"unknown view %" G_GINT64_FORMAT " (views.list gives the ids)", id);
		return NULL;
	}
	return view;
}

/*
 * resolve_view:
 * @id_name: the id parameter ("view", "parent", ...)
 * @allow_index: also accept "index" (a window position)
 * @default_active: without either, the active view
 *
 * Returns: (nullable): the view, or NULL with @error set
 */
static GsurfView *
resolve_view(GsurfWindow *window, JsonObject *params, const gchar *id_name,
             gboolean allow_index, gboolean default_active, GError **error)
{
	gint64 value = 0;
	guint n;

	if (arg_has(params, id_name)) {
		if (!arg_int(params, id_name, &value, error))
			return NULL;
		return view_by_id(window, value, error);
	}
	if (allow_index && arg_has(params, "index")) {
		if (!arg_int(params, "index", &value, error))
			return NULL;
		n = gsurf_window_get_n_views(window);
		if (value < 0 || value >= (gint64)n) {
			g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_NOT_FOUND,
				"index %" G_GINT64_FORMAT " out of range (the window has %u views)",
				value, n);
			return NULL;
		}
		return gsurf_window_get_nth_view(window, (guint)value);
	}
	if (default_active && gsurf_window_get_active_view(window) != NULL)
		return gsurf_window_get_active_view(window);
	if (default_active)
		g_set_error_literal(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_NOT_FOUND,
			"no active view");
	else
		g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_INVALID_PARAMS,
			"missing '%s'%s", id_name, allow_index ? " (or 'index')" : "");
	return NULL;
}

/* The window and the view a page method addresses ("view"/"index"). */
static GsurfView *
target_view(GsurfControlServer *server, JsonObject *params, GsurfWindow **window_out,
            GError **error)
{
	GsurfWindow *window = get_window(server, error);

	if (window_out != NULL)
		*window_out = window;
	if (window == NULL)
		return NULL;
	return resolve_view(window, params, "view", TRUE, TRUE, error);
}

/* Kiosk mode refuses @what. */
static gboolean
kiosk_refuses(const gchar *what, GError **error)
{
	if (!gsurf_kiosk_is_enabled())
		return FALSE;
	g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_PERMISSION_DENIED,
		"kiosk mode: %s is not allowed", what);
	return TRUE;
}

static GsurfWindowLayout *
get_layout(GsurfWindow *window, GError **error)
{
	GsurfWindowLayout *layout = gsurf_window_get_layout(window);

	if (layout == NULL && gsurf_kiosk_is_enabled())
		g_set_error_literal(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_PERMISSION_DENIED,
			"kiosk mode shows a single view: wmux stays inactive");
	else if (layout == NULL)
		g_set_error_literal(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_UNSUPPORTED,
			"no window layout is installed: enable the wmux module "
			"(modules.wmux.enabled: true) to get panes and windows");
	return layout;
}

static GsurfTabTreeProvider *
tree_provider(void)
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

/* The tree parent of @view: the provider's, else the opener tree's
 * (docs/tabs.org, "Where the tree comes from"). */
static guint64
tree_parent(GsurfWindow *window, GsurfView *view)
{
	GsurfTabTreeProvider *provider = tree_provider();
	g_autoptr(GArray) stack = NULL;
	guint n;
	guint i;

	if (provider != NULL)
		return gsurf_tab_tree_provider_get_parent(provider, window, view);
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

/* The group (wmux window) and pane showing @view, when one does. */
static gboolean
find_pane(GsurfWindow *window, GsurfView *view, guint *group, guint *pane)
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

static void
add_string_or_null(JsonBuilder *b, const gchar *name, const gchar *value)
{
	json_builder_set_member_name(b, name);
	if (value != NULL)
		json_builder_add_string_value(b, value);
	else
		json_builder_add_null_value(b);
}

/* The members describing @view (at @index in @window): index, id, uri,
 * title, active, visible, opener_id, parent_id, container (+ name, color,
 * temporary), pane. */
static void
add_view_members(JsonBuilder *b, GsurfWindow *window, GsurfView *view, guint index)
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

	/* 2. Focus and visibility. */
	json_builder_set_member_name(b, "active");
	json_builder_add_boolean_value(b, view == gsurf_window_get_active_view(window));
	json_builder_set_member_name(b, "visible");
	json_builder_add_boolean_value(b, gsurf_window_get_view_visible(window, view));

	/* 3. Tree: the opener (fixed) and the tree parent. */
	json_builder_set_member_name(b, "opener_id");
	json_builder_add_int_value(b, (gint64)gsurf_view_get_opener_id(view));
	json_builder_set_member_name(b, "parent_id");
	json_builder_add_int_value(b, (gint64)tree_parent(window, view));

	/* 4. Container (null = default). */
	if (container_id != NULL && registry != NULL)
		container = gsurf_container_registry_lookup(registry, container_id);
	add_string_or_null(b, "container", container_id);
	add_string_or_null(b, "container_name",
		container != NULL ? gsurf_container_get_name(container) : NULL);
	add_string_or_null(b, "container_color",
		container != NULL ? gsurf_container_get_color(container) : NULL);
	json_builder_set_member_name(b, "container_temporary");
	json_builder_add_boolean_value(b, container != NULL &&
		gsurf_container_get_temporary(container));

	/* 5. Pane: which wmux window and pane show it. */
	json_builder_set_member_name(b, "pane");
	if (find_pane(window, view, &group, &pane)) {
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

static JsonNode *
view_node(GsurfWindow *window, GsurfView *view)
{
	g_autoptr(JsonBuilder) b = json_builder_new();

	json_builder_begin_object(b);
	add_view_members(b, window, view, (guint)MAX(gsurf_window_get_view_index(window, view), 0));
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

static JsonNode *
string_node(const gchar *text)
{
	JsonNode *node = json_node_new(JSON_NODE_VALUE);

	json_node_set_string(node, text != NULL ? text : "");
	return node;
}

/* Answers @inv with @error (consumed). */
static void
fail(GsurfControlInvocation *inv, GError *error)
{
	gsurf_control_invocation_return_gerror(inv, error);
	g_error_free(error);
}

/* Loads @input in @view, letting URI-handler modules (search engines)
 * rewrite it first, as the gsurf binary does for typed input. */
static void
load_rewritten(GsurfView *view, const gchar *input)
{
	g_autofree gchar *rewritten = gsurf_module_manager_dispatch_rewrite_uri(
		gsurf_module_manager_get_default(), input);

	gsurf_view_load_uri(view, rewritten != NULL ? rewritten : input);
}

/* A container argument: an id from the registry, "temporary" or
 * "default". */
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
	g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_NOT_FOUND,
		"unknown container '%s' (containers.list gives the ids; \"temporary\" and "
		"\"default\" are always accepted)", container != NULL ? container : "");
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* instance.info                                                       */
/* ------------------------------------------------------------------ */

static void
m_instance_info(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GsurfApplication *app = gsurf_control_server_get_application(server);
	GsurfWindow *window = gsurf_control_server_get_window(server);
	GsurfView *view = window != NULL ? gsurf_window_get_active_view(window) : NULL;
	GPtrArray *windows = app != NULL ? gsurf_application_get_windows(app) : NULL;
	g_autoptr(JsonBuilder) b = json_builder_new();
	guint n_views = 0;
	guint i;

	(void)data;
	for (i = 0; windows != NULL && i < windows->len; i++)
		n_views += gsurf_window_get_n_views(g_ptr_array_index(windows, i));

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "pid");
	json_builder_add_int_value(b, (gint64)getpid());
	json_builder_set_member_name(b, "version");
	json_builder_add_string_value(b, gsurf_get_version_string());
	json_builder_set_member_name(b, "backend");
	json_builder_add_string_value(b, gsurf_backend_get_name());
	add_string_or_null(b, "name", gsurf_control_server_get_name(server));
	add_string_or_null(b, "socket", gsurf_control_server_get_socket_path(server));
	json_builder_set_member_name(b, "kiosk");
	json_builder_add_boolean_value(b, gsurf_kiosk_is_enabled());
	json_builder_set_member_name(b, "windows");
	json_builder_add_int_value(b, windows != NULL ? windows->len : 0);
	json_builder_set_member_name(b, "views");
	json_builder_add_int_value(b, n_views);
	json_builder_set_member_name(b, "active_view");
	if (view != NULL)
		json_builder_add_int_value(b, (gint64)gsurf_view_get_id(view));
	else
		json_builder_add_null_value(b);
	add_string_or_null(b, "uri", view != NULL ? gsurf_view_get_uri(view) : NULL);
	add_string_or_null(b, "title", view != NULL ? gsurf_view_get_title(view) : NULL);
	json_builder_set_member_name(b, "focused_at");
	json_builder_add_int_value(b, gsurf_control_server_get_focused_at(server));
	json_builder_set_member_name(b, "now");
	json_builder_add_int_value(b, g_get_monotonic_time());
	json_builder_end_object(b);
	gsurf_control_invocation_return_value(inv, json_builder_get_root(b));
}

/* ------------------------------------------------------------------ */
/* Opening and navigating                                              */
/* ------------------------------------------------------------------ */

static void
m_open(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GsurfConfig *config = gsurf_config_get_default();
	GError *error = NULL;
	GsurfWindow *window;
	GsurfView *opener = NULL;
	g_autoptr(GsurfView) view = NULL;
	const gchar *uri = NULL;
	const gchar *container = NULL;
	gboolean background = FALSE;

	(void)data;
	window = kiosk_refuses("opening views", &error) ? NULL : get_window(server, &error);
	if (window == NULL ||
	    !arg_string(params, "uri", &uri, &error) ||
	    !arg_string(params, "container", &container, &error) ||
	    !arg_bool(params, "background", &background, &error)) {
		fail(inv, error);
		return;
	}

	/* 1. The tree parent and the container. */
	if (arg_has(params, "parent")) {
		opener = resolve_view(window, params, "parent", FALSE, FALSE, &error);
		if (opener == NULL) {
			fail(inv, error);
			return;
		}
	}
	if (container != NULL && !check_container(container, &error)) {
		fail(inv, error);
		return;
	}
	if (uri == NULL || *uri == '\0')
		uri = (config != NULL && config->new_view_uri != NULL) ? config->new_view_uri
			: "about:blank";

	/* 2. A view like the host's own tabs: settings applied, added (the
	 *    host and the tabs module wire it up on view-added), activated
	 *    unless it is a background tab, loaded. */
	view = gsurf_view_new_full(opener, container, uri, FALSE);
	if (view == NULL) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_FAILED,
			"could not create a view");
		return;
	}
	if (config != NULL && config->settings != NULL)
		gsurf_view_apply_settings(view, config->settings);
	gsurf_window_add_view(window, view);
	if (!background)
		gsurf_window_set_active_view(window, view);
	load_rewritten(view, uri);
	gsurf_control_invocation_return_value(inv, view_node(window, view));
}

static void
m_navigate(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfWindow *window;
	GsurfView *view;
	const gchar *uri = NULL;

	(void)data;
	if (!arg_string(params, "uri", &uri, &error) || kiosk_refuses("navigating", &error)) {
		fail(inv, error);
		return;
	}
	if (uri == NULL || *uri == '\0') {
		gsurf_control_invocation_return_error_literal(inv,
			GSURF_CONTROL_ERROR_INVALID_PARAMS, "missing 'uri'");
		return;
	}
	view = target_view(server, params, &window, &error);
	if (view == NULL) {
		fail(inv, error);
		return;
	}
	load_rewritten(view, uri);
	gsurf_control_invocation_return_value(inv, view_node(window, view));
}

typedef enum {
	NAV_BACK,
	NAV_FORWARD,
	NAV_RELOAD,
	NAV_STOP
} NavOp;

static void
m_nav(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfWindow *window;
	GsurfView *view;
	gboolean hard = FALSE;

	view = target_view(server, params, &window, &error);
	if (view == NULL || !arg_bool(params, "hard", &hard, &error)) {
		fail(inv, error);
		return;
	}
	switch ((NavOp)GPOINTER_TO_INT(data)) {
	case NAV_BACK:    gsurf_view_go_back(view); break;
	case NAV_FORWARD: gsurf_view_go_forward(view); break;
	case NAV_RELOAD:  gsurf_view_reload(view, hard); break;
	case NAV_STOP:    gsurf_view_stop_loading(view); break;
	default:          break;
	}
	gsurf_control_invocation_return_value(inv, view_node(window, view));
}

/* ------------------------------------------------------------------ */
/* views.*                                                             */
/* ------------------------------------------------------------------ */

static void
m_views_list(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GError *error = NULL;
	GsurfWindow *window = get_window(server, &error);
	g_autoptr(JsonBuilder) b = NULL;
	guint i;

	(void)data;
	if (window == NULL) {
		fail(inv, error);
		return;
	}
	b = json_builder_new();
	json_builder_begin_array(b);
	for (i = 0; i < gsurf_window_get_n_views(window); i++) {
		json_builder_begin_object(b);
		add_view_members(b, window, gsurf_window_get_nth_view(window, i), i);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	gsurf_control_invocation_return_value(inv, json_builder_get_root(b));
}

static void
m_views_activate(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfWindow *window = get_window(server, &error);
	GsurfView *view = window != NULL
		? resolve_view(window, params, "view", TRUE, FALSE, &error) : NULL;

	(void)data;
	if (view == NULL) {
		fail(inv, error);
		return;
	}
	gsurf_window_set_active_view(window, view);
	gsurf_control_invocation_return_value(inv, view_node(window, view));
}

static void
m_views_close(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfWindow *window = get_window(server, &error);
	GsurfView *view = window != NULL
		? resolve_view(window, params, "view", TRUE, TRUE, &error) : NULL;
	g_autoptr(JsonBuilder) b = NULL;
	guint64 id;

	(void)data;
	if (view == NULL) {
		fail(inv, error);
		return;
	}
	if (gsurf_window_get_n_views(window) <= 1) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_FAILED,
			"cannot close the last view");
		return;
	}
	id = gsurf_view_get_id(view);
	gsurf_window_remove_view(window, view);
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "closed");
	json_builder_add_int_value(b, (gint64)id);
	json_builder_end_object(b);
	gsurf_control_invocation_return_value(inv, json_builder_get_root(b));
}

/* ------------------------------------------------------------------ */
/* page.*                                                              */
/* ------------------------------------------------------------------ */

static void
m_page_uri(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GError *error = NULL;
	GsurfView *view = target_view(server, gsurf_control_invocation_get_params(inv), NULL,
		&error);

	(void)data;
	if (view == NULL) {
		fail(inv, error);
		return;
	}
	gsurf_control_invocation_return_value(inv, string_node(gsurf_view_get_uri(view)));
}

static void
m_page_title(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GError *error = NULL;
	GsurfView *view = target_view(server, gsurf_control_invocation_get_params(inv), NULL,
		&error);

	(void)data;
	if (view == NULL) {
		fail(inv, error);
		return;
	}
	gsurf_control_invocation_return_value(inv, string_node(gsurf_view_get_title(view)));
}

/* A script run for page.text / page.eval; holds the invocation until
 * the page answers. */
typedef struct {
	GsurfControlInvocation *inv;
	GsurfView              *view;     /* ref */
	gchar                  *script;   /* the raw script, for the CSP fallback */
	gboolean                json;     /* the result is JSON text */
	gboolean                retried;
} JsRun;

static void
js_run_free(JsRun *run)
{
	g_clear_object(&run->inv);
	g_clear_object(&run->view);
	g_free(run->script);
	g_free(run);
}

static void js_done(GObject *source, GAsyncResult *result, gpointer user_data);

static void
js_start(JsRun *run, const gchar *script)
{
	gsurf_view_run_javascript_async(run->view, script, NULL, js_done, run);
}

static void
js_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	JsRun *run = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;

	(void)source;
	text = gsurf_view_run_javascript_finish(run->view, result, &error);

	/* 1. A page whose CSP forbids eval(): run the script as it is, and
	 *    answer its value as text. */
	if (text == NULL && run->json && !run->retried && error != NULL &&
	    (strstr(error->message, "EvalError") != NULL ||
	     strstr(error->message, "unsafe-eval") != NULL)) {
		run->retried = TRUE;
		run->json = FALSE;
		js_start(run, run->script);
		return;
	}
	if (text == NULL) {
		gsurf_control_invocation_return_error(run->inv, GSURF_CONTROL_ERROR_FAILED,
			"javascript failed: %s", error != NULL ? error->message : "no result");
		js_run_free(run);
		return;
	}

	/* 2. The value: parsed JSON for page.eval, text otherwise. */
	if (run->json) {
		g_autoptr(JsonParser) parser = json_parser_new();

		if (json_parser_load_from_data(parser, text, -1, NULL) &&
		    json_parser_get_root(parser) != NULL) {
			gsurf_control_invocation_return_value(run->inv,
				json_node_copy(json_parser_get_root(parser)));
			js_run_free(run);
			return;
		}
	}
	gsurf_control_invocation_return_value(run->inv, string_node(text));
	js_run_free(run);
}

static void
m_page_text(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GError *error = NULL;
	GsurfView *view = target_view(server, gsurf_control_invocation_get_params(inv), NULL,
		&error);
	JsRun *run;

	(void)data;
	if (view == NULL) {
		fail(inv, error);
		return;
	}
	run = g_new0(JsRun, 1);
	run->inv = g_object_ref(inv);
	run->view = g_object_ref(view);
	js_start(run, "document.body ? document.body.innerText : ''");
}

static void
m_page_eval(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfView *view;
	const gchar *script = NULL;
	g_autoptr(JsonNode) literal = NULL;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *wrapped = NULL;
	JsRun *run;

	(void)data;
	if (!arg_string(params, "script", &script, &error) ||
	    kiosk_refuses("running scripts", &error)) {
		fail(inv, error);
		return;
	}
	if (script == NULL || *script == '\0') {
		gsurf_control_invocation_return_error_literal(inv,
			GSURF_CONTROL_ERROR_INVALID_PARAMS, "missing 'script'");
		return;
	}
	view = target_view(server, params, NULL, &error);
	if (view == NULL) {
		fail(inv, error);
		return;
	}

	/* The script runs through eval() so statements work and its last
	 * value comes back; JSON.stringify makes that value exact (a DOM
	 * node or a cycle falls back to its string form). */
	literal = string_node(script);
	quoted = json_to_string(literal, FALSE);
	wrapped = g_strdup_printf(
		"(function(){var r=(0,eval)(%s);var s;"
		"try{s=JSON.stringify(r===undefined?null:r);}catch(e){s=JSON.stringify(String(r));}"
		"return s===undefined?'null':s;})()", quoted);
	run = g_new0(JsRun, 1);
	run->inv = g_object_ref(inv);
	run->view = g_object_ref(view);
	run->script = g_strdup(script);
	run->json = TRUE;
	js_start(run, wrapped);
}

typedef struct {
	GsurfControlInvocation *inv;
	GsurfView              *view;
	gchar                  *path;
} Shot;

static void
shot_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Shot *shot = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(JsonBuilder) b = NULL;
	gconstpointer png;
	gsize size = 0;

	(void)source;
	bytes = gsurf_view_get_snapshot_finish(shot->view, result, &error);
	if (bytes == NULL) {
		gsurf_control_invocation_return_error(shot->inv, GSURF_CONTROL_ERROR_FAILED,
			"snapshot failed: %s", error != NULL ? error->message : "no image");
		goto out;
	}
	png = g_bytes_get_data(bytes, &size);
	if (!g_file_set_contents_full(shot->path, png, (gssize)size,
			G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error)) {
		gsurf_control_invocation_return_error(shot->inv, GSURF_CONTROL_ERROR_FAILED,
			"cannot write %s: %s", shot->path, error->message);
		goto out;
	}
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "path");
	json_builder_add_string_value(b, shot->path);
	json_builder_set_member_name(b, "bytes");
	json_builder_add_int_value(b, (gint64)size);
	json_builder_end_object(b);
	gsurf_control_invocation_return_value(shot->inv, json_builder_get_root(b));
out:
	g_object_unref(shot->inv);
	g_object_unref(shot->view);
	g_free(shot->path);
	g_free(shot);
}

static void
m_page_screenshot(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfView *view;
	const gchar *path = NULL;
	Shot *shot;

	(void)data;
	if (!arg_string(params, "path", &path, &error)) {
		fail(inv, error);
		return;
	}
	if (path == NULL || *path == '\0') {
		gsurf_control_invocation_return_error_literal(inv,
			GSURF_CONTROL_ERROR_INVALID_PARAMS, "missing 'path' (the PNG file to write)");
		return;
	}
	view = target_view(server, params, NULL, &error);
	if (view == NULL) {
		fail(inv, error);
		return;
	}
	shot = g_new0(Shot, 1);
	shot->inv = g_object_ref(inv);
	shot->view = g_object_ref(view);
	/* Relative paths resolve against the directory gsurf started in. */
	shot->path = gsurf_resolve_path(path);
	gsurf_view_get_snapshot_async(view, NULL, shot_done, shot);
}

/* ------------------------------------------------------------------ */
/* Actions and modes                                                   */
/* ------------------------------------------------------------------ */

static void
m_actions_list(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	gboolean kiosk = gsurf_kiosk_is_enabled();
	gint a;

	(void)server;
	(void)data;
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
	gsurf_control_invocation_return_value(inv, json_builder_get_root(b));
}

static void
m_action_run(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	g_autoptr(JsonBuilder) b = NULL;
	GsurfWindow *window;
	GsurfView *view;
	GsurfAction action;
	const gchar *name = NULL;

	(void)data;
	if (!arg_string(params, "name", &name, &error)) {
		fail(inv, error);
		return;
	}
	if (name == NULL) {
		gsurf_control_invocation_return_error_literal(inv,
			GSURF_CONTROL_ERROR_INVALID_PARAMS, "missing 'name' (actions.list gives them)");
		return;
	}
	action = gsurf_action_from_string(name);
	if (action == GSURF_ACTION_NONE) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
			"unknown action '%s' (actions.list gives them)", name);
		return;
	}
	view = target_view(server, params, &window, &error);
	if (view == NULL ||
	    !gsurf_module_manager_execute_action(gsurf_module_manager_get_default(), window, view,
			action, &error)) {
		fail(inv, error);
		return;
	}
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "action");
	json_builder_add_string_value(b, gsurf_action_to_string(action));
	json_builder_set_member_name(b, "performed");
	json_builder_add_boolean_value(b, TRUE);
	json_builder_end_object(b);
	gsurf_control_invocation_return_value(inv, json_builder_get_root(b));
}

static gboolean
modal_active(void)
{
	GsurfModule *modal = gsurf_module_manager_get_module(gsurf_module_manager_get_default(),
		"modal");

	return modal != NULL && gsurf_module_is_active(modal);
}

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
	json_builder_set_member_name(b, "modal_active");
	json_builder_add_boolean_value(b, modal_active());
	json_builder_end_object(b);
	g_type_class_unref(klass);
	return json_builder_get_root(b);
}

static void
m_mode_get(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	(void)server;
	(void)data;
	gsurf_control_invocation_return_value(inv, mode_node());
}

static void
m_mode_set(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GError *error = NULL;
	GsurfWindow *window;
	GsurfView *view;
	GsurfModePolicy want;
	GsurfAction action;
	const gchar *mode = NULL;

	(void)data;
	if (!arg_string(params, "mode", &mode, &error)) {
		fail(inv, error);
		return;
	}
	if (g_strcmp0(mode, "normal") == 0) {
		want = GSURF_MODE_NORMAL;
		action = GSURF_ACTION_ENTER_NORMAL_MODE;
	} else if (g_strcmp0(mode, "insert") == 0) {
		want = GSURF_MODE_INSERT;
		action = GSURF_ACTION_ENTER_INSERT_MODE;
	} else {
		gsurf_control_invocation_return_error_literal(inv,
			GSURF_CONTROL_ERROR_INVALID_PARAMS, "'mode' must be \"normal\" or \"insert\"");
		return;
	}
	if (!modal_active()) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_UNSUPPORTED,
			"input modes need the modal module (modules.modal.enabled: true)");
		return;
	}
	view = target_view(server, params, &window, &error);
	if (view == NULL || !gsurf_module_manager_execute_action(mgr, window, view, action, &error)) {
		fail(inv, error);
		return;
	}
	if (gsurf_module_manager_get_mode(mgr) != want) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_FAILED,
			"the mode did not change to %s", mode);
		return;
	}
	gsurf_control_invocation_return_value(inv, mode_node());
}

/* ------------------------------------------------------------------ */
/* tree.*                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
	GsurfWindow          *window;
	GsurfTabTreeProvider *provider;
	GPtrArray            *views;    /* GsurfView*, borrowed, window order */
	GArray               *parents;  /* guint64, parallel to views */
	GHashTable           *in_pane;  /* view ids shown by a pane */
} Tree;

static void
tree_init(Tree *t, GsurfWindow *window)
{
	guint i;

	t->window = window;
	t->provider = tree_provider();
	t->views = g_ptr_array_new();
	t->parents = g_array_new(FALSE, FALSE, sizeof(guint64));
	t->in_pane = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	for (i = 0; i < gsurf_window_get_n_views(window); i++) {
		GsurfView *view = gsurf_window_get_nth_view(window, i);
		guint64 parent = tree_parent(window, view);

		g_ptr_array_add(t->views, view);
		g_array_append_val(t->parents, parent);
	}
}

static void
tree_clear(Tree *t)
{
	g_clear_pointer(&t->views, g_ptr_array_unref);
	g_clear_pointer(&t->parents, g_array_unref);
	g_clear_pointer(&t->in_pane, g_hash_table_unref);
}

static gboolean
tree_in_pane(Tree *t, guint64 id)
{
	gint64 key = (gint64)id;

	return g_hash_table_contains(t->in_pane, &key);
}

/* One node: the view's members, "collapsed" and its children. With
 * @skip_panes, children shown in a pane are left out. */
static void
tree_add_node(Tree *t, JsonBuilder *b, guint index, gboolean skip_panes, gint pane)
{
	GsurfView *view = g_ptr_array_index(t->views, index);
	guint64 id = gsurf_view_get_id(view);
	guint i;

	json_builder_begin_object(b);
	add_view_members(b, t->window, view, index);
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
		if (g_array_index(t->parents, guint64, i) != id)
			continue;
		if (skip_panes && tree_in_pane(t, gsurf_view_get_id(g_ptr_array_index(t->views, i))))
			continue;
		tree_add_node(t, b, i, skip_panes, -1);
	}
	json_builder_end_array(b);
	json_builder_end_object(b);
}

static gint
tree_index(Tree *t, guint64 id)
{
	guint i;

	for (i = 0; i < t->views->len; i++)
		if (gsurf_view_get_id(g_ptr_array_index(t->views, i)) == id)
			return (gint)i;
	return -1;
}

/* The tabs module's label for the background group. */
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

/* {"index", "name", "current", "background", "tabs": [...]} of a group. */
static void
add_group_tabs(Tree *t, JsonBuilder *b, GsurfLayoutGroup *group)
{
	guint n_ids;
	const guint64 *ids = gsurf_layout_group_get_view_ids(group, &n_ids);
	guint i;

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
		gint index = tree_index(t, ids[i]);

		if (index >= 0)
			tree_add_node(t, b, (guint)index, TRUE, (gint)i);
	}
	json_builder_end_array(b);
	json_builder_end_object(b);
}

static void
m_tree_get(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GError *error = NULL;
	GsurfWindow *window = get_window(server, &error);
	GsurfWindowLayout *layout;
	g_autoptr(GPtrArray) groups = NULL;
	g_autoptr(JsonBuilder) b = NULL;
	Tree t = { 0 };
	gboolean any_background = FALSE;
	guint i;
	guint g;

	(void)data;
	if (window == NULL) {
		fail(inv, error);
		return;
	}
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

	/* 2. The plain tree: the roots in window order. */
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "has_tree_provider");
	json_builder_add_boolean_value(b, t.provider != NULL);
	json_builder_set_member_name(b, "tree");
	json_builder_begin_array(b);
	for (i = 0; i < t.views->len; i++)
		if (g_array_index(t.parents, guint64, i) == 0)
			tree_add_node(&t, b, i, FALSE, -1);
	json_builder_end_array(b);

	/* 3. Grouped by wmux window, as the sidebar shows it, then the
	 *    background tabs under no pane tab. */
	json_builder_set_member_name(b, "grouped");
	json_builder_add_boolean_value(b, groups != NULL && groups->len > 0);
	json_builder_set_member_name(b, "groups");
	if (groups == NULL || groups->len == 0) {
		json_builder_add_null_value(b);
	} else {
		g_autofree gchar *label = background_label();

		json_builder_begin_array(b);
		for (g = 0; g < groups->len; g++)
			add_group_tabs(&t, b, g_ptr_array_index(groups, g));
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
	gsurf_control_invocation_return_value(inv, json_builder_get_root(b));
}

static void
m_tree_move(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	static const gchar *const places[] = { "parent", "before", "after", "index" };
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GsurfTabTreeProvider *provider = tree_provider();
	GError *error = NULL;
	GsurfWindow *window;
	GsurfView *view;
	GsurfView *target = NULL;
	const gchar *place = NULL;
	gint64 value = 0;
	guint i;

	(void)data;
	window = get_window(server, &error);
	view = window != NULL ? resolve_view(window, params, "view", FALSE, FALSE, &error) : NULL;
	if (view == NULL) {
		fail(inv, error);
		return;
	}

	/* 1. Exactly one destination. */
	for (i = 0; i < G_N_ELEMENTS(places); i++) {
		if (!arg_has(params, places[i]))
			continue;
		if (place != NULL) {
			gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_INVALID_PARAMS,
				"give only one of 'parent', 'before', 'after' and 'index' (got '%s' and "
				"'%s')", place, places[i]);
			return;
		}
		place = places[i];
	}
	if (place == NULL) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_INVALID_PARAMS,
			"missing destination: give 'parent', 'before', 'after' or 'index'");
		return;
	}
	if (!arg_int(params, place, &value, &error)) {
		fail(inv, error);
		return;
	}

	/* 2. A window position: a plain reorder. */
	if (g_strcmp0(place, "index") == 0) {
		if (value < 0) {
			gsurf_control_invocation_return_error_literal(inv,
				GSURF_CONTROL_ERROR_INVALID_PARAMS, "'index' must be 0 or more");
			return;
		}
		gsurf_window_move_view(window, view, (guint)value);
		gsurf_control_invocation_return_value(inv, view_node(window, view));
		return;
	}

	/* 3. Relative to another view (parent 0: a root at the end). */
	if (!(g_strcmp0(place, "parent") == 0 && value == 0)) {
		target = view_by_id(window, value, &error);
		if (target == NULL) {
			fail(inv, error);
			return;
		}
		if (target == view) {
			gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_INVALID_PARAMS,
				"cannot move view %" G_GUINT64_FORMAT " relative to itself",
				gsurf_view_get_id(view));
			return;
		}
	}
	if (provider != NULL) {
		GsurfTabTreePosition position = g_strcmp0(place, "before") == 0
			? GSURF_TAB_TREE_POSITION_BEFORE
			: g_strcmp0(place, "after") == 0 ? GSURF_TAB_TREE_POSITION_AFTER
			: GSURF_TAB_TREE_POSITION_CHILD;

		if (!gsurf_tab_tree_provider_move_view(provider, window, view, target, position)) {
			gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_FAILED,
				"cannot move view %" G_GUINT64_FORMAT " (a view cannot move into its own "
				"subtree)", gsurf_view_get_id(view));
			return;
		}
		gsurf_control_invocation_return_value(inv, view_node(window, view));
		return;
	}

	/* 4. No tree: only sibling moves, as window reorders. */
	if (g_strcmp0(place, "parent") == 0) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_UNSUPPORTED,
			"re-parenting needs a tab tree: enable the tabs module "
			"(modules.tabs.enabled: true)");
		return;
	}
	{
		gint from = gsurf_window_get_view_index(window, view);
		gint to = gsurf_window_get_view_index(window, target);

		if (g_strcmp0(place, "before") == 0)
			to = from < to ? to - 1 : to;
		else
			to = from < to ? to : to + 1;
		gsurf_window_move_view(window, view, (guint)MAX(to, 0));
	}
	gsurf_control_invocation_return_value(inv, view_node(window, view));
}

static void
m_tree_collapse(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GsurfTabTreeProvider *provider = tree_provider();
	GError *error = NULL;
	GsurfWindow *window = get_window(server, &error);
	gboolean collapsed = TRUE;
	GsurfView *view;

	(void)data;
	view = window != NULL ? resolve_view(window, params, "view", FALSE, TRUE, &error) : NULL;
	if (view == NULL || !arg_bool(params, "collapsed", &collapsed, &error)) {
		fail(inv, error);
		return;
	}
	if (provider == NULL) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_UNSUPPORTED,
			"collapsing needs a tab tree: enable the tabs module (modules.tabs.enabled: true)");
		return;
	}
	if (!gsurf_tab_tree_provider_set_collapsed(provider, window, view, collapsed)) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_FAILED,
			"view %" G_GUINT64_FORMAT " has no children to %s", gsurf_view_get_id(view),
			collapsed ? "collapse" : "expand");
		return;
	}
	gsurf_control_invocation_return_value(inv, view_node(window, view));
}

/* ------------------------------------------------------------------ */
/* containers.*                                                        */
/* ------------------------------------------------------------------ */

/* The containers module's configured entry for @container. */
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

static void
add_container(JsonBuilder *b, GsurfContainer *c, guint n_default, gboolean rules_active)
{
	YamlMapping *entry = c != NULL ? container_config(c) : NULL;
	YamlSequence *hosts = NULL;
	guint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "id");
	json_builder_add_string_value(b, c != NULL ? gsurf_container_get_id(c)
		: GSURF_CONTAINER_DEFAULT);
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b, c != NULL ? gsurf_container_get_name(c) : "Default");
	add_string_or_null(b, "color", c != NULL ? gsurf_container_get_color(c) : NULL);
	json_builder_set_member_name(b, "temporary");
	json_builder_add_boolean_value(b, c != NULL && gsurf_container_get_temporary(c));
	json_builder_set_member_name(b, "default");
	json_builder_add_boolean_value(b, c == NULL);
	json_builder_set_member_name(b, "n_views");
	json_builder_add_int_value(b, c != NULL ? gsurf_container_get_n_views(c) : n_default);
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
	json_builder_set_member_name(b, "rules_active");
	json_builder_add_boolean_value(b, rules_active);
	json_builder_end_object(b);
}

static void
m_containers_list(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	GsurfModule *policy = gsurf_module_manager_get_module(gsurf_module_manager_get_default(),
		"containers");
	gboolean rules_active = policy != NULL && gsurf_module_is_active(policy);
	GsurfWindow *window = gsurf_control_server_get_window(server);
	g_autoptr(GPtrArray) list = NULL;
	g_autoptr(JsonBuilder) b = json_builder_new();
	guint n_default = 0;
	guint i;

	(void)data;
	for (i = 0; window != NULL && i < gsurf_window_get_n_views(window); i++)
		if (gsurf_view_get_container_id(gsurf_window_get_nth_view(window, i)) == NULL)
			n_default++;
	json_builder_begin_array(b);
	add_container(b, NULL, n_default, rules_active);
	list = registry != NULL ? gsurf_container_registry_list(registry) : NULL;
	for (i = 0; list != NULL && i < list->len; i++)
		add_container(b, g_ptr_array_index(list, i), 0, rules_active);
	json_builder_end_array(b);
	gsurf_control_invocation_return_value(inv, json_builder_get_root(b));
}

static void
m_containers_reopen(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfWindow *window;
	GsurfView *view;
	GsurfView *fresh;
	const gchar *container = NULL;

	(void)data;
	if (kiosk_refuses("reopening views", &error) ||
	    !arg_string(params, "container", &container, &error)) {
		fail(inv, error);
		return;
	}
	view = target_view(server, params, &window, &error);
	if (view == NULL) {
		fail(inv, error);
		return;
	}
	if (container == NULL) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_INVALID_PARAMS,
			"missing 'container' (an id, \"temporary\" or \"default\")");
		return;
	}
	if (!check_container(container, &error)) {
		fail(inv, error);
		return;
	}
	/* The new view takes the old one's place, opener, children and pane;
	 * the old view closes and its id is gone. */
	fresh = gsurf_window_reopen_view(window, view, container);
	if (fresh == NULL) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_FAILED,
			"could not reopen view %" G_GUINT64_FORMAT, gsurf_view_get_id(view));
		return;
	}
	gsurf_control_invocation_return_value(inv, view_node(window, fresh));
}

/* ------------------------------------------------------------------ */
/* layout.*                                                            */
/* ------------------------------------------------------------------ */

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

static GsurfLayoutGroup *
find_group(GPtrArray *groups, guint index)
{
	guint i;

	for (i = 0; i < groups->len; i++)
		if (gsurf_layout_group_get_index(g_ptr_array_index(groups, i)) == index)
			return g_ptr_array_index(groups, i);
	return NULL;
}

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

/* The window and its layout, or FALSE with @error set. */
static gboolean
window_layout(GsurfControlServer *server, GsurfWindow **window, GsurfWindowLayout **layout,
              GError **error)
{
	*layout = NULL;
	*window = get_window(server, error);
	if (*window == NULL)
		return FALSE;
	*layout = get_layout(*window, error);
	return *layout != NULL;
}

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

/* A group object: index, name, current, last, zoomed, panes. */
static void
add_group(JsonBuilder *b, GsurfWindowLayout *layout, GsurfWindow *window,
          GsurfLayoutGroup *group)
{
	guint index = gsurf_layout_group_get_index(group);
	g_autoptr(GPtrArray) panes = gsurf_window_layout_list_panes(layout, window, index);
	guint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "index");
	json_builder_add_int_value(b, index);
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b, gsurf_layout_group_get_name(group) != NULL
		? gsurf_layout_group_get_name(group) : "");
	json_builder_set_member_name(b, "current");
	json_builder_add_boolean_value(b, gsurf_layout_group_get_current(group));
	json_builder_set_member_name(b, "last");
	json_builder_add_boolean_value(b, gsurf_layout_group_get_last(group));
	json_builder_set_member_name(b, "zoomed");
	json_builder_add_boolean_value(b, gsurf_layout_group_get_zoomed(group));
	json_builder_set_member_name(b, "panes");
	json_builder_begin_array(b);
	for (i = 0; panes != NULL && i < panes->len; i++)
		add_pane(b, window, g_ptr_array_index(panes, i), gsurf_layout_group_get_current(group),
			gsurf_layout_group_get_zoomed(group));
	json_builder_end_array(b);
	json_builder_end_object(b);
}

/* The group object for @index (-1: the current group). */
static JsonNode *
group_node(GsurfWindowLayout *layout, GsurfWindow *window, gint64 index, GError **error)
{
	g_autoptr(GPtrArray) groups = get_groups(layout, window);
	g_autoptr(JsonBuilder) b = NULL;
	GsurfLayoutGroup *group;

	if (index < 0)
		index = current_group(layout, window);
	group = find_group(groups, (guint)index);
	if (group == NULL) {
		g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_NOT_FOUND,
			"unknown wmux window %" G_GINT64_FORMAT " (there are %u; layout.groups gives "
			"the indices)", index, groups->len);
		return NULL;
	}
	b = json_builder_new();
	add_group(b, layout, window, group);
	return json_builder_get_root(b);
}

static void
m_layout_describe(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GsurfModule *wmux = gsurf_module_manager_get_module(gsurf_module_manager_get_default(),
		"wmux");
	GsurfWindow *window = gsurf_control_server_get_window(server);
	GsurfWindowLayout *layout = window != NULL ? gsurf_window_get_layout(window) : NULL;
	g_autoptr(GVariant) info = NULL;
	g_autoptr(JsonBuilder) b = json_builder_new();

	(void)data;
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "loaded");
	json_builder_add_boolean_value(b, wmux != NULL);
	json_builder_set_member_name(b, "enabled");
	json_builder_add_boolean_value(b, wmux != NULL && gsurf_module_is_active(wmux));
	json_builder_set_member_name(b, "active");
	json_builder_add_boolean_value(b, layout != NULL);
	json_builder_set_member_name(b, "kiosk");
	json_builder_add_boolean_value(b, gsurf_kiosk_is_enabled());
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
	gsurf_control_invocation_return_value(inv, json_builder_get_root(b));
}

static void
m_layout_groups(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GError *error = NULL;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	g_autoptr(GPtrArray) groups = NULL;
	g_autoptr(JsonBuilder) b = NULL;
	guint i;

	(void)data;
	if (!window_layout(server, &window, &layout, &error)) {
		fail(inv, error);
		return;
	}
	groups = get_groups(layout, window);
	b = json_builder_new();
	json_builder_begin_array(b);
	for (i = 0; i < groups->len; i++)
		add_group(b, layout, window, g_ptr_array_index(groups, i));
	json_builder_end_array(b);
	gsurf_control_invocation_return_value(inv, json_builder_get_root(b));
}

static void
m_layout_panes(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	JsonNode *node;
	gint64 index = -1;

	(void)data;
	if (!window_layout(server, &window, &layout, &error) ||
	    !arg_int(params, "window", &index, &error)) {
		fail(inv, error);
		return;
	}
	if (arg_has(params, "window") && index < 0) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_INVALID_PARAMS,
			"'window' must be 0 or more");
		return;
	}
	node = group_node(layout, window, index, &error);
	if (node == NULL) {
		fail(inv, error);
		return;
	}
	gsurf_control_invocation_return_value(inv, node);
}

static void
m_layout_command(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	g_autoptr(GError) local = NULL;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	JsonNode *node;
	const gchar *command = NULL;

	(void)data;
	if (!window_layout(server, &window, &layout, &error) ||
	    !arg_string(params, "command", &command, &error)) {
		fail(inv, error);
		return;
	}
	if (command == NULL || *command == '\0') {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_INVALID_PARAMS,
			"missing 'command' (a wmux command line, e.g. \"split-window -h\")");
		return;
	}
	if (!gsurf_window_layout_run_command(layout, window, command, &local)) {
		gsurf_control_invocation_return_error(inv,
			local->domain == G_IO_ERROR && local->code == G_IO_ERROR_NOT_SUPPORTED
				? GSURF_CONTROL_ERROR_UNSUPPORTED : GSURF_CONTROL_ERROR_FAILED,
			"%s: %s", command, local->message);
		return;
	}
	/* The current window as it is now (the command may have killed the
	 * last pane of another one, or changed windows). */
	node = group_node(layout, window, -1, &error);
	if (node == NULL) {
		fail(inv, error);
		return;
	}
	gsurf_control_invocation_return_value(inv, node);
}

static void
m_layout_select_group(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	JsonNode *node;
	gint64 index = -1;

	(void)data;
	if (!window_layout(server, &window, &layout, &error) ||
	    !arg_int(params, "window", &index, &error)) {
		fail(inv, error);
		return;
	}
	if (index < 0) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_INVALID_PARAMS,
			"missing 'window' (a wmux window index, layout.groups gives them)");
		return;
	}
	if (!gsurf_window_layout_select_group(layout, window, (guint)index)) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
			"unknown wmux window %" G_GINT64_FORMAT " (layout.groups gives the indices)",
			index);
		return;
	}
	node = group_node(layout, window, index, &error);
	if (node == NULL) {
		fail(inv, error);
		return;
	}
	gsurf_control_invocation_return_value(inv, node);
}

static void
m_layout_rename_group(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	JsonNode *node;
	const gchar *name = NULL;
	gint64 index = -1;

	(void)data;
	if (!window_layout(server, &window, &layout, &error) ||
	    !arg_int(params, "window", &index, &error) ||
	    !arg_string(params, "name", &name, &error)) {
		fail(inv, error);
		return;
	}
	if (index < 0)
		index = current_group(layout, window);
	if (!gsurf_window_layout_rename_group(layout, window, (guint)index, name)) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
			"unknown wmux window %" G_GINT64_FORMAT " (layout.groups gives the indices)",
			index);
		return;
	}
	node = group_node(layout, window, index, &error);
	if (node == NULL) {
		fail(inv, error);
		return;
	}
	gsurf_control_invocation_return_value(inv, node);
}

static void
m_layout_show_view_at(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	static const struct {
		const gchar        *name;
		GsurfLayoutDropZone zone;
	} zones[] = {
		{ "center", GSURF_LAYOUT_DROP_CENTER }, { "centre", GSURF_LAYOUT_DROP_CENTER },
		{ "left", GSURF_LAYOUT_DROP_LEFT }, { "right", GSURF_LAYOUT_DROP_RIGHT },
		{ "top", GSURF_LAYOUT_DROP_TOP }, { "bottom", GSURF_LAYOUT_DROP_BOTTOM },
	};
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	g_autoptr(GPtrArray) panes = NULL;
	GsurfLayoutDropZone zone = GSURF_LAYOUT_DROP_CENTER;
	GsurfWindowLayout *layout;
	GsurfWindow *window;
	GsurfView *view;
	GsurfView *target = NULL;
	JsonNode *node;
	const gchar *zone_name = NULL;
	gint64 pane = -1;
	gint64 win = -1;
	gboolean known;
	guint i;

	(void)data;
	if (!window_layout(server, &window, &layout, &error) ||
	    !arg_string(params, "zone", &zone_name, &error) ||
	    !arg_int(params, "pane", &pane, &error) || !arg_int(params, "window", &win, &error)) {
		fail(inv, error);
		return;
	}
	view = resolve_view(window, params, "view", FALSE, FALSE, &error);
	if (view == NULL) {
		fail(inv, error);
		return;
	}

	/* 1. The zone. */
	known = zone_name == NULL;
	for (i = 0; zone_name != NULL && i < G_N_ELEMENTS(zones); i++) {
		if (g_ascii_strcasecmp(zone_name, zones[i].name) == 0) {
			zone = zones[i].zone;
			known = TRUE;
		}
	}
	if (!known) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_INVALID_PARAMS,
			"'zone' must be center, left, right, top or bottom");
		return;
	}

	/* 2. The target: a view on screen ("target"), or a pane of the
	 *    current (or given) wmux window. */
	if (arg_has(params, "target")) {
		target = resolve_view(window, params, "target", FALSE, FALSE, &error);
		if (target == NULL) {
			fail(inv, error);
			return;
		}
	} else {
		if (pane < 0) {
			gsurf_control_invocation_return_error_literal(inv,
				GSURF_CONTROL_ERROR_INVALID_PARAMS,
				"give 'pane' (a pane index) or 'target' (a view id on screen)");
			return;
		}
		if (win >= 0 && (guint)win != current_group(layout, window) &&
		    !gsurf_window_layout_select_group(layout, window, (guint)win)) {
			gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
				"unknown wmux window %" G_GINT64_FORMAT, win);
			return;
		}
		panes = gsurf_window_layout_list_panes(layout, window, current_group(layout, window));
		for (i = 0; panes != NULL && i < panes->len && target == NULL; i++) {
			GsurfLayoutPane *p = g_ptr_array_index(panes, i);

			if ((gint64)gsurf_layout_pane_get_index(p) == pane &&
			    gsurf_layout_pane_get_view_id(p) != 0)
				target = gsurf_view_lookup(gsurf_layout_pane_get_view_id(p));
		}
		if (target == NULL) {
			gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
				"no pane %" G_GINT64_FORMAT " in the current wmux window (layout.panes "
				"gives the indices)", pane);
			return;
		}
	}
	if (!gsurf_window_get_view_visible(window, target)) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_INVALID_PARAMS,
			"view %" G_GUINT64_FORMAT " is not on screen", gsurf_view_get_id(target));
		return;
	}

	/* 3. The drop. */
	if (!gsurf_window_layout_show_view_at(layout, window, view, target, zone)) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_FAILED,
			"could not show view %" G_GUINT64_FORMAT " at view %" G_GUINT64_FORMAT
			" (a pane's own view only accepts the center zone; a split must leave both "
			"panes above the minimum size)", gsurf_view_get_id(view),
			gsurf_view_get_id(target));
		return;
	}
	node = group_node(layout, window, -1, &error);
	if (node == NULL) {
		fail(inv, error);
		return;
	}
	gsurf_control_invocation_return_value(inv, node);
}

/* ------------------------------------------------------------------ */
/* config.get, modules.*                                               */
/* ------------------------------------------------------------------ */

/* A scalar as JSON: true/false, an integer or a number when it reads as
 * one (and was not quoted), else a string. */
static JsonNode *
scalar_to_json(YamlNode *node)
{
	const gchar *text = yaml_node_get_scalar(node);
	JsonNode *out = json_node_new(JSON_NODE_VALUE);
	gboolean quoted = yaml_node_get_scalar_style(node) == YAML_SCALAR_STYLE_SINGLE_QUOTED ||
		yaml_node_get_scalar_style(node) == YAML_SCALAR_STYLE_DOUBLE_QUOTED;
	gchar *end = NULL;
	gint64 i;
	gdouble d;

	if (text == NULL) {
		json_node_init_null(out);
		return out;
	}
	if (!quoted) {
		if (g_strcmp0(text, "true") == 0 || g_strcmp0(text, "false") == 0) {
			json_node_set_boolean(out, text[0] == 't');
			return out;
		}
		if (*text != '\0') {
			i = g_ascii_strtoll(text, &end, 10);
			if (end != NULL && *end == '\0') {
				json_node_set_int(out, i);
				return out;
			}
			d = g_ascii_strtod(text, &end);
			if (end != NULL && *end == '\0') {
				json_node_set_double(out, d);
				return out;
			}
		}
	}
	json_node_set_string(out, text);
	return out;
}

JsonNode *
gsurf_control_yaml_to_json(gpointer yaml_node)
{
	YamlNode *node = yaml_node;
	JsonNode *out;
	guint i;

	if (node == NULL)
		return json_node_new(JSON_NODE_NULL);
	switch (yaml_node_get_node_type(node)) {
	case YAML_NODE_MAPPING: {
		YamlMapping *m = yaml_node_get_mapping(node);
		JsonObject *object = json_object_new();

		for (i = 0; i < yaml_mapping_get_size(m); i++)
			json_object_set_member(object, yaml_mapping_get_key(m, i),
				gsurf_control_yaml_to_json(yaml_mapping_get_value(m, i)));
		out = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(out, object);
		return out;
	}
	case YAML_NODE_SEQUENCE: {
		YamlSequence *s = yaml_node_get_sequence(node);
		JsonArray *array = json_array_new();

		for (i = 0; i < yaml_sequence_get_length(s); i++)
			json_array_add_element(array,
				gsurf_control_yaml_to_json(yaml_sequence_get_element(s, i)));
		out = json_node_new(JSON_NODE_ARRAY);
		json_node_take_array(out, array);
		return out;
	}
	case YAML_NODE_SCALAR:
		return scalar_to_json(node);
	case YAML_NODE_NULL:
	default:
		return json_node_new(JSON_NODE_NULL);
	}
}

/* The core keys config.get knows, as one object. */
static JsonNode *
core_config(GsurfConfig *c)
{
	g_autoptr(JsonBuilder) b = json_builder_new();

	json_builder_begin_object(b);
#define ADD_STR(k, v) add_string_or_null(b, k, v)
#define ADD_INT(k, v) G_STMT_START { json_builder_set_member_name(b, k); \
	json_builder_add_int_value(b, (v)); } G_STMT_END
#define ADD_DBL(k, v) G_STMT_START { json_builder_set_member_name(b, k); \
	json_builder_add_double_value(b, (v)); } G_STMT_END
#define ADD_BOOL(k, v) G_STMT_START { json_builder_set_member_name(b, k); \
	json_builder_add_boolean_value(b, (v)); } G_STMT_END
	ADD_STR("browser.homepage", c->homepage);
	ADD_STR("browser.new_view_uri", c->new_view_uri);
	ADD_DBL("browser.default_zoom", c->default_zoom);
	ADD_BOOL("browser.smooth_scroll", c->smooth_scroll);
	ADD_BOOL("browser.fullscreen_on_start", c->fullscreen_on_start);
	ADD_STR("user_agent", c->user_agent);
	ADD_STR("window.title", c->window_title);
	ADD_INT("window.width", c->window_width);
	ADD_INT("window.height", c->window_height);
	ADD_STR("storage.cookie_jar", c->cookie_jar);
	ADD_STR("storage.data_dir", c->data_dir);
	ADD_STR("storage.cache_dir", c->cache_dir);
	ADD_BOOL("storage.enable_disk_cache", c->enable_disk_cache);
	ADD_STR("network.proxy.mode", c->proxy_mode);
	ADD_STR("network.proxy.uri", c->proxy_uri);
	ADD_BOOL("network.do_not_track", c->do_not_track);
	ADD_BOOL("tls.strict", c->tls_strict);
	ADD_BOOL("ephemeral", c->ephemeral);
	ADD_BOOL("kiosk", c->kiosk);
	ADD_BOOL("control.enabled", c->control_enabled);
	ADD_STR("control.name", c->control_name);
	ADD_STR("lrg.arrangement", c->lrg_arrangement);
	ADD_STR("lrg.environment", c->lrg_environment);
	ADD_DBL("lrg.background-fps", c->lrg_background_fps);
	ADD_INT("lrg.history-depth", c->lrg_history_depth);
	ADD_BOOL("lrg.reading-single-tab", c->lrg_reading_single_tab);
#undef ADD_STR
#undef ADD_INT
#undef ADD_DBL
#undef ADD_BOOL
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

static void
m_config_get(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GsurfConfig *config = gsurf_config_get_default();
	GError *error = NULL;
	g_autoptr(JsonNode) core = NULL;
	const gchar *key = NULL;

	(void)server;
	(void)data;
	if (!arg_string(params, "key", &key, &error)) {
		fail(inv, error);
		return;
	}
	if (config == NULL) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
			"no configuration");
		return;
	}

	/* 1. No key: the core keys. */
	core = core_config(config);
	if (key == NULL || *key == '\0') {
		gsurf_control_invocation_return_value(inv, g_steal_pointer(&core));
		return;
	}

	/* 2. A core key. */
	if (json_object_has_member(json_node_get_object(core), key)) {
		gsurf_control_invocation_return_value(inv,
			json_object_dup_member(json_node_get_object(core), key));
		return;
	}

	/* 3. modules[.<name>[.<key>...]]: the module's YAML section. */
	if (g_strcmp0(key, "modules") == 0 || g_str_has_prefix(key, "modules.")) {
		g_auto(GStrv) parts = g_strsplit(key, ".", -1);
		YamlNode *node = NULL;
		guint i;

		if (parts[1] == NULL) {
			GPtrArray *modules = gsurf_module_manager_get_modules(
				gsurf_module_manager_get_default());
			JsonObject *object = json_object_new();
			JsonNode *out = json_node_new(JSON_NODE_OBJECT);

			for (i = 0; modules != NULL && i < modules->len; i++) {
				const gchar *name = gsurf_module_get_name(g_ptr_array_index(modules, i));

				json_object_set_member(object, name, gsurf_control_yaml_to_json(
					gsurf_config_get_module_node(config, name)));
			}
			json_node_take_object(out, object);
			gsurf_control_invocation_return_value(inv, out);
			return;
		}
		node = gsurf_config_get_module_node(config, parts[1]);
		for (i = 2; node != NULL && parts[i] != NULL; i++) {
			YamlMapping *m;

			if (yaml_node_get_node_type(node) != YAML_NODE_MAPPING) {
				node = NULL;
				break;
			}
			m = yaml_node_get_mapping(node);
			node = yaml_mapping_has_member(m, parts[i])
				? yaml_mapping_get_member(m, parts[i]) : NULL;
		}
		if (node != NULL) {
			gsurf_control_invocation_return_value(inv, gsurf_control_yaml_to_json(node));
			return;
		}
	}
	gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
		"unknown config key '%s' (config.get without a key lists the core keys; "
		"modules.<name>.<key> reads a module's section)", key);
}

static JsonNode *
module_node(GsurfModule *module)
{
	g_autoptr(JsonBuilder) b = json_builder_new();

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b, gsurf_module_get_name(module));
	json_builder_set_member_name(b, "description");
	json_builder_add_string_value(b, gsurf_module_get_description(module) != NULL
		? gsurf_module_get_description(module) : "");
	json_builder_set_member_name(b, "enabled");
	json_builder_add_boolean_value(b, gsurf_module_get_enabled(module));
	json_builder_set_member_name(b, "active");
	json_builder_add_boolean_value(b, gsurf_module_is_active(module));
	json_builder_set_member_name(b, "priority");
	json_builder_add_int_value(b, gsurf_module_get_priority(module));
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

static void
m_modules_list(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GPtrArray *modules = gsurf_module_manager_get_modules(gsurf_module_manager_get_default());
	JsonArray *array = json_array_new();
	JsonNode *out = json_node_new(JSON_NODE_ARRAY);
	guint i;

	(void)server;
	(void)data;
	for (i = 0; modules != NULL && i < modules->len; i++)
		json_array_add_element(array, module_node(g_ptr_array_index(modules, i)));
	json_node_take_array(out, array);
	gsurf_control_invocation_return_value(inv, out);
}

static void
m_modules_toggle(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfModule *module;
	const gchar *name = NULL;
	gboolean enable;

	(void)data;
	if (kiosk_refuses("toggling modules", &error) ||
	    !arg_string(params, "name", &name, &error)) {
		fail(inv, error);
		return;
	}
	if (name == NULL) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_INVALID_PARAMS,
			"missing 'name' (modules.list gives them)");
		return;
	}
	module = gsurf_module_manager_get_module(gsurf_module_manager_get_default(), name);
	if (module == NULL) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
			"no module '%s' is loaded (modules.list gives them)", name);
		return;
	}
	/* Without "enabled" the module flips. */
	enable = !gsurf_module_is_active(module);
	if (!arg_bool(params, "enabled", &enable, &error)) {
		fail(inv, error);
		return;
	}
	gsurf_module_set_enabled(module, enable);
	if (enable && !gsurf_module_is_active(module))
		gsurf_module_activate(module);
	else if (!enable && gsurf_module_is_active(module))
		gsurf_module_deactivate(module);
	if (enable != gsurf_module_is_active(module)) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_FAILED,
			"module '%s' did not %s", name, enable ? "activate" : "deactivate");
		return;
	}
	/* Its control methods follow its state. */
	gsurf_control_server_refresh_providers(server);
	gsurf_control_invocation_return_value(inv, module_node(module));
}

/* ------------------------------------------------------------------ */
/* Table                                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* The --lrg=3d scene (lrg.*)                                          */
/* ------------------------------------------------------------------ */

#ifdef GSURF_HAVE_LRG_3D
/* The 3D scene of the active window, or UNSUPPORTED outside --lrg=3d. */
static GsurfLrgScene *
lrg_scene_for(GsurfControlServer *server, GsurfWindow **window_out, GError **error)
{
	GsurfWindow *window = get_window(server, error);
	GsurfLrgScene *scene = NULL;

	if (window_out != NULL)
		*window_out = window;
	if (window == NULL)
		return NULL;
	if (GSURF_IS_LRG_WINDOW(window))
		scene = gsurf_lrg_window_get_scene(GSURF_LRG_WINDOW(window));
	if (scene == NULL)
		g_set_error_literal(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_UNSUPPORTED,
			"this window is not a 3D scene (start gsurf with --lrg=3d)");
	return scene;
}

/* An optional finite number parameter; FALSE (with INVALID_PARAMS) if it is
 * present but not a number. */
static gboolean
arg_double(JsonObject *params, const gchar *name, gdouble *out, GError **error)
{
	JsonNode *node = member(params, name);

	if (node == NULL)
		return TRUE;
	if (JSON_NODE_HOLDS_VALUE(node)
	    && (json_node_get_value_type(node) == G_TYPE_DOUBLE
	        || json_node_get_value_type(node) == G_TYPE_INT64)) {
		*out = json_node_get_double(node);
		if (isfinite(*out))
			return TRUE;
	}
	g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_INVALID_PARAMS,
		"'%s' must be a number", name);
	return FALSE;
}

/* The scene's state, the reply of lrg.state and of every lrg.* change. */
static JsonNode *
lrg_state_node(GsurfWindow *window, GsurfLrgScene *scene)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_auto(GStrv) bookmarks = gsurf_lrg_scene_list_views(scene);
	GPtrArray *views = gsurf_window_get_views(window);
	guint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "mode");
	json_builder_add_string_value(b, "3d");
	json_builder_set_member_name(b, "arrangement");
	json_builder_add_string_value(b, gsurf_lrg_scene_get_arrangement(scene));
	json_builder_set_member_name(b, "environment");
	json_builder_add_string_value(b, gsurf_lrg_scene_get_environment(scene));
	json_builder_set_member_name(b, "reading");
	json_builder_add_boolean_value(b, gsurf_lrg_scene_get_reading(scene));
	json_builder_set_member_name(b, "history");
	json_builder_add_boolean_value(b, gsurf_lrg_scene_get_history(scene));
	json_builder_set_member_name(b, "animating");
	json_builder_add_boolean_value(b, gsurf_lrg_scene_is_animating(scene));
	json_builder_set_member_name(b, "panels");
	json_builder_add_int_value(b, gsurf_lrg_scene_get_panel_count(scene));
	json_builder_set_member_name(b, "background_fps");
	json_builder_add_double_value(b, gsurf_lrg_scene_get_background_fps(scene));
	json_builder_set_member_name(b, "history_depth");
	json_builder_add_int_value(b, gsurf_lrg_scene_get_history_limit(scene));

	/* Pinned tabs with their placement, so an agent can save and restore
	 * a layout (lrg.pin with "placement"). */
	json_builder_set_member_name(b, "pinned");
	json_builder_begin_array(b);
	for (i = 0; views != NULL && i < views->len; i++) {
		GsurfView *v = g_ptr_array_index(views, i);
		g_autofree gchar *placement = NULL;

		if (!gsurf_lrg_scene_is_view_pinned(scene, v))
			continue;
		placement = gsurf_lrg_scene_get_view_placement(scene, v);
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "view");
		json_builder_add_int_value(b, (gint64)gsurf_view_get_id(v));
		json_builder_set_member_name(b, "placement");
		json_builder_add_string_value(b, placement != NULL ? placement : "");
		json_builder_end_object(b);
	}
	json_builder_end_array(b);

	json_builder_set_member_name(b, "bookmarks");
	json_builder_begin_array(b);
	for (i = 0; bookmarks != NULL && bookmarks[i] != NULL; i++)
		json_builder_add_string_value(b, bookmarks[i]);
	json_builder_end_array(b);
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

/* Reply with the new state and announce the change as an lrg.* event. */
static void
lrg_reply(GsurfControlInvocation *inv, GsurfWindow *window, GsurfLrgScene *scene,
          const gchar *what)
{
	if (what != NULL)
		gsurf_lrg_window_notify_scene(GSURF_LRG_WINDOW(window), what);
	gsurf_control_invocation_return_value(inv, lrg_state_node(window, scene));
}

static void
m_lrg_state(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GError *error = NULL;
	GsurfWindow *window = NULL;
	GsurfLrgScene *scene = lrg_scene_for(server, &window, &error);

	(void)data;
	if (scene == NULL) {
		fail(inv, error);
		return;
	}
	lrg_reply(inv, window, scene, NULL);
}

/* lrg.arrange / lrg.environment: set by id, or step to the next one with
 * no id.  @data is 0 for the arrangement, 1 for the environment. */
static void
m_lrg_mode(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	gboolean environment = GPOINTER_TO_INT(data) == 1;
	const gchar *member_name = environment ? "environment" : "arrangement";
	GError *error = NULL;
	GsurfWindow *window = NULL;
	GsurfLrgScene *scene = lrg_scene_for(server, &window, &error);
	const gchar *id = NULL;

	if (scene == NULL || !arg_string(params, member_name, &id, &error)) {
		fail(inv, error);
		return;
	}
	if (id == NULL) {
		if (environment)
			gsurf_lrg_scene_cycle_environment(scene);
		else
			gsurf_lrg_scene_cycle_arrangement(scene);
	} else if (!(environment ? gsurf_lrg_scene_set_environment(scene, id)
	                         : gsurf_lrg_scene_set_arrangement(scene, id))) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
			environment
			? "unknown environment '%s' (studio, sky, workshop, cockpit, void)"
			: "unknown arrangement '%s' (carousel, grid, free, single-panel, stack, "
			  "per-window)", id);
		return;
	}
	lrg_reply(inv, window, scene, member_name);
}

/* lrg.reading / lrg.history: "enabled" sets, no "enabled" toggles.  @data
 * is 0 for reading mode, 1 for history depth. */
static void
m_lrg_toggle(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	gboolean history = GPOINTER_TO_INT(data) == 1;
	GError *error = NULL;
	GsurfWindow *window = NULL;
	GsurfLrgScene *scene = lrg_scene_for(server, &window, &error);
	gboolean on;

	if (scene == NULL) {
		fail(inv, error);
		return;
	}
	on = history ? !gsurf_lrg_scene_get_history(scene)
	             : !gsurf_lrg_scene_get_reading(scene);
	if (!arg_bool(params, "enabled", &on, &error)) {
		fail(inv, error);
		return;
	}
	if (history)
		gsurf_lrg_scene_set_history(scene, on);
	else
		gsurf_lrg_scene_set_reading(scene, on);
	lrg_reply(inv, window, scene, history ? "history" : "reading");
}

static void
m_lrg_focus(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfWindow *window = NULL;
	GsurfLrgScene *scene = lrg_scene_for(server, &window, &error);
	GsurfView *view;

	(void)data;
	if (scene == NULL) {
		fail(inv, error);
		return;
	}
	view = resolve_view(window, params, "view", TRUE, TRUE, &error);
	if (view == NULL) {
		fail(inv, error);
		return;
	}
	/* Fly first, so a view without a panel changes nothing; focusing a tab
	 * also makes it the active one, as Ctrl+click does. */
	if (!gsurf_lrg_scene_focus_view(scene, view)) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
			"that view has no panel yet (its first frame is still rendering)");
		return;
	}
	gsurf_window_set_active_view(window, view);
	lrg_reply(inv, window, scene, "focus");
}

/* lrg.pin: pin a view where it is, at a saved "placement", or ("enabled":
 * false) unpin it. */
static void
m_lrg_pin(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfWindow *window = NULL;
	GsurfLrgScene *scene = lrg_scene_for(server, &window, &error);
	const gchar *placement = NULL;
	gboolean on = TRUE;
	GsurfView *view;

	(void)data;
	if (scene == NULL || !arg_bool(params, "enabled", &on, &error)
	    || !arg_string(params, "placement", &placement, &error)) {
		fail(inv, error);
		return;
	}
	view = resolve_view(window, params, "view", TRUE, TRUE, &error);
	if (view == NULL) {
		fail(inv, error);
		return;
	}
	if (!on) {
		gsurf_lrg_scene_unpin_view(scene, view);
	} else if (placement != NULL) {
		if (!gsurf_lrg_scene_set_view_placement(scene, view, placement)) {
			gsurf_control_invocation_return_error_literal(inv,
				GSURF_CONTROL_ERROR_INVALID_PARAMS,
				"'placement' must be six numbers: x y z yaw width height");
			return;
		}
	} else if (!gsurf_lrg_scene_pin_view(scene, view)) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
			"that view has no panel yet (its first frame is still rendering)");
		return;
	}
	lrg_reply(inv, window, scene, "pin");
}

static void
m_lrg_unpin_all(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	GError *error = NULL;
	GsurfWindow *window = NULL;
	GsurfLrgScene *scene = lrg_scene_for(server, &window, &error);

	(void)data;
	if (scene == NULL) {
		fail(inv, error);
		return;
	}
	gsurf_lrg_scene_unpin_view(scene, NULL);
	lrg_reply(inv, window, scene, "pin");
}

/* lrg.camera: op = orbit {yaw, pitch} | zoom {factor} | reset | overview |
 * save {name} | restore {name}. */
static void
m_lrg_camera(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfWindow *window = NULL;
	GsurfLrgScene *scene = lrg_scene_for(server, &window, &error);
	const gchar *op = NULL;
	const gchar *name = NULL;
	gdouble yaw = 0.0, pitch = 0.0, factor = 1.0;

	(void)data;
	if (scene == NULL || !arg_string(params, "op", &op, &error)
	    || !arg_string(params, "name", &name, &error)
	    || !arg_double(params, "yaw", &yaw, &error)
	    || !arg_double(params, "pitch", &pitch, &error)
	    || !arg_double(params, "factor", &factor, &error)) {
		fail(inv, error);
		return;
	}
	if (g_strcmp0(op, "orbit") == 0) {
		gsurf_lrg_scene_orbit(scene, (gfloat)yaw, (gfloat)pitch, TRUE);
	} else if (g_strcmp0(op, "zoom") == 0) {
		if (factor <= 0.0) {
			gsurf_control_invocation_return_error_literal(inv,
				GSURF_CONTROL_ERROR_INVALID_PARAMS,
				"'factor' must be positive (<1 closer, >1 away)");
			return;
		}
		gsurf_lrg_scene_zoom(scene, (gfloat)factor, TRUE);
	} else if (g_strcmp0(op, "reset") == 0) {
		gsurf_lrg_scene_reset_camera(scene);
	} else if (g_strcmp0(op, "overview") == 0) {
		gsurf_lrg_scene_overview(scene);
	} else if (g_strcmp0(op, "save") == 0 || g_strcmp0(op, "restore") == 0) {
		if (name == NULL || *name == '\0') {
			gsurf_control_invocation_return_error_literal(inv,
				GSURF_CONTROL_ERROR_INVALID_PARAMS, "missing 'name' (the bookmark)");
			return;
		}
		if (op[0] == 's') {
			gsurf_lrg_scene_save_view(scene, name);
		} else if (!gsurf_lrg_scene_restore_view(scene, name)) {
			gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_NOT_FOUND,
				"no camera bookmark '%s' (lrg.state lists them)", name);
			return;
		}
	} else {
		gsurf_control_invocation_return_error_literal(inv,
			GSURF_CONTROL_ERROR_INVALID_PARAMS,
			"'op' must be orbit, zoom, reset, overview, save or restore");
		return;
	}
	lrg_reply(inv, window, scene, "camera");
}

/* lrg.screenshot answers once the next frame has been written. */
static void
lrg_shot_done(GsurfLrgScene *scene, const gchar *path, gboolean ok, gpointer user_data)
{
	GsurfControlInvocation *inv = user_data;
	g_autoptr(JsonBuilder) b = NULL;
	GStatBuf st;

	(void)scene;
	if (!ok) {
		gsurf_control_invocation_return_error(inv, GSURF_CONTROL_ERROR_FAILED,
			"could not write '%s'", path);
		g_object_unref(inv);
		return;
	}
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "path");
	json_builder_add_string_value(b, path);
	json_builder_set_member_name(b, "bytes");
	json_builder_add_int_value(b, g_stat(path, &st) == 0 ? (gint64)st.st_size : 0);
	json_builder_end_object(b);
	gsurf_control_invocation_return_value(inv, json_builder_get_root(b));
	g_object_unref(inv);
}

static void
m_lrg_screenshot(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonObject *params = gsurf_control_invocation_get_params(inv);
	GError *error = NULL;
	GsurfLrgScene *scene = lrg_scene_for(server, NULL, &error);
	const gchar *path = NULL;
	g_autofree gchar *resolved = NULL;

	(void)data;
	if (scene == NULL || !arg_string(params, "path", &path, &error)) {
		fail(inv, error);
		return;
	}
	if (path == NULL || *path == '\0') {
		gsurf_control_invocation_return_error_literal(inv,
			GSURF_CONTROL_ERROR_INVALID_PARAMS, "missing 'path' (the PNG file to write)");
		return;
	}
	/* Relative paths resolve against the directory gsurf started in. */
	resolved = gsurf_resolve_path(path);
	gsurf_lrg_scene_request_screenshot(scene, resolved, lrg_shot_done,
	                                   g_object_ref(inv));
}
#else /* !GSURF_HAVE_LRG_3D */
static void
m_lrg_unsupported(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	(void)server;
	(void)data;
	gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_UNSUPPORTED,
		"this build has no 3D scene (build with LRG_BACKEND=1 LRG_3D=1)");
}
#define m_lrg_state      m_lrg_unsupported
#define m_lrg_mode       m_lrg_unsupported
#define m_lrg_toggle     m_lrg_unsupported
#define m_lrg_focus      m_lrg_unsupported
#define m_lrg_pin        m_lrg_unsupported
#define m_lrg_unpin_all  m_lrg_unsupported
#define m_lrg_camera     m_lrg_unsupported
#define m_lrg_screenshot m_lrg_unsupported
#endif /* GSURF_HAVE_LRG_3D */

#define VIEW_DOC "view?: id, index?: int (default: the active view)"

typedef struct {
	const gchar            *name;
	const gchar            *description;
	const gchar            *params;
	GsurfControlMethodFunc  handler;
	gint                    data;
} CoreMethod;

static const CoreMethod core_methods[] = {
	{ "instance.info", "This instance: pid, version, backend, name, socket, kiosk, windows, "
	  "views, active view (id, uri, title), focused_at and now (monotonic microseconds)",
	  "{}", m_instance_info, 0 },
	{ "open", "Open a URI in a new view (a tab; a child of 'parent' in the tab tree) and focus "
	  "it unless 'background'; returns the view. Refused in kiosk mode",
	  "{uri?: string (default: new_view_uri), background?: bool, parent?: id, "
	  "container?: id|\"temporary\"|\"default\"}", m_open, 0 },
	{ "navigate", "Load a URI in a view (search-engine prefixes apply); returns the view. "
	  "Refused in kiosk mode", "{uri: string, " VIEW_DOC "}", m_navigate, 0 },
	{ "back", "Go back in a view's history", "{" VIEW_DOC "}", m_nav, NAV_BACK },
	{ "forward", "Go forward in a view's history", "{" VIEW_DOC "}", m_nav, NAV_FORWARD },
	{ "reload", "Reload a view ('hard' bypasses the cache)", "{hard?: bool, " VIEW_DOC "}",
	  m_nav, NAV_RELOAD },
	{ "stop", "Stop loading a view", "{" VIEW_DOC "}", m_nav, NAV_STOP },
	{ "views.list", "The views (tabs) of the window in order: index, id, uri, title, active, "
	  "visible, opener_id, parent_id, container, container_name, container_color, "
	  "container_temporary, pane ({window, index} or null)", "{}", m_views_list, 0 },
	{ "views.activate", "Make a view the active (focused) one; returns it",
	  "{view: id | index: int}", m_views_activate, 0 },
	{ "views.close", "Close a view (default: the active one; not the last one)",
	  "{" VIEW_DOC "}",
	  m_views_close, 0 },
	{ "page.uri", "A view's URI", "{" VIEW_DOC "}", m_page_uri, 0 },
	{ "page.title", "A view's page title", "{" VIEW_DOC "}", m_page_title, 0 },
	{ "page.text", "A view's rendered text (document.body.innerText)", "{" VIEW_DOC "}",
	  m_page_text, 0 },
	{ "page.eval", "Run JavaScript in a view's page; returns its last value as JSON. Refused "
	  "in kiosk mode", "{script: string, " VIEW_DOC "}", m_page_eval, 0 },
	{ "lrg.state", "The --lrg=3d scene: arrangement, environment, reading, history, "
	  "panels, pinned tabs (with placements) and camera bookmarks", "{}", m_lrg_state, 0 },
	{ "lrg.arrange", "Set the 3D tab arrangement (carousel, grid, free, single-panel, stack, "
	  "per-window); without one, step to the next", "{arrangement?: string}", m_lrg_mode, 0 },
	{ "lrg.environment", "Set the 3D environment (studio, sky, workshop, cockpit, void); "
	  "without one, step to the next", "{environment?: string}", m_lrg_mode, 1 },
	{ "lrg.reading", "3D reading mode: fly to the active tab and show it pixel-exact "
	  "(no 'enabled': toggle)", "{enabled?: bool}", m_lrg_toggle, 0 },
	{ "lrg.history", "3D history depth: the active tab's back/forward pages as a stack "
	  "(no 'enabled': toggle)", "{enabled?: bool}", m_lrg_toggle, 1 },
	{ "lrg.focus", "Make a view active and fly the 3D camera to it", "{" VIEW_DOC "}",
	  m_lrg_focus, 0 },
	{ "lrg.pin", "Pin a view's 3D panel where it is, or at 'placement' (from lrg.state); "
	  "'enabled': false unpins", "{enabled?: bool, placement?: string, " VIEW_DOC "}",
	  m_lrg_pin, 0 },
	{ "lrg.unpin_all", "Unpin every 3D panel", "{}", m_lrg_unpin_all, 0 },
	{ "lrg.camera", "Move the 3D camera: op = orbit {yaw, pitch degrees} | zoom {factor} | "
	  "reset | overview | save {name} | restore {name}",
	  "{op: string, yaw?: number, pitch?: number, factor?: number, name?: string}",
	  m_lrg_camera, 0 },
	{ "lrg.screenshot", "Write the next 3D frame (scene and chrome, as shown) as a PNG to "
	  "'path'; returns {path, bytes}", "{path: string}", m_lrg_screenshot, 0 },
	{ "page.screenshot", "Write a PNG of a view's visible page to 'path' (relative paths "
	  "resolve against gsurf's launch directory); returns {path, bytes}",
	  "{path: string, " VIEW_DOC "}", m_page_screenshot, 0 },
	{ "actions.list", "Every action name action.run accepts, with 'allowed' (false when kiosk "
	  "mode forbids it)", "{}", m_actions_list, 0 },
	{ "action.run", "Perform an action exactly as its key binding would (zoom-in, tab-next, "
	  "pane-split-right, ...)", "{name: string, " VIEW_DOC "}", m_action_run, 0 },
	{ "mode.get", "The input mode: {mode, label, passthrough, modal_active}", "{}",
	  m_mode_get, 0 },
	{ "mode.set", "Switch the vim-style input mode (needs the modal module); returns the mode",
	  "{mode: \"normal\"|\"insert\", " VIEW_DOC "}", m_mode_set, 0 },
	{ "tree.get", "The tab tree: 'tree' (roots with 'children' and 'collapsed'), and with wmux "
	  "'grouped' and 'groups' (per wmux window, then the background group)", "{}",
	  m_tree_get, 0 },
	{ "tree.move", "Move a view with its subtree: exactly one of parent (0 = a root at the "
	  "end), before, after (view ids) or index (a window position); returns the view",
	  "{view: id, parent?: id, before?: id, after?: id, index?: int}", m_tree_move, 0 },
	{ "tree.collapse", "Collapse (or with collapsed=false expand) a view's subtree in the tab "
	  "sidebar (needs the tabs module)", "{view?: id, collapsed?: bool (default true)}",
	  m_tree_collapse, 0 },
	{ "containers.list", "The containers: default first, then each defined one (id, name, "
	  "color, temporary, default, n_views, rules, rules_active)", "{}", m_containers_list, 0 },
	{ "containers.reopen", "Reopen a view's page in another container in place; the new view "
	  "(a NEW id) is returned. Refused in kiosk mode",
	  "{container: id|\"temporary\"|\"default\", " VIEW_DOC "}", m_containers_reopen, 0 },
	{ "layout.describe", "The window layout (wmux): loaded, enabled, active, kiosk and what "
	  "the layout says about itself (name, prefix, status, mouse, width, height)", "{}",
	  m_layout_describe, 0 },
	{ "layout.groups", "The wmux windows with their panes (index, name, current, last, "
	  "zoomed, panes)", "{}", m_layout_groups, 0 },
	{ "layout.panes", "One wmux window (default: the current one) with its panes: index, id, "
	  "view, uri, title, active, focused, zoomed, visible, rect, fraction",
	  "{window?: int}", m_layout_panes, 0 },
	{ "layout.command", "Run a wmux (tmux-subset) command line, e.g. \"split-window -h "
	  "https://example.org\"; returns the current wmux window", "{command: string}",
	  m_layout_command, 0 },
	{ "layout.select_group", "Show a wmux window; returns it", "{window: int}",
	  m_layout_select_group, 0 },
	{ "layout.rename_group", "Rename a wmux window (default: the current one; no name = "
	  "automatic naming); returns it", "{window?: int, name?: string}",
	  m_layout_rename_group, 0 },
	{ "layout.show_view_at", "Show a view in a pane (zone center) or split a pane and show it "
	  "on one side (left, right, top, bottom); the pane is 'pane' of the current (or given) "
	  "wmux window, or the one showing view 'target'",
	  "{view: id, pane?: int, window?: int, target?: id, zone?: string}",
	  m_layout_show_view_at, 0 },
	{ "config.get", "A configuration value: a core key (browser.homepage, control.name, ...; "
	  "no key lists them all) or modules[.<name>[.<key>...]] from the YAML",
	  "{key?: string}", m_config_get, 0 },
	{ "modules.list", "The loaded modules: name, description, enabled, active, priority",
	  "{}", m_modules_list, 0 },
	{ "modules.toggle", "Activate or deactivate a loaded module (without 'enabled' it flips); "
	  "returns it. Refused in kiosk mode", "{name: string, enabled?: bool}",
	  m_modules_toggle, 0 },
	{ NULL, NULL, NULL, NULL, 0 }
};

void
gsurf_control_methods_register(GsurfControlServer *server)
{
	guint i;

	for (i = 0; core_methods[i].name != NULL; i++)
		gsurf_control_server_add_method(server, core_methods[i].name,
			core_methods[i].description, core_methods[i].params, core_methods[i].handler,
			GINT_TO_POINTER(core_methods[i].data), NULL);
}
