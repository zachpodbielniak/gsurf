/*
 * test-containers-webkit.c - Container isolation and script worlds in WebKit
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Drives real WebKit views (GTK3/4.1 or GTK4/6.0) against a loopback HTTP
 * server to check what the headless registry tests cannot:
 *   - each container has its own cookie jar (persistent, temporary and
 *     the default container never see each other's cookies);
 *   - a persistent container stores its data in a private directory;
 *   - a popup is a related view in its opener's container, with a live
 *     window.opener;
 *   - isolated script worlds hide page globals, share the DOM, and post
 *     messages to GsurfView::script-message.
 * Runs only with GSURF_TEST_GUI=1 (make test-gui).
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "test-util.h"
#include "webkit-test-util.h"

/* /set sets a cookie, /get echoes the Cookie header, /popup opens /get
 * in a popup, /page defines a page global for the world test. */
static void
serve(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query,
      gpointer data)
{
	SoupMessageHeaders *req = soup_server_message_get_request_headers(msg);
	SoupMessageHeaders *resp = soup_server_message_get_response_headers(msg);
	g_autofree gchar *body = NULL;

	(void)server;
	(void)query;
	(void)data;
	if (g_str_equal(path, "/set")) {
		soup_message_headers_append(resp, "Set-Cookie", "jar=filled; Path=/; Max-Age=3600");
		body = g_strdup("<html><body>set</body></html>");
	} else if (g_str_equal(path, "/get")) {
		const gchar *cookie = soup_message_headers_get_one(req, "Cookie");

		body = g_strdup_printf("<html><body><pre id=c>%s</pre></body></html>",
			cookie != NULL ? cookie : "none");
	} else if (g_str_equal(path, "/popup")) {
		body = g_strdup("<html><body><a id=go href='/get' target='_blank'>go</a>"
			"<script>window.marker = 'opener-page';</script></body></html>");
	} else {
		body = g_strdup("<html><body><div id=dom>shared</div>"
			"<script>window.pageSecret = 42;</script></body></html>");
	}
	soup_server_message_set_status(msg, 200, NULL);
	soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_COPY, body, strlen(body));
}

/* The Cookie header the server saw, as rendered by /get. */
static gchar *
cookie_seen(GsurfView *view, guint16 port)
{
	g_autofree gchar *uri = g_strdup_printf("http://127.0.0.1:%u/get", port);

	gsurf_webkit_test_load(gsurf_webkit_test_native(view), uri);
	return gsurf_webkit_test_eval(gsurf_webkit_test_native(view),
		"document.getElementById('c').textContent");
}

static void
set_cookie(GsurfView *view, guint16 port)
{
	g_autofree gchar *uri = g_strdup_printf("http://127.0.0.1:%u/set", port);

	gsurf_webkit_test_load(gsurf_webkit_test_native(view), uri);
}

/* Host side of a popup, as main.c does it: a related view of @source. */
static GsurfView *
on_create_view(GsurfView *source, const gchar *uri, gpointer data)
{
	GsurfView **popup = data;

	*popup = gsurf_view_new_full(source, NULL, uri, TRUE);
	return *popup;
}

static void
test_cookie_isolation(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	g_autoptr(SoupServer) server = NULL;
	g_autoptr(GsurfView) a1 = NULL;
	g_autoptr(GsurfView) a2 = NULL;
	g_autoptr(GsurfView) b = NULL;
	g_autoptr(GsurfView) plain = NULL;
	g_autoptr(GsurfView) temp1 = NULL;
	g_autoptr(GsurfView) temp2 = NULL;
	g_autofree gchar *data_dir = NULL;
	g_autofree gchar *seen = NULL;
	guint16 port;
	GStatBuf st;

	if (!gsurf_webkit_test_init())
		return;

	server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, serve, NULL, NULL);
	port = gsurf_webkit_test_listen(server);

	g_assert_nonnull(gsurf_container_registry_define(registry, "gui-a", "A", "#f00", NULL));
	g_assert_nonnull(gsurf_container_registry_define(registry, "gui-b", "B", "#0f0", NULL));

	a1 = gsurf_view_new_full(NULL, "gui-a", NULL, FALSE);
	b = gsurf_view_new_full(NULL, "gui-b", NULL, FALSE);
	plain = gsurf_view_new_full(NULL, NULL, NULL, FALSE);
	g_assert_cmpstr(gsurf_view_get_container_id(a1), ==, "gui-a");
	g_assert_nonnull(gsurf_view_get_session(a1));
	g_assert_true(gsurf_view_get_session(a1) != gsurf_view_get_session(b));
	g_assert_null(gsurf_view_get_session(plain));

	/* A cookie set in container A is sent by every view in A ... */
	set_cookie(a1, port);
	seen = cookie_seen(a1, port);
	g_assert_cmpstr(seen, ==, "jar=filled");
	g_clear_pointer(&seen, g_free);
	a2 = gsurf_view_new_full(NULL, "gui-a", NULL, FALSE);
	seen = cookie_seen(a2, port);
	g_assert_cmpstr(seen, ==, "jar=filled");
	g_clear_pointer(&seen, g_free);

	/* ... and by nothing outside it. */
	seen = cookie_seen(b, port);
	g_assert_cmpstr(seen, ==, "none");
	g_clear_pointer(&seen, g_free);
	seen = cookie_seen(plain, port);
	g_assert_cmpstr(seen, ==, "none");
	g_clear_pointer(&seen, g_free);

	/* Temporary containers are isolated from each other too. */
	temp1 = gsurf_view_new_full(NULL, GSURF_CONTAINER_TEMPORARY, NULL, FALSE);
	temp2 = gsurf_view_new_full(NULL, GSURF_CONTAINER_TEMPORARY, NULL, FALSE);
	g_assert_cmpstr(gsurf_view_get_container_id(temp1), !=, gsurf_view_get_container_id(temp2));
	set_cookie(temp1, port);
	seen = cookie_seen(temp1, port);
	g_assert_cmpstr(seen, ==, "jar=filled");
	g_clear_pointer(&seen, g_free);
	seen = cookie_seen(temp2, port);
	g_assert_cmpstr(seen, ==, "none");
	g_clear_pointer(&seen, g_free);

	/* A persistent container keeps its data in a private directory. */
	data_dir = gsurf_container_registry_get_data_dir("gui-a");
	g_assert_cmpint(g_stat(data_dir, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);
}

static void
test_popup_inherits_container(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	g_autoptr(SoupServer) server = NULL;
	g_autoptr(GsurfView) opener = NULL;
	g_autoptr(GsurfView) popup = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *seen = NULL;
	g_autofree gchar *marker = NULL;
	WebKitWebView *native;
	guint16 port;
	guint i;

	if (!gsurf_webkit_test_init())
		return;

	server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, serve, NULL, NULL);
	port = gsurf_webkit_test_listen(server);
	g_assert_nonnull(gsurf_container_registry_define(registry, "gui-pop", NULL, NULL, NULL));

	opener = gsurf_view_new_full(NULL, "gui-pop", NULL, FALSE);
	native = gsurf_webkit_test_native(opener);
	webkit_settings_set_javascript_can_open_windows_automatically(
		webkit_web_view_get_settings(native), TRUE);
	g_signal_connect(opener, "create-view", G_CALLBACK(on_create_view), &popup);
	set_cookie(opener, port);

	uri = g_strdup_printf("http://127.0.0.1:%u/popup", port);
	gsurf_webkit_test_load(native, uri);
	/* window.open keeps the opener; a target=_blank link would be
	 * implicitly rel=noopener in current WebKit. */
	g_free(gsurf_webkit_test_eval(native, "window.open('/get'); 'ok'"));
	for (i = 0; i < 100 && popup == NULL; i++)
		gsurf_webkit_test_spin(50);
	g_assert_nonnull(popup);
	/* Hold our own ref: the handler handed WebKit the native widget only. */
	g_object_ref(popup);

	g_assert_true(gsurf_view_get_related(popup));
	g_assert_true(gsurf_view_get_opener(popup) == opener);
	g_assert_cmpstr(gsurf_view_get_container_id(popup), ==, "gui-pop");

	/* The popup's request carried the opener container's cookie, and it
	 * can script its opener. The popup may finish loading before we look,
	 * so poll its DOM (bounded) rather than wait for a load event. */
	for (i = 0; i < 200; i++) {
		g_clear_pointer(&seen, g_free);
		seen = gsurf_webkit_test_eval(gsurf_webkit_test_native(popup),
			"document.getElementById('c') ? document.getElementById('c').textContent : ''");
		if (seen != NULL && *seen != '\0')
			break;
		gsurf_webkit_test_spin(50);
	}
	g_assert_cmpstr(seen, ==, "jar=filled");
	marker = gsurf_webkit_test_eval(gsurf_webkit_test_native(popup),
		"window.opener ? window.opener.marker : 'no-opener'");
	g_assert_cmpstr(marker, ==, "opener-page");
}

static void
on_probe(GsurfView *view, const gchar *name, const gchar *json, gpointer data)
{
	gchar **out = data;

	(void)view;
	g_assert_cmpstr(name, ==, "probe");
	g_free(*out);
	*out = g_strdup(json);
}

typedef struct {
	GMainLoop *loop;
	gchar     *value;
} WorldResult;

static void
on_world_done(GObject *source, GAsyncResult *result, gpointer data)
{
	WorldResult *out = data;
	g_autoptr(GError) error = NULL;

	out->value = gsurf_view_run_javascript_in_world_finish(GSURF_VIEW(source), result, &error);
	g_assert_no_error(error);
	g_main_loop_quit(out->loop);
}

static void
test_script_worlds(void)
{
	g_autoptr(SoupServer) server = NULL;
	g_autoptr(GsurfView) view = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *page_view = NULL;
	WorldResult world = { NULL, NULL };
	guint16 port;
	guint i;

	if (!gsurf_webkit_test_init())
		return;

	server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, serve, NULL, NULL);
	port = gsurf_webkit_test_listen(server);

	view = gsurf_view_new_full(NULL, NULL, NULL, FALSE);
	g_signal_connect(view, "script-message::probe", G_CALLBACK(on_probe), &message);
	gsurf_view_register_script_message(view, "probe", "gsurf-test");
	gsurf_view_register_script_message(view, "probe", "gsurf-test");  /* no-op */
	gsurf_view_add_user_script_in_world(view,
		"window.worldOnly = 'hidden';"
		"window.webkit.messageHandlers.probe.postMessage({"
		"  secret: typeof window.pageSecret,"
		"  dom: document.getElementById('dom') ? document.getElementById('dom').textContent : null"
		"});", TRUE, NULL, "gsurf-test");

	uri = g_strdup_printf("http://127.0.0.1:%u/page", port);
	gsurf_webkit_test_load(gsurf_webkit_test_native(view), uri);
	for (i = 0; i < 100 && message == NULL; i++)
		gsurf_webkit_test_spin(20);

	/* The world shares the DOM but not the page's globals. */
	g_assert_cmpstr(message, ==, "{\"secret\":\"undefined\",\"dom\":\"shared\"}");

	/* And the page cannot see the world's globals either. */
	page_view = gsurf_webkit_test_eval(gsurf_webkit_test_native(view),
		"typeof window.worldOnly + ':' + window.pageSecret");
	g_assert_cmpstr(page_view, ==, "undefined:42");

	loop = g_main_loop_new(NULL, FALSE);
	world.loop = loop;
	gsurf_view_run_javascript_in_world_async(view, "window.worldOnly", "gsurf-test",
		NULL, on_world_done, &world);
	g_main_loop_run(loop);
	g_assert_cmpstr(world.value, ==, "hidden");
	g_free(world.value);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *home = g_dir_make_tmp("gsurf-containers-gui-XXXXXX", NULL);
	g_autofree gchar *data = g_build_filename(home, "data", NULL);
	g_autofree gchar *cache = g_build_filename(home, "cache", NULL);
	gint result;

	/* Keep container profiles out of the user's real data directory. */
	g_setenv("XDG_DATA_HOME", data, TRUE);
	g_setenv("XDG_CACHE_HOME", cache, TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/containers/webkit/cookie-isolation", test_cookie_isolation);
	g_test_add_func("/gsurf/containers/webkit/popup-inherits", test_popup_inherits_container);
	g_test_add_func("/gsurf/containers/webkit/script-worlds", test_script_worlds);
	result = g_test_run();
	gsurf_test_rmtree(home);
	return result;
}
