/*
 * test-mcp-webkit.c - The mcp module's tools on real windows and views
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the real mcp.so with wmux, tabs, containers and modal into the
 * default module manager, gives them an application with a real backend
 * window (GTK3 or GTK4) and loopback pages, and drives the browser only
 * through MCP tools (in-process, gsurf_mcp_module_call_tool()), checking
 * every result against the core's own view of the window (views, layout
 * groups and panes, containers, the published mode):
 *
 *   - panes: split (direction, ratio, URI), select (direction, view,
 *     pane), zoom, layouts, kill, pixel and fractional rectangles,
 *     command-language errors (unknown pane / window / command / layout,
 *     a refused split, no last window);
 *   - windows: new, rename (and back to automatic), select (index, last),
 *     kill;
 *   - tabs into panes: background children, show_view_in_pane (edge,
 *     centre swap), split_pane of a background tab;
 *   - the tab tree grouped by wmux window (pane tabs, background
 *     children, the background group), move_view, set_collapsed;
 *   - containers: list with host rules, reopen in place (new id), a child
 *     in a temporary container;
 *   - modes and actions: set/get mode, follow hints, run_action through
 *     modules and through a host action executor;
 *   - wmux switched off: clear errors, no groups;
 *   - one real round trip over the module's Unix socket.
 *
 * Runs only with GSURF_TEST_GUI=1 (make test-gui) and MCP=1, on both
 * toolkits.
 */

#include <gsurf.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <gmodule.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "test-util.h"
#include "webkit-test-util.h"

#define WINDOW_WIDTH  (1000)
#define WINDOW_HEIGHT (700)
#define SOCKET_NAME   "test-mcp-gui"

typedef gchar *(*CallToolFunc) (GsurfModule *module, const gchar *name, const gchar *args,
                                gboolean *is_error, GError **error);

static GModule      *so_handle;
static CallToolFunc  call_tool;

/* ------------------------------------------------------------------ */
/* Loopback pages                                                      */
/* ------------------------------------------------------------------ */

static SoupServer *server;
static guint16     port;

/* /p/<name>: a page titled <name>. */
static void
serve(SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *query,
      gpointer user_data)
{
	g_autofree gchar *body = NULL;

	(void)srv;
	(void)query;
	(void)user_data;
	body = g_strdup_printf("<!doctype html><html><head><title>%s</title></head>"
		"<body><a href=\"/p/linked\">link</a> %s</body></html>",
		g_str_has_prefix(path, "/p/") ? path + 3 : "page", path);
	soup_server_message_set_status(msg, 200, NULL);
	soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_COPY, body, strlen(body));
}

static gchar *
page(const gchar *name)
{
	return g_strdup_printf("http://127.0.0.1:%u/p/%s", port, name);
}

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
	GsurfConfig        *config;
	GsurfApplication   *app;
	GsurfModuleManager *mgr;
	GsurfWindow        *window;
	GtkWidget          *toplevel;
	GsurfModule        *mcp;
	GsurfView          *first;
	guint               zooms;   /* the test executor's zoom-in count */
} H;

#define WAIT_UNTIL(cond) G_STMT_START { \
	guint wait_i_; \
	for (wait_i_ = 0; wait_i_ < 400 && !(cond); wait_i_++) \
		gsurf_webkit_test_spin(25); \
	g_assert_true(cond); \
} G_STMT_END

#define ASSERT_HAS(text, needle) \
	G_STMT_START { \
		if (strstr((text), (needle)) == NULL) \
			g_error("'%s' not in: %s", (needle), (text)); \
	} G_STMT_END

/* Load module @name into the manager, or configure the loaded one again;
 * either way enabled. */
static void
use_module(H *h, const gchar *name)
{
	g_autofree gchar *path = gsurf_webkit_test_module_path(name);
	g_autoptr(GError) error = NULL;
	GsurfModule *module = gsurf_module_manager_get_module(h->mgr, name);

	if (module == NULL) {
		module = gsurf_module_manager_load_module(h->mgr, path, &error);
		g_assert_no_error(error);
		g_assert_nonnull(module);
	} else {
		gsurf_module_configure(module, h->config);
	}
	gsurf_module_set_enabled(module, TRUE);
}

/* A host action executor like the gsurf binary's, for zoom-in only;
 * everything else goes to the modules. */
static gboolean
executor(GsurfWindow *window, GsurfView *view, GsurfAction action, gpointer user_data)
{
	H *h = user_data;

	(void)window;
	if (action == GSURF_ACTION_ZOOM_IN && view != NULL) {
		gsurf_view_set_zoom_level(view, gsurf_view_get_zoom_level(view) + 0.1);
		h->zooms++;
		return TRUE;
	}
	return gsurf_module_manager_dispatch_action(h->mgr, view, action);
}

/*
 * h_init:
 * @extra_yaml: (nullable): more module sections (indented 2)
 *
 * A config enabling every MCP tool and wmux, tabs, containers (Media for
 * localhost, new tabs in the default container) and modal; an
 * application with one real window holding one view on /p/first; the
 * modules loaded and activated; the window presented and its first page
 * loaded. FALSE (skipped) when a module is not built.
 */
static gboolean
h_init(H *h, const gchar *extra_yaml)
{
	static const gchar *const modules[] = { "wmux", "tabs", "containers", "modal", "mcp", NULL };
	static const gchar *const tools[] = {
		"navigate", "get_title", "list_views", "switch_view", "open_view", "close_view",
		"open_child_view", "move_view", "set_collapsed", "get_tab_tree", "list_containers",
		"reopen_in_container", "new_temporary_container_view", "get_mode", "set_mode",
		"follow_hints", "run_action", "list_actions", "wmux_status", "list_wmux_windows",
		"list_panes", "wmux_command", "split_pane", "select_pane", "kill_pane", "zoom_pane",
		"select_layout", "show_view_in_pane", "select_window", "new_window",
		"rename_window", "kill_window", "toggle_module", NULL
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) yaml = NULL;
	g_autofree gchar *first = page("first");
	guint i;

	memset(h, 0, sizeof(*h));
	for (i = 0; modules[i] != NULL; i++) {
		g_autofree gchar *so = gsurf_webkit_test_module_path(modules[i]);

		if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
			g_test_skip("a module is not built (mcp needs make MCP=1)");
			return FALSE;
		}
	}

	/* 1. Config. */
	yaml = g_string_new(NULL);
	g_string_append_printf(yaml, "browser:\n  homepage: about:blank\n"
		"  new_view_uri: http://127.0.0.1:%u/p/new\n"
		"modules:\n  wmux:\n    enabled: true\n"
		"  tabs:\n    enabled: true\n    sidebar: true\n"
		"  containers:\n    enabled: true\n    new_tab: default\n    temp_cleanup_delay: 0\n"
		"    containers:\n"
		"      - name: Media\n        color: '#f9e2af'\n        hosts: ['^localhost$']\n"
		"  modal:\n    enabled: true\n"
		"  mcp:\n    enabled: true\n    socket_name: " SOCKET_NAME "\n    tools:\n", port);
	for (i = 0; tools[i] != NULL; i++)
		g_string_append_printf(yaml, "      %s: true\n", tools[i]);
	if (extra_yaml != NULL)
		g_string_append(yaml, extra_yaml);
	h->config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(h->config, yaml->str, -1, &error));
	g_assert_no_error(error);
	gsurf_config_set_default(h->config);

	/* 2. Application and window with one startup view. */
	h->app = gsurf_application_new(h->config);
	h->window = gsurf_window_new();
	h->toplevel = gsurf_window_get_native_widget(h->window);
#ifdef GSURF_BACKEND_GTK4
	gtk_window_set_default_size(GTK_WINDOW(h->toplevel), WINDOW_WIDTH, WINDOW_HEIGHT);
#else
	gtk_window_resize(GTK_WINDOW(h->toplevel), WINDOW_WIDTH, WINDOW_HEIGHT);
#endif
	h->first = gsurf_view_new_full(NULL, NULL, first, FALSE);
	gsurf_window_add_view(h->window, h->first);
	g_object_unref(h->first);
	gsurf_application_add_window(h->app, h->window);

	/* 3. Modules in the default manager (the tools use it). A module
	 *    loaded by an earlier case is configured again; others are
	 *    disabled first. */
	h->mgr = g_object_ref(gsurf_module_manager_get_default());
	gsurf_module_manager_set_application(h->mgr, h->app);
	gsurf_module_manager_set_config(h->mgr, h->config);
	gsurf_module_manager_set_action_executor(h->mgr, executor, h, NULL);
	{
		GPtrArray *loaded = gsurf_module_manager_get_modules(h->mgr);

		for (i = 0; i < loaded->len; i++)
			gsurf_module_set_enabled(g_ptr_array_index(loaded, i), FALSE);
	}
	for (i = 0; modules[i] != NULL; i++)
		use_module(h, modules[i]);
	gsurf_module_manager_activate_all(h->mgr);
	h->mcp = gsurf_module_manager_get_module(h->mgr, "mcp");
	if (so_handle == NULL) {
		g_autofree gchar *so = gsurf_webkit_test_module_path("mcp");

		so_handle = g_module_open(so, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
		g_assert_nonnull(so_handle);
		g_assert_true(g_module_symbol(so_handle, "gsurf_mcp_module_call_tool",
			(gpointer *)&call_tool));
	}
	g_assert_nonnull(gsurf_window_get_layout(h->window));

	gsurf_window_present(h->window);
	gsurf_view_load_uri(h->first, first);
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(h->first), "first") == 0);
	return TRUE;
}

static void
h_free(H *h)
{
	GPtrArray *loaded;
	guint i;

	if (h->mgr != NULL) {
		gsurf_module_manager_deactivate_all(h->mgr);
		loaded = gsurf_module_manager_get_modules(h->mgr);
		for (i = 0; i < loaded->len; i++)
			gsurf_module_set_enabled(g_ptr_array_index(loaded, i), FALSE);
		gsurf_module_manager_set_action_executor(h->mgr, NULL, NULL, NULL);
		gsurf_module_manager_set_application(h->mgr, NULL);
		gsurf_module_manager_set_config(h->mgr, NULL);
		g_clear_object(&h->mgr);
	}
	gsurf_config_set_default(NULL);
	if (h->window != NULL)
		gtk_widget_set_visible(h->toplevel, FALSE);
	g_clear_object(&h->window);
	g_clear_object(&h->app);
	g_clear_object(&h->config);
	gsurf_webkit_test_spin(50);
}

/* Call @tool with @args; assert whether it failed; return the text. */
static gchar *
call(H *h, const gchar *tool, const gchar *args, gboolean expect_error)
{
	g_autoptr(GError) error = NULL;
	gboolean is_error = !expect_error;
	gchar *text = call_tool(h->mcp, tool, args, &is_error, &error);

	g_assert_no_error(error);
	g_assert_nonnull(text);
	if (is_error != expect_error)
		g_error("%s(%s): %s: %s", tool, args != NULL ? args : "",
			expect_error ? "expected an error" : "unexpected error", text);
	return text;
}

static gchar *callf(H *h, const gchar *tool, gboolean expect_error, const gchar *format, ...)
	G_GNUC_PRINTF(4, 5);

static gchar *
callf(H *h, const gchar *tool, gboolean expect_error, const gchar *format, ...)
{
	g_autofree gchar *args = NULL;
	va_list ap;

	va_start(ap, format);
	args = g_strdup_vprintf(format, ap);
	va_end(ap);
	return call(h, tool, args, expect_error);
}

/* A successful call parsed as JSON. */
static JsonNode *
call_json(H *h, const gchar *tool, const gchar *args)
{
	g_autofree gchar *text = call(h, tool, args, FALSE);
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(json_parser_load_from_data(parser, text, -1, &error));
	g_assert_no_error(error);
	return json_node_copy(json_parser_get_root(parser));
}

static JsonNode *call_jsonf(H *h, const gchar *tool, const gchar *format, ...) G_GNUC_PRINTF(3, 4);

static JsonNode *
call_jsonf(H *h, const gchar *tool, const gchar *format, ...)
{
	g_autofree gchar *args = NULL;
	va_list ap;

	va_start(ap, format);
	args = g_strdup_vprintf(format, ap);
	va_end(ap);
	return call_json(h, tool, args);
}

/* The "panes" array of a list_panes-style result. */
static JsonArray *
panes_of(JsonNode *node)
{
	return json_object_get_array_member(json_node_get_object(node), "panes");
}

/* The pane object showing view @id in @panes, or NULL. */
static JsonObject *
pane_showing(JsonArray *panes, guint64 id)
{
	guint i;

	for (i = 0; i < json_array_get_length(panes); i++) {
		JsonObject *p = json_array_get_object_element(panes, i);

		if (!json_object_get_null_member(p, "view") &&
		    json_object_get_int_member(p, "view") == (gint64)id)
			return p;
	}
	return NULL;
}

/* The index of the active pane in @panes, or -1. */
static gint
active_index(JsonArray *panes)
{
	guint i;

	for (i = 0; i < json_array_get_length(panes); i++)
		if (json_object_get_boolean_member(json_array_get_object_element(panes, i), "active"))
			return (gint)i;
	return -1;
}

static gdouble
frac(JsonObject *pane, const gchar *member)
{
	return json_object_get_double_member(json_object_get_object_member(pane, "fraction"),
		member);
}

static gint64
rect(JsonObject *pane, const gchar *member)
{
	return json_object_get_int_member(json_object_get_object_member(pane, "rect"), member);
}

/* The current wmux window's pane count, from the core seam itself. */
static guint
n_panes(H *h)
{
	GsurfWindowLayout *layout = gsurf_window_get_layout(h->window);
	g_autoptr(GPtrArray) groups = gsurf_window_layout_list_groups(layout, h->window);
	guint i;

	for (i = 0; i < groups->len; i++) {
		GsurfLayoutGroup *g = g_ptr_array_index(groups, i);

		if (gsurf_layout_group_get_current(g)) {
			g_autoptr(GPtrArray) panes = gsurf_window_layout_list_panes(layout, h->window,
				gsurf_layout_group_get_index(g));

			return panes->len;
		}
	}
	return 0;
}

/* The id of the view titled @title in the window, or 0. */
static guint64
view_titled(H *h, const gchar *title)
{
	guint i;

	for (i = 0; i < gsurf_window_get_n_views(h->window); i++) {
		GsurfView *v = gsurf_window_get_nth_view(h->window, i);

		if (g_strcmp0(gsurf_view_get_title(v), title) == 0)
			return gsurf_view_get_id(v);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Panes                                                               */
/* ------------------------------------------------------------------ */

static void
test_panes(void)
{
	H h;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *two = page("two");
	g_autofree gchar *three = page("three");
	guint64 first;
	guint64 two_id;
	guint64 three_id;
	JsonArray *panes;
	JsonObject *p;
	gchar *text;
	guint i;

	if (!gsurf_webkit_test_init() || !h_init(&h, NULL))
		return;
	first = gsurf_view_get_id(h.first);

	/* 1. Status. */
	text = call(&h, "wmux_status", NULL, FALSE);
	ASSERT_HAS(text, "\"enabled\":true");
	ASSERT_HAS(text, "\"active\":true");
	ASSERT_HAS(text, "\"name\":\"wmux\"");
	ASSERT_HAS(text, "\"windows\":1");
	ASSERT_HAS(text, "\"prefix\":\"");
	g_free(text);

	/* 2. Split right with a URI: the new pane is active and loads it. */
	{
		g_autofree gchar *args = g_strdup_printf("{\"uri\":\"%s\"}", two);

		node = call_json(&h, "split_pane", args);
	}
	panes = panes_of(node);
	g_assert_cmpuint(json_array_get_length(panes), ==, 2);
	g_assert_cmpint(active_index(panes), ==, 1);
	g_assert_cmpuint(n_panes(&h), ==, 2);
	WAIT_UNTIL(view_titled(&h, "two") != 0);
	two_id = view_titled(&h, "two");
	g_assert_cmpuint(gsurf_view_get_id(gsurf_window_get_active_view(h.window)), ==, two_id);

	/* Rectangles: halves (split_ratio 50), real pixels once allocated. */
	g_clear_pointer(&node, json_node_unref);
	for (i = 0; i < 200; i++) {
		node = call_json(&h, "list_panes", NULL);
		if (rect(json_array_get_object_element(panes_of(node), 0), "width") > 0)
			break;
		g_clear_pointer(&node, json_node_unref);
		gsurf_webkit_test_spin(25);
	}
	g_assert_nonnull(node);
	panes = panes_of(node);
	p = json_array_get_object_element(panes, 0);
	g_assert_cmpint(json_object_get_int_member(p, "view"), ==, (gint64)first);
	g_assert_cmpfloat_with_epsilon(frac(p, "width"), 0.5, 0.01);
	g_assert_cmpfloat_with_epsilon(frac(json_array_get_object_element(panes, 1), "x"), 0.5, 0.01);
	g_assert_cmpint(rect(p, "width"), >, 100);
	g_assert_cmpint(rect(json_array_get_object_element(panes, 1), "x"), >=, rect(p, "width"));
	g_assert_true(json_object_get_boolean_member(p, "visible"));
	g_clear_pointer(&node, json_node_unref);

	/* 3. Split the right pane down at 30 %. */
	{
		g_autofree gchar *args = g_strdup_printf("{\"direction\":\"down\",\"ratio\":0.3,"
			"\"uri\":\"%s\"}", three);

		node = call_json(&h, "split_pane", args);
	}
	panes = panes_of(node);
	g_assert_cmpuint(json_array_get_length(panes), ==, 3);
	WAIT_UNTIL(view_titled(&h, "three") != 0);
	three_id = view_titled(&h, "three");
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "list_panes", NULL);
	p = pane_showing(panes_of(node), three_id);
	g_assert_nonnull(p);
	g_assert_cmpfloat_with_epsilon(frac(p, "x"), 0.5, 0.01);
	g_assert_cmpfloat_with_epsilon(frac(p, "height"), 0.3, 0.02);
	g_assert_true(json_object_get_boolean_member(p, "active"));
	g_clear_pointer(&node, json_node_unref);

	/* 4. Select by direction, by view, by pane index. */
	node = call_json(&h, "select_pane", "{\"direction\":\"left\"}");
	g_assert_true(gsurf_window_get_active_view(h.window) == h.first);
	g_clear_pointer(&node, json_node_unref);
	node = call_jsonf(&h, "select_pane", "{\"view\":%" G_GUINT64_FORMAT "}", three_id);
	g_assert_cmpuint(gsurf_view_get_id(gsurf_window_get_active_view(h.window)), ==, three_id);
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "select_pane", "{\"pane\":1}");
	g_assert_cmpint(active_index(panes_of(node)), ==, 1);
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "select_pane", "{\"last\":true}");
	g_assert_cmpuint(gsurf_view_get_id(gsurf_window_get_active_view(h.window)), ==, three_id);
	g_clear_pointer(&node, json_node_unref);

	/* 5. Zoom (toggle, then an explicit unzoom); only the zoomed pane is
	 *    on screen meanwhile. */
	node = call_json(&h, "zoom_pane", NULL);
	p = pane_showing(panes_of(node), three_id);
	g_assert_true(json_object_get_boolean_member(p, "zoomed"));
	g_assert_cmpfloat(frac(p, "width"), ==, 1.0);
	g_assert_false(json_object_get_boolean_member(pane_showing(panes_of(node), first),
		"visible"));
	g_assert_false(gsurf_window_get_view_visible(h.window, h.first));
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "zoom_pane", "{\"zoomed\":true}");
	g_assert_true(json_object_get_boolean_member(pane_showing(panes_of(node), three_id),
		"zoomed"));
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "zoom_pane", "{\"zoomed\":false}");
	g_assert_false(json_object_get_boolean_member(pane_showing(panes_of(node), three_id),
		"zoomed"));
	g_assert_true(gsurf_window_get_view_visible(h.window, h.first));
	g_clear_pointer(&node, json_node_unref);

	/* 6. Layouts. */
	node = call_json(&h, "select_layout", "{\"name\":\"even-horizontal\"}");
	panes = panes_of(node);
	for (i = 0; i < 3; i++) {
		p = json_array_get_object_element(panes, i);
		g_assert_cmpfloat_with_epsilon(frac(p, "width"), 1.0 / 3.0, 0.01);
		g_assert_cmpfloat(frac(p, "y"), ==, 0.0);
	}
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "select_layout", "{\"name\":\"even-vertical\"}");
	g_assert_cmpfloat_with_epsilon(frac(json_array_get_object_element(panes_of(node), 2), "y"),
		2.0 / 3.0, 0.01);
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "select_layout", "{\"name\":\"next\"}");
	g_clear_pointer(&node, json_node_unref);
	text = call(&h, "select_layout", "{\"name\":\"bogus\"}", TRUE);
	ASSERT_HAS(text, "bogus");
	g_free(text);

	/* 7. Kill a pane by index: the pane and its tab go. */
	node = call_json(&h, "kill_pane", "{\"pane\":2}");
	g_assert_cmpuint(json_array_get_length(panes_of(node)), ==, 2);
	g_assert_cmpuint(gsurf_window_get_n_views(h.window), ==, 2);
	g_clear_pointer(&node, json_node_unref);

	/* 8. Command-language errors come back as text. */
	text = call(&h, "wmux_command", "{\"command\":\"select-pane -t 7\"}", TRUE);
	ASSERT_HAS(text, "can't find pane 7");
	g_free(text);
	text = call(&h, "wmux_command", "{\"command\":\"frobnicate\"}", TRUE);
	ASSERT_HAS(text, "frobnicate");
	g_free(text);
	text = call(&h, "wmux_command", "{\"command\":\"last-window\"}", TRUE);
	ASSERT_HAS(text, "no last window");
	g_free(text);
	text = call(&h, "split_pane", "{\"ratio\":0.05}", TRUE);
	ASSERT_HAS(text, "split-window");
	g_free(text);
	g_assert_cmpuint(n_panes(&h), ==, 2);

	/* 9. Any command: rotate, then break the active pane out. */
	node = call_json(&h, "wmux_command", "{\"command\":\"rotate-window\"}");
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "wmux_command", "{\"command\":\"break-pane\"}");
	g_assert_cmpuint(json_array_get_length(panes_of(node)), ==, 1);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node), "window"), ==, 1);
	g_clear_pointer(&node, json_node_unref);
	text = call(&h, "wmux_command", "{\"command\":\"break-pane\"}", TRUE);
	ASSERT_HAS(text, "only one pane");
	g_free(text);
	h_free(&h);
	(void)two_id;
}

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

static void
test_windows(void)
{
	H h;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *docs = page("docs");
	JsonArray *list;
	JsonObject *w;
	gchar *text;

	if (!gsurf_webkit_test_init() || !h_init(&h, NULL))
		return;

	/* 1. A new named window with its own tab. */
	{
		g_autofree gchar *args = g_strdup_printf("{\"name\":\"my docs\",\"uri\":\"%s\"}", docs);

		node = call_json(&h, "new_window", args);
	}
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node), "window"), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(node), "name"), ==,
		"my docs");
	g_clear_pointer(&node, json_node_unref);
	WAIT_UNTIL(view_titled(&h, "docs") != 0);
	node = call_json(&h, "list_wmux_windows", NULL);
	list = json_node_get_array(node);
	g_assert_cmpuint(json_array_get_length(list), ==, 2);
	w = json_array_get_object_element(list, 0);
	g_assert_false(json_object_get_boolean_member(w, "current"));
	g_assert_true(json_object_get_boolean_member(w, "last"));
	g_assert_true(json_object_get_boolean_member(json_array_get_object_element(list, 1),
		"current"));
	g_clear_pointer(&node, json_node_unref);

	/* 2. Rename window 0; select it; back to the last one. */
	node = call_json(&h, "rename_window", "{\"index\":0,\"name\":\"web\"}");
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(node), "name"), ==,
		"web");
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "select_window", "{\"index\":0}");
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node), "window"), ==, 0);
	g_assert_true(gsurf_window_get_active_view(h.window) == h.first);
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "select_window", "{\"which\":\"last\"}");
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node), "window"), ==, 1);
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "select_window", "{\"which\":\"next\"}");
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node), "window"), ==, 0);
	g_clear_pointer(&node, json_node_unref);
	text = call(&h, "select_window", "{\"index\":5}", TRUE);
	ASSERT_HAS(text, "can't find window 5");
	g_free(text);

	/* 3. Automatic naming again: the window follows its page title. */
	node = call_json(&h, "rename_window", "{\"index\":0,\"name\":\"\"}");
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(node), "name"), ==,
		"first");
	g_clear_pointer(&node, json_node_unref);
	text = call(&h, "rename_window", "{\"index\":9,\"name\":\"x\"}", TRUE);
	ASSERT_HAS(text, "unknown wmux window 9");
	g_free(text);

	/* 4. Kill window 1: its tab closes, one window is left. */
	node = call_json(&h, "kill_window", "{\"index\":1}");
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "list_wmux_windows", NULL);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), ==, 1);
	g_assert_cmpuint(gsurf_window_get_n_views(h.window), ==, 1);
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* Tabs into panes, and the grouped tree                               */
/* ------------------------------------------------------------------ */

/* The member @name of the object @node. */
static JsonObject *
obj(JsonNode *node)
{
	return json_node_get_object(node);
}

/* Whether @children (tree nodes) holds view @id. */
static gboolean
has_child(JsonArray *children, guint64 id)
{
	guint i;

	for (i = 0; i < json_array_get_length(children); i++)
		if (json_object_get_int_member(json_array_get_object_element(children, i), "id") ==
		    (gint64)id)
			return TRUE;
	return FALSE;
}

static void
test_tree_and_drops(void)
{
	H h;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *child_uri = page("child");
	g_autofree gchar *other_uri = page("other");
	g_autofree gchar *temp_uri = page("temp");
	guint64 first;
	guint64 child;
	guint64 other;
	guint64 temp;
	JsonArray *groups;
	JsonObject *g;
	JsonObject *tab;
	gchar *text;

	if (!gsurf_webkit_test_init() || !h_init(&h, NULL))
		return;
	first = gsurf_view_get_id(h.first);

	/* 1. A background child: not in a pane, not focused, under first. */
	node = call_jsonf(&h, "open_child_view", "{\"parent\":%" G_GUINT64_FORMAT ",\"uri\":\"%s\","
		"\"background\":true}", first, child_uri);
	child = (guint64)json_object_get_int_member(obj(node), "id");
	g_assert_cmpint(json_object_get_int_member(obj(node), "parent_id"), ==, (gint64)first);
	g_assert_cmpint(json_object_get_int_member(obj(node), "opener_id"), ==, (gint64)first);
	g_assert_true(json_object_get_null_member(obj(node), "pane"));
	g_assert_false(json_object_get_boolean_member(obj(node), "active"));
	g_assert_true(gsurf_window_get_active_view(h.window) == h.first);
	g_clear_pointer(&node, json_node_unref);

	/* The grouped tree lists it under its pane tab. */
	node = call_json(&h, "get_tab_tree", NULL);
	g_assert_true(json_object_get_boolean_member(obj(node), "grouped"));
	g_assert_true(json_object_get_boolean_member(obj(node), "has_tree_provider"));
	groups = json_object_get_array_member(obj(node), "groups");
	g_assert_cmpuint(json_array_get_length(groups), ==, 1);
	g = json_array_get_object_element(groups, 0);
	g_assert_true(json_object_get_boolean_member(g, "current"));
	tab = json_array_get_object_element(json_object_get_array_member(g, "tabs"), 0);
	g_assert_cmpint(json_object_get_int_member(tab, "id"), ==, (gint64)first);
	g_assert_true(has_child(json_object_get_array_member(tab, "children"), child));
	g_clear_pointer(&node, json_node_unref);

	/* 2. Drop it on pane 0's right edge: a split showing it. */
	node = call_jsonf(&h, "show_view_in_pane", "{\"view\":%" G_GUINT64_FORMAT ",\"pane\":0,"
		"\"zone\":\"right\"}", child);
	g_assert_cmpuint(json_array_get_length(panes_of(node)), ==, 2);
	g_assert_nonnull(pane_showing(panes_of(node), child));
	g_assert_cmpuint(gsurf_view_get_id(gsurf_window_get_active_view(h.window)), ==, child);
	g_clear_pointer(&node, json_node_unref);

	/* 3. Centre on the other pane's view: the two swap places. */
	node = call_jsonf(&h, "show_view_in_pane", "{\"view\":%" G_GUINT64_FORMAT ",\"target\":%"
		G_GUINT64_FORMAT "}", first, child);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(panes_of(node), 0),
		"view"), ==, (gint64)child);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(panes_of(node), 1),
		"view"), ==, (gint64)first);
	g_clear_pointer(&node, json_node_unref);
	text = callf(&h, "show_view_in_pane", TRUE, "{\"view\":%" G_GUINT64_FORMAT ",\"target\":%"
		G_GUINT64_FORMAT ",\"zone\":\"left\"}", first, first);
	ASSERT_HAS(text, "could not show view");
	g_free(text);

	/* 4. Another background child, split in by split_pane. */
	node = call_jsonf(&h, "open_child_view", "{\"uri\":\"%s\",\"background\":true}", other_uri);
	other = (guint64)json_object_get_int_member(obj(node), "id");
	g_clear_pointer(&node, json_node_unref);
	node = call_jsonf(&h, "split_pane", "{\"view\":%" G_GUINT64_FORMAT ",\"direction\":\"down\"}",
		other);
	g_assert_cmpuint(json_array_get_length(panes_of(node)), ==, 3);
	g_assert_nonnull(pane_showing(panes_of(node), other));
	g_clear_pointer(&node, json_node_unref);

	/* 5. A root background tab in a temporary container: the
	 *    background group. */
	node = call_jsonf(&h, "new_temporary_container_view", "{\"uri\":\"%s\","
		"\"background\":true}", temp_uri);
	temp = (guint64)json_object_get_int_member(obj(node), "id");
	g_assert_true(json_object_get_boolean_member(obj(node), "container_temporary"));
	g_assert_cmpint(json_object_get_int_member(obj(node), "parent_id"), ==, 0);
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "get_tab_tree", NULL);
	groups = json_object_get_array_member(obj(node), "groups");
	g_assert_cmpuint(json_array_get_length(groups), ==, 2);
	g = json_array_get_object_element(groups, 1);
	g_assert_true(json_object_get_boolean_member(g, "background"));
	g_assert_true(json_object_get_null_member(g, "index"));
	g_assert_cmpstr(json_object_get_string_member(g, "name"), ==, "Background");
	g_assert_true(has_child(json_object_get_array_member(g, "tabs"), temp));
	g_clear_pointer(&node, json_node_unref);

	/* 6. Re-parent it under first, collapse first, and back to a root. */
	node = call_jsonf(&h, "move_view", "{\"view\":%" G_GUINT64_FORMAT ",\"parent\":%"
		G_GUINT64_FORMAT "}", temp, first);
	g_assert_cmpint(json_object_get_int_member(obj(node), "parent_id"), ==, (gint64)first);
	g_clear_pointer(&node, json_node_unref);
	node = call_jsonf(&h, "set_collapsed", "{\"view\":%" G_GUINT64_FORMAT "}", first);
	g_clear_pointer(&node, json_node_unref);
	node = call_json(&h, "get_tab_tree", NULL);
	tab = json_array_get_object_element(json_object_get_array_member(obj(node), "tree"), 0);
	g_assert_cmpint(json_object_get_int_member(tab, "id"), ==, (gint64)first);
	g_assert_true(json_object_get_boolean_member(tab, "collapsed"));
	g_assert_true(has_child(json_object_get_array_member(tab, "children"), temp));
	g_clear_pointer(&node, json_node_unref);
	node = call_jsonf(&h, "set_collapsed", "{\"view\":%" G_GUINT64_FORMAT ",\"collapsed\":false}",
		first);
	g_clear_pointer(&node, json_node_unref);
	text = callf(&h, "set_collapsed", TRUE, "{\"view\":%" G_GUINT64_FORMAT "}", temp);
	ASSERT_HAS(text, "has no children");
	g_free(text);
	text = callf(&h, "move_view", TRUE, "{\"view\":%" G_GUINT64_FORMAT ",\"parent\":%"
		G_GUINT64_FORMAT "}", first, temp);
	ASSERT_HAS(text, "own subtree");
	g_free(text);
	node = call_jsonf(&h, "move_view", "{\"view\":%" G_GUINT64_FORMAT ",\"parent\":0}", temp);
	g_assert_cmpint(json_object_get_int_member(obj(node), "parent_id"), ==, 0);
	g_assert_cmpint(json_object_get_int_member(obj(node), "index"), ==,
		(gint64)gsurf_window_get_n_views(h.window) - 1);
	g_clear_pointer(&node, json_node_unref);
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* Containers                                                          */
/* ------------------------------------------------------------------ */

static void
test_containers(void)
{
	H h;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *uri = page("x");
	guint64 first;
	guint64 fresh;
	JsonArray *list;
	JsonObject *media = NULL;
	gchar *text;
	guint i;

	if (!gsurf_webkit_test_init() || !h_init(&h, NULL))
		return;
	first = gsurf_view_get_id(h.first);

	/* 1. The default container, then Media with its host rule. */
	node = call_json(&h, "list_containers", NULL);
	list = json_node_get_array(node);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(list, 0), "id"),
		==, "default");
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(list, 0),
		"n_views"), ==, 1);
	for (i = 0; i < json_array_get_length(list); i++)
		if (g_strcmp0(json_object_get_string_member(json_array_get_object_element(list, i),
				"id"), "media") == 0)
			media = json_array_get_object_element(list, i);
	g_assert_nonnull(media);
	g_assert_cmpstr(json_object_get_string_member(media, "name"), ==, "Media");
	g_assert_true(json_object_get_boolean_member(media, "rules_active"));
	g_assert_cmpstr(json_array_get_string_element(json_object_get_array_member(media, "rules"),
		0), ==, "^localhost$");
	g_clear_pointer(&node, json_node_unref);

	/* 2. Reopen in Media, in place: a new id in the old one's pane. */
	node = call_jsonf(&h, "reopen_in_container", "{\"view\":%" G_GUINT64_FORMAT
		",\"container\":\"media\"}", first);
	fresh = (guint64)json_object_get_int_member(obj(node), "id");
	g_assert_cmpuint(fresh, !=, first);
	g_assert_cmpstr(json_object_get_string_member(obj(node), "container"), ==, "media");
	g_assert_cmpstr(json_object_get_string_member(obj(node), "container_name"), ==, "Media");
	g_assert_cmpstr(json_object_get_string_member(obj(node), "container_color"), ==, "#f9e2af");
	g_assert_false(json_object_get_null_member(obj(node), "pane"));
	g_assert_true(json_object_get_boolean_member(obj(node), "active"));
	g_clear_pointer(&node, json_node_unref);
	text = callf(&h, "switch_view", TRUE, "{\"view\":%" G_GUINT64_FORMAT "}", first);
	ASSERT_HAS(text, "unknown view");
	g_free(text);
	WAIT_UNTIL(view_titled(&h, "first") == fresh);

	/* 3. A child in a new temporary container. */
	node = call_jsonf(&h, "open_child_view", "{\"uri\":\"%s\",\"container\":\"temporary\","
		"\"background\":true}", uri);
	g_assert_true(json_object_get_boolean_member(obj(node), "container_temporary"));
	g_assert_cmpint(json_object_get_int_member(obj(node), "parent_id"), ==, (gint64)fresh);
	g_clear_pointer(&node, json_node_unref);

	/* 4. Unknown containers are named in the error. */
	text = call(&h, "reopen_in_container", "{\"container\":\"nope\"}", TRUE);
	ASSERT_HAS(text, "unknown container 'nope'");
	g_free(text);
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* Modes and actions                                                   */
/* ------------------------------------------------------------------ */

static void
test_modes_actions(void)
{
	H h;
	g_autoptr(JsonNode) node = NULL;
	gdouble zoom;
	gchar *text;

	if (!gsurf_webkit_test_init() || !h_init(&h, NULL))
		return;

	/* 1. Modes through the modal module. */
	text = call(&h, "get_mode", NULL, FALSE);
	ASSERT_HAS(text, "\"mode\":\"normal\"");
	ASSERT_HAS(text, "\"modal_active\":true");
	g_free(text);
	text = call(&h, "set_mode", "{\"mode\":\"insert\"}", FALSE);
	ASSERT_HAS(text, "\"mode\":\"insert\"");
	ASSERT_HAS(text, "\"passthrough\":true");
	g_free(text);
	g_assert_cmpint(gsurf_module_manager_get_mode(h.mgr), ==, GSURF_MODE_INSERT);
	text = call(&h, "set_mode", "{\"mode\":\"normal\"}", FALSE);
	ASSERT_HAS(text, "\"mode\":\"normal\"");
	g_free(text);

	/* 2. Link hints: follow mode, left with set_mode normal. */
	text = call(&h, "follow_hints", NULL, FALSE);
	ASSERT_HAS(text, "\"mode\":\"follow\"");
	g_free(text);
	text = call(&h, "set_mode", "{\"mode\":\"normal\"}", FALSE);
	ASSERT_HAS(text, "\"mode\":\"normal\"");
	g_free(text);

	/* 3. run_action: module actions (wmux, modal, tabs) and the host
	 *    executor's own (zoom-in). */
	text = call(&h, "run_action", "{\"action\":\"pane-split-right\"}", FALSE);
	ASSERT_HAS(text, "\"performed\":true");
	g_free(text);
	g_assert_cmpuint(n_panes(&h), ==, 2);
	g_free(call(&h, "run_action", "{\"action\":\"enter-insert-mode\"}", FALSE));
	g_assert_cmpint(gsurf_module_manager_get_mode(h.mgr), ==, GSURF_MODE_INSERT);
	g_free(call(&h, "run_action", "{\"action\":\"enter_normal_mode\"}", FALSE));
	g_assert_cmpint(gsurf_module_manager_get_mode(h.mgr), ==, GSURF_MODE_NORMAL);
	g_free(call(&h, "run_action", "{\"action\":\"tab-sidebar-toggle\"}", FALSE));
	zoom = gsurf_view_get_zoom_level(h.first);
	g_free(callf(&h, "run_action", FALSE, "{\"action\":\"zoom-in\",\"view\":%" G_GUINT64_FORMAT
		"}", gsurf_view_get_id(h.first)));
	g_assert_cmpuint(h.zooms, ==, 1);
	g_assert_cmpfloat_with_epsilon(gsurf_view_get_zoom_level(h.first), zoom + 0.1, 0.001);
	text = call(&h, "run_action", "{\"action\":\"paste-uri-somewhere\"}", TRUE);
	ASSERT_HAS(text, "unknown action");
	g_free(text);

	/* 4. list_actions names them all. */
	node = call_json(&h, "list_actions", NULL);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), ==,
		GSURF_ACTION_LAST - 1);
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* wmux switched off                                                   */
/* ------------------------------------------------------------------ */

static void
test_wmux_off(void)
{
	H h;
	g_autoptr(JsonNode) node = NULL;
	gchar *text;

	if (!gsurf_webkit_test_init() || !h_init(&h, NULL))
		return;
	g_free(call(&h, "toggle_module", "{\"name\":\"wmux\",\"enabled\":false}", FALSE));
	g_assert_null(gsurf_window_get_layout(h.window));

	text = call(&h, "wmux_status", NULL, FALSE);
	ASSERT_HAS(text, "\"loaded\":true");
	ASSERT_HAS(text, "\"enabled\":false");
	ASSERT_HAS(text, "\"active\":false");
	g_free(text);
	text = call(&h, "split_pane", NULL, TRUE);
	ASSERT_HAS(text, "enable the wmux module");
	g_free(text);
	text = call(&h, "list_wmux_windows", NULL, TRUE);
	ASSERT_HAS(text, "enable the wmux module");
	g_free(text);
	node = call_json(&h, "get_tab_tree", NULL);
	g_assert_false(json_object_get_boolean_member(obj(node), "grouped"));
	g_clear_pointer(&node, json_node_unref);
	text = call(&h, "list_views", NULL, FALSE);
	ASSERT_HAS(text, "\"pane\":null");
	g_free(text);
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* The socket                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	gchar   *line;
	gboolean done;
} LineRead;

static void
on_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
	LineRead *r = user_data;

	r->line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, NULL,
		NULL);
	r->done = TRUE;
}

/* A response line answers request @id (mcp-glib echoes ids as strings). */
#define ASSERT_ID(line, id) \
	G_STMT_START { \
		if (strstr((line), "\"id\":" #id) == NULL && \
		    strstr((line), "\"id\":\"" #id "\"") == NULL) \
			g_error("not the response to request " #id ": %s", (line)); \
	} G_STMT_END

/* Write one JSON-RPC line, and (for a request) read the response line,
 * spinning the main loop the server runs in. */
static gchar *
rpc(GDataInputStream *in, GOutputStream *out, const gchar *line, gboolean response)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *framed = g_strconcat(line, "\n", NULL);
	LineRead r = { NULL, FALSE };
	guint i;

	g_assert_true(g_output_stream_write_all(out, framed, strlen(framed), NULL, NULL, &error));
	g_assert_no_error(error);
	if (!response)
		return NULL;
	g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL, on_line, &r);
	for (i = 0; i < 400 && !r.done; i++)
		gsurf_webkit_test_spin(25);
	g_assert_true(r.done);
	g_assert_nonnull(r.line);
	return r.line;
}

static void
test_socket(void)
{
	H h;
	g_autofree gchar *path = NULL;
	g_autoptr(GSocketClient) client = NULL;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(GDataInputStream) in = NULL;
	g_autoptr(GError) error = NULL;
	GOutputStream *out;
	gchar *line;
	guint i;

	if (!gsurf_webkit_test_init() || !h_init(&h, NULL))
		return;
	path = g_strdup_printf("%s/gsurf-mcp-" SOCKET_NAME ".sock", g_get_user_runtime_dir());
	WAIT_UNTIL(g_file_test(path, G_FILE_TEST_EXISTS));

	client = g_socket_client_new();
	address = g_unix_socket_address_new(path);
	connection = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, &error);
	g_assert_no_error(error);
	in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
	out = g_io_stream_get_output_stream(G_IO_STREAM(connection));

	line = rpc(in, out, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
		"{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
		"\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}", TRUE);
	ASSERT_ID(line, 1);
	ASSERT_HAS(line, "\"result\"");
	g_free(line);
	rpc(in, out, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", FALSE);

	line = rpc(in, out, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}", TRUE);
	ASSERT_HAS(line, "\"split_pane\"");
	ASSERT_HAS(line, "\"show_view_in_pane\"");
	g_free(line);
	line = rpc(in, out, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
		"{\"name\":\"wmux_command\",\"arguments\":{\"command\":\"split-window -v\"}}}", TRUE);
	ASSERT_ID(line, 3);
	ASSERT_HAS(line, "panes");
	g_free(line);
	g_assert_cmpuint(n_panes(&h), ==, 2);
	line = rpc(in, out, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":"
		"{\"name\":\"wmux_command\",\"arguments\":{\"command\":\"select-window -t 4\"}}}", TRUE);
	ASSERT_HAS(line, "\"isError\":true");
	ASSERT_HAS(line, "can't find window 4");
	g_free(line);

	/* Close the client and let the server see the EOF before it stops:
	 * mcp-glib's transport does not keep itself alive for a pending read,
	 * so stopping the server with a client still connected runs the
	 * cancelled read's callback on a finalized transport. The server
	 * warns about every client that hangs up (mcp-glib logs without a
	 * domain). */
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "Transport error: *closed*");
	g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
	for (i = 0; i < 40; i++)
		gsurf_webkit_test_spin(25);
	g_test_assert_expected_messages();
	h_free(&h);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *tmp = NULL;
	int ret;

	g_test_init(&argc, &argv, NULL);
	tmp = g_dir_make_tmp("gsurf-mcp-webkit-XXXXXX", NULL);
	g_assert_nonnull(tmp);
	g_setenv("XDG_DATA_HOME", tmp, TRUE);
	g_setenv("XDG_CACHE_HOME", tmp, TRUE);
	g_setenv("XDG_CONFIG_HOME", tmp, TRUE);
	g_setenv("XDG_RUNTIME_DIR", tmp, TRUE);

	server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, serve, NULL, NULL);
	port = gsurf_webkit_test_listen(server);

	g_test_add_func("/gsurf/mcp/webkit/panes", test_panes);
	g_test_add_func("/gsurf/mcp/webkit/windows", test_windows);
	g_test_add_func("/gsurf/mcp/webkit/tree-and-drops", test_tree_and_drops);
	g_test_add_func("/gsurf/mcp/webkit/containers", test_containers);
	g_test_add_func("/gsurf/mcp/webkit/modes-actions", test_modes_actions);
	g_test_add_func("/gsurf/mcp/webkit/wmux-off", test_wmux_off);
	g_test_add_func("/gsurf/mcp/webkit/socket", test_socket);

	ret = g_test_run();
	if (so_handle != NULL)
		g_module_close(so_handle);
	g_clear_object(&server);
	gsurf_test_rmtree(tmp);
	return ret;
}
