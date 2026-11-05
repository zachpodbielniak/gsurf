/*
 * test-control-webkit.c - The control socket's core methods on a real window
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An application with a real backend window (GTK3 or GTK4) and loopback
 * pages, the real wmux, tabs, containers and modal modules in the default
 * module manager, and a GsurfControlServer on a temporary socket. Every
 * core method is driven through the server (in-process with
 * gsurf_control_server_dispatch(), and over the socket for the events)
 * and checked against the core's own view of the window:
 *
 *   - views: instance.info, open (background, child, container), the
 *     lists, activate, navigate, back/forward/reload/stop, close (not the
 *     last one);
 *   - pages: uri, title, text, eval (values as JSON), screenshot;
 *   - the tab tree and containers: tree.get grouped by wmux window,
 *     tree.move, tree.collapse, containers.list, containers.reopen;
 *   - wmux: layout.describe/groups/panes/command/select_group/
 *     rename_group/show_view_at and their errors;
 *   - modes and actions through the modal module and a host executor;
 *   - wmux switched off: clear "unsupported" errors;
 *   - kiosk refusals (code -32003);
 *   - events over the socket, in order; the socket gone after stop and
 *     the server finalized.
 *
 * Runs only with GSURF_TEST_GUI=1 (make test-gui), on both toolkits.
 */

#include <gsurf.h>
#include <control/gsurf-control.h>
#include <gio/gio.h>
#include <string.h>

#include "test-util.h"
#include "webkit-test-util.h"

#define WINDOW_WIDTH  (1000)
#define WINDOW_HEIGHT (700)

static gchar *tmp_root;

/* ------------------------------------------------------------------ */
/* Loopback pages                                                      */
/* ------------------------------------------------------------------ */

static SoupServer *soup;
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
		"<body><p>hello %s</p></body></html>",
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
	GsurfView          *first;
	GsurfControlServer *server;
	gchar              *socket;
	guint               zooms;
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

/* A host action executor like the gsurf binary's for zoom-in; the rest
 * goes to the modules. */
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

static gboolean
h_init(H *h)
{
	static const gchar *const modules[] = { "wmux", "tabs", "containers", "modal", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *first = page("first");
	GPtrArray *loaded;
	guint i;

	memset(h, 0, sizeof(*h));
	for (i = 0; modules[i] != NULL; i++) {
		g_autofree gchar *so = gsurf_webkit_test_module_path(modules[i]);

		if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
			g_test_skip("a module is not built");
			return FALSE;
		}
	}

	/* 1. Config. */
	yaml = g_strdup_printf("browser:\n  homepage: about:blank\n"
		"  new_view_uri: http://127.0.0.1:%u/p/new\n"
		"control:\n  name: cwk\n"
		"modules:\n  wmux:\n    enabled: true\n"
		"  tabs:\n    enabled: true\n    sidebar: true\n"
		"  containers:\n    enabled: true\n    new_tab: default\n    temp_cleanup_delay: 0\n"
		"    containers:\n"
		"      - name: Media\n        color: '#f9e2af'\n        hosts: ['^localhost$']\n"
		"  modal:\n    enabled: true\n", port);
	h->config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(h->config, yaml, -1, &error));
	g_assert_no_error(error);
	gsurf_config_set_default(h->config);

	/* 2. Application and window with one view. */
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

	/* 3. Modules (earlier cases' modules are disabled first). */
	h->mgr = g_object_ref(gsurf_module_manager_get_default());
	gsurf_module_manager_set_application(h->mgr, h->app);
	gsurf_module_manager_set_config(h->mgr, h->config);
	gsurf_module_manager_set_action_executor(h->mgr, executor, h, NULL);
	loaded = gsurf_module_manager_get_modules(h->mgr);
	for (i = 0; i < loaded->len; i++)
		gsurf_module_set_enabled(g_ptr_array_index(loaded, i), FALSE);
	for (i = 0; modules[i] != NULL; i++)
		use_module(h, modules[i]);
	gsurf_module_manager_activate_all(h->mgr);
	g_assert_nonnull(gsurf_window_get_layout(h->window));

	/* 4. The control server, as the gsurf binary starts it. */
	h->server = gsurf_control_server_new(h->app);
	h->socket = g_build_filename(tmp_root, "cwk.sock", NULL);
	g_assert_true(gsurf_control_server_start(h->server, h->socket, &error));
	g_assert_no_error(error);

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

	if (h->server != NULL) {
		gsurf_control_server_stop(h->server);
		g_object_add_weak_pointer(G_OBJECT(h->server), (gpointer *)&h->server);
		g_object_unref(h->server);
		g_assert_null(h->server);
		g_assert_false(g_file_test(h->socket, G_FILE_TEST_EXISTS));
	}
	g_clear_pointer(&h->socket, g_free);
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

typedef struct {
	gboolean done;
	gchar   *line;
} Answer;

static void
on_dispatched(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Answer *a = user_data;

	a->line = gsurf_control_server_dispatch_finish(GSURF_CONTROL_SERVER(source), result);
	a->done = TRUE;
}

static JsonNode *
parse(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(json_parser_load_from_data(parser, text, -1, &error));
	g_assert_no_error(error);
	return json_node_copy(json_parser_get_root(parser));
}

/*
 * rpc:
 * @expect_code: 0 for success, else the JSON-RPC error code expected
 *
 * Calls @method with @params (JSON text or NULL) and returns the result
 * (success) or the error object.
 */
static JsonNode *
rpc(H *h, const gchar *method, const gchar *params, gint expect_code)
{
	g_autofree gchar *request = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\"%s%s}", method,
		params != NULL ? ",\"params\":" : "", params != NULL ? params : "");
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object;
	Answer a = { FALSE, NULL };
	guint i;

	gsurf_control_server_dispatch(h->server, request, on_dispatched, &a);
	for (i = 0; i < 400 && !a.done; i++)
		gsurf_webkit_test_spin(25);
	g_assert_true(a.done);
	g_assert_nonnull(a.line);
	node = parse(a.line);
	object = json_node_get_object(node);
	if (expect_code == 0 && json_object_has_member(object, "error"))
		g_error("%s(%s): unexpected error: %s", method, params != NULL ? params : "", a.line);
	if (expect_code != 0) {
		if (!json_object_has_member(object, "error"))
			g_error("%s(%s): expected error %d: %s", method, params != NULL ? params : "",
				expect_code, a.line);
		g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(object,
			"error"), "code"), ==, expect_code);
		g_free(a.line);
		return json_object_dup_member(object, "error");
	}
	g_free(a.line);
	return json_object_dup_member(object, "result");
}

static JsonNode *rpcf(H *h, const gchar *method, gint expect_code, const gchar *format, ...)
	G_GNUC_PRINTF(4, 5);

static JsonNode *
rpcf(H *h, const gchar *method, gint expect_code, const gchar *format, ...)
{
	g_autofree gchar *params = NULL;
	va_list ap;

	va_start(ap, format);
	params = g_strdup_vprintf(format, ap);
	va_end(ap);
	return rpc(h, method, params, expect_code);
}

/* The text of @node (an error's message, or any JSON). */
static gchar *
text_of(JsonNode *node)
{
	return json_to_string(node, FALSE);
}

static JsonObject *
obj(JsonNode *node)
{
	g_assert_true(JSON_NODE_HOLDS_OBJECT(node));
	return json_node_get_object(node);
}

static guint
n_panes(H *h)
{
	GsurfWindowLayout *layout = gsurf_window_get_layout(h->window);
	g_autoptr(GPtrArray) groups = gsurf_window_layout_list_groups(layout, h->window);
	guint current = 0;
	guint i;
	g_autoptr(GPtrArray) panes = NULL;

	for (i = 0; groups != NULL && i < groups->len; i++)
		if (gsurf_layout_group_get_current(g_ptr_array_index(groups, i)))
			current = gsurf_layout_group_get_index(g_ptr_array_index(groups, i));
	panes = gsurf_window_layout_list_panes(layout, h->window, current);
	return panes != NULL ? panes->len : 0;
}

/* ------------------------------------------------------------------ */
/* Views and pages                                                     */
/* ------------------------------------------------------------------ */

static void
test_views(void)
{
	H h;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *second_uri = NULL;
	g_autofree gchar *png = NULL;
	GsurfView *second;
	GsurfView *child;
	guint64 id;
	gchar *text;

	if (!gsurf_webkit_test_init() || !h_init(&h))
		return;

	/* 1. instance.info. */
	node = rpc(&h, "instance.info", NULL, 0);
	g_assert_cmpint(json_object_get_int_member(obj(node), "windows"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(obj(node), "views"), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(obj(node), "title"), ==, "first");
	/* Unnamed: a name would link into the real runtime directory. */
	g_assert_null(json_object_get_string_member(obj(node), "name"));
	g_assert_cmpint(json_object_get_int_member(obj(node), "active_view"), ==,
		(gint64)gsurf_view_get_id(h.first));
	g_clear_pointer(&node, json_node_unref);

	/* 2. open in the background: a new root that is not active. */
	second_uri = page("second");
	node = rpcf(&h, "open", 0, "{\"uri\":\"%s\",\"background\":true}", second_uri);
	id = (guint64)json_object_get_int_member(obj(node), "id");
	g_assert_false(json_object_get_boolean_member(obj(node), "active"));
	g_clear_pointer(&node, json_node_unref);
	second = gsurf_view_lookup(id);
	g_assert_nonnull(second);
	g_assert_true(gsurf_window_get_active_view(h.window) == h.first);
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(second), "second") == 0);

	/* 3. views.list agrees with the window. */
	node = rpc(&h, "views.list", NULL, 0);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), ==,
		gsurf_window_get_n_views(h.window));
	text = text_of(node);
	ASSERT_HAS(text, "\"title\":\"second\"");
	g_free(text);
	g_clear_pointer(&node, json_node_unref);

	/* 4. A foreground child of the first view. */
	node = rpcf(&h, "open", 0, "{\"uri\":\"%s\",\"parent\":%" G_GUINT64_FORMAT "}",
		second_uri, gsurf_view_get_id(h.first));
	child = gsurf_view_lookup((guint64)json_object_get_int_member(obj(node), "id"));
	g_assert_nonnull(child);
	g_assert_cmpint(json_object_get_int_member(obj(node), "opener_id"), ==,
		(gint64)gsurf_view_get_id(h.first));
	g_assert_true(gsurf_window_get_active_view(h.window) == child);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "open", "{\"container\":\"nope\"}", -32001);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "open", "{\"parent\":999999}", -32001);
	g_clear_pointer(&node, json_node_unref);

	/* 5. Activate, navigate, the page methods. */
	node = rpcf(&h, "views.activate", 0, "{\"view\":%" G_GUINT64_FORMAT "}",
		gsurf_view_get_id(h.first));
	g_clear_pointer(&node, json_node_unref);
	g_assert_true(gsurf_window_get_active_view(h.window) == h.first);
	node = rpc(&h, "views.activate", "{\"index\":99}", -32001);
	g_clear_pointer(&node, json_node_unref);
	{
		g_autofree gchar *third = page("third");

		node = rpcf(&h, "navigate", 0, "{\"uri\":\"%s\",\"view\":%" G_GUINT64_FORMAT "}",
			third, id);
		g_clear_pointer(&node, json_node_unref);
	}
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(second), "third") == 0);
	node = rpcf(&h, "page.title", 0, "{\"view\":%" G_GUINT64_FORMAT "}", id);
	g_assert_cmpstr(json_node_get_string(node), ==, "third");
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "page.uri", NULL, 0);
	g_assert_cmpstr(json_node_get_string(node), ==, gsurf_view_get_uri(h.first));
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "page.text", NULL, 0);
	ASSERT_HAS(json_node_get_string(node), "hello /p/first");
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "page.eval", "{\"script\":\"1 + 2\"}", 0);
	g_assert_cmpint(json_node_get_int(node), ==, 3);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "page.eval", "{\"script\":\"var o = {a: [1, 'x']}; o\"}", 0);
	text = text_of(node);
	g_assert_cmpstr(text, ==, "{\"a\":[1,\"x\"]}");
	g_free(text);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "page.eval", "{\"script\":\"document.title\"}", 0);
	g_assert_cmpstr(json_node_get_string(node), ==, "first");
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "page.eval", "{\"script\":\"undefined\"}", 0);
	g_assert_true(JSON_NODE_HOLDS_NULL(node));
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "page.eval", "{\"script\":\"throw new Error('boom')\"}", -32000);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "page.eval", NULL, -32602);
	g_clear_pointer(&node, json_node_unref);
	png = g_build_filename(tmp_root, "shot.png", NULL);
	node = rpcf(&h, "page.screenshot", 0, "{\"path\":\"%s\"}", png);
	g_assert_cmpint(json_object_get_int_member(obj(node), "bytes"), >, 0);
	g_clear_pointer(&node, json_node_unref);
	{
		g_autofree gchar *data = NULL;
		gsize size = 0;

		g_assert_true(g_file_get_contents(png, &data, &size, NULL));
		g_assert_cmpuint(size, >, 8);
		g_assert_true(memcmp(data, "\x89PNG", 4) == 0);
	}

	/* 6. History and loading. */
	node = rpcf(&h, "back", 0, "{\"view\":%" G_GUINT64_FORMAT "}", id);
	g_clear_pointer(&node, json_node_unref);
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(second), "second") == 0);
	node = rpcf(&h, "forward", 0, "{\"view\":%" G_GUINT64_FORMAT "}", id);
	g_clear_pointer(&node, json_node_unref);
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(second), "third") == 0);
	node = rpc(&h, "reload", "{\"hard\":true}", 0);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "stop", NULL, 0);
	g_clear_pointer(&node, json_node_unref);

	/* 7. Close: a view, then never the last one. */
	node = rpcf(&h, "views.close", 0, "{\"view\":%" G_GUINT64_FORMAT "}", id);
	g_assert_cmpint(json_object_get_int_member(obj(node), "closed"), ==, (gint64)id);
	g_clear_pointer(&node, json_node_unref);
	g_assert_null(gsurf_view_lookup(id));
	while (gsurf_window_get_n_views(h.window) > 1) {
		node = rpc(&h, "views.close", "{\"index\":1}", 0);
		g_clear_pointer(&node, json_node_unref);
	}
	node = rpc(&h, "views.close", NULL, -32000);
	g_clear_pointer(&node, json_node_unref);
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* Tree and containers                                                 */
/* ------------------------------------------------------------------ */

static void
test_tree_containers(void)
{
	H h;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *uri = NULL;
	guint64 a;
	guint64 b;
	guint64 fresh;
	gchar *text;

	if (!gsurf_webkit_test_init() || !h_init(&h))
		return;
	uri = page("a");

	/* 1. Two background children of the first view. */
	node = rpcf(&h, "open", 0, "{\"uri\":\"%s\",\"background\":true,\"parent\":%"
		G_GUINT64_FORMAT "}", uri, gsurf_view_get_id(h.first));
	a = (guint64)json_object_get_int_member(obj(node), "id");
	g_clear_pointer(&node, json_node_unref);
	node = rpcf(&h, "open", 0, "{\"uri\":\"%s\",\"background\":true,\"parent\":%"
		G_GUINT64_FORMAT "}", uri, gsurf_view_get_id(h.first));
	b = (guint64)json_object_get_int_member(obj(node), "id");
	g_clear_pointer(&node, json_node_unref);

	/* 2. tree.get: provider present, grouped by wmux window. */
	node = rpc(&h, "tree.get", NULL, 0);
	g_assert_true(json_object_get_boolean_member(obj(node), "has_tree_provider"));
	g_assert_true(json_object_get_boolean_member(obj(node), "grouped"));
	text = text_of(node);
	ASSERT_HAS(text, "\"children\":[{");
	g_free(text);
	g_clear_pointer(&node, json_node_unref);

	/* 3. Move b under a; collapse a; bad moves. */
	node = rpcf(&h, "tree.move", 0, "{\"view\":%" G_GUINT64_FORMAT ",\"parent\":%"
		G_GUINT64_FORMAT "}", b, a);
	g_assert_cmpint(json_object_get_int_member(obj(node), "parent_id"), ==, (gint64)a);
	g_clear_pointer(&node, json_node_unref);
	node = rpcf(&h, "tree.collapse", 0, "{\"view\":%" G_GUINT64_FORMAT "}", a);
	g_clear_pointer(&node, json_node_unref);
	node = rpcf(&h, "tree.collapse", 0, "{\"view\":%" G_GUINT64_FORMAT ",\"collapsed\":false}",
		a);
	g_clear_pointer(&node, json_node_unref);
	node = rpcf(&h, "tree.collapse", -32000, "{\"view\":%" G_GUINT64_FORMAT "}", b);
	g_clear_pointer(&node, json_node_unref);
	node = rpcf(&h, "tree.move", -32602, "{\"view\":%" G_GUINT64_FORMAT "}", b);
	g_clear_pointer(&node, json_node_unref);
	node = rpcf(&h, "tree.move", -32602, "{\"view\":%" G_GUINT64_FORMAT ",\"before\":%"
		G_GUINT64_FORMAT ",\"after\":%" G_GUINT64_FORMAT "}", b, a, a);
	g_clear_pointer(&node, json_node_unref);
	node = rpcf(&h, "tree.move", -32000, "{\"view\":%" G_GUINT64_FORMAT ",\"parent\":%"
		G_GUINT64_FORMAT "}", a, b);
	g_clear_pointer(&node, json_node_unref);

	/* 4. Containers: the configured one, a reopen in a temporary one. */
	node = rpc(&h, "containers.list", NULL, 0);
	text = text_of(node);
	ASSERT_HAS(text, "\"id\":\"default\"");
	ASSERT_HAS(text, "\"name\":\"Media\"");
	ASSERT_HAS(text, "\"rules_active\":true");
	g_free(text);
	g_clear_pointer(&node, json_node_unref);
	node = rpcf(&h, "containers.reopen", 0, "{\"view\":%" G_GUINT64_FORMAT
		",\"container\":\"temporary\"}", a);
	fresh = (guint64)json_object_get_int_member(obj(node), "id");
	g_assert_cmpuint(fresh, !=, a);
	g_assert_true(json_object_get_boolean_member(obj(node), "container_temporary"));
	g_clear_pointer(&node, json_node_unref);
	g_assert_null(gsurf_view_lookup(a));
	node = rpc(&h, "containers.reopen", "{\"container\":\"nope\"}", -32001);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "containers.reopen", NULL, -32602);
	g_clear_pointer(&node, json_node_unref);
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* wmux                                                                */
/* ------------------------------------------------------------------ */

static void
test_layout(void)
{
	H h;
	g_autoptr(JsonNode) node = NULL;
	gchar *text;

	if (!gsurf_webkit_test_init() || !h_init(&h))
		return;

	/* 1. describe, groups, panes. */
	node = rpc(&h, "layout.describe", NULL, 0);
	g_assert_true(json_object_get_boolean_member(obj(node), "active"));
	g_assert_cmpstr(json_object_get_string_member(obj(node), "name"), ==, "wmux");
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.groups", NULL, 0);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), ==, 1);
	g_clear_pointer(&node, json_node_unref);

	/* 2. Commands. */
	node = rpc(&h, "layout.command", "{\"command\":\"split-window -h\"}", 0);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(obj(node), "panes")),
		==, 2);
	g_clear_pointer(&node, json_node_unref);
	g_assert_cmpuint(n_panes(&h), ==, 2);
	node = rpc(&h, "layout.panes", NULL, 0);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(obj(node), "panes")),
		==, 2);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.command", "{\"command\":\"select-window -t 4\"}", -32000);
	text = text_of(node);
	ASSERT_HAS(text, "can't find window 4");
	g_free(text);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.command", NULL, -32602);
	g_clear_pointer(&node, json_node_unref);

	/* 3. Windows: new, select, rename (and back to automatic). */
	node = rpc(&h, "layout.command", "{\"command\":\"new-window\"}", 0);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.groups", NULL, 0);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), ==, 2);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.select_group", "{\"window\":0}", 0);
	g_assert_true(json_object_get_boolean_member(obj(node), "current"));
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.select_group", "{\"window\":9}", -32001);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.rename_group", "{\"window\":1,\"name\":\"mail\"}", 0);
	g_assert_cmpstr(json_object_get_string_member(obj(node), "name"), ==, "mail");
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.rename_group", "{\"window\":1}", 0);
	g_assert_cmpstr(json_object_get_string_member(obj(node), "name"), !=, "mail");
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.panes", "{\"window\":7}", -32001);
	g_clear_pointer(&node, json_node_unref);

	/* 4. show_view_at: a background tab into pane 0 (center). */
	{
		g_autofree gchar *uri = page("bg");
		guint64 id;

		node = rpcf(&h, "open", 0, "{\"uri\":\"%s\",\"background\":true}", uri);
		id = (guint64)json_object_get_int_member(obj(node), "id");
		g_clear_pointer(&node, json_node_unref);
		g_assert_false(gsurf_window_get_view_visible(h.window, gsurf_view_lookup(id)));
		node = rpcf(&h, "layout.show_view_at", 0, "{\"view\":%" G_GUINT64_FORMAT
			",\"pane\":0}", id);
		g_clear_pointer(&node, json_node_unref);
		g_assert_true(gsurf_window_get_view_visible(h.window, gsurf_view_lookup(id)));
		node = rpcf(&h, "layout.show_view_at", -32602, "{\"view\":%" G_GUINT64_FORMAT
			",\"pane\":0,\"zone\":\"diagonal\"}", id);
		g_clear_pointer(&node, json_node_unref);
		node = rpcf(&h, "layout.show_view_at", -32602, "{\"view\":%" G_GUINT64_FORMAT "}", id);
		g_clear_pointer(&node, json_node_unref);
	}
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* Modes, actions, wmux off, kiosk                                     */
/* ------------------------------------------------------------------ */

static void
test_modes_actions(void)
{
	H h;
	g_autoptr(JsonNode) node = NULL;
	gdouble zoom;

	if (!gsurf_webkit_test_init() || !h_init(&h))
		return;

	/* 1. Modes. */
	node = rpc(&h, "mode.get", NULL, 0);
	g_assert_true(json_object_get_boolean_member(obj(node), "modal_active"));
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "mode.set", "{\"mode\":\"insert\"}", 0);
	g_assert_cmpstr(json_object_get_string_member(obj(node), "mode"), ==, "insert");
	g_clear_pointer(&node, json_node_unref);
	g_assert_cmpint(gsurf_module_manager_get_mode(h.mgr), ==, GSURF_MODE_INSERT);
	node = rpc(&h, "mode.set", "{\"mode\":\"normal\"}", 0);
	g_clear_pointer(&node, json_node_unref);
	g_assert_cmpint(gsurf_module_manager_get_mode(h.mgr), ==, GSURF_MODE_NORMAL);

	/* 2. Actions: a module's (wmux) and the host's (zoom-in). */
	node = rpc(&h, "action.run", "{\"name\":\"pane-split-right\"}", 0);
	g_assert_true(json_object_get_boolean_member(obj(node), "performed"));
	g_clear_pointer(&node, json_node_unref);
	g_assert_cmpuint(n_panes(&h), ==, 2);
	zoom = gsurf_view_get_zoom_level(h.first);
	node = rpcf(&h, "action.run", 0, "{\"name\":\"zoom-in\",\"view\":%" G_GUINT64_FORMAT "}",
		gsurf_view_get_id(h.first));
	g_clear_pointer(&node, json_node_unref);
	g_assert_cmpuint(h.zooms, ==, 1);
	g_assert_cmpfloat_with_epsilon(gsurf_view_get_zoom_level(h.first), zoom + 0.1, 0.001);
	node = rpc(&h, "action.run", NULL, -32602);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "actions.list", NULL, 0);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), ==,
		GSURF_ACTION_LAST - 1);
	g_clear_pointer(&node, json_node_unref);

	/* 3. wmux off: the layout methods say so; the tree is not grouped. */
	node = rpc(&h, "modules.toggle", "{\"name\":\"wmux\",\"enabled\":false}", 0);
	g_assert_false(json_object_get_boolean_member(obj(node), "active"));
	g_clear_pointer(&node, json_node_unref);
	g_assert_null(gsurf_window_get_layout(h.window));
	node = rpc(&h, "layout.panes", NULL, -32002);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.command", "{\"command\":\"split-window\"}", -32002);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "tree.get", NULL, 0);
	g_assert_false(json_object_get_boolean_member(obj(node), "grouped"));
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.describe", NULL, 0);
	g_assert_false(json_object_get_boolean_member(obj(node), "active"));
	g_clear_pointer(&node, json_node_unref);

	/* 4. Kiosk refuses opening, eval, toggles and the actions it forbids;
	 *    reading still works. */
	h.config->kiosk = TRUE;
	node = rpc(&h, "open", NULL, -32003);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "page.eval", "{\"script\":\"1\"}", -32003);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "action.run", "{\"name\":\"tab-new\"}", -32003);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "modules.toggle", "{\"name\":\"wmux\"}", -32003);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "layout.panes", NULL, -32003);
	g_clear_pointer(&node, json_node_unref);
	node = rpcf(&h, "page.title", 0, "{\"view\":%" G_GUINT64_FORMAT "}",
		gsurf_view_get_id(h.first));
	g_assert_cmpstr(json_node_get_string(node), ==, "first");
	g_clear_pointer(&node, json_node_unref);
	h.config->kiosk = FALSE;
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* Events over the socket                                              */
/* ------------------------------------------------------------------ */

typedef struct {
	const gchar *path;
	gint         ready;
	gint         done;
	GPtrArray   *events;   /* "topic event", in order */
	gchar       *failure;
} Watch;

static gpointer
watch_thread(gpointer data)
{
	Watch *w = data;
	g_autoptr(GsurfControlClient) client = gsurf_control_client_new();
	g_autoptr(GError) error = NULL;
	const gchar *const topics[] = { "view", "uri", "title", "mode", NULL };
	guint i;

	if (!gsurf_control_client_connect(client, w->path, &error) ||
	    !gsurf_control_client_subscribe(client, topics, &error)) {
		w->failure = g_strdup(error->message);
		g_atomic_int_set(&w->done, TRUE);
		return NULL;
	}
	g_atomic_int_set(&w->ready, TRUE);
	for (i = 0; i < 200; i++) {
		g_autoptr(JsonNode) event = gsurf_control_client_read_event(client, 100, &error);
		JsonObject *object;
		JsonObject *payload;
		gboolean last;

		if (error != NULL)
			break;
		if (event == NULL)
			continue;
		object = json_node_get_object(event);
		payload = JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "data"))
			? json_object_get_object_member(object, "data") : NULL;
		g_ptr_array_add(w->events, g_strdup_printf("%s %s",
			json_object_get_string_member(object, "topic"),
			json_object_get_string_member(object, "event")));
		last = g_strcmp0(json_object_get_string_member(object, "topic"), "mode") == 0 &&
			payload != NULL &&
			g_strcmp0(json_object_get_string_member(payload, "mode"), "normal") == 0;
		if (last)
			break;
	}
	g_atomic_int_set(&w->done, TRUE);
	return NULL;
}

/* The position of @what in the event list, or -1. */
static gint
event_at(Watch *w, const gchar *what)
{
	guint i;

	for (i = 0; i < w->events->len; i++)
		if (g_strcmp0(g_ptr_array_index(w->events, i), what) == 0)
			return (gint)i;
	return -1;
}

static void
test_events(void)
{
	H h;
	Watch w = { 0 };
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *uri = NULL;
	GThread *thread;
	guint i;

	if (!gsurf_webkit_test_init() || !h_init(&h))
		return;
	w.path = h.socket;
	w.events = g_ptr_array_new_with_free_func(g_free);
	thread = g_thread_new("control-watch", watch_thread, &w);
	for (i = 0; i < 400 && !g_atomic_int_get(&w.ready) && !g_atomic_int_get(&w.done); i++)
		gsurf_webkit_test_spin(10);
	g_assert_true(g_atomic_int_get(&w.ready));
	g_assert_cmpuint(gsurf_control_server_get_n_clients(h.server), ==, 1);

	/* A foreground tab, its load, a mode round trip. */
	uri = page("evt");
	node = rpcf(&h, "open", 0, "{\"uri\":\"%s\"}", uri);
	g_clear_pointer(&node, json_node_unref);
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(gsurf_window_get_active_view(h.window)),
		"evt") == 0);
	node = rpc(&h, "mode.set", "{\"mode\":\"insert\"}", 0);
	g_clear_pointer(&node, json_node_unref);
	node = rpc(&h, "mode.set", "{\"mode\":\"normal\"}", 0);
	g_clear_pointer(&node, json_node_unref);
	for (i = 0; i < 400 && !g_atomic_int_get(&w.done); i++)
		gsurf_webkit_test_spin(10);
	g_thread_join(thread);
	if (w.failure != NULL)
		g_error("watch: %s", w.failure);

	/* In order: added, active, the URI, the title, the modes. */
	g_assert_cmpint(event_at(&w, "view view.added"), >=, 0);
	g_assert_cmpint(event_at(&w, "view view.added"), <, event_at(&w, "view view.active"));
	g_assert_cmpint(event_at(&w, "view view.active"), <, event_at(&w, "uri uri"));
	g_assert_cmpint(event_at(&w, "uri uri"), <, event_at(&w, "title title"));
	g_assert_cmpint(event_at(&w, "title title"), <, event_at(&w, "mode mode"));
	/* Not subscribed: no load events. */
	g_assert_cmpint(event_at(&w, "load load"), ==, -1);
	g_ptr_array_unref(w.events);
	h_free(&h);
}

int
main(int argc, char *argv[])
{
	int ret;

	g_test_init(&argc, &argv, NULL);
	tmp_root = g_dir_make_tmp("gsurf-control-webkit-XXXXXX", NULL);
	g_assert_nonnull(tmp_root);
	g_setenv("XDG_DATA_HOME", tmp_root, TRUE);
	g_setenv("XDG_CACHE_HOME", tmp_root, TRUE);
	g_setenv("XDG_CONFIG_HOME", tmp_root, TRUE);

	soup = soup_server_new(NULL, NULL);
	soup_server_add_handler(soup, NULL, serve, NULL, NULL);
	port = gsurf_webkit_test_listen(soup);

	g_test_add_func("/gsurf/control/webkit/views", test_views);
	g_test_add_func("/gsurf/control/webkit/tree-containers", test_tree_containers);
	g_test_add_func("/gsurf/control/webkit/layout", test_layout);
	g_test_add_func("/gsurf/control/webkit/modes-actions", test_modes_actions);
	g_test_add_func("/gsurf/control/webkit/events", test_events);

	ret = g_test_run();
	g_clear_object(&soup);
	gsurf_test_rmtree(tmp_root);
	g_free(tmp_root);
	return ret;
}
