/*
 * test-dark-mode-webkit.c - dark_mode in a real WebKit view
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the real dark_mode module and drives WebKit views (GTK3/4.1 or
 * GTK4/6.0) against a loopback HTTP server reachable under two host
 * names, 127.0.0.1 and localhost, so per-host rules can be told apart:
 *   - filter, native and dynamic (Dark Reader) modes produce the
 *     expected computed styles; off leaves the page untouched;
 *   - the engine runs in the isolated world "gsurf-dark-mode": the page
 *     cannot see its globals, and decide() can be evaluated there;
 *   - subframes follow the top-level host (a cross-origin child of a
 *     filtered page only re-inverts its media);
 *   - the per-site toggle flips every open view of that host at once and
 *     persists to the state file (0600); the global toggle switches all
 *     views; a navigation in a view created before a toggle starts from
 *     the toggled state.
 * Runs only with GSURF_TEST_GUI=1 (make test-gui).
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "test-util.h"
#include "webkit-test-util.h"

#define WORLD "gsurf-dark-mode"

/* The filter mode's media re-inversion, as WebKit serializes it. */
#define MEDIA_FILTER "invert(1) hue-rotate(180deg)"

static guint16 server_port;

/* /plain: text + image + a page global; /scheme: a site with its own dark
 * theme; /frame: embeds /child from the *other* host name; /child posts
 * its computed styles to the parent. */
static void
serve(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query,
      gpointer data)
{
	GUri *uri = soup_server_message_get_uri(msg);
	g_autofree gchar *body = NULL;
	const gchar *other;

	(void)server;
	(void)query;
	(void)data;
	other = g_strcmp0(g_uri_get_host(uri), "localhost") == 0 ? "127.0.0.1" : "localhost";
	if (g_str_equal(path, "/scheme")) {
		body = g_strdup("<!DOCTYPE html><html><head><style>"
			"body{background:rgb(250,250,250)}"
			"@media (prefers-color-scheme: dark){body{background:rgb(1,2,3)}}"
			"@media (prefers-color-scheme: light){p{color:rgb(9,9,9)}}"
			"</style></head><body><p id=t>scheme</p></body></html>");
	} else if (g_str_equal(path, "/frame")) {
		body = g_strdup_printf("<!DOCTYPE html><html><body><p>top</p>"
			"<script>window.addEventListener('message', function (e) {"
			"window.childReport = e.data; });</script>"
			"<iframe src='http://%s:%u/child'></iframe></body></html>",
			other, server_port);
	} else if (g_str_equal(path, "/child")) {
		body = g_strdup("<!DOCTYPE html><html><body><img id=i width=4 height=4>"
			"<script>window.addEventListener('load', function () {"
			"var s = function (el) { return getComputedStyle(el); };"
			"parent.postMessage(JSON.stringify({"
			"html: s(document.documentElement).filter,"
			"img: s(document.getElementById('i')).filter,"
			"scheme: s(document.documentElement).colorScheme}), '*'); });"
			"</script></body></html>");
	} else {
		body = g_strdup("<!DOCTYPE html><html><head><title>plain</title></head>"
			"<body style='background:#fff;color:#000'><p id=t>plain text</p>"
			"<img id=i width=4 height=4>"
			"<script>window.pageSecret = 42;</script></body></html>");
	}
	soup_server_message_set_status(msg, 200, NULL);
	soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_COPY, body, strlen(body));
}

static gchar *
url(const gchar *host, const gchar *path)
{
	return g_strdup_printf("http://%s:%u%s", host, server_port, path);
}

/* A manager with only dark_mode, configured from @yaml (the module's
 * section, indented under modules: dark_mode:). */
static GsurfModuleManager *
manager_with(const gchar *section)
{
	g_autofree gchar *so = gsurf_webkit_test_module_path("dark_mode");
	g_autofree gchar *yaml = g_strdup_printf("modules:\n  dark_mode:\n    enabled: true\n%s",
		section);
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GError) error = NULL;
	GsurfModuleManager *manager;

	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	manager = gsurf_module_manager_new();
	gsurf_module_manager_set_config(manager, config);
	g_assert_nonnull(gsurf_module_manager_load_module(manager, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(manager);
	return manager;
}

static GsurfModule *
module_of(GsurfModuleManager *manager)
{
	GsurfModule *module = gsurf_module_manager_get_module(manager, "dark_mode");

	g_assert_nonnull(module);
	return module;
}

/* A new view with the module's scripts injected, as main.c does. */
static GsurfView *
new_view(GsurfModuleManager *manager)
{
	GsurfView *view = gsurf_view_new();

	gsurf_module_manager_dispatch_inject_scripts(manager, view, NULL);
	return view;
}

static void
load(GsurfView *view, const gchar *host, const gchar *path)
{
	g_autofree gchar *uri = url(host, path);

	gsurf_webkit_test_load(gsurf_webkit_test_native(view), uri);
}

static gchar *
eval(GsurfView *view, const gchar *expression)
{
	return gsurf_webkit_test_eval(gsurf_webkit_test_native(view), expression);
}

static gchar *
eval_world(GsurfView *view, const gchar *expression)
{
	return gsurf_webkit_test_eval_in(gsurf_webkit_test_native(view), WORLD, expression);
}

static gchar *
html_filter(GsurfView *view)
{
	return eval(view, "getComputedStyle(document.documentElement).filter");
}

/* Poll @expression in the page world until it equals @want (bounded). */
static void
wait_for(GsurfView *view, const gchar *expression, const gchar *want)
{
	g_autofree gchar *last = NULL;
	guint i;

	for (i = 0; i < 100; i++) {
		g_clear_pointer(&last, g_free);
		last = eval(view, expression);
		if (g_strcmp0(last, want) == 0)
			return;
		gsurf_webkit_test_spin(50);
	}
	g_assert_cmpstr(last, ==, want);
}

/* Finalize a view and check it really went away. A live update still in
 * flight holds the view (its GTask) until WebKit answers, so give those
 * a bounded moment to complete. */
static void
drop_view(GsurfView *view)
{
	gpointer weak = view;
	guint i;

	g_object_add_weak_pointer(G_OBJECT(view), &weak);
	g_object_unref(view);
	for (i = 0; i < 100 && weak != NULL; i++)
		gsurf_webkit_test_spin(50);
	g_assert_null(weak);
}

static void
test_filter_and_native_per_host(void)
{
	g_autoptr(GsurfModuleManager) manager = NULL;
	GsurfView *view;
	gchar *text;

	if (!gsurf_webkit_test_init())
		return;
	manager = manager_with(
		"    mode: filter\n"
		"    brightness: 90\n"
		"    sepia: 10\n"
		"    sites:\n"
		"      - match: '^localhost$'\n"
		"        mode: native\n");
	view = new_view(manager);

	/* 127.0.0.1: the global filter with its adjustments, media
	 * re-inverted. */
	load(view, "127.0.0.1", "/plain");
	text = html_filter(view);
	g_assert_cmpstr(text, ==, "invert(1) hue-rotate(180deg) brightness(0.9) sepia(0.1)");
	g_free(text);
	text = eval(view, "getComputedStyle(document.getElementById('i')).filter");
	g_assert_cmpstr(text, ==, MEDIA_FILTER);
	g_free(text);

	/* The page world sees neither the engine nor its state... */
	text = eval(view, "typeof window.__gsurf_dark + ':' + window.pageSecret");
	g_assert_cmpstr(text, ==, "undefined:42");
	g_free(text);
	/* ...which lives in the isolated world (that cannot see the page). */
	text = eval_world(view, "typeof __gsurf_dark + ':' + typeof window.pageSecret + ':' + "
		"__gsurf_dark.current.mode + ':' + __gsurf_dark.current.source");
	g_assert_cmpstr(text, ==, "object:undefined:filter:global");
	g_free(text);

	/* localhost: native. The page's own dark theme applies, its light
	 * rules do not, and the adjustments become a plain filter. */
	load(view, "localhost", "/scheme");
	text = eval(view, "getComputedStyle(document.documentElement).colorScheme + '|' + "
		"getComputedStyle(document.body).backgroundColor + '|' + "
		"getComputedStyle(document.getElementById('t')).color");
	g_assert_cmpstr(text, !=, NULL);
	g_assert_true(g_str_has_prefix(text, "dark|rgb(1, 2, 3)|"));
	g_assert_false(g_str_has_suffix(text, "rgb(9, 9, 9)"));
	g_free(text);
	text = html_filter(view);
	g_assert_cmpstr(text, ==, "brightness(0.9) sepia(0.1)");
	g_free(text);
	text = eval_world(view, "__gsurf_dark.current.source + ':' + __gsurf_dark.current.rule");
	g_assert_cmpstr(text, ==, "rule:0");
	g_free(text);

	drop_view(view);
}

static void
test_off_leaves_page_alone(void)
{
	g_autoptr(GsurfModuleManager) manager = NULL;
	GsurfView *view;
	gchar *text;

	if (!gsurf_webkit_test_init())
		return;
	manager = manager_with(
		"    mode: filter\n"
		"    sites:\n"
		"      - match: '^127\\.0\\.0\\.1$'\n"
		"        mode: off\n");
	view = new_view(manager);
	load(view, "127.0.0.1", "/scheme");

	/* No style element, no filter, the page's own scheme, its light
	 * rules intact. */
	text = eval(view, "document.querySelectorAll('style').length + '|' + "
		"getComputedStyle(document.documentElement).filter + '|' + "
		"getComputedStyle(document.documentElement).colorScheme + '|' + "
		"document.styleSheets[0].cssRules[1].media.mediaText");
	g_assert_cmpstr(text, ==, "1|none|normal|(prefers-color-scheme: dark)");
	g_free(text);
	text = eval_world(view, "__gsurf_dark.current.mode");
	g_assert_cmpstr(text, ==, "off");
	g_free(text);
	drop_view(view);
}

static void
test_decide_in_world(void)
{
	g_autoptr(GsurfModuleManager) manager = NULL;
	GsurfView *view;
	gchar *text;
	const gchar *probe =
		"(function () {"
		"var st = { version: 1, enabled: true, fallback: 'filter',"
		"  global: { mode: 'native', scheme: 'light dark', brightness: 100, contrast: 100,"
		"            sepia: 0, grayscale: 0 },"
		"  rules: [ { pattern: '(^|\\\\.)github\\\\.com$', mode: 'dynamic', scheme: 'dark' },"
		"           { pattern: 'news', brightness: 90 },"
		"           { pattern: '(', mode: 'filter', scheme: 'dark' },"
		"           { pattern: '(^|\\\\.)example\\\\.org$', mode: 'off', scheme: 'dark' } ],"
		"  overrides: { 'example.org': true, 'news.test': false, 'plain.test': true } };"
		"var d = function (h) { var r = __gsurf_dark.decide(st, h);"
		"  return r.mode + '/' + r.scheme + '/' + r.source + '/' + r.brightness; };"
		"var off = JSON.parse(JSON.stringify(st)); off.enabled = false;"
		"return [d('GitHub.com.'), d('news.example'), d('news.test'), d('example.org'),"
		"  d('www.example.org'), d('plain.test'), d('other.test'), d(''),"
		"  __gsurf_dark.decide(off, 'github.com').source].join(' ');"
		"})()";

	if (!gsurf_webkit_test_init())
		return;
	manager = manager_with("    mode: off\n");
	view = new_view(manager);
	load(view, "127.0.0.1", "/plain");

	text = eval_world(view, probe);
	g_assert_cmpstr(text, ==,
		"dynamic/dark/rule/100 "               /* case + trailing dot ignored */
		"native/light dark/rule/90 "           /* rule without mode keeps global's */
		"off/light dark/override/90 "          /* override false */
		"filter/dark/override/100 "            /* override true on a rule-off host: fallback */
		"off/dark/rule/100 "                   /* subdomain rule */
		"filter/dark/override/100 "            /* override true on an auto host: fallback */
		"native/light dark/global/100 "        /* the bad pattern never matches */
		"native/light dark/global/100 "        /* no host: global */
		"disabled");                           /* the global switch wins */
	g_free(text);
	drop_view(view);
}

static void
test_dynamic_dark_reader(void)
{
	g_autoptr(GsurfModuleManager) manager = NULL;
	GsurfView *view;
	gboolean preload = FALSE;
	gchar *text;

	if (!gsurf_webkit_test_init())
		return;

	/* 1. Global dynamic: Dark Reader is preloaded into every frame. */
	manager = manager_with("    mode: dynamic\n");
	g_object_get(module_of(manager), "preload-engine", &preload, NULL);
	g_assert_true(preload);
	view = new_view(manager);
	load(view, "127.0.0.1", "/plain");
	wait_for(view, "document.documentElement.getAttribute('data-darkreader-mode')", "dynamic");
	text = eval(view, "(document.querySelector('style.darkreader') !== null) + '|' + "
		"typeof window.DarkReader + '|' + typeof window.__gsurf_dark");
	g_assert_cmpstr(text, ==, "true|undefined|undefined");
	g_free(text);
	text = eval_world(view, "typeof DarkReader + ':' + DarkReader.isEnabled()");
	g_assert_cmpstr(text, ==, "object:true");
	g_free(text);
	/* The white body became dark. */
	text = eval(view, "getComputedStyle(document.body).backgroundColor");
	g_assert_cmpstr(text, !=, "rgb(255, 255, 255)");
	g_free(text);
	drop_view(view);
	g_clear_object(&manager);

	/* 2. Dynamic through a rule only: loaded on demand into the main
	 * frame, other hosts never get it. */
	manager = manager_with(
		"    mode: filter\n"
		"    sites:\n"
		"      - match: '^localhost$'\n"
		"        mode: dynamic\n");
	g_object_get(module_of(manager), "preload-engine", &preload, NULL);
	g_assert_false(preload);
	view = new_view(manager);
	load(view, "127.0.0.1", "/plain");
	text = eval_world(view, "typeof DarkReader");
	g_assert_cmpstr(text, ==, "undefined");
	g_free(text);
	load(view, "localhost", "/plain");
	wait_for(view, "document.documentElement.getAttribute('data-darkreader-mode')", "dynamic");
	text = eval_world(view, "DarkReader.isEnabled() + ':' + __gsurf_dark.current.mode");
	g_assert_cmpstr(text, ==, "true:dynamic");
	g_free(text);
	drop_view(view);
}

static void
test_frames_follow_top(void)
{
	g_autoptr(GsurfModuleManager) manager = NULL;
	GsurfView *view;
	gchar *text;

	if (!gsurf_webkit_test_init())
		return;
	/* localhost alone would be off: the child frame is on localhost but
	 * its top page is on 127.0.0.1 (filter), so it follows the top. */
	manager = manager_with(
		"    mode: filter\n"
		"    sites:\n"
		"      - match: '^localhost$'\n"
		"        mode: off\n");
	view = new_view(manager);
	load(view, "127.0.0.1", "/frame");
	wait_for(view, "typeof window.childReport", "string");
	text = eval(view, "window.childReport");
	/* No second inversion inside the frame; its media re-inverted. */
	g_assert_cmpstr(text, ==,
		"{\"html\":\"none\",\"img\":\"" MEDIA_FILTER "\",\"scheme\":\"normal\"}");
	g_free(text);
	drop_view(view);
}

static void
test_toggles(void)
{
	g_autoptr(GsurfModuleManager) manager = NULL;
	g_autofree gchar *state_file = NULL;
	g_autofree gchar *contents = NULL;
	GsurfModule *module;
	GsurfView *a, *b, *c, *late;
	GStatBuf st;
	gchar *text;

	if (!gsurf_webkit_test_init())
		return;
	manager = manager_with("    mode: filter\n");
	module = module_of(manager);
	g_object_get(module, "state-file", &state_file, NULL);
	g_assert_false(g_file_test(state_file, G_FILE_TEST_EXISTS));

	/* Two views on 127.0.0.1, one on localhost, and one created now but
	 * loaded only after the toggle. */
	a = new_view(manager);
	b = new_view(manager);
	c = new_view(manager);
	late = new_view(manager);
	load(a, "127.0.0.1", "/plain");
	load(b, "127.0.0.1", "/plain");
	load(c, "localhost", "/plain");
	text = html_filter(a);
	g_assert_cmpstr(text, ==, "invert(1) hue-rotate(180deg)");
	g_free(text);

	/* 1. Per-site toggle from view a: every open 127.0.0.1 page turns
	 * off at once, localhost stays dark, the override is persisted. */
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), a,
		GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	wait_for(a, "getComputedStyle(document.documentElement).filter", "none");
	wait_for(b, "getComputedStyle(document.documentElement).filter", "none");
	text = html_filter(c);
	g_assert_cmpstr(text, ==, "invert(1) hue-rotate(180deg)");
	g_free(text);
	g_assert_true(g_file_get_contents(state_file, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "\"127.0.0.1\": false"));
	g_assert_cmpint(g_stat(state_file, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);

	/* 2. A view created before the toggle starts its next document from
	 * the new state: off as soon as the load finishes (no flash). */
	load(late, "127.0.0.1", "/plain");
	text = eval_world(late, "__gsurf_dark.current.mode + ':' + __gsurf_dark.current.source");
	g_assert_cmpstr(text, ==, "off:override");
	g_free(text);
	text = html_filter(late);
	g_assert_cmpstr(text, ==, "none");
	g_free(text);
	/* And so does a navigation away and back in an existing view. */
	load(c, "127.0.0.1", "/plain");
	text = html_filter(c);
	g_assert_cmpstr(text, ==, "none");
	g_free(text);
	load(c, "localhost", "/plain");

	/* 3. Global toggle: everything off, then back. */
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), c,
		GSURF_ACTION_DARK_MODE_TOGGLE));
	wait_for(c, "getComputedStyle(document.documentElement).filter", "none");
	text = eval_world(c, "__gsurf_dark.current.source");
	g_assert_cmpstr(text, ==, "disabled");
	g_free(text);
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), c,
		GSURF_ACTION_DARK_MODE_TOGGLE));
	wait_for(c, "getComputedStyle(document.documentElement).filter", "invert(1) hue-rotate(180deg)");
	/* 127.0.0.1 keeps its override through the global toggle. */
	text = html_filter(a);
	g_assert_cmpstr(text, ==, "none");
	g_free(text);

	/* 4. Toggling the site again restores the configured mode and drops
	 * the override from the file. */
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), b,
		GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	wait_for(a, "getComputedStyle(document.documentElement).filter", "invert(1) hue-rotate(180deg)");
	wait_for(b, "getComputedStyle(document.documentElement).filter", "invert(1) hue-rotate(180deg)");
	g_clear_pointer(&contents, g_free);
	g_assert_true(g_file_get_contents(state_file, &contents, NULL, NULL));
	g_assert_null(strstr(contents, "127.0.0.1"));

	drop_view(a);
	drop_view(b);
	drop_view(c);
	drop_view(late);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *home = g_dir_make_tmp("gsurf-dark-mode-gui-XXXXXX", NULL);
	g_autofree gchar *data = g_build_filename(home, "data", NULL);
	g_autofree gchar *cache = g_build_filename(home, "cache", NULL);
	g_autoptr(SoupServer) server = NULL;
	gint result;

	/* Keep the state file and WebKit data out of the user's home. */
	g_setenv("XDG_DATA_HOME", data, TRUE);
	g_setenv("XDG_CACHE_HOME", cache, TRUE);
	g_test_init(&argc, &argv, NULL);

	server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, serve, NULL, NULL);
	server_port = gsurf_webkit_test_listen(server);

	g_test_add_func("/gsurf/dark-mode/webkit/filter-and-native", test_filter_and_native_per_host);
	g_test_add_func("/gsurf/dark-mode/webkit/off", test_off_leaves_page_alone);
	g_test_add_func("/gsurf/dark-mode/webkit/decide", test_decide_in_world);
	g_test_add_func("/gsurf/dark-mode/webkit/dynamic", test_dynamic_dark_reader);
	g_test_add_func("/gsurf/dark-mode/webkit/frames", test_frames_follow_top);
	g_test_add_func("/gsurf/dark-mode/webkit/toggles", test_toggles);
	result = g_test_run();
	gsurf_test_rmtree(home);
	return result;
}
