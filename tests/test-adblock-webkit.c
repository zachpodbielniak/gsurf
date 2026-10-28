/*
 * test-adblock-webkit.c - adblock filters enforced by a real WebKit view
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the adblock module with uBO-syntax filters, then a page from a
 * loopback HTTP server, and checks every enforcement layer end to end:
 * native network blocking and element hiding (content blocker), and the
 * web-process extension's $redirect, $removeparam, scriptlets and
 * procedural cosmetics. Needs a display and a web process, so it only
 * runs with GSURF_TEST_GUI=1 (make test-gui).
 */

#include <gsurf.h>
#include "backend/webkit/gsurf-webkit-content.h"
#include <libsoup/soup.h>
#include <string.h>

static const gchar *PAGE =
	"<!DOCTYPE html><html><head><title>adblock</title>"
	"<script>window.adConfig = { enabled: true };</script>"
	"<script src=\"/ads/banner.js\"></script>"
	"<script src=\"/lib/tracker.js\" onload=\"window.trackerOnload = 1\"></script>"
	"</head><body>"
	"<div class=\"ad-generic\">generic</div>"
	"<div id=\"specific\">specific</div>"
	"<div class=\"proc\">Sponsored content</div>"
	"<div class=\"proc\">normal content</div>"
	"<img src=\"/img/pixel.gif?utm_source=test&amp;keep=1\">"
	"</body></html>";

static const gchar *FILTERS =
	"/ads/banner.js\n"
	"/lib/tracker.js$script,redirect=noop.js\n"
	"$removeparam=utm_source\n"
	"##.ad-generic\n"
	"127.0.0.1###specific\n"
	"127.0.0.1##.proc:has-text(Sponsored)\n"
	"127.0.0.1##+js(set, adConfig.enabled, false)\n";

typedef struct {
	SoupServer *server;
	gchar      *pixel_query;   /* query string the image request arrived with */
	gboolean    tracker_hit;   /* the redirected script must never be fetched */
	guint16     port;
} TestServer;

static void
serve(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query,
      gpointer data)
{
	TestServer *ts = data;
	const gchar *body = "";
	const gchar *type = "text/plain";

	if (g_str_equal(path, "/page.html")) {
		body = PAGE;
		type = "text/html";
	} else if (g_str_equal(path, "/ads/banner.js")) {
		body = "window.bannerLoaded = 1;";
		type = "text/javascript";
	} else if (g_str_equal(path, "/lib/tracker.js")) {
		ts->tracker_hit = TRUE;
		body = "window.trackerLoaded = 1;";
		type = "text/javascript";
	} else if (g_str_equal(path, "/img/pixel.gif")) {
		GUri *uri = soup_server_message_get_uri(msg);

		g_free(ts->pixel_query);
		ts->pixel_query = g_strdup(g_uri_get_query(uri));
		type = "image/gif";
	}
	soup_server_message_set_status(msg, 200, NULL);
	soup_server_message_set_response(msg, type, SOUP_MEMORY_COPY, body, strlen(body));
}

static void
test_server_start(TestServer *ts)
{
	g_autoptr(GError) error = NULL;
	GSList *uris;

	ts->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(ts->server, NULL, serve, ts, NULL);
	g_assert_true(soup_server_listen_local(ts->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(ts->server);
	ts->port = (guint16)g_uri_get_port(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
}

static gboolean
deadline(gpointer data)
{
	g_error("adblock WebKit integration timed out");
	return G_SOURCE_REMOVE;
}

static void
loaded(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
	if (event == WEBKIT_LOAD_FINISHED)
		g_main_loop_quit(data);
}

static void
wait_load(WebKitWebView *view, const gchar *uri)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	gulong handler = g_signal_connect(view, "load-changed", G_CALLBACK(loaded), loop);
	guint timeout = g_timeout_add_seconds(20, deadline, NULL);

	webkit_web_view_load_uri(view, uri);
	g_main_loop_run(loop);
	g_source_remove(timeout);
	g_signal_handler_disconnect(view, handler);
}

typedef struct {
	GMainLoop *loop;
	gchar     *text;
} ScriptResult;

static void
script_done(GObject *source, GAsyncResult *result, gpointer data)
{
	ScriptResult *script = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JSCValue) value = webkit_web_view_evaluate_javascript_finish(
		WEBKIT_WEB_VIEW(source), result, &error);

	g_assert_no_error(error);
	script->text = jsc_value_to_string(value);
	g_main_loop_quit(script->loop);
}

static gchar *
evaluate(WebKitWebView *view, const gchar *expression)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	ScriptResult result = { loop, NULL };
	guint timeout = g_timeout_add_seconds(10, deadline, NULL);

	webkit_web_view_evaluate_javascript(view, expression, -1, NULL, NULL, NULL,
		script_done, &result);
	g_main_loop_run(loop);
	g_source_remove(timeout);
	return result.text;
}

static gboolean
quit_loop(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

/* Let pending work (filter compilation, mutation observers) run. */
static void
spin(guint ms)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

	g_timeout_add(ms, quit_loop, loop);
	g_main_loop_run(loop);
}

static gchar *
module_so_path(const gchar *name)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);
	g_autofree gchar *base = g_strconcat(name, ".so", NULL);

	return g_build_filename(dir, "modules", base, NULL);
}

static void
test_filters_enforced(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = NULL;
	g_autoptr(GsurfModuleManager) manager = NULL;
	g_autoptr(GsurfView) view = NULL;
	g_autofree gchar *so = module_so_path("adblock");
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *cache = NULL;
	TestServer ts = { 0 };
	WebKitWebView *native;
	const gchar *expected =
		"{\"banner\":false,\"tracker\":false,\"trackerOnload\":true,"
		"\"generic\":\"none\",\"specific\":\"none\",\"sponsored\":\"none\","
		"\"normal\":\"block\",\"scriptlet\":false}";
	const gchar *probe =
		"JSON.stringify({"
		"banner: !!window.bannerLoaded,"
		"tracker: !!window.trackerLoaded,"
		"trackerOnload: !!window.trackerOnload,"
		"generic: getComputedStyle(document.querySelector('.ad-generic')).display,"
		"specific: getComputedStyle(document.getElementById('specific')).display,"
		"sponsored: getComputedStyle(document.querySelectorAll('.proc')[0]).display,"
		"normal: getComputedStyle(document.querySelectorAll('.proc')[1]).display,"
		"scriptlet: window.adConfig.enabled})";
	gchar *result = NULL;
	guint attempt;

	if (g_getenv("GSURF_TEST_GUI") == NULL) {
		g_test_skip("set GSURF_TEST_GUI=1 to exercise adblock in WebKit");
		return;
	}
	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("adblock.so not built");
		return;
	}
	g_assert_true(gtk_init_check(NULL, NULL));

	/* Keep compiled rulesets out of the user's cache. */
	cache = g_dir_make_tmp("gsurf-adblock-cache-XXXXXX", &error);
	g_assert_no_error(error);
	g_setenv("XDG_CACHE_HOME", cache, TRUE);

	/* The module must register its web-process extension before the
	 * first view (and so the first web process) exists. */
	{
		g_auto(GStrv) lines = g_strsplit(FILTERS, "\n", -1);
		g_autofree gchar *joined = g_strjoinv("\n      ", lines);

		yaml = g_strdup_printf("modules:\n  adblock:\n    enabled: true\n"
			"    filters: |\n      %s\n", joined);
	}
	config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	manager = gsurf_module_manager_new();
	gsurf_module_manager_set_config(manager, config);
	g_assert_nonnull(gsurf_module_manager_load_module(manager, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(manager);

	test_server_start(&ts);
	view = gsurf_view_new();
	native = WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
	gsurf_module_manager_dispatch_inject_scripts(manager, view, NULL);
	uri = g_strdup_printf("http://127.0.0.1:%u/page.html", ts.port);

	/* Content-blocker rulesets compile asynchronously; retry until they
	 * are in force (bounded). */
	for (attempt = 0; attempt < 20; attempt++) {
		g_clear_pointer(&result, g_free);
		ts.tracker_hit = FALSE;
		wait_load(native, uri);
		spin(300);
		result = evaluate(native, probe);
		if (g_str_equal(result, expected))
			break;
		spin(250);
	}
	g_assert_cmpstr(result, ==, expected);
	g_free(result);

	/* $redirect replaced the request: the server never saw it. */
	g_assert_false(ts.tracker_hit);
	/* $removeparam stripped the tracking parameter, nothing else. */
	g_assert_cmpstr(ts.pixel_query, ==, "keep=1");

	g_free(ts.pixel_query);
	g_object_unref(ts.server);
	g_unsetenv("XDG_CACHE_HOME");
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/adblock/webkit/filters-enforced", test_filters_enforced);
	return g_test_run();
}
