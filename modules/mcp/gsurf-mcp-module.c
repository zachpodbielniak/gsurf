/*
 * gsurf-mcp-module.c - MCP server for AI assistant control of the browser
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exposes browser navigation, content, and scripting as MCP tools over a
 * Unix-domain socket (via deps/mcp-glib). Built only when MCP=1. Every
 * tool defaults OFF and is enabled per-tool under modules.mcp.tools.
 *
 * This file holds the module (configuration, server life cycle, tool
 * registration, the in-process entry points) and the page tools; the
 * view / tab tree / container / mode / action tools live in
 * gsurf-mcp-views.c and the wmux tools in gsurf-mcp-wmux.c (see
 * gsurf-mcp-private.h).
 */

#include <gmodule.h>
#include <yaml-glib.h>
#include <string.h>
#include <unistd.h>

#include "gsurf-mcp-private.h"

#define GSURF_TYPE_MCP_MODULE (gsurf_mcp_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfMcpModule, gsurf_mcp_module, GSURF, MCP_MODULE, GsurfModule)

struct _GsurfMcpModule
{
	GsurfModule parent_instance;

	gchar *transport;     /* "unix-socket" (default), "stdio" */
	gchar *socket_name;   /* optional custom name */

	McpUnixSocketServer *unix_server;
	McpServer           *local_server; /* in-process calls (gsurf_mcp_module_call_tool) */

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

static McpToolResult *
text_result(const gchar *text)
{
	return gsurf_mcp_text(text);
}

static McpToolResult *
error_result(const gchar *text)
{
	return gsurf_mcp_error("%s", text != NULL ? text : "error");
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
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	const gchar *uri = args && json_object_has_member(args, "uri")
		? json_object_get_string_member(args, "uri") : NULL;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	if (uri == NULL)
		return error_result("missing 'uri'");

	gsurf_view_load_uri(view, uri);
	return text_result("ok");
}

static McpToolResult *
tool_get_current_uri(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	return text_result(gsurf_view_get_uri(view));
}

static McpToolResult *
tool_get_title(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	return text_result(gsurf_view_get_title(view));
}

static McpToolResult *
tool_go_back(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	gsurf_view_go_back(view);
	return text_result("ok");
}

static McpToolResult *
tool_go_forward(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	gsurf_view_go_forward(view);
	return text_result("ok");
}

static McpToolResult *
tool_reload(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	gboolean bypass = args && json_object_has_member(args, "bypass_cache")
		? json_object_get_boolean_member(args, "bypass_cache") : FALSE;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	gsurf_view_reload(view, bypass);
	return text_result("ok");
}

static McpToolResult *
tool_stop(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	gsurf_view_stop_loading(view);
	return text_result("ok");
}

static McpToolResult *
tool_get_page_text(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	text = run_js_sync(view, "document.body ? document.body.innerText : ''", &error);
	if (text == NULL)
		return error_result(error ? error->message : "javascript failed");
	return text_result(text);
}

static McpToolResult *
tool_get_page_html(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *html = NULL;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	html = run_js_sync(view, "document.documentElement.outerHTML", &error);
	if (html == NULL)
		return error_result(error ? error->message : "javascript failed");
	return text_result(html);
}

static McpToolResult *
tool_get_links(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	static const gchar *js =
		"JSON.stringify(Array.from(document.querySelectorAll('a[href]'))"
		".slice(0,500).map(a=>({text:(a.innerText||'').trim().slice(0,200),href:a.href})))";

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	json = run_js_sync(view, js, &error);
	if (json == NULL)
		return error_result(error ? error->message : "javascript failed");
	return text_result(json);
}

static McpToolResult *
tool_execute_javascript(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	const gchar *code = args && json_object_has_member(args, "code")
		? json_object_get_string_member(args, "code") : NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
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
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	const gchar *dir = args && json_object_has_member(args, "direction")
		? json_object_get_string_member(args, "direction") : "down";
	const gchar *js;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");

	if (g_strcmp0(dir, "up") == 0)        js = "window.scrollBy(0,-window.innerHeight*0.9)";
	else if (g_strcmp0(dir, "top") == 0)  js = "window.scrollTo(0,0)";
	else if (g_strcmp0(dir, "bottom") == 0) js = "window.scrollTo(0,document.body.scrollHeight)";
	else                                  js = "window.scrollBy(0,window.innerHeight*0.9)";

	gsurf_view_run_javascript_async(view, js, NULL, NULL, NULL);
	return text_result("ok");
}

/* ----- find / interaction ----- */

static McpToolResult *
tool_find_text(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	const gchar *text = args && json_object_has_member(args, "text")
		? json_object_get_string_member(args, "text") : NULL;
	gboolean forward = !(args && json_object_has_member(args, "backward") &&
		json_object_get_boolean_member(args, "backward"));

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	if (text == NULL)
		return error_result("missing 'text'");
	gsurf_view_find(view, text, FALSE, forward);
	return text_result("ok");
}

static McpToolResult *
tool_click_link(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *js = NULL;
	g_autofree gchar *result = NULL;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");

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

/* ----- input: click, type_text, press_key ----- */

/*
 * Where the element matching @selector is: scrolled into view, focused,
 * and its centre as view coordinates -- gsurf_view_locate_script(), the
 * same reading the control socket's page.click uses.  FALSE with
 * @error_text when nothing matches.
 */
static gboolean
locate(GsurfView *view, const gchar *selector, gdouble *x, gdouble *y,
       gchar **error_text)
{
	g_autofree gchar *script = gsurf_view_locate_script(selector);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *where = run_js_sync(view, script, &error);
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	JsonArray *xy;

	if (where == NULL) {
		*error_text = g_strdup(error != NULL ? error->message : "javascript failed");
		return FALSE;
	}
	if (!json_parser_load_from_data(parser, where, -1, NULL) ||
	    (root = json_parser_get_root(parser)) == NULL || !JSON_NODE_HOLDS_ARRAY(root) ||
	    json_array_get_length(xy = json_node_get_array(root)) != 2) {
		*error_text = g_strdup_printf("nothing on the page matches '%s'", selector);
		return FALSE;
	}
	*x = json_array_get_double_element(xy, 0);
	*y = json_array_get_double_element(xy, 1);
	return TRUE;
}

static McpToolResult *
tool_click(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	const gchar *selector = args && json_object_has_member(args, "selector")
		? json_object_get_string_member(args, "selector") : NULL;
	gint64 button = args && json_object_has_member(args, "button")
		? json_object_get_int_member(args, "button") : 1;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *said = NULL;
	gdouble x = -1;
	gdouble y = -1;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");

	if (selector != NULL && *selector != '\0') {
		if (!locate(view, selector, &x, &y, &why))
			return error_result(why);
	} else if (args && json_object_has_member(args, "x") &&
	           json_object_has_member(args, "y")) {
		x = json_object_get_double_member(args, "x");
		y = json_object_get_double_member(args, "y");
	} else {
		return error_result("give a 'selector', or 'x' and 'y'");
	}

	if (!gsurf_view_send_click(view, x, y, (guint)CLAMP(button, 1, 3), 0, &error))
		return error_result(error->message);
	said = g_strdup_printf("clicked at %.0f, %.0f", x, y);
	return text_result(said);
}

static McpToolResult *
tool_type_text(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	const gchar *text = args && json_object_has_member(args, "text")
		? json_object_get_string_member(args, "text") : NULL;
	const gchar *selector = args && json_object_has_member(args, "selector")
		? json_object_get_string_member(args, "selector") : NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *why = NULL;
	gdouble x;
	gdouble y;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	if (text == NULL)
		return error_result("missing 'text'");
	/* locating focuses the element, which is where the text goes */
	if (selector != NULL && *selector != '\0' && !locate(view, selector, &x, &y, &why))
		return error_result(why);
	if (!gsurf_view_send_text(view, text, &error))
		return error_result(error->message);
	return text_result("typed");
}

static McpToolResult *
tool_press_key(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	const gchar *key = args && json_object_has_member(args, "key")
		? json_object_get_string_member(args, "key") : NULL;
	const gchar *mods_text = args && json_object_has_member(args, "modifiers")
		? json_object_get_string_member(args, "modifiers") : NULL;
	g_autoptr(GError) error = NULL;
	guint keyval = gsurf_view_keyval_from_name(key);
	guint mods = 0;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	if (keyval == 0)
		return error_result("not a key: name one (Enter, Tab, Escape, Backspace, Delete, "
		                    "ArrowUp, Home, PageDown, space, ...) or give one character");

	/* "ctrl+shift" -- one string, since a tool argument here is a scalar */
	if (mods_text != NULL) {
		g_auto(GStrv) names = g_strsplit_set(mods_text, "+, ", -1);
		guint i;

		for (i = 0; names[i] != NULL; i++) {
			if (*names[i] == '\0')
				continue;
			if (g_ascii_strcasecmp(names[i], "ctrl") == 0 ||
			    g_ascii_strcasecmp(names[i], "control") == 0)
				mods |= GSURF_MOD_CTRL;
			else if (g_ascii_strcasecmp(names[i], "shift") == 0)
				mods |= GSURF_MOD_SHIFT;
			else if (g_ascii_strcasecmp(names[i], "alt") == 0)
				mods |= GSURF_MOD_ALT;
			else if (g_ascii_strcasecmp(names[i], "super") == 0 ||
			         g_ascii_strcasecmp(names[i], "meta") == 0)
				mods |= GSURF_MOD_SUPER;
			else
				return error_result("modifiers are ctrl, shift, alt and super, "
				                    "joined with '+'");
		}
	}

	if (!gsurf_view_send_key(view, keyval, mods, &error))
		return error_result(error->message);
	return text_result("pressed");
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
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *b64 = NULL;
	gsize len;
	const guchar *data;
	McpToolResult *r;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
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
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	const gchar *path = args && json_object_has_member(args, "path")
		? json_object_get_string_member(args, "path") : NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	gsize len;
	const guchar *data;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
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

	expanded = gsurf_resolve_path(file);
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
	g_autoptr(GError) verr = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &verr);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookies = NULL;

	if (view == NULL)
		return gsurf_mcp_gerror(verr, "no active view");
	/* Non-HttpOnly cookies visible to the current document. */
	cookies = run_js_sync(view, "document.cookie", &error);
	if (cookies == NULL)
		return error_result(error ? error->message : "javascript failed");
	return text_result(cookies);
}

typedef struct {
	GMainLoop *loop;
	GsurfView *view;
	gboolean   done;
	GError    *error;
} CookieCtx;

static void
cookie_done(GObject *source, GAsyncResult *res, gpointer data)
{
	CookieCtx *c = data;

	c->done = gsurf_view_add_cookie_finish(c->view, res, &c->error);
	g_main_loop_quit(c->loop);
}

/* set_cookie: put one cookie into the view's session cookie store, HttpOnly
 * included, as if a server had set it. A page that needs a sign-in cookie
 * (an access gate, a saved session) opens signed in from its first request. */
static McpToolResult *
tool_set_cookie(McpServer *s, const gchar *name, JsonObject *args, gpointer ud)
{
	g_autoptr(GError) error = NULL;
	GsurfView *view = gsurf_mcp_target_view(args, &error);
	const gchar *same = gsurf_mcp_arg_string(args, "same_site");
	GsurfCookie cookie = { 0 };
	CookieCtx c = { 0 };
	gint64 expires = 0;

	if (view == NULL)
		return gsurf_mcp_gerror(error, "no active view");
	cookie.name = gsurf_mcp_arg_string(args, "name");
	cookie.value = gsurf_mcp_arg_string(args, "value");
	cookie.domain = gsurf_mcp_arg_string(args, "domain");
	cookie.path = gsurf_mcp_arg_string(args, "path");
	if (cookie.name == NULL || *cookie.name == '\0')
		return error_result("missing 'name'");
	if (cookie.value == NULL)
		return error_result("missing 'value'");
	if (cookie.domain == NULL || *cookie.domain == '\0')
		return error_result("missing 'domain'");
	if (gsurf_mcp_arg_has(args, "expires") && !gsurf_mcp_arg_int(args, "expires", &expires, &error))
		return gsurf_mcp_gerror(error, "bad 'expires'");
	cookie.expires = expires;
	if (!gsurf_mcp_arg_bool(args, "secure", &cookie.secure, &error) ||
	    !gsurf_mcp_arg_bool(args, "http_only", &cookie.http_only, &error))
		return gsurf_mcp_gerror(error, "bad flag");
	if (same == NULL || g_ascii_strcasecmp(same, "lax") == 0)
		cookie.same_site = 1;
	else if (g_ascii_strcasecmp(same, "none") == 0)
		cookie.same_site = 0;
	else if (g_ascii_strcasecmp(same, "strict") == 0)
		cookie.same_site = 2;
	else
		return gsurf_mcp_error("'same_site' must be none, lax or strict, not '%s'", same);

	c.view = view;
	c.loop = g_main_loop_new(NULL, FALSE);
	gsurf_view_add_cookie_async(view, &cookie, NULL, cookie_done, &c);
	g_main_loop_run(c.loop);
	g_main_loop_unref(c.loop);
	if (!c.done) {
		McpToolResult *r = gsurf_mcp_gerror(c.error, "cookie not stored");
		g_clear_error(&c.error);
		return r;
	}
	return text_result("ok");
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

#define VIEW_ARG { "view", "integer", "stable view id (list_views 'id'); default: the active view", FALSE }

static const GsurfMcpToolDef page_tools[] = {
	{ "navigate", "Load a URI in a view (default: the active view)",
	  {{ "uri", "string", "the URI to load", TRUE }, VIEW_ARG }, tool_navigate },
	{ "get_current_uri", "Get a view's current URI", {VIEW_ARG}, tool_get_current_uri },
	{ "get_title", "Get a view's page title", {VIEW_ARG}, tool_get_title },
	{ "go_back", "Navigate back in a view's history", {VIEW_ARG}, tool_go_back },
	{ "go_forward", "Navigate forward in a view's history", {VIEW_ARG}, tool_go_forward },
	{ "reload", "Reload a view",
	  {{ "bypass_cache", "boolean", "bypass the cache", FALSE }, VIEW_ARG }, tool_reload },
	{ "stop", "Stop loading a view", {VIEW_ARG}, tool_stop },
	{ "get_page_text", "Get the rendered text of a view's page", {VIEW_ARG}, tool_get_page_text },
	{ "get_page_html", "Get the full HTML of a view's page", {VIEW_ARG}, tool_get_page_html },
	{ "get_links", "List the links on a view's page as JSON [{text, href}]", {VIEW_ARG},
	  tool_get_links },
	{ "execute_javascript", "Run JavaScript in a view's page and return the result",
	  {{ "code", "string", "the JavaScript to run", TRUE }, VIEW_ARG }, tool_execute_javascript },
	{ "scroll", "Scroll a view's page (direction: up/down/top/bottom)",
	  {{ "direction", "string", "scroll direction", FALSE }, VIEW_ARG }, tool_scroll },
	{ "find_text", "Find text in a view's page",
	  {{ "text", "string", "the text to find", TRUE },
	   { "backward", "boolean", "search backward", FALSE }, VIEW_ARG }, tool_find_text },
	{ "click_link", "Click a link by index (a[href] order) or CSS selector",
	  {{ "index", "number", "link index", FALSE },
	   { "selector", "string", "CSS selector", FALSE }, VIEW_ARG }, tool_click_link },
	{ "click", "Click in a view: at the element matching a CSS selector (scrolled into view "
	  "first) or at x, y in CSS pixels. Trusted input on GTK3",
	  {{ "selector", "string", "CSS selector of the element to click", FALSE },
	   { "x", "number", "horizontal position, CSS pixels", FALSE },
	   { "y", "number", "vertical position, CSS pixels", FALSE },
	   { "button", "number", "1 left (default), 2 middle, 3 right", FALSE }, VIEW_ARG },
	  tool_click },
	{ "type_text", "Type text into the focused element, or the one matching 'selector' "
	  "(focused first); a newline is Enter. Trusted input on GTK3",
	  {{ "text", "string", "the text to type", TRUE },
	   { "selector", "string", "CSS selector to focus first", FALSE }, VIEW_ARG },
	  tool_type_text },
	{ "press_key", "Press one key: Enter, Tab, Escape, Backspace, Delete, ArrowUp, Home, "
	  "PageDown, space, ... or one character. Trusted input on GTK3",
	  {{ "key", "string", "the key", TRUE },
	   { "modifiers", "string", "held modifiers joined with '+': ctrl+shift", FALSE },
	   VIEW_ARG }, tool_press_key },
	{ "screenshot", "Capture a view's visible page as a PNG image", {VIEW_ARG}, tool_screenshot },
	{ "save_screenshot", "Capture a view's visible page to a PNG file",
	  {{ "path", "string", "output file path", TRUE }, VIEW_ARG }, tool_save_screenshot },
	{ "get_history", "Get recent history entries",
	  {{ "limit", "number", "max entries", FALSE }}, tool_get_history },
	{ "get_cookies", "Get the cookies visible to a view's document", {VIEW_ARG}, tool_get_cookies },
	{ "set_cookie", "Store a cookie (HttpOnly too) in a view's session, as a server would set it",
	  {{ "name", "string", "cookie name", TRUE },
	   { "value", "string", "cookie value", TRUE },
	   { "domain", "string", "host, or .domain for its subdomains too", TRUE },
	   { "path", "string", "path (default /)", FALSE },
	   { "expires", "integer", "expiry in Unix seconds (default: a session cookie)", FALSE },
	   { "secure", "boolean", "https only", FALSE },
	   { "http_only", "boolean", "hidden from page scripts", FALSE },
	   { "same_site", "string", "none, lax (default) or strict", FALSE },
	   VIEW_ARG }, tool_set_cookie },
	{ "get_config", "Get the browser configuration as JSON", {{ NULL, NULL, NULL, FALSE }},
	  tool_get_config },
	{ "set_config", "Set a configuration value",
	  {{ "key", "string", "config key", TRUE },
	   { "value", "string", "new value", TRUE }}, tool_set_config },
	{ "set_setting", "Set a web-engine setting on the active view",
	  {{ "key", "string", "setting key", TRUE },
	   { "value", "string", "new value", TRUE }}, tool_set_setting },
	{ "list_modules", "List loaded modules as JSON", {{ NULL, NULL, NULL, FALSE }},
	  tool_list_modules },
	{ "toggle_module", "Activate or deactivate a module",
	  {{ "name", "string", "module name", TRUE },
	   { "enabled", "boolean", "enable (default true)", FALSE }}, tool_toggle_module },
	{ NULL, NULL, {{ NULL, NULL, NULL, FALSE }}, NULL }
};

/* Every tool table, in tools/list order. */
static const GsurfMcpToolDef *const tool_tables[] = {
	page_tools,
	gsurf_mcp_view_tools,
	gsurf_mcp_wmux_tools,
	NULL
};

static JsonNode *
build_schema(const GsurfMcpToolDef *def)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	gint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "object");
	json_builder_set_member_name(b, "properties");
	json_builder_begin_object(b);
	for (i = 0; i < GSURF_MCP_TOOL_MAX_ARGS && def->args[i].name != NULL; i++) {
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
	for (i = 0; i < GSURF_MCP_TOOL_MAX_ARGS && def->args[i].name != NULL; i++)
		if (def->args[i].required)
			json_builder_add_string_value(b, def->args[i].name);
	json_builder_end_array(b);

	json_builder_end_object(b);

	return json_builder_get_root(b);
}

static void
register_tools(GsurfMcpModule *self, McpServer *server)
{
	guint t;
	guint i;

	for (t = 0; tool_tables[t] != NULL; t++) {
		for (i = 0; tool_tables[t][i].name != NULL; i++) {
			const GsurfMcpToolDef *def = &tool_tables[t][i];
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
}

/* The tools of an in-process server, (re)built after configure() so it
 * follows the enabled flags. */
static McpServer *
local_server(GsurfMcpModule *self)
{
	if (self->local_server == NULL) {
		self->local_server = mcp_server_new("gsurf", GSURF_VERSION_STRING);
		register_tools(self, self->local_server);
	}
	return self->local_server;
}

/**
 * gsurf_mcp_module_call_tool:
 * @module: the mcp module (gsurf_module_manager_get_module(mgr, "mcp"))
 * @name: a tool name
 * @arguments_json: (nullable): the arguments as a JSON object text; %NULL
 *   or "" for none
 * @is_error: (out) (optional): whether the tool reported an error
 * @error: return location for a #GError (bad JSON, a disabled or unknown
 *   tool)
 *
 * Calls a tool in-process, through the same registration (enabled flags,
 * schemas, handlers) the socket server uses, without a client or a
 * socket: for embedders and tests. Found with g_module_symbol() on
 * mcp.so; the module is not linked by anything.
 *
 * Returns: (transfer full) (nullable): the text of the result (text items
 *   joined with newlines; an image item as "[image <mime>]"), or %NULL
 *   with @error set
 *
 * Since: 0.4.4
 */
G_MODULE_EXPORT gchar *
gsurf_mcp_module_call_tool(GsurfModule *module, const gchar *name, const gchar *arguments_json,
                           gboolean *is_error, GError **error);

G_MODULE_EXPORT gchar *
gsurf_mcp_module_call_tool(GsurfModule *module, const gchar *name, const gchar *arguments_json,
                           gboolean *is_error, GError **error)
{
	GsurfMcpModule *self;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *args = NULL;
	McpToolResult *result;
	JsonArray *content;
	GString *out;
	guint i;

	g_return_val_if_fail(GSURF_IS_MCP_MODULE(module), NULL);
	g_return_val_if_fail(name != NULL, NULL);
	self = GSURF_MCP_MODULE(module);

	/* 1. Arguments: a JSON object, or none. */
	if (arguments_json != NULL && *arguments_json != '\0') {
		JsonNode *root;

		parser = json_parser_new();
		if (!json_parser_load_from_data(parser, arguments_json, -1, error))
			return NULL;
		root = json_parser_get_root(parser);
		if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				"the arguments must be a JSON object");
			return NULL;
		}
		args = json_node_get_object(root);
	}

	/* 2. The call, through the registered (enabled) tools only. */
	result = mcp_server_invoke_tool(local_server(self), name, args, error);
	if (result == NULL)
		return NULL;

	/* 3. Its content as text. */
	out = g_string_new(NULL);
	content = mcp_tool_result_get_content(result);
	for (i = 0; content != NULL && i < json_array_get_length(content); i++) {
		JsonObject *item = json_array_get_object_element(content, i);
		const gchar *type = item != NULL && json_object_has_member(item, "type")
			? json_object_get_string_member(item, "type") : NULL;

		if (out->len > 0)
			g_string_append_c(out, '\n');
		if (g_strcmp0(type, "text") == 0)
			g_string_append(out, json_object_get_string_member(item, "text"));
		else if (g_strcmp0(type, "image") == 0)
			g_string_append_printf(out, "[image %s]",
				json_object_has_member(item, "mimeType")
					? json_object_get_string_member(item, "mimeType") : "?");
	}
	if (is_error != NULL)
		*is_error = mcp_tool_result_get_is_error(result);
	mcp_tool_result_unref(result);
	return g_string_free(out, FALSE);
}

/**
 * gsurf_mcp_module_list_tools:
 * @module: the mcp module
 *
 * The enabled tools as the socket server's tools/list announces them.
 *
 * Returns: (transfer full): a JSON array text of {name, description,
 *   inputSchema}
 *
 * Since: 0.4.4
 */
G_MODULE_EXPORT gchar *gsurf_mcp_module_list_tools(GsurfModule *module);

G_MODULE_EXPORT gchar *
gsurf_mcp_module_list_tools(GsurfModule *module)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	GList *tools;
	GList *l;

	g_return_val_if_fail(GSURF_IS_MCP_MODULE(module), NULL);

	tools = mcp_server_list_tools(local_server(GSURF_MCP_MODULE(module)));
	json_builder_begin_array(b);
	for (l = tools; l != NULL; l = l->next)
		json_builder_add_value(b, mcp_tool_to_json(MCP_TOOL(l->data)));
	json_builder_end_array(b);
	g_list_free_full(tools, g_object_unref);
	root = json_builder_get_root(b);
	json_generator_set_root(gen, root);
	return json_generator_to_data(gen, NULL);
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

	/* The in-process server follows the new flags. */
	g_clear_object(&self->local_server);

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
		"Control the gsurf web browser: navigate, read page content, run JavaScript, "
		"manage views by stable id, the tab tree, containers, input modes, any keybind "
		"action (run_action), and wmux panes and windows (wmux_status first).");
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
	g_clear_object(&self->local_server);

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
