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

/* ----- find / interaction ----- */

static McpToolResult *
tool_find_text(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	const gchar *text = args && json_object_has_member(args, "text")
		? json_object_get_string_member(args, "text") : NULL;
	gboolean forward = !(args && json_object_has_member(args, "backward") &&
		json_object_get_boolean_member(args, "backward"));

	if (view == NULL)
		return error_result("no active view");
	if (text == NULL)
		return error_result("missing 'text'");
	gsurf_view_find(view, text, FALSE, forward);
	return text_result("ok");
}

static McpToolResult *
tool_click_link(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *js = NULL;
	g_autofree gchar *result = NULL;

	if (view == NULL)
		return error_result("no active view");

	if (args && json_object_has_member(args, "selector")) {
		const gchar *sel = json_object_get_string_member(args, "selector");
		g_autofree gchar *esc = g_strescape(sel ? sel : "", NULL);
		js = g_strdup_printf(
			"(function(){var e=document.querySelector(\"%s\");"
			"if(e){e.click();return e.href||'clicked';}return null;})()", esc);
	} else {
		gint64 idx = (args && json_object_has_member(args, "index"))
			? json_object_get_int_member(args, "index") : 0;
		js = g_strdup_printf(
			"(function(){var a=document.querySelectorAll('a[href]');"
			"if(a[%" G_GINT64_FORMAT "]){a[%" G_GINT64_FORMAT "].click();"
			"return a[%" G_GINT64_FORMAT "].href;}return null;})()",
			idx, idx, idx);
	}

	result = run_js_sync(view, js, &error);
	if (result == NULL)
		return error_result(error ? error->message : "no matching link");
	return text_result(result);
}

/* ----- view management ----- */

static McpToolResult *
tool_switch_view(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfWindow *window = active_window();
	gint64 idx = (args && json_object_has_member(args, "index"))
		? json_object_get_int_member(args, "index") : -1;

	if (window == NULL)
		return error_result("no active window");
	if (idx < 0 || idx >= gsurf_window_get_n_views(window))
		return error_result("index out of range");
	gsurf_window_set_active_view(window, gsurf_window_get_nth_view(window, (guint)idx));
	return text_result("ok");
}

static McpToolResult *
tool_open_view(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfWindow *window = active_window();
	GsurfConfig *config = gsurf_config_get_default();
	const gchar *uri = args && json_object_has_member(args, "uri")
		? json_object_get_string_member(args, "uri") : NULL;
	GsurfView *view;
	g_autofree gchar *msg = NULL;

	if (window == NULL)
		return error_result("no active window");

	view = gsurf_view_new();
	if (config != NULL && config->settings != NULL)
		gsurf_view_apply_settings(view, config->settings);
	gsurf_window_add_view(window, view);
	gsurf_window_set_active_view(window, view);
	gsurf_view_load_uri(view, (uri != NULL && *uri != '\0') ? uri
		: ((config && config->new_view_uri) ? config->new_view_uri : "about:blank"));
	g_object_unref(view);

	msg = g_strdup_printf("%u", gsurf_window_get_n_views(window) - 1);
	return text_result(msg);
}

static McpToolResult *
tool_close_view(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfWindow *window = active_window();
	gint64 idx = (args && json_object_has_member(args, "index"))
		? json_object_get_int_member(args, "index") : -1;

	if (window == NULL)
		return error_result("no active window");
	if (gsurf_window_get_n_views(window) <= 1)
		return error_result("cannot close the last view");
	if (idx < 0 || idx >= gsurf_window_get_n_views(window))
		return error_result("index out of range");
	gsurf_window_remove_view(window, gsurf_window_get_nth_view(window, (guint)idx));
	return text_result("ok");
}

/* ----- capture ----- */

typedef struct {
	GMainLoop *loop;
	GsurfView *view;
	GBytes    *bytes;
	GError    *error;
} SnapCtx;

static void
snap_done(GObject *source, GAsyncResult *res, gpointer data)
{
	SnapCtx *c = data;
	c->bytes = gsurf_view_get_snapshot_finish(c->view, res, &c->error);
	g_main_loop_quit(c->loop);
}

static GBytes *
snapshot_sync(GsurfView *view, GError **error)
{
	SnapCtx c = { 0 };

	c.view = view;
	c.loop = g_main_loop_new(NULL, FALSE);
	gsurf_view_get_snapshot_async(view, NULL, snap_done, &c);
	g_main_loop_run(c.loop);
	g_main_loop_unref(c.loop);

	if (c.error != NULL) {
		g_propagate_error(error, c.error);
		return NULL;
	}
	return c.bytes;
}

static McpToolResult *
tool_screenshot(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *b64 = NULL;
	gsize len;
	const guchar *data;
	McpToolResult *r;

	if (view == NULL)
		return error_result("no active view");
	bytes = snapshot_sync(view, &error);
	if (bytes == NULL)
		return error_result(error ? error->message : "snapshot failed");

	data = g_bytes_get_data(bytes, &len);
	b64 = g_base64_encode(data, len);
	r = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_image(r, b64, "image/png");
	return r;
}

static McpToolResult *
tool_save_screenshot(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	const gchar *path = args && json_object_has_member(args, "path")
		? json_object_get_string_member(args, "path") : NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	gsize len;
	const guchar *data;

	if (view == NULL)
		return error_result("no active view");
	if (path == NULL)
		return error_result("missing 'path'");
	bytes = snapshot_sync(view, &error);
	if (bytes == NULL)
		return error_result(error ? error->message : "snapshot failed");

	data = g_bytes_get_data(bytes, &len);
	if (!g_file_set_contents(path, (const gchar *)data, len, &error))
		return error_result(error ? error->message : "write failed");
	return text_result(path);
}

/* ----- history / cookies ----- */

static McpToolResult *
tool_get_history(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfConfig *config = gsurf_config_get_default();
	YamlNode *node;
	const gchar *file = NULL;
	g_autofree gchar *expanded = NULL;
	g_autofree gchar *contents = NULL;
	gint64 limit = (args && json_object_has_member(args, "limit"))
		? json_object_get_int_member(args, "limit") : 100;

	if (config == NULL)
		return error_result("no config");
	node = gsurf_config_get_module_node(config, "history");
	if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING) {
		YamlMapping *m = yaml_node_get_mapping(node);
		if (yaml_mapping_has_member(m, "file"))
			file = yaml_mapping_get_string_member(m, "file");
	}
	if (file == NULL)
		return error_result("history file not configured");

	expanded = (file[0] == '~' && file[1] == '/')
		? g_build_filename(g_get_home_dir(), file + 2, NULL) : g_strdup(file);
	if (!g_file_get_contents(expanded, &contents, NULL, NULL))
		return text_result("");

	/* Return the last `limit` lines. */
	{
		g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
		guint total = g_strv_length(lines);
		guint start = (limit > 0 && total > limit) ? (guint)(total - limit) : 0;
		GString *out = g_string_new(NULL);
		for (guint i = start; i < total; i++)
			if (*lines[i] != '\0')
				g_string_append_printf(out, "%s\n", lines[i]);
		return text_result(g_string_free_and_steal(out));
	}
}

static McpToolResult *
tool_get_cookies(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfView *view = active_view();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookies = NULL;

	if (view == NULL)
		return error_result("no active view");
	/* Non-HttpOnly cookies visible to the current document. */
	cookies = run_js_sync(view, "document.cookie", &error);
	if (cookies == NULL)
		return error_result(error ? error->message : "javascript failed");
	return text_result(cookies);
}

/* ----- config / settings ----- */

static gchar *
config_to_json(GsurfConfig *c)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonGenerator) gen = NULL;
	g_autoptr(JsonNode) root = NULL;

	json_builder_begin_object(b);
#define ADD_STR(k, v) do { json_builder_set_member_name(b, k); \
	json_builder_add_string_value(b, (v) ? (v) : ""); } while (0)
#define ADD_INT(k, v) do { json_builder_set_member_name(b, k); \
	json_builder_add_int_value(b, (v)); } while (0)
#define ADD_DBL(k, v) do { json_builder_set_member_name(b, k); \
	json_builder_add_double_value(b, (v)); } while (0)
#define ADD_BOOL(k, v) do { json_builder_set_member_name(b, k); \
	json_builder_add_boolean_value(b, (v)); } while (0)
	ADD_STR("homepage", c->homepage);
	ADD_STR("new_view_uri", c->new_view_uri);
	ADD_STR("user_agent", c->user_agent);
	ADD_STR("window_title", c->window_title);
	ADD_INT("window_width", c->window_width);
	ADD_INT("window_height", c->window_height);
	ADD_DBL("default_zoom", c->default_zoom);
	ADD_BOOL("smooth_scroll", c->smooth_scroll);
	ADD_BOOL("fullscreen_on_start", c->fullscreen_on_start);
	ADD_STR("cookie_jar", c->cookie_jar);
	ADD_STR("proxy_mode", c->proxy_mode);
	ADD_STR("proxy_uri", c->proxy_uri);
	ADD_BOOL("do_not_track", c->do_not_track);
	ADD_BOOL("tls_strict", c->tls_strict);
	ADD_BOOL("ephemeral", c->ephemeral);
	ADD_BOOL("enable_disk_cache", c->enable_disk_cache);
#undef ADD_STR
#undef ADD_INT
#undef ADD_DBL
#undef ADD_BOOL
	json_builder_end_object(b);

	root = json_builder_get_root(b);
	gen = json_generator_new();
	json_generator_set_root(gen, root);
	return json_generator_to_data(gen, NULL);
}

static McpToolResult *
tool_get_config(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfConfig *config = gsurf_config_get_default();
	g_autofree gchar *json = NULL;

	if (config == NULL)
		return error_result("no config");
	json = config_to_json(config);
	return text_result(json);
}

static gboolean
parse_bool(const gchar *v)
{
	return g_strcmp0(v, "true") == 0 || g_strcmp0(v, "1") == 0 ||
	       g_strcmp0(v, "yes") == 0 || g_strcmp0(v, "on") == 0;
}

static McpToolResult *
tool_set_config(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfConfig *config = gsurf_config_get_default();
	const gchar *key = args && json_object_has_member(args, "key")
		? json_object_get_string_member(args, "key") : NULL;
	const gchar *value = args && json_object_has_member(args, "value")
		? json_object_get_string_member(args, "value") : NULL;

	if (config == NULL)
		return error_result("no config");
	if (key == NULL || value == NULL)
		return error_result("missing 'key' or 'value'");

	if (g_strcmp0(key, "homepage") == 0) {
		g_free(config->homepage); config->homepage = g_strdup(value);
	} else if (g_strcmp0(key, "new_view_uri") == 0) {
		g_free(config->new_view_uri); config->new_view_uri = g_strdup(value);
	} else if (g_strcmp0(key, "user_agent") == 0) {
		g_free(config->user_agent); config->user_agent = g_strdup(value);
	} else if (g_strcmp0(key, "default_zoom") == 0) {
		config->default_zoom = g_ascii_strtod(value, NULL);
	} else if (g_strcmp0(key, "tls_strict") == 0) {
		config->tls_strict = parse_bool(value);
	} else if (g_strcmp0(key, "do_not_track") == 0) {
		config->do_not_track = parse_bool(value);
	} else if (g_strcmp0(key, "ephemeral") == 0) {
		config->ephemeral = parse_bool(value);
	} else if (g_strcmp0(key, "enable_disk_cache") == 0) {
		config->enable_disk_cache = parse_bool(value);
	} else {
		return error_result("unknown or read-only config key");
	}
	return text_result("ok");
}

static McpToolResult *
tool_set_setting(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfConfig *config = gsurf_config_get_default();
	GsurfView *view = active_view();
	GsurfSettings *st = config ? config->settings : NULL;
	const gchar *key = args && json_object_has_member(args, "key")
		? json_object_get_string_member(args, "key") : NULL;
	const gchar *value = args && json_object_has_member(args, "value")
		? json_object_get_string_member(args, "value") : NULL;
	gboolean on;

	if (st == NULL)
		return error_result("no settings");
	if (key == NULL || value == NULL)
		return error_result("missing 'key' or 'value'");
	on = parse_bool(value);

	if      (g_strcmp0(key, "javascript") == 0)             st->javascript = on;
	else if (g_strcmp0(key, "images") == 0)                 st->images = on;
	else if (g_strcmp0(key, "webgl") == 0)                  st->webgl = on;
	else if (g_strcmp0(key, "webaudio") == 0)               st->webaudio = on;
	else if (g_strcmp0(key, "media") == 0)                  st->media = on;
	else if (g_strcmp0(key, "media_stream") == 0)           st->media_stream = on;
	else if (g_strcmp0(key, "webrtc") == 0)                 st->webrtc = on;
	else if (g_strcmp0(key, "plugins") == 0)                st->plugins = on;
	else if (g_strcmp0(key, "smooth_scrolling") == 0)       st->smooth_scrolling = on;
	else if (g_strcmp0(key, "caret_browsing") == 0)         st->caret_browsing = on;
	else if (g_strcmp0(key, "dns_prefetch") == 0)           st->dns_prefetch = on;
	else if (g_strcmp0(key, "hyperlink_auditing") == 0)     st->hyperlink_auditing = on;
	else if (g_strcmp0(key, "site_quirks") == 0)            st->site_quirks = on;
	else if (g_strcmp0(key, "developer_extras") == 0)       st->developer_extras = on;
	else if (g_strcmp0(key, "js_can_open_windows") == 0)    st->js_can_open_windows = on;
	else if (g_strcmp0(key, "js_can_access_clipboard") == 0) st->js_can_access_clipboard = on;
	else if (g_strcmp0(key, "user_agent") == 0)             gsurf_settings_set_user_agent(st, value);
	else
		return error_result("unknown setting key");

	/* Apply immediately to the active view. */
	if (view != NULL)
		gsurf_view_apply_settings(view, st);
	return text_result("ok");
}

/* ----- modules ----- */

static McpToolResult *
tool_list_modules(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GPtrArray *mods = gsurf_module_manager_get_modules(mgr);
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonGenerator) gen = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *out = NULL;
	guint i;

	json_builder_begin_array(b);
	for (i = 0; mods != NULL && i < mods->len; i++) {
		GsurfModule *mod = g_ptr_array_index(mods, i);
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, gsurf_module_get_name(mod));
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, gsurf_module_get_description(mod));
		json_builder_set_member_name(b, "active");
		json_builder_add_boolean_value(b, gsurf_module_is_active(mod));
		json_builder_end_object(b);
	}
	json_builder_end_array(b);

	root = json_builder_get_root(b);
	gen = json_generator_new();
	json_generator_set_root(gen, root);
	out = json_generator_to_data(gen, NULL);
	return text_result(out);
}

static McpToolResult *
tool_toggle_module(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	const gchar *mod_name = args && json_object_has_member(args, "name")
		? json_object_get_string_member(args, "name") : NULL;
	gboolean enable = !(args && json_object_has_member(args, "enabled") &&
		!json_object_get_boolean_member(args, "enabled"));
	GsurfModule *mod;

	if (mod_name == NULL)
		return error_result("missing 'name'");
	mod = gsurf_module_manager_get_module(mgr, mod_name);
	if (mod == NULL)
		return error_result("no such module");

	if (enable)
		gsurf_module_activate(mod);
	else
		gsurf_module_deactivate(mod);
	return text_result(enable ? "activated" : "deactivated");
}

/* ===== tool registration ===== */

#define MCP_TOOL_MAX_ARGS 4

typedef struct {
	const gchar *name;      /* NULL-name entry terminates the arg list */
	const gchar *type;      /* "string", "boolean", "number" */
	const gchar *desc;
	gboolean     required;
} ToolArg;

typedef struct {
	const gchar    *name;
	const gchar    *description;
	ToolArg         args[MCP_TOOL_MAX_ARGS];
	McpToolHandler  handler;
} ToolDef;

static const ToolDef tool_defs[] = {
	{ "navigate", "Load a URI in the active view",
	  {{ "uri", "string", "the URI to load", TRUE }}, tool_navigate },
	{ "get_current_uri", "Get the active view's current URI", {{0}}, tool_get_current_uri },
	{ "get_title", "Get the active view's page title", {{0}}, tool_get_title },
	{ "go_back", "Navigate back in history", {{0}}, tool_go_back },
	{ "go_forward", "Navigate forward in history", {{0}}, tool_go_forward },
	{ "reload", "Reload the active view",
	  {{ "bypass_cache", "boolean", "bypass the cache", FALSE }}, tool_reload },
	{ "stop", "Stop loading the active view", {{0}}, tool_stop },
	{ "get_page_text", "Get the rendered text of the page", {{0}}, tool_get_page_text },
	{ "get_page_html", "Get the full HTML of the page", {{0}}, tool_get_page_html },
	{ "get_links", "List the links on the page as JSON", {{0}}, tool_get_links },
	{ "execute_javascript", "Run JavaScript in the page and return the result",
	  {{ "code", "string", "the JavaScript to run", TRUE }}, tool_execute_javascript },
	{ "scroll", "Scroll the page (direction: up/down/top/bottom)",
	  {{ "direction", "string", "scroll direction", FALSE }}, tool_scroll },
	{ "list_views", "List open views/tabs as JSON", {{0}}, tool_list_views },
	{ "find_text", "Find text in the page",
	  {{ "text", "string", "the text to find", TRUE },
	   { "backward", "boolean", "search backward", FALSE }}, tool_find_text },
	{ "click_link", "Click a link by index (a[href] order) or CSS selector",
	  {{ "index", "number", "link index", FALSE },
	   { "selector", "string", "CSS selector", FALSE }}, tool_click_link },
	{ "switch_view", "Switch the active view by index",
	  {{ "index", "number", "view index", TRUE }}, tool_switch_view },
	{ "open_view", "Open a new view/tab (returns its index)",
	  {{ "uri", "string", "the URI to load", FALSE }}, tool_open_view },
	{ "close_view", "Close a view/tab by index",
	  {{ "index", "number", "view index", TRUE }}, tool_close_view },
	{ "screenshot", "Capture the visible page as a PNG image", {{0}}, tool_screenshot },
	{ "save_screenshot", "Capture the visible page to a PNG file",
	  {{ "path", "string", "output file path", TRUE }}, tool_save_screenshot },
	{ "get_history", "Get recent history entries",
	  {{ "limit", "number", "max entries", FALSE }}, tool_get_history },
	{ "get_cookies", "Get the current document's cookies", {{0}}, tool_get_cookies },
	{ "get_config", "Get the browser configuration as JSON", {{0}}, tool_get_config },
	{ "set_config", "Set a configuration value",
	  {{ "key", "string", "config key", TRUE },
	   { "value", "string", "new value", TRUE }}, tool_set_config },
	{ "set_setting", "Set a web-engine setting on the active view",
	  {{ "key", "string", "setting key", TRUE },
	   { "value", "string", "new value", TRUE }}, tool_set_setting },
	{ "list_modules", "List loaded modules as JSON", {{0}}, tool_list_modules },
	{ "toggle_module", "Activate or deactivate a module",
	  {{ "name", "string", "module name", TRUE },
	   { "enabled", "boolean", "enable (default true)", FALSE }}, tool_toggle_module },
	{ NULL, NULL, {{0}}, NULL }
};

static JsonNode *
build_schema(const ToolDef *def)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	gint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "object");
	json_builder_set_member_name(b, "properties");
	json_builder_begin_object(b);
	for (i = 0; i < MCP_TOOL_MAX_ARGS && def->args[i].name != NULL; i++) {
		json_builder_set_member_name(b, def->args[i].name);
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, def->args[i].type);
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, def->args[i].desc);
		json_builder_end_object(b);
	}
	json_builder_end_object(b); /* properties */

	json_builder_set_member_name(b, "required");
	json_builder_begin_array(b);
	for (i = 0; i < MCP_TOOL_MAX_ARGS && def->args[i].name != NULL; i++)
		if (def->args[i].required)
			json_builder_add_string_value(b, def->args[i].name);
	json_builder_end_array(b);

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
