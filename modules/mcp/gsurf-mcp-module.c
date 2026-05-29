/*
 * gsurf-mcp-module.c - MCP server for AI assistant control of the browser
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exposes browser navigation, content, and scripting as MCP tools over a
 * Unix-domain socket (via deps/mcp-glib). Built only when MCP=1. Every
 * tool defaults OFF and is enabled per-tool under modules.mcp.tools.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <json-glib/json-glib.h>
#include <mcp.h>
#include <string.h>
#include <unistd.h>

#define GSURF_TYPE_MCP_MODULE (gsurf_mcp_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfMcpModule, gsurf_mcp_module, GSURF, MCP_MODULE, GsurfModule)

struct _GsurfMcpModule
{
	GsurfModule parent_instance;

	gchar *transport;     /* "unix-socket" (default), "stdio" */
	gchar *socket_name;   /* optional custom name */

	McpUnixSocketServer *unix_server;

	/* Per-tool enable flags (all default FALSE). */
	GHashTable *tools;    /* gchar* tool-name -> GINT(enabled) */
};

G_DEFINE_FINAL_TYPE(GsurfMcpModule, gsurf_mcp_module, GSURF_TYPE_MODULE)

/* ===== helpers ===== */

static GsurfView *
active_view(void)
{
	return gsurf_module_manager_get_active_view(gsurf_module_manager_get_default());
}

static GsurfWindow *
active_window(void)
{
	return gsurf_module_manager_get_active_window(gsurf_module_manager_get_default());
}

static McpToolResult *
text_result(const gchar *text)
{
	McpToolResult *r = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(r, text ? text : "");
	return r;
}

static McpToolResult *
error_result(const gchar *text)
{
	McpToolResult *r = mcp_tool_result_new(TRUE);
	mcp_tool_result_add_text(r, text ? text : "error");
	return r;
}

typedef struct {
	GMainLoop *loop;
	GsurfView *view;
	gchar     *result;
	GError    *error;
} JsCtx;

static void
js_done(GObject *source, GAsyncResult *res, gpointer data)
{
	JsCtx *c = data;
	c->result = gsurf_view_run_javascript_finish(c->view, res, &c->error);
	g_main_loop_quit(c->loop);
}

/* Run JS and return its (string/JSON) value synchronously via a nested
 * main loop. Returns newly-allocated text, or %NULL on error. */
static gchar *
run_js_sync(GsurfView *view, const gchar *script, GError **error)
{
	JsCtx c = { 0 };

	c.view = view;
	c.loop = g_main_loop_new(NULL, FALSE);
	gsurf_view_run_javascript_async(view, script, NULL, js_done, &c);
	g_main_loop_run(c.loop);
	g_main_loop_unref(c.loop);

	if (c.error != NULL) {
		g_propagate_error(error, c.error);
		return NULL;
	}
	return c.result;
}

/* ===== tool handlers ===== */

static McpToolResult *
tool_navigate(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	const gchar *uri = args && json_object_has_member(args, "uri")
		? json_object_get_string_member(args, "uri") : NULL;

	if (view == NULL)
		return error_result("no active view");
	if (uri == NULL)
		return error_result("missing 'uri'");

	gsurf_view_load_uri(view, uri);
	return text_result("ok");
}

static McpToolResult *
tool_get_current_uri(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	if (view == NULL)
		return error_result("no active view");
	return text_result(gsurf_view_get_uri(view));
}

static McpToolResult *
tool_get_title(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	if (view == NULL)
		return error_result("no active view");
	return text_result(gsurf_view_get_title(view));
}

static McpToolResult *
tool_go_back(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	if (view == NULL)
		return error_result("no active view");
	gsurf_view_go_back(view);
	return text_result("ok");
}

static McpToolResult *
tool_go_forward(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	if (view == NULL)
		return error_result("no active view");
	gsurf_view_go_forward(view);
	return text_result("ok");
}

static McpToolResult *
tool_reload(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	gboolean bypass = args && json_object_has_member(args, "bypass_cache")
		? json_object_get_boolean_member(args, "bypass_cache") : FALSE;

	if (view == NULL)
		return error_result("no active view");
	gsurf_view_reload(view, bypass);
	return text_result("ok");
}

static McpToolResult *
tool_stop(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	if (view == NULL)
		return error_result("no active view");
	gsurf_view_stop_loading(view);
	return text_result("ok");
}

static McpToolResult *
tool_get_page_text(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;

	if (view == NULL)
		return error_result("no active view");
	text = run_js_sync(view, "document.body ? document.body.innerText : ''", &error);
	if (text == NULL)
		return error_result(error ? error->message : "javascript failed");
	return text_result(text);
}

static McpToolResult *
tool_get_page_html(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *html = NULL;

	if (view == NULL)
		return error_result("no active view");
	html = run_js_sync(view, "document.documentElement.outerHTML", &error);
	if (html == NULL)
		return error_result(error ? error->message : "javascript failed");
	return text_result(html);
}

static McpToolResult *
tool_get_links(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	static const gchar *js =
		"JSON.stringify(Array.from(document.querySelectorAll('a[href]'))"
		".slice(0,500).map(a=>({text:(a.innerText||'').trim().slice(0,200),href:a.href})))";

	if (view == NULL)
		return error_result("no active view");
	json = run_js_sync(view, js, &error);
	if (json == NULL)
		return error_result(error ? error->message : "javascript failed");
	return text_result(json);
}

static McpToolResult *
tool_execute_javascript(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	const gchar *code = args && json_object_has_member(args, "code")
		? json_object_get_string_member(args, "code") : NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;

	if (view == NULL)
		return error_result("no active view");
	if (code == NULL)
		return error_result("missing 'code'");
	result = run_js_sync(view, code, &error);
	if (result == NULL && error != NULL)
		return error_result(error->message);
	return text_result(result ? result : "null");
}

static McpToolResult *
tool_scroll(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	const gchar *dir = args && json_object_has_member(args, "direction")
		? json_object_get_string_member(args, "direction") : "down";
	const gchar *js;

	if (view == NULL)
		return error_result("no active view");

	if (g_strcmp0(dir, "up") == 0)        js = "window.scrollBy(0,-window.innerHeight*0.9)";
	else if (g_strcmp0(dir, "top") == 0)  js = "window.scrollTo(0,0)";
	else if (g_strcmp0(dir, "bottom") == 0) js = "window.scrollTo(0,document.body.scrollHeight)";
	else                                  js = "window.scrollBy(0,window.innerHeight*0.9)";

	gsurf_view_run_javascript_async(view, js, NULL, NULL, NULL);
	return text_result("ok");
}

static McpToolResult *
tool_list_views(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfWindow *window = active_window();
	GsurfView *active = window ? gsurf_window_get_active_view(window) : NULL;
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonGenerator) gen = NULL;
	g_autoptr(JsonNode) root = NULL;
	guint i, n;
	gchar *out;

	if (window == NULL)
		return error_result("no active window");

	json_builder_begin_array(b);
	n = gsurf_window_get_n_views(window);
	for (i = 0; i < n; i++) {
		GsurfView *v = gsurf_window_get_nth_view(window, i);
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "index");
		json_builder_add_int_value(b, i);
		json_builder_set_member_name(b, "uri");
		json_builder_add_string_value(b, gsurf_view_get_uri(v) ? gsurf_view_get_uri(v) : "");
		json_builder_set_member_name(b, "title");
		json_builder_add_string_value(b, gsurf_view_get_title(v) ? gsurf_view_get_title(v) : "");
		json_builder_set_member_name(b, "active");
		json_builder_add_boolean_value(b, v == active);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);

	root = json_builder_get_root(b);
	gen = json_generator_new();
	json_generator_set_root(gen, root);
	out = json_generator_to_data(gen, NULL);

	{
		McpToolResult *r = text_result(out);
		g_free(out);
		return r;
	}
}

/* ===== tool registration ===== */

typedef struct {
	const gchar    *name;
	const gchar    *description;
	const gchar    *arg;      /* single string/bool argument, or NULL */
	const gchar    *arg_type; /* "string" or "boolean" */
	const gchar    *arg_desc;
	gboolean        arg_required;
	McpToolHandler  handler;
} ToolDef;

static const ToolDef tool_defs[] = {
	{ "navigate", "Load a URI in the active view", "uri", "string", "the URI to load", TRUE, tool_navigate },
	{ "get_current_uri", "Get the active view's current URI", NULL, NULL, NULL, FALSE, tool_get_current_uri },
	{ "get_title", "Get the active view's page title", NULL, NULL, NULL, FALSE, tool_get_title },
	{ "go_back", "Navigate back in history", NULL, NULL, NULL, FALSE, tool_go_back },
	{ "go_forward", "Navigate forward in history", NULL, NULL, NULL, FALSE, tool_go_forward },
	{ "reload", "Reload the active view", "bypass_cache", "boolean", "bypass the cache", FALSE, tool_reload },
	{ "stop", "Stop loading the active view", NULL, NULL, NULL, FALSE, tool_stop },
	{ "get_page_text", "Get the rendered text of the page", NULL, NULL, NULL, FALSE, tool_get_page_text },
	{ "get_page_html", "Get the full HTML of the page", NULL, NULL, NULL, FALSE, tool_get_page_html },
	{ "get_links", "List the links on the page as JSON", NULL, NULL, NULL, FALSE, tool_get_links },
	{ "execute_javascript", "Run JavaScript in the page and return the result", "code", "string", "the JavaScript to run", TRUE, tool_execute_javascript },
	{ "scroll", "Scroll the page (direction: up/down/top/bottom)", "direction", "string", "scroll direction", FALSE, tool_scroll },
	{ "list_views", "List open views/tabs as JSON", NULL, NULL, NULL, FALSE, tool_list_views },
	{ NULL, NULL, NULL, NULL, NULL, FALSE, NULL }
};

static JsonNode *
build_schema(const ToolDef *def)
{
	g_autoptr(JsonBuilder) b = json_builder_new();

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "object");
	json_builder_set_member_name(b, "properties");
	json_builder_begin_object(b);
	if (def->arg != NULL) {
		json_builder_set_member_name(b, def->arg);
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, def->arg_type);
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, def->arg_desc);
		json_builder_end_object(b);
	}
	json_builder_end_object(b); /* properties */
	if (def->arg != NULL && def->arg_required) {
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, def->arg);
		json_builder_end_array(b);
	}
	json_builder_end_object(b);

	return json_builder_get_root(b);
}

static void
register_tools(GsurfMcpModule *self, McpServer *server)
{
	gint i;

	for (i = 0; tool_defs[i].name != NULL; i++) {
		const ToolDef *def = &tool_defs[i];
		McpTool *tool;
		JsonNode *schema;

		if (!GPOINTER_TO_INT(g_hash_table_lookup(self->tools, def->name)))
			continue;

		tool = mcp_tool_new(def->name, def->description);
		schema = build_schema(def);
		mcp_tool_set_input_schema(tool, schema);
		json_node_unref(schema);

		mcp_server_add_tool(server, tool, def->handler, self, NULL);
		g_object_unref(tool);
	}
}

static void
on_session_created(McpUnixSocketServer *server, McpServer *session, gpointer user_data)
{
	register_tools(GSURF_MCP_MODULE(user_data), session);
}

/* ===== module lifecycle ===== */

static void
gsurf_mcp_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfMcpModule *self = GSURF_MCP_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m, *tools;

	node = gsurf_config_get_module_node(config, "mcp");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "transport")) {
		g_free(self->transport);
		self->transport = g_strdup(yaml_mapping_get_string_member(m, "transport"));
	}
	if (yaml_mapping_has_member(m, "socket_name")) {
		g_free(self->socket_name);
		self->socket_name = g_strdup(yaml_mapping_get_string_member(m, "socket_name"));
	}

	tools = yaml_mapping_get_mapping_member(m, "tools");
	if (tools != NULL) {
		GList *names = yaml_mapping_get_members(tools), *l;
		for (l = names; l != NULL; l = l->next) {
			const gchar *tname = l->data;
			gboolean on = yaml_mapping_get_boolean_member(tools, tname);
			g_hash_table_replace(self->tools, g_strdup(tname),
				GINT_TO_POINTER(on ? 1 : 0));
		}
		g_list_free(names);
	}
}

static gboolean
gsurf_mcp_activate(GsurfModule *module)
{
	GsurfMcpModule *self = GSURF_MCP_MODULE(module);
	g_autofree gchar *sock = NULL;
	g_autoptr(GError) error = NULL;

	/* Unix socket is the only transport wired up here; stdio is handled
	 * by the standalone gsurf-mcp relay. */
	if (self->transport != NULL && g_strcmp0(self->transport, "unix-socket") != 0) {
		g_message("gsurf mcp: transport '%s' not supported in-process; "
			"use the gsurf-mcp relay", self->transport);
		return TRUE;
	}

	if (self->socket_name != NULL)
		sock = g_strdup_printf("%s/gsurf-mcp-%s.sock",
			g_get_user_runtime_dir(), self->socket_name);
	else
		sock = g_strdup_printf("%s/gsurf-mcp-%d.sock",
			g_get_user_runtime_dir(), (int)getpid());

	self->unix_server = mcp_unix_socket_server_new("gsurf", GSURF_VERSION_STRING, sock);
	mcp_unix_socket_server_set_instructions(self->unix_server,
		"Control the gsurf web browser: navigate, read page content, run JavaScript.");
	g_signal_connect(self->unix_server, "session-created",
		G_CALLBACK(on_session_created), self);

	if (!mcp_unix_socket_server_start(self->unix_server, &error)) {
		g_warning("gsurf mcp: failed to start server: %s",
			error ? error->message : "unknown");
		g_clear_object(&self->unix_server);
		return FALSE;
	}

	g_message("gsurf mcp: listening on %s", sock);
	return TRUE;
}

static void
gsurf_mcp_deactivate(GsurfModule *module)
{
	GsurfMcpModule *self = GSURF_MCP_MODULE(module);

	if (self->unix_server != NULL) {
		mcp_unix_socket_server_stop(self->unix_server);
		g_clear_object(&self->unix_server);
	}
}

static const gchar *
gsurf_mcp_get_name(GsurfModule *module)
{
	return "mcp";
}

static const gchar *
gsurf_mcp_get_description(GsurfModule *module)
{
	return "MCP server for AI assistant integration";
}

static void
gsurf_mcp_module_finalize(GObject *object)
{
	GsurfMcpModule *self = GSURF_MCP_MODULE(object);

	g_clear_pointer(&self->transport, g_free);
	g_clear_pointer(&self->socket_name, g_free);
	g_clear_pointer(&self->tools, g_hash_table_unref);

	G_OBJECT_CLASS(gsurf_mcp_module_parent_class)->finalize(object);
}

static void
gsurf_mcp_module_class_init(GsurfMcpModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_mcp_module_finalize;

	module_class->activate = gsurf_mcp_activate;
	module_class->deactivate = gsurf_mcp_deactivate;
	module_class->configure = gsurf_mcp_configure;
	module_class->get_name = gsurf_mcp_get_name;
	module_class->get_description = gsurf_mcp_get_description;
}

static void
gsurf_mcp_module_init(GsurfMcpModule *self)
{
	self->transport = g_strdup("unix-socket");
	self->socket_name = NULL;
	self->unix_server = NULL;
	self->tools = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_MCP_MODULE;
}
