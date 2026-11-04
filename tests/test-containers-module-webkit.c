/*
 * test-containers-module-webkit.c - The containers module in real windows
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the real containers.so into the default module manager, gives it
 * a host application with a real window, and drives WebKit views
 * (GTK3/4.1 or GTK4/6.0) against a loopback HTTP server reachable as two
 * hosts: 127.0.0.1 (unassigned) and localhost (assigned to "Media").
 * Checks what the headless test cannot:
 *   - an address loaded into a default tab is reopened in Media in the
 *     same place, the blank tab is closed, and the cookie lands in
 *     Media's jar;
 *   - a main-frame navigation by script moves too (main-frame response),
 *     keeping the old tab, which becomes the new tab's opener;
 *   - an iframe of the assigned host is NOT moved;
 *   - container-new-temp and container-reopen-next.
 * Runs only with GSURF_TEST_GUI=1 (make test-gui).
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>

#include "test-util.h"
#include "webkit-test-util.h"

static guint16 port;
static guint   frame_requests;

/* /set sets a cookie, /get echoes the Cookie header, /frame embeds the
 * assigned host in an iframe, anything else is a plain page. */
static void
serve(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query,
      gpointer data)
{
	SoupMessageHeaders *req = soup_server_message_get_request_headers(msg);
	SoupMessageHeaders *resp = soup_server_message_get_response_headers(msg);
	g_autofree gchar *body = NULL;

	(void)server;
	(void)data;
	if (g_str_equal(path, "/set")) {
		soup_message_headers_append(resp, "Set-Cookie", "jar=filled; Path=/; Max-Age=3600");
		body = g_strdup("<html><body>set</body></html>");
	} else if (g_str_equal(path, "/get")) {
		const gchar *cookie = soup_message_headers_get_one(req, "Cookie");

		if (query != NULL && g_hash_table_contains(query, "frame"))
			frame_requests++;
		body = g_strdup_printf("<html><body><pre id=c>%s</pre></body></html>",
			cookie != NULL ? cookie : "none");
	} else if (g_str_equal(path, "/frame")) {
		body = g_strdup_printf("<html><body><iframe id=f "
			"src='http://localhost:%u/get?frame=1'></iframe></body></html>", port);
	} else {
		body = g_strdup("<html><body>page</body></html>");
	}
	soup_server_message_set_status(msg, 200, NULL);
	soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_COPY, body, strlen(body));
}

static gchar *
url(const gchar *host, const gchar *path)
{
	return g_strdup_printf("http://%s:%u%s", host, port, path);
}

/* Spin (bounded) until @view shows a finished load whose URI starts
 * with @prefix. Polling avoids racing a load that already finished. */
static void
wait_loaded(GsurfView *view, const gchar *prefix)
{
	WebKitWebView *native = gsurf_webkit_test_native(view);
	guint i;

	for (i = 0; i < 400; i++) {
		const gchar *uri = webkit_web_view_get_uri(native);

		if (!webkit_web_view_is_loading(native) && uri != NULL &&
		    g_str_has_prefix(uri, prefix))
			return;
		gsurf_webkit_test_spin(25);
	}
	g_error("timed out waiting for %s (at %s)", prefix,
		webkit_web_view_get_uri(native) != NULL ? webkit_web_view_get_uri(native) : "nothing");
}

/* Spin (bounded) until the window's view @index is not @old. */
static GsurfView *
wait_replaced(GsurfWindow *window, guint index, GsurfView *old)
{
	guint i;

	for (i = 0; i < 400; i++) {
		GsurfView *now = (index < gsurf_window_get_n_views(window))
			? gsurf_window_get_nth_view(window, index) : NULL;

		if (now != NULL && now != old)
			return now;
		gsurf_webkit_test_spin(25);
	}
	g_error("timed out waiting for view %u to change", index);
	return NULL;
}

/* The Cookie header the server saw from @view for the Media host. */
static gchar *
cookie_seen(GsurfView *view)
{
	g_autofree gchar *uri = url("localhost", "/get");

	gsurf_webkit_test_load(gsurf_webkit_test_native(view), uri);
	return gsurf_webkit_test_eval(gsurf_webkit_test_native(view),
		"document.getElementById('c').textContent");
}

/* A tab like the host's make_tab(): in @container, added to @window. */
static GsurfView *
add_tab(GsurfWindow *window, const gchar *container, const gchar *uri)
{
	GsurfView *view = gsurf_view_new_full(NULL, container, uri, FALSE);

	gsurf_window_add_view(window, view);
	gsurf_window_set_active_view(window, view);
	g_object_unref(view);
	return view;
}

/* Close every tab so each test starts with an empty window. */
static void
clear_window(GsurfWindow *window)
{
	while (gsurf_window_get_n_views(window) > 0)
		gsurf_window_remove_view(window, gsurf_window_get_nth_view(window, 0));
	gsurf_webkit_test_spin(50);
}

typedef struct {
	GsurfApplication *app;
	GsurfWindow      *window;
	SoupServer       *server;
} Fixture;

static Fixture fx;

static gboolean
setup(void)
{
	static gboolean done = FALSE;
	GsurfModuleManager *mgr;
	g_autofree gchar *so = NULL;
	g_autoptr(GError) error = NULL;
	GsurfConfig *config;

	if (!gsurf_webkit_test_init())
		return FALSE;
	if (done)
		return TRUE;
	done = TRUE;

	fx.server = soup_server_new(NULL, NULL);
	soup_server_add_handler(fx.server, NULL, serve, NULL, NULL);
	port = gsurf_webkit_test_listen(fx.server);

	/* Media gets localhost; new tabs default to the default container
	 * so the unassigned host stays put. */
	config = gsurf_config_new();
	{
		g_autofree gchar *yaml = g_strdup(
			"browser:\n  homepage: about:blank\n  new_view_uri: about:blank\n"
			"modules:\n  containers:\n    enabled: true\n    new_tab: default\n"
			"    temp_cleanup_delay: 0\n    temp_color: '#89b4fa'\n"
			"    containers:\n"
			"      - name: Media\n        color: '#f9e2af'\n        hosts: ['^localhost$']\n"
			"      - name: Work\n");

		g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
		g_assert_no_error(error);
	}
	gsurf_config_set_default(config);

	fx.app = gsurf_application_new(config);
	fx.window = gsurf_window_new();
	gsurf_application_add_window(fx.app, fx.window);

	mgr = gsurf_module_manager_get_default();
	gsurf_module_manager_set_application(mgr, fx.app);
	gsurf_module_manager_set_config(mgr, config);
	so = gsurf_webkit_test_module_path("containers");
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(mgr);
	g_object_unref(config);
	return TRUE;
}

/* A typed address (gsurf_view_load_uri) to an assigned host moves before
 * the request: the blank tab is replaced in place and Media's jar gets
 * the cookie. */
static void
test_typed_address_moves(void)
{
	g_autofree gchar *set = NULL;
	g_autofree gchar *seen = NULL;
	GsurfView *first;
	GsurfView *blank;
	GsurfView *moved;
	gpointer weak;

	if (!setup())
		return;
	clear_window(fx.window);

	first = add_tab(fx.window, GSURF_CONTAINER_DEFAULT, NULL);
	gsurf_webkit_test_load(gsurf_webkit_test_native(first), "about:blank");
	blank = add_tab(fx.window, GSURF_CONTAINER_DEFAULT, NULL);
	gsurf_webkit_test_load(gsurf_webkit_test_native(blank), "about:blank");
	g_assert_cmpint(gsurf_window_get_view_index(fx.window, blank), ==, 1);

	weak = blank;
	g_object_add_weak_pointer(G_OBJECT(blank), &weak);
	set = url("localhost", "/set");
	gsurf_view_load_uri(blank, set);

	moved = wait_replaced(fx.window, 1, blank);
	g_assert_cmpstr(gsurf_view_get_container_id(moved), ==, "media");
	g_assert_true(gsurf_window_get_active_view(fx.window) == moved);
	g_assert_cmpuint(gsurf_window_get_n_views(fx.window), ==, 2);
	g_assert_true(gsurf_window_get_nth_view(fx.window, 0) == first);
	wait_loaded(moved, set);

	/* The old blank tab is gone for good. */
	gsurf_webkit_test_spin(100);
	g_assert_null(weak);

	/* The cookie is in Media's jar; the default jar never saw it (an
	 * explicitly placed default tab keeps its first load). */
	seen = cookie_seen(moved);
	g_assert_cmpstr(seen, ==, "jar=filled");
	g_clear_pointer(&seen, g_free);
	{
		g_autofree gchar *get = url("localhost", "/get");
		GsurfView *in_default = add_tab(fx.window, GSURF_CONTAINER_DEFAULT, get);

		gsurf_webkit_test_load(gsurf_webkit_test_native(in_default), get);
		seen = gsurf_webkit_test_eval(gsurf_webkit_test_native(in_default),
			"document.getElementById('c').textContent");
		g_assert_cmpstr(seen, ==, "none");
		g_assert_cmpstr(gsurf_view_get_container_id(in_default), ==, NULL);
	}
	clear_window(fx.window);
}

/* A script navigation of the main frame is only recognisable at the
 * response; the old tab (with a real page) stays and opens the new one. */
static void
test_main_frame_script_moves(void)
{
	g_autofree gchar *page = url("127.0.0.1", "/page");
	g_autofree gchar *target = url("localhost", "/get");
	g_autofree gchar *js = NULL;
	GsurfView *tab;
	GsurfView *moved;

	if (!setup())
		return;
	clear_window(fx.window);

	tab = add_tab(fx.window, GSURF_CONTAINER_DEFAULT, NULL);
	gsurf_webkit_test_load(gsurf_webkit_test_native(tab), page);
	g_assert_null(gsurf_view_get_container_id(tab));

	js = g_strdup_printf("location.href = '%s'; 'ok'", target);
	g_free(gsurf_webkit_test_eval(gsurf_webkit_test_native(tab), js));

	moved = wait_replaced(fx.window, 1, NULL);
	g_assert_cmpstr(gsurf_view_get_container_id(moved), ==, "media");
	g_assert_true(gsurf_view_get_opener(moved) == tab);
	g_assert_true(gsurf_window_get_active_view(fx.window) == moved);
	g_assert_true(gsurf_window_get_nth_view(fx.window, 0) == tab);
	wait_loaded(moved, target);

	/* The old tab stayed on its page. */
	wait_loaded(tab, page);
	g_assert_cmpuint(gsurf_window_get_n_views(fx.window), ==, 2);
	clear_window(fx.window);
}

/* An iframe of the assigned host loads inside the page; nothing moves. */
static void
test_subframe_stays(void)
{
	g_autofree gchar *page = url("127.0.0.1", "/frame");
	GsurfView *tab;
	guint i;

	if (!setup())
		return;
	clear_window(fx.window);
	frame_requests = 0;

	tab = add_tab(fx.window, GSURF_CONTAINER_DEFAULT, NULL);
	gsurf_webkit_test_load(gsurf_webkit_test_native(tab), page);
	for (i = 0; i < 200 && frame_requests == 0; i++)
		gsurf_webkit_test_spin(25);
	g_assert_cmpuint(frame_requests, >=, 1);
	/* Give a (wrong) reopen every chance to run. */
	gsurf_webkit_test_spin(300);
	g_assert_cmpuint(gsurf_window_get_n_views(fx.window), ==, 1);
	g_assert_true(gsurf_window_get_nth_view(fx.window, 0) == tab);
	g_assert_null(gsurf_view_get_container_id(tab));
	wait_loaded(tab, page);
	clear_window(fx.window);
}

static gboolean
act(GsurfView *view, GsurfAction action)
{
	return gsurf_module_manager_dispatch_action(gsurf_module_manager_get_default(),
		view, action);
}

static void
test_actions(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	g_autofree gchar *page = url("127.0.0.1", "/page");
	GsurfContainer *temp;
	GsurfView *base;
	GsurfView *view;
	GsurfView *next;
	GsurfView *other;

	if (!setup())
		return;
	clear_window(fx.window);

	/* container-new-temp: a new active tab, last, in a fresh temporary
	 * container with temp_color, on new_view_uri. */
	base = add_tab(fx.window, GSURF_CONTAINER_DEFAULT, NULL);
	gsurf_webkit_test_load(gsurf_webkit_test_native(base), page);
	g_assert_true(act(base, GSURF_ACTION_CONTAINER_NEW_TEMP));
	g_assert_cmpuint(gsurf_window_get_n_views(fx.window), ==, 2);
	view = gsurf_window_get_nth_view(fx.window, 1);
	g_assert_true(gsurf_window_get_active_view(fx.window) == view);
	temp = gsurf_container_registry_lookup(registry, gsurf_view_get_container_id(view));
	g_assert_nonnull(temp);
	g_assert_true(gsurf_container_get_temporary(temp));
	g_assert_cmpstr(gsurf_container_get_color(temp), ==, "#89b4fa");
	wait_loaded(view, "about:blank");

	/* container-reopen-next on a child tab in the middle: default ->
	 * Media -> Work -> temporary -> default, same place, same opener,
	 * old tab closed, same page. */
	other = add_tab(fx.window, GSURF_CONTAINER_DEFAULT, NULL);
	gsurf_window_move_view(fx.window, other, 2);
	view = gsurf_view_new_full(base, GSURF_CONTAINER_DEFAULT, page, FALSE);
	gsurf_window_insert_view(fx.window, view, 1);
	g_object_unref(view);
	gsurf_webkit_test_load(gsurf_webkit_test_native(view), page);

	{
		static const gchar *const cycle[] = { "media", "work", "tmp-", NULL };
		guint i;

		for (i = 0; i < G_N_ELEMENTS(cycle); i++) {
			gpointer weak = view;
			const gchar *id;

			g_object_add_weak_pointer(G_OBJECT(view), &weak);
			g_assert_true(act(view, GSURF_ACTION_CONTAINER_REOPEN_NEXT));
			next = gsurf_window_get_nth_view(fx.window, 1);
			g_assert_true(next != view);
			g_assert_true(gsurf_view_get_opener(next) == base);
			g_assert_true(gsurf_window_get_active_view(fx.window) == next);
			g_assert_cmpuint(gsurf_window_get_n_views(fx.window), ==, 4);
			id = gsurf_view_get_container_id(next);
			if (cycle[i] == NULL)
				g_assert_null(id);
			else
				g_assert_true(g_str_has_prefix(id, cycle[i]));
			wait_loaded(next, page);
			gsurf_webkit_test_spin(50);
			g_assert_null(weak);
			view = next;
		}
	}
	clear_window(fx.window);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *home = g_dir_make_tmp("gsurf-containers-module-gui-XXXXXX", NULL);
	g_autofree gchar *data = g_build_filename(home, "data", NULL);
	g_autofree gchar *cache = g_build_filename(home, "cache", NULL);
	gint result;

	/* Keep container profiles out of the user's real data directory. */
	g_setenv("XDG_DATA_HOME", data, TRUE);
	g_setenv("XDG_CACHE_HOME", cache, TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/containers-module/webkit/typed-address-moves",
		test_typed_address_moves);
	g_test_add_func("/gsurf/containers-module/webkit/main-frame-script-moves",
		test_main_frame_script_moves);
	g_test_add_func("/gsurf/containers-module/webkit/subframe-stays", test_subframe_stays);
	g_test_add_func("/gsurf/containers-module/webkit/actions", test_actions);
	result = g_test_run();
	/* The window, application and module stay alive until exit: the
	 * default module manager outlives the test functions. */
	g_clear_object(&fx.server);
	gsurf_test_rmtree(home);
	return result;
}
