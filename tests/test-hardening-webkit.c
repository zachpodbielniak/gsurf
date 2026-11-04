/*
 * test-hardening-webkit.c - hardening enforced by a real WebKit view
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the hardening module (and so its web-process extension), then
 * pages from a loopback libsoup server that answers both plain HTTP and
 * HTTPS (the committed TLS fixture, with tls_strict off). The server is
 * reachable as two hosts, `127.0.0.1` and `localhost`, so a page on one
 * makes cross-origin requests to the other. Every assertion is about what
 * the SERVER received (headers, query strings, scheme) or what the page
 * sees (`navigator.globalPrivacyControl`).
 *
 * The web-process policy is fixed when the first web process starts, so
 * each scenario runs in its own child process (see in_subprocess()).
 * Needs a display and a web process: only with GSURF_TEST_GUI=1.
 */

#include "webkit-test-util.h"
#include "test-util.h"
#include <string.h>

#define TLS_FIXTURE "tests/fixtures/gemini-server.pem"

/* One request as the server saw it. */
typedef struct {
	gchar   *scheme;
	gchar   *query;
	gchar   *referer;
	gchar   *gpc;
	gchar   *dnt;
	guint    count;
} Hit;

typedef struct {
	SoupServer *server;
	GHashTable *hits;        /* path -> Hit* */
	guint16     http_port;   /* plain HTTP */
	guint16     https_port;  /* TLS */
} TestServer;

static TestServer ts;

static void
hit_free(gpointer data)
{
	Hit *hit = data;

	g_free(hit->scheme);
	g_free(hit->query);
	g_free(hit->referer);
	g_free(hit->gpc);
	g_free(hit->dnt);
	g_free(hit);
}

/* The page served at @path. `unsafe-url` makes WebKit send the full
 * URL cross-origin, so only hardening can trim it. The hardened page
 * reaches over to localhost on the TLS port, the exempt one to
 * 127.0.0.1 on the plain port. */
static gchar *
page_for(const gchar *path)
{
	if (g_str_equal(path, "/hard.html"))
		return g_strdup_printf(
			"<!DOCTYPE html><html><head><meta name=\"referrer\" content=\"unsafe-url\">"
			"<link rel=\"prefetch\" href=\"/prefetch-hard.txt\"></head><body>"
			"<img src=\"https://localhost:%u/cross-hard.gif?fbclid=1&amp;k=2#frag\">"
			"<img src=\"/same-hard.gif?utm_medium=x&amp;k=3\">"
			"<a id=\"ping\" ping=\"/ping-hard\" href=\"#here\">ping</a>"
			"</body></html>", ts.https_port);
	if (g_str_equal(path, "/exempt.html"))
		return g_strdup_printf(
			"<!DOCTYPE html><html><head><meta name=\"referrer\" content=\"unsafe-url\">"
			"<link rel=\"prefetch\" href=\"/prefetch-exempt.txt\"></head><body>"
			"<img src=\"http://127.0.0.1:%u/cross-exempt.gif?fbclid=1&amp;k=2\">"
			"<a id=\"ping\" ping=\"/ping-exempt\" href=\"#here\">ping</a>"
			"</body></html>", ts.http_port);
	if (g_str_equal(path, "/strip.html"))
		return g_strdup_printf(
			"<!DOCTYPE html><html><head><meta name=\"referrer\" content=\"unsafe-url\">"
			"</head><body><img src=\"http://localhost:%u/cross-strip.gif\">"
			"<img src=\"/same-strip.gif\"></body></html>", ts.http_port);
	return NULL;
}

static void
serve(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query,
      gpointer data)
{
	SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
	GUri *uri = soup_server_message_get_uri(msg);
	g_autofree gchar *body = page_for(path);
	Hit *hit = g_hash_table_lookup(ts.hits, path);

	/* Record what arrived (the last request per path, and a count). */
	if (hit == NULL) {
		hit = g_new0(Hit, 1);
		g_hash_table_insert(ts.hits, g_strdup(path), hit);
	}
	hit->count++;
	g_free(hit->scheme);
	hit->scheme = g_strdup(g_uri_get_scheme(uri));
	g_free(hit->query);
	hit->query = g_strdup(g_uri_get_query(uri));
	g_free(hit->referer);
	hit->referer = g_strdup(soup_message_headers_get_one(headers, "Referer"));
	g_free(hit->gpc);
	hit->gpc = g_strdup(soup_message_headers_get_one(headers, "Sec-GPC"));
	g_free(hit->dnt);
	hit->dnt = g_strdup(soup_message_headers_get_one(headers, "DNT"));

	/* /down redirects to its own http:// twin (a downgrade loop under
	 * https-only); /moved to a different http:// URI (just upgraded). */
	if (g_str_equal(path, "/down") || g_str_equal(path, "/moved")) {
		g_autofree gchar *target = g_strdup_printf("http://127.0.0.1:%u%s", ts.https_port,
			g_str_equal(path, "/down") ? "/down" : "/target.html");

		soup_server_message_set_redirect(msg, 302, target);
		return;
	}
	soup_server_message_set_status(msg, 200, NULL);
	if (body != NULL)
		soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_COPY, body, strlen(body));
	else
		soup_server_message_set_response(msg, "text/plain", SOUP_MEMORY_STATIC, "ok", 2);
}

/* One server, two listeners on 127.0.0.1: plain and TLS. `localhost`
 * reaches the same sockets. */
static void
server_start(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GTlsCertificate) cert = g_tls_certificate_new_from_file(TLS_FIXTURE, &error);
	GSList *uris, *l;

	g_assert_no_error(error);
	ts.hits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, hit_free);
	ts.server = soup_server_new("tls-certificate", cert, NULL);
	soup_server_add_handler(ts.server, NULL, serve, NULL, NULL);
	g_assert_true(soup_server_listen_local(ts.server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	g_assert_true(soup_server_listen_local(ts.server, 0,
		SOUP_SERVER_LISTEN_IPV4_ONLY | SOUP_SERVER_LISTEN_HTTPS, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(ts.server);
	for (l = uris; l != NULL; l = l->next) {
		if (g_str_equal(g_uri_get_scheme(l->data), "https"))
			ts.https_port = (guint16)g_uri_get_port(l->data);
		else
			ts.http_port = (guint16)g_uri_get_port(l->data);
	}
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	g_assert_cmpuint(ts.http_port, !=, 0);
	g_assert_cmpuint(ts.https_port, !=, 0);
}

static Hit *
hit(const gchar *path)
{
	return g_hash_table_lookup(ts.hits, path);
}

/* Everything one scenario needs: private XDG dirs, a non-strict TLS
 * default config carrying @module_yaml, the loaded module, one view. */
typedef struct {
	gchar              *home;
	GsurfConfig        *config;
	GsurfModuleManager *manager;
	GsurfView          *view;
	WebKitWebView      *native;
} Scenario;

static gboolean
scenario_start(Scenario *sc, const gchar *module_yaml)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *so = gsurf_webkit_test_module_path("hardening");
	g_autofree gchar *webext = NULL;
	g_autofree gchar *dir = NULL;
	/* Not XDG_RUNTIME_DIR: the display connection (Wayland) lives there. */
	const gchar *const xdg[] = { "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", NULL };
	guint i;

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("hardening.so not built");
		return FALSE;
	}
	dir = g_path_get_dirname(so);
	webext = g_build_filename(dir, "hardening-webext", "gsurf-hardening-webext.so", NULL);
	if (!g_file_test(webext, G_FILE_TEST_EXISTS)) {
		g_test_skip("hardening web-process extension not built");
		return FALSE;
	}
	if (!g_file_test(TLS_FIXTURE, G_FILE_TEST_EXISTS)) {
		g_test_skip("run from the source tree (TLS fixture not found)");
		return FALSE;
	}

	/* Never touch the user's real config, data or cache. */
	sc->home = g_dir_make_tmp("gsurf-hardening-XXXXXX", &error);
	g_assert_no_error(error);
	for (i = 0; xdg[i] != NULL; i++) {
		g_autofree gchar *sub = g_build_filename(sc->home, xdg[i], NULL);

		g_assert_cmpint(g_mkdir_with_parents(sub, 0700), ==, 0);
		g_setenv(xdg[i], sub, TRUE);
	}

	/* The default config: ephemeral, TLS errors ignored (self-signed
	 * fixture), plus the module section under test. */
	sc->config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(sc->config, module_yaml, -1, &error));
	g_assert_no_error(error);
	sc->config->tls_strict = FALSE;
	sc->config->ephemeral = TRUE;
	gsurf_config_set_default(sc->config);

	/* The module registers its extension before the first web process. */
	sc->manager = gsurf_module_manager_new();
	gsurf_module_manager_set_config(sc->manager, sc->config);
	g_assert_nonnull(gsurf_module_manager_load_module(sc->manager, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(sc->manager);

	server_start();
	sc->view = gsurf_view_new();
	sc->native = WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(sc->view));
	return TRUE;
}

static void
scenario_stop(Scenario *sc)
{
	g_clear_object(&sc->view);
	g_clear_object(&sc->manager);
	gsurf_config_set_default(NULL);
	g_clear_object(&sc->config);
	g_clear_object(&ts.server);
	g_clear_pointer(&ts.hits, g_hash_table_unref);
	g_assert_true(gsurf_test_rmtree(sc->home));
	g_free(sc->home);
}

/* Load @uri, then give pings and late subresources time to arrive. */
static void
load_and_settle(Scenario *sc, const gchar *uri)
{
	gsurf_webkit_test_load(sc->native, uri);
	gsurf_webkit_test_spin(300);
}

/* Click the page's <a ping> and wait for the ping, if any, to land. */
static void
click_ping(Scenario *sc)
{
	g_autofree gchar *done = gsurf_webkit_test_eval(sc->native,
		"document.getElementById('ping').click(); 'clicked'");

	g_assert_cmpstr(done, ==, "clicked");
	gsurf_webkit_test_spin(700);
}

/* ITP is off here: WebKit's ITP trims cross-site referrers to the origin
 * on its own, which would hide whether hardening (or the exemption) did. */
static const gchar *MAIN_YAML =
	"modules:\n"
	"  hardening:\n"
	"    enabled: true\n"
	"    https_only: true\n"
	"    https_only_local: true\n"
	"    strip_params: [utm_*, fbclid]\n"
	"    referer: trim_cross_origin\n"
	"    gpc: true\n"
	"    dnt: true\n"
	"    no_prefetch: true\n"
	"    itp: false\n"
	"    exempt:\n"
	"      - match: '^localhost$'\n"
	"        skip: [https_only, strip_params, referer, gpc, dnt, no_prefetch]\n";

/* The hardened page (top-level 127.0.0.1) and the exempt one (top-level
 * localhost) in one web process. */
static void
run_main_scenario(void)
{
	Scenario sc = { 0 };
	g_autofree gchar *uri = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *gpc = NULL;
	Hit *h;

	if (!gsurf_webkit_test_init() || !scenario_start(&sc, MAIN_YAML))
		return;

	/* --- hardened: http:// typed, upgraded to https, params stripped --- */
	uri = g_strdup_printf("http://127.0.0.1:%u/hard.html?utm_source=x&keep=1", ts.https_port);
	load_and_settle(&sc, uri);
	g_assert_true(g_str_has_prefix(webkit_web_view_get_uri(sc.native), "https://127.0.0.1:"));
	h = hit("/hard.html");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->scheme, ==, "https");
	g_assert_cmpstr(h->query, ==, "keep=1");
	g_assert_cmpstr(h->gpc, ==, "1");
	g_assert_cmpstr(h->dnt, ==, "1");

	/* Cross-origin subresource (localhost, NOT exempt as a request host:
	 * the top-level page decides): referer trimmed to the origin. */
	h = hit("/cross-hard.gif");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->query, ==, "k=2");
	g_clear_pointer(&expected, g_free);
	expected = g_strdup_printf("https://127.0.0.1:%u/", ts.https_port);
	g_assert_cmpstr(h->referer, ==, expected);
	g_assert_cmpstr(h->gpc, ==, "1");
	g_assert_cmpstr(h->dnt, ==, "1");

	/* Same-origin subresource: full referer kept, params still stripped. */
	h = hit("/same-hard.gif");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->query, ==, "k=3");
	g_clear_pointer(&expected, g_free);
	expected = g_strdup_printf("https://127.0.0.1:%u/hard.html?keep=1", ts.https_port);
	g_assert_cmpstr(h->referer, ==, expected);

	/* The page sees GPC; prefetch and ping never reach the server. */
	gpc = gsurf_webkit_test_eval(sc.native, "String(navigator.globalPrivacyControl)");
	g_assert_cmpstr(gpc, ==, "true");
	click_ping(&sc);
	g_assert_null(hit("/prefetch-hard.txt"));
	g_assert_null(hit("/ping-hard"));

	/* --- https-only redirects --- */
	/* A redirect to a different http:// URI is upgraded and followed. */
	g_clear_pointer(&uri, g_free);
	uri = g_strdup_printf("https://127.0.0.1:%u/moved", ts.https_port);
	load_and_settle(&sc, uri);
	h = hit("/target.html");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->scheme, ==, "https");
	/* A redirect to its own http:// twin is a loop: refused at once. */
	g_clear_pointer(&uri, g_free);
	uri = g_strdup_printf("https://127.0.0.1:%u/down", ts.https_port);
	load_and_settle(&sc, uri);
	h = hit("/down");
	g_assert_nonnull(h);
	g_assert_cmpuint(h->count, ==, 1);

	/* --- exempt top-level host: every skipped feature untouched --- */
	g_clear_pointer(&uri, g_free);
	uri = g_strdup_printf("http://localhost:%u/exempt.html?utm_source=x&keep=1", ts.http_port);
	load_and_settle(&sc, uri);
	h = hit("/exempt.html");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->scheme, ==, "http");
	g_assert_cmpstr(h->query, ==, "utm_source=x&keep=1");
	g_assert_null(h->gpc);
	g_assert_null(h->dnt);

	/* The subresource goes to 127.0.0.1 (a hardened host on its own),
	 * but its page is exempt: full referer, params, no GPC/DNT. */
	h = hit("/cross-exempt.gif");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->scheme, ==, "http");
	g_assert_cmpstr(h->query, ==, "fbclid=1&k=2");
	g_assert_cmpstr(h->referer, ==, uri);
	g_assert_null(h->gpc);
	g_assert_null(h->dnt);
	g_clear_pointer(&gpc, g_free);
	/* Recent WebKit has a native getter that answers false; older
	 * releases have none. Either way, not hardening's true. */
	gpc = gsurf_webkit_test_eval(sc.native, "String(navigator.globalPrivacyControl)");
	g_assert_cmpstr(gpc, !=, "true");
	/* The exempt page's ping does go out, which proves the hardened
	 * page's ping was cancelled rather than never sent. (WebKitGTK does
	 * not implement <link rel=prefetch>, so neither page prefetches.) */
	click_ping(&sc);
	g_assert_nonnull(hit("/ping-exempt"));

	scenario_stop(&sc);
}

/* Built-in local-host handling: without https_only_local, loopback is
 * never upgraded (an upgrade to a host without TLS just breaks it). Also
 * the strip_cross_origin Referer policy. */
static void
run_local_scenario(void)
{
	Scenario sc = { 0 };
	g_autofree gchar *uri = NULL;
	Hit *h;

	if (!gsurf_webkit_test_init() ||
	    !scenario_start(&sc, "modules:\n  hardening:\n    enabled: true\n"
	                         "    referer: strip_cross_origin\n    itp: false\n"))
		return;

	uri = g_strdup_printf("http://127.0.0.1:%u/plain.txt?utm_source=x", ts.http_port);
	load_and_settle(&sc, uri);
	h = hit("/plain.txt");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->scheme, ==, "http");
	/* Defaults still strip utm_* and send GPC; DNT follows the core
	 * network.do_not_track, which defaults to off. */
	g_assert_null(h->query);
	g_assert_cmpstr(h->gpc, ==, "1");
	g_assert_null(h->dnt);

	g_clear_pointer(&uri, g_free);
	uri = g_strdup_printf("http://localhost:%u/plain-localhost.txt", ts.http_port);
	load_and_settle(&sc, uri);
	h = hit("/plain-localhost.txt");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->scheme, ==, "http");

	/* strip_cross_origin: no Referer to the other origin, the full one
	 * to the page's own. */
	g_clear_pointer(&uri, g_free);
	uri = g_strdup_printf("http://127.0.0.1:%u/strip.html", ts.http_port);
	load_and_settle(&sc, uri);
	h = hit("/cross-strip.gif");
	g_assert_nonnull(h);
	g_assert_null(h->referer);
	h = hit("/same-strip.gif");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->referer, ==, uri);

	scenario_stop(&sc);
}

/* A disabled module registers nothing: the server sees the page as sent. */
static void
run_disabled_scenario(void)
{
	Scenario sc = { 0 };
	g_autofree gchar *uri = NULL;
	Hit *h;

	if (!gsurf_webkit_test_init() ||
	    !scenario_start(&sc, "modules:\n  hardening:\n    enabled: false\n"))
		return;

	uri = g_strdup_printf("http://127.0.0.1:%u/plain.txt?utm_source=x", ts.http_port);
	load_and_settle(&sc, uri);
	h = hit("/plain.txt");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->query, ==, "utm_source=x");
	g_assert_null(h->gpc);

	scenario_stop(&sc);
}

/* Run @scenario in a fresh process: the extension's policy is per web
 * process and the web process is per gsurf process. The child is this
 * binary again, running only this test path with GSURF_HARDENING_CHILD
 * set. (Not g_test_trap_subprocess(): under --GTestSubprocess GTK4's
 * Wayland display open aborts.) */
static void
in_subprocess(gconstpointer data)
{
	void (*scenario)(void) = data;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *self = NULL;

	if (g_getenv("GSURF_TEST_GUI") == NULL) {
		g_test_skip("set GSURF_TEST_GUI=1 (make test-gui) to run WebKit tests");
		return;
	}
	if (g_getenv("GSURF_HARDENING_CHILD") != NULL) {
		scenario();
		return;
	}
	self = g_file_read_link("/proc/self/exe", &error);
	g_assert_no_error(error);
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
	g_subprocess_launcher_setenv(launcher, "GSURF_HARDENING_CHILD", "1", TRUE);
	child = g_subprocess_launcher_spawn(launcher, &error, self, "-p", g_test_get_path(), NULL);
	g_assert_no_error(error);
	/* Every wait inside the child is bounded, so this returns. */
	g_assert_true(g_subprocess_wait_check(child, NULL, &error));
	g_assert_no_error(error);
}

int
main(int argc, char *argv[])
{
	/* WebKit derives its data directory from the program name; the child
	 * is spawned by absolute path, which would make that a bogus path. */
	g_set_prgname("test-hardening-webkit");
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/gsurf/hardening/webkit/enforced-and-exempt",
		run_main_scenario, in_subprocess);
	g_test_add_data_func("/gsurf/hardening/webkit/local-not-upgraded",
		run_local_scenario, in_subprocess);
	g_test_add_data_func("/gsurf/hardening/webkit/disabled",
		run_disabled_scenario, in_subprocess);
	return g_test_run();
}
