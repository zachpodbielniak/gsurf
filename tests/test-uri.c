/*
 * test-uri.c - URI normalization (auto https:// for bare hosts)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <glib.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* file URI for a filesystem path, matching GIO's encoding. */
static gchar *
uri_for_path(const gchar *path)
{
	g_autoptr(GFile) file = g_file_new_for_path(path);

	return g_file_get_uri(file);
}

/* A bare host (or host:port / host/path / IPv4) gains an https:// scheme. */
static void
test_bare_host_gets_https(void)
{
	struct { const char *in; const char *out; } cases[] = {
		{ "duckduckgo.com",            "https://duckduckgo.com" },
		{ "example.com/path?q=1",      "https://example.com/path?q=1" },
		{ "example.com:8080",          "https://example.com:8080" },
		{ "example.com:8080/x",        "https://example.com:8080/x" },
		{ "sub.domain.co.uk",          "https://sub.domain.co.uk" },
		{ "192.168.0.1",               "https://192.168.0.1" },
		{ "192.168.0.1:3000/admin",    "https://192.168.0.1:3000/admin" },
		{ "localhost",                 "https://localhost" },
		{ "localhost:8080",            "https://localhost:8080" },
		{ "localhost/app",             "https://localhost/app" },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *got = gsurf_view_normalize_uri(cases[i].in);
		g_assert_cmpstr(got, ==, cases[i].out);
	}
}

/* Inputs that already carry a scheme are returned unchanged. */
static void
test_existing_scheme_unchanged(void)
{
	const char *passthrough[] = {
		"https://duckduckgo.com",
		"http://example.com",
		"about:blank",
		"about:config",
		"file:///etc/hosts",
		"data:text/html,<h1>hi</h1>",
		"mailto:someone@example.com",
		"javascript:void(0)",
		"view-source:https://example.com",
		"blob:https://example.com/uuid",
		"ftp://files.example.com",
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(passthrough); i++) {
		g_autofree gchar *got = gsurf_view_normalize_uri(passthrough[i]);
		g_assert_cmpstr(got, ==, passthrough[i]);
	}
}

/* Things that don't look like a host (search phrases, bare words) are left
 * alone so a search-engine handler can deal with them. */
static void
test_non_host_unchanged(void)
{
	const char *passthrough[] = {
		"how to cook pasta",   /* spaces -> search phrase */
		"hello",               /* single word, no dot */
		"foo bar.baz",         /* has a dot but also a space */
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(passthrough); i++) {
		g_autofree gchar *got = gsurf_view_normalize_uri(passthrough[i]);
		g_assert_cmpstr(got, ==, passthrough[i]);
	}
}

/* Leading/trailing whitespace is trimmed before the decision + prefix. */
static void
test_whitespace_trimmed(void)
{
	g_autofree gchar *a = gsurf_view_normalize_uri("  duckduckgo.com  ");
	g_autofree gchar *b = gsurf_view_normalize_uri("\thttps://example.com\n");
	g_assert_cmpstr(a, ==, "https://duckduckgo.com");
	g_assert_cmpstr(b, ==, "https://example.com");
}

/* Empty + NULL edge cases. */
static void
test_empty_and_null(void)
{
	g_autofree gchar *empty = gsurf_view_normalize_uri("");
	g_autofree gchar *blank = gsurf_view_normalize_uri(" \t\n");
	g_assert_cmpstr(empty, ==, "");
	g_assert_cmpstr(blank, ==, "");
	g_assert_null(gsurf_view_normalize_uri(NULL));
}

/* Absolute, relative, home, and existing names become file URIs. Hosts that
 * merely contain a slash stay https. Missing files are still file URIs when
 * the input is unambiguously a path. */
static void
test_filesystem_paths(void)
{
	g_autofree gchar *previous = g_get_current_dir();
	g_autofree gchar *dir = g_dir_make_tmp("gsurf-uri-XXXXXX", NULL);
	g_autofree gchar *home = uri_for_path(g_get_home_dir());
	g_autofree gchar *home_foo_path = g_build_filename(g_get_home_dir(), "foo", NULL);
	g_autofree gchar *home_foo = NULL;
	g_autofree gchar *user_foo = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *page_uri = NULL;
	g_autofree gchar *spaced = NULL;
	g_autofree gchar *spaced_uri = NULL;
	g_autofree gchar *nested = NULL;
	g_autofree gchar *nested_uri = NULL;
	g_autofree gchar *dot = NULL;
	g_autofree gchar *dotdot = NULL;
	g_autofree gchar *relative = NULL;
	g_autofree gchar *missing = NULL;
	g_autofree gchar *missing_uri = NULL;
	g_autofree gchar *parent = NULL;
	g_autofree gchar *parent_uri = NULL;
	g_autofree gchar *unicode_path = NULL;
	g_autofree gchar *unicode_uri = NULL;
	g_autofree gchar *makefile_path = NULL;
	g_autofree gchar *got = NULL;
	g_autofree gchar *again = NULL;
	struct passwd *pw;
	const gchar *inputs[] = {
		"/etc/hosts",
		"/tmp/../etc/hosts",
		"file:///etc/hosts",
		"FILE:///etc/hosts",
		"file:/etc/hosts",
		"file://localhost/etc/hosts",
		"  /etc/hosts\n",
	};
	gsize i;

	home_foo = uri_for_path(home_foo_path);
	g_assert_cmpint(chdir(dir), ==, 0);
	g_assert_true(g_file_set_contents("index.html", "<h1>local</h1>\n", -1, NULL));
	g_assert_true(g_file_set_contents("Makefile", "all:\n", -1, NULL));
	page = g_build_filename(dir, "index.html", NULL);
	page_uri = uri_for_path(page);
	spaced = g_build_filename(dir, "my file.html", NULL);
	g_assert_true(g_file_set_contents(spaced, "<p>space</p>\n", -1, NULL));
	spaced_uri = uri_for_path(spaced);
	unicode_path = g_build_filename(dir, "caf\xc3\xa9.html", NULL);
	g_assert_true(g_file_set_contents(unicode_path, "<p>unicode</p>\n", -1, NULL));
	unicode_uri = uri_for_path(unicode_path);
	nested = g_build_filename(dir, "docs", "readme.org", NULL);
	nested_uri = uri_for_path(nested);
	missing = g_build_filename(dir, "docs", "missing.html", NULL);
	missing_uri = uri_for_path(missing);
	parent = g_build_filename(dir, "..", "outside.html", NULL);
	parent_uri = uri_for_path(parent);

	for (i = 0; i < G_N_ELEMENTS(inputs); i++) {
		g_clear_pointer(&got, g_free);
		got = gsurf_view_normalize_uri(inputs[i]);
		g_assert_cmpstr(got, ==, "file:///etc/hosts");
		g_clear_pointer(&again, g_free);
		again = gsurf_view_normalize_uri(got);
		g_assert_cmpstr(again, ==, got);
	}

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("/tmp/");
	g_assert_cmpstr(got, ==, "file:///tmp");

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri(page);
	g_assert_cmpstr(got, ==, page_uri);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("./index.html");
	g_assert_cmpstr(got, ==, page_uri);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("index.html");
	g_assert_cmpstr(got, ==, page_uri);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("Makefile");
	makefile_path = g_build_filename(dir, "Makefile", NULL);
	dot = uri_for_path(makefile_path);
	g_assert_cmpstr(got, ==, dot);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri(".");
	dotdot = uri_for_path(dir);
	g_assert_cmpstr(got, ==, dotdot);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("docs/readme.org");
	g_assert_cmpstr(got, ==, nested_uri);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("./docs/missing.html");
	g_assert_cmpstr(got, ==, missing_uri);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("docs/missing.html");
	g_assert_cmpstr(got, ==, missing_uri);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("../outside.html");
	g_assert_cmpstr(got, ==, parent_uri);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri(spaced);
	g_assert_cmpstr(got, ==, spaced_uri);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri(unicode_path);
	g_assert_cmpstr(got, ==, unicode_uri);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("~/foo");
	g_assert_cmpstr(got, ==, home_foo);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("~");
	g_assert_cmpstr(got, ==, home);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("~/");
	g_assert_cmpstr(got, ==, home);

	pw = getpwuid(getuid());
	g_assert_nonnull(pw);
	relative = g_strconcat("~", pw->pw_name, "/foo", NULL);
	user_foo = uri_for_path(g_build_filename(pw->pw_dir, "foo", NULL));
	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri(relative);
	g_assert_cmpstr(got, ==, user_foo);

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("~nosuchuser-gsurf-zz/file");
	g_assert_cmpstr(got, ==, "~nosuchuser-gsurf-zz/file");

	/* A slash after a dotted authority is still a web address. */
	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("example.com/path?q=1");
	g_assert_cmpstr(got, ==, "https://example.com/path?q=1");

	g_clear_pointer(&got, g_free);
	got = gsurf_view_normalize_uri("how to cook pasta/noodles");
	g_assert_cmpstr(got, ==, "how to cook pasta/noodles");

	g_assert_cmpint(chdir(previous), ==, 0);
}

/* file: queries, fragments, spaces, and non-local hosts. */
static void
test_file_uri_forms(void)
{
	g_autofree gchar *space = gsurf_view_normalize_uri("file:///tmp/my file.html");
	g_autofree gchar *encoded = gsurf_view_normalize_uri("file:///tmp/my%20file.html");
	g_autofree gchar *fragment = gsurf_view_normalize_uri("file:///tmp/x.html#section");
	g_autofree gchar *query = gsurf_view_normalize_uri("file:///tmp/x.html?q=a b#c");
	g_autofree gchar *both = gsurf_view_normalize_uri("FILE:///tmp/x.html?q=1&b=2#frag");
	g_autofree gchar *remote = gsurf_view_normalize_uri("file://example.com/tmp/x");
	g_autofree gchar *loopback = gsurf_view_normalize_uri("file://127.0.0.1/tmp/x");
	g_autofree gchar *relative = gsurf_view_normalize_uri("file:foo/bar");
	g_autofree gchar *dots = gsurf_view_normalize_uri("file:///tmp/../etc/hosts#section");
	g_autofree gchar *again = NULL;

	g_assert_cmpstr(space, ==, "file:///tmp/my%20file.html");
	g_assert_cmpstr(encoded, ==, "file:///tmp/my%20file.html");
	g_assert_cmpstr(fragment, ==, "file:///tmp/x.html#section");
	g_assert_cmpstr(query, ==, "file:///tmp/x.html?q=a%20b#c");
	g_assert_cmpstr(both, ==, "file:///tmp/x.html?q=1&b=2#frag");
	g_assert_cmpstr(dots, ==, "file:///etc/hosts#section");
	g_assert_true(g_str_has_prefix(remote, "file://example.com/"));
	g_assert_true(strstr(loopback, "127.0.0.1") != NULL);
	g_assert_true(g_str_has_prefix(relative, "file:"));
	g_assert_null(strstr(relative, "file:///"));

	again = gsurf_view_normalize_uri(query);
	g_assert_cmpstr(again, ==, query);
}

/* Other schemes stay byte-for-byte, apart from surrounding whitespace. */
static void
test_other_schemes(void)
{
	const gchar *passthrough[] = {
		"view-source:file:///etc/hosts",
		"view-source:https://example.com/a",
		"resource:///org/gtk/libgtk/theme.css",
		"gopher://gopher.floodgap.com/1/",
		"gemini://geminiprotocol.net/",
		"ftp://files.example.com/pub/",
		"magnet:?xt=urn:btih:abc",
		"blob:https://example.com/uuid",
		"about:blank",
		"ABOUT:blank",
		"data:text/html,<h1>hi</h1>",
		"mailto:someone@example.com",
		"javascript:void(0)",
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(passthrough); i++) {
		g_autofree gchar *got = gsurf_view_normalize_uri(passthrough[i]);
		g_autofree gchar *again = gsurf_view_normalize_uri(got);
		g_assert_cmpstr(got, ==, passthrough[i]);
		g_assert_cmpstr(again, ==, got);
	}
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/uri/bare-host-gets-https", test_bare_host_gets_https);
	g_test_add_func("/gsurf/uri/existing-scheme-unchanged",
	                test_existing_scheme_unchanged);
	g_test_add_func("/gsurf/uri/non-host-unchanged", test_non_host_unchanged);
	g_test_add_func("/gsurf/uri/whitespace-trimmed", test_whitespace_trimmed);
	g_test_add_func("/gsurf/uri/empty-and-null", test_empty_and_null);
	g_test_add_func("/gsurf/uri/filesystem-paths", test_filesystem_paths);
	g_test_add_func("/gsurf/uri/file-forms", test_file_uri_forms);
	g_test_add_func("/gsurf/uri/other-schemes", test_other_schemes);
	return g_test_run();
}
