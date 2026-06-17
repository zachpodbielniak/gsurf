/*
 * test-uri.c - URI normalization (auto https:// for bare hosts)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <glib.h>

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
	g_assert_cmpstr(empty, ==, "");
	g_assert_null(gsurf_view_normalize_uri(NULL));
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
	return g_test_run();
}
