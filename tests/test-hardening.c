/*
 * test-hardening.c - hardening module: shared rules, config, extension data
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Headless checks of the hardening module:
 *   - the pure decision logic shared with the web-process extension
 *     (modules/hardening/gsurf-hardening-rules.c, compiled in here):
 *     parameter stripping, Referer trimming, the https upgrade decision,
 *     local hosts, speculative requests, exemptions and the policy
 *     #GVariant;
 *   - the real hardening.so: config parsing, defaults, validation
 *     warnings, reconfigure, the #GVariant it registers for the
 *     extension, the container-registry session policy (through a fake
 *     session backend), the setting provider, and finalization.
 * The end-to-end behaviour in WebKit is test-hardening-webkit (make
 * test-gui).
 */

#include <gsurf.h>
#include <yaml-glib.h>
#include <string.h>

#include "test-util.h"
#include "../modules/hardening/gsurf-hardening-rules.c"

/* ---------------------------------------------------------------------
 * Shared rules
 * ------------------------------------------------------------------- */

/* Feature and Referer-policy names map both ways; unknown names are 0. */
static void
test_rules_names(void)
{
	static const gchar *const names[] = {
		"https_only", "strip_params", "referer", "gpc", "dnt", "no_prefetch", "webrtc", NULL
	};
	GsurfHardeningReferer policy = GSURF_HARDENING_REFERER_OFF;
	guint i, seen = 0;

	for (i = 0; names[i] != NULL; i++) {
		guint feature = gsurf_hardening_feature_from_name(names[i]);

		g_assert_cmpuint(feature, !=, 0);
		g_assert_cmpuint(seen & feature, ==, 0);   /* one distinct bit each */
		seen |= feature;
		g_assert_cmpstr(gsurf_hardening_feature_to_name(feature), ==, names[i]);
	}
	g_assert_cmpuint(seen, ==, GSURF_HARDENING_ALL);
	g_assert_cmpuint(gsurf_hardening_feature_from_name("all"), ==, GSURF_HARDENING_ALL);
	g_assert_cmpuint(gsurf_hardening_feature_from_name("HTTPS_ONLY"), ==,
		GSURF_HARDENING_HTTPS_ONLY);
	g_assert_cmpuint(gsurf_hardening_feature_from_name("itp"), ==, 0);
	g_assert_cmpuint(gsurf_hardening_feature_from_name("bogus"), ==, 0);
	g_assert_cmpuint(gsurf_hardening_feature_from_name(NULL), ==, 0);
	g_assert_null(gsurf_hardening_feature_to_name(GSURF_HARDENING_ALL));

	g_assert_true(gsurf_hardening_referer_from_name("strip_cross_origin", &policy));
	g_assert_cmpint(policy, ==, GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN);
	g_assert_true(gsurf_hardening_referer_from_name("trim_cross_origin", &policy));
	g_assert_cmpint(policy, ==, GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN);
	g_assert_true(gsurf_hardening_referer_from_name("off", &policy));
	g_assert_cmpint(policy, ==, GSURF_HARDENING_REFERER_OFF);
	g_assert_false(gsurf_hardening_referer_from_name("trim", &policy));
	g_assert_false(gsurf_hardening_referer_from_name(NULL, &policy));
	g_assert_cmpint(policy, ==, GSURF_HARDENING_REFERER_OFF);   /* untouched */
	for (i = GSURF_HARDENING_REFERER_OFF; i <= GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN; i++) {
		g_assert_true(gsurf_hardening_referer_from_name(
			gsurf_hardening_referer_to_name(i), &policy));
		g_assert_cmpint(policy, ==, i);
	}
}

/* Which hosts count as local (never upgraded by default). */
static void
test_rules_local_hosts(void)
{
	static const gchar *const local[] = {
		"localhost", "LocalHost", "app.localhost", "router", "nas.local", "pi.lan",
		"svc.internal", "box.home.arpa", "printer.lan.", "127.0.0.1", "127.8.9.10",
		"10.1.2.3", "172.16.0.1", "172.31.255.254", "192.168.1.1", "169.254.1.1",
		"100.64.0.1", "0.0.0.0", "::1", "[::1]", "fe80::1", "fd12:3456::1", "fc00::1",
		"::", "", NULL
	};
	static const gchar *const remote[] = {
		"example.com", "www.example.com", "localhost.example.com", "local.example",
		"8.8.8.8", "172.32.0.1", "192.169.0.1", "100.128.0.1", "2001:db8::1",
		"[2606:4700::1111]", NULL
	};
	guint i;

	for (i = 0; local[i] != NULL; i++)
		g_assert_true(gsurf_hardening_host_is_local(local[i]));
	for (i = 0; remote[i] != NULL; i++)
		g_assert_false(gsurf_hardening_host_is_local(remote[i]));
	g_assert_true(gsurf_hardening_host_is_local(NULL));
}

/* The https upgrade decision. */
static void
test_rules_upgrade(void)
{
	static const struct {
		const gchar *in;
		gboolean     local;
		const gchar *out;   /* NULL = left alone */
	} cases[] = {
		{ "http://example.com/", FALSE, "https://example.com/" },
		{ "http://example.com", FALSE, "https://example.com" },
		{ "HTTP://Example.com/a?b=c#d", FALSE, "https://Example.com/a?b=c#d" },
		{ "http://example.com:80/x", FALSE, "https://example.com/x" },
		{ "http://example.com:8080/x", FALSE, "https://example.com:8080/x" },
		{ "http://user@example.com/p%20q?x=%2F", FALSE, "https://user@example.com/p%20q?x=%2F" },
		{ "http://[2001:db8::1]/", FALSE, "https://[2001:db8::1]/" },
		{ "https://example.com/", FALSE, NULL },
		{ "ftp://example.com/", FALSE, NULL },
		{ "about:blank", FALSE, NULL },
		{ "not a uri", FALSE, NULL },
		{ "", FALSE, NULL },
		/* Local hosts only with upgrade_local. */
		{ "http://localhost:8080/", FALSE, NULL },
		{ "http://127.0.0.1/", FALSE, NULL },
		{ "http://192.168.0.10/admin", FALSE, NULL },
		{ "http://[::1]:3000/", FALSE, NULL },
		{ "http://127.0.0.1:4443/", TRUE, "https://127.0.0.1:4443/" },
		{ "http://nas.local/", TRUE, "https://nas.local/" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *out = gsurf_hardening_upgrade_uri(cases[i].in, cases[i].local);

		g_test_message("upgrade %s (local %d)", cases[i].in, cases[i].local);
		g_assert_cmpstr(out, ==, cases[i].out);
	}
	g_assert_null(gsurf_hardening_upgrade_uri(NULL, TRUE));
}

/* Only a redirect that would be upgraded straight back is a loop. */
static void
test_rules_downgrade(void)
{
	g_assert_true(gsurf_hardening_is_downgrade("https://example.com/x", "http://example.com/x"));
	g_assert_true(gsurf_hardening_is_downgrade("https://example.com/x?a=1",
		"http://example.com:80/x?a=1"));
	g_assert_true(gsurf_hardening_is_downgrade("https://127.0.0.1:4443/d",
		"http://127.0.0.1:4443/d"));
	/* A different target is simply upgraded. */
	g_assert_false(gsurf_hardening_is_downgrade("https://example.com/x", "http://example.com/y"));
	g_assert_false(gsurf_hardening_is_downgrade("https://example.com/", "http://example.org/"));
	/* Not from https, or not to http. */
	g_assert_false(gsurf_hardening_is_downgrade("http://example.com/x", "http://example.com/x"));
	g_assert_false(gsurf_hardening_is_downgrade("https://example.com/x", "https://example.com/x"));
	g_assert_false(gsurf_hardening_is_downgrade(NULL, "http://example.com/x"));
	g_assert_false(gsurf_hardening_is_downgrade("https://example.com/x", "garbage"));
}

/* Glob parameter stripping keeps everything else byte-for-byte. */
static void
test_rules_strip_params(void)
{
	static const gchar *const globs[] = { "utm_*", "fbclid", "gclid", "_hsenc", "x?y", NULL };
	static const struct {
		const gchar *in;
		const gchar *out;   /* NULL = unchanged */
	} cases[] = {
		{ "https://e.com/p?utm_source=a&keep=1&utm_medium=b#frag", "https://e.com/p?keep=1#frag" },
		{ "https://e.com/p?keep=1&fbclid=X", "https://e.com/p?keep=1" },
		{ "https://e.com/p?fbclid=X", "https://e.com/p" },
		{ "https://e.com/p?fbclid=X#top", "https://e.com/p#top" },
		{ "https://e.com/p?a=1&b=2", NULL },
		{ "https://e.com/p", NULL },
		{ "https://e.com/p?", NULL },
		{ "https://e.com/p#utm_source=fragment-is-not-a-query", NULL },
		{ "https://e.com/p?a=%20x&UTM_Campaign=1&b=%2F", "https://e.com/p?a=%20x&b=%2F" },
		{ "https://e.com/p?utm%5Fsource=1&k", "https://e.com/p?k" },          /* encoded name */
		{ "https://e.com/p?gclid&keep", "https://e.com/p?keep" },             /* valueless */
		{ "https://e.com/p?a=1&&fbclid=2&b=3", "https://e.com/p?a=1&&b=3" },  /* empty kept */
		{ "https://e.com/p?xzy=1&xy=2", "https://e.com/p?xy=2" },             /* ? wildcard */
		{ "https://e.com/p?_hsenc=1&gclidx=2", "https://e.com/p?gclidx=2" },  /* exact name */
		{ "https://e.com/p?q=fbclid", NULL },                                 /* value only */
	};
	static const gchar *const empty[] = { NULL };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *out = gsurf_hardening_strip_params(cases[i].in, globs);

		g_test_message("strip %s", cases[i].in);
		g_assert_cmpstr(out, ==, cases[i].out);
	}
	g_assert_null(gsurf_hardening_strip_params("https://e.com/?fbclid=1", NULL));
	g_assert_null(gsurf_hardening_strip_params("https://e.com/?fbclid=1", empty));
	g_assert_null(gsurf_hardening_strip_params(NULL, globs));
}

/* Referer policy per request. */
static void
test_rules_referer(void)
{
	const gchar *page = "https://shop.example/cart?id=42#pay";
	gchar *value = NULL;

	/* Same origin: kept, whatever the policy. */
	g_assert_cmpint(gsurf_hardening_referer_rewrite(GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN,
		page, "https://shop.example:443/img.png", &value), ==, GSURF_HARDENING_REFERER_KEEP);
	g_assert_null(value);

	/* Cross origin (other host, other scheme, other port). */
	g_assert_cmpint(gsurf_hardening_referer_rewrite(GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN,
		page, "https://cdn.example/x.js", &value), ==, GSURF_HARDENING_REFERER_REPLACE);
	g_assert_cmpstr(value, ==, "https://shop.example/");
	g_clear_pointer(&value, g_free);
	g_assert_cmpint(gsurf_hardening_referer_rewrite(GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN,
		page, "http://shop.example/x", &value), ==, GSURF_HARDENING_REFERER_REPLACE);
	g_clear_pointer(&value, g_free);
	g_assert_cmpint(gsurf_hardening_referer_rewrite(GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN,
		"http://127.0.0.1:8080/a/b", "http://127.0.0.1:9090/", &value), ==,
		GSURF_HARDENING_REFERER_REPLACE);
	g_assert_cmpstr(value, ==, "http://127.0.0.1:8080/");
	g_clear_pointer(&value, g_free);
	g_assert_cmpint(gsurf_hardening_referer_rewrite(GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN,
		page, "https://cdn.example/x.js", &value), ==, GSURF_HARDENING_REFERER_REMOVE);
	g_assert_null(value);

	/* Already an origin: nothing to trim. */
	g_assert_cmpint(gsurf_hardening_referer_rewrite(GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN,
		"https://shop.example/", "https://cdn.example/", &value), ==, GSURF_HARDENING_REFERER_KEEP);

	/* Off, no header, junk header. */
	g_assert_cmpint(gsurf_hardening_referer_rewrite(GSURF_HARDENING_REFERER_OFF,
		page, "https://cdn.example/", &value), ==, GSURF_HARDENING_REFERER_KEEP);
	g_assert_cmpint(gsurf_hardening_referer_rewrite(GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN,
		NULL, "https://cdn.example/", &value), ==, GSURF_HARDENING_REFERER_KEEP);
	g_assert_cmpint(gsurf_hardening_referer_rewrite(GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN,
		"", "https://cdn.example/", &value), ==, GSURF_HARDENING_REFERER_KEEP);
	g_assert_cmpint(gsurf_hardening_referer_rewrite(GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN,
		"not a uri", "https://cdn.example/", &value), ==, GSURF_HARDENING_REFERER_REMOVE);
	g_assert_null(value);

	/* Origins: brackets for IPv6, default ports dropped, case folded. */
	{
		g_autofree gchar *a = gsurf_hardening_origin_of("http://[::1]:8000/x?y");
		g_autofree gchar *b = gsurf_hardening_origin_of("HTTPS://Shop.Example:443/x");
		g_autofree gchar *c = gsurf_hardening_origin_of("http://e.com:80/");
		g_autofree gchar *d = gsurf_hardening_origin_of("about:blank");

		g_assert_cmpstr(a, ==, "http://[::1]:8000/");
		g_assert_cmpstr(b, ==, "https://shop.example/");
		g_assert_cmpstr(c, ==, "http://e.com/");
		g_assert_null(d);
	}
	g_assert_true(gsurf_hardening_same_origin("http://E.com/a", "http://e.com:80/b"));
	g_assert_false(gsurf_hardening_same_origin("http://e.com/", "http://e.com:81/"));
	g_assert_false(gsurf_hardening_same_origin(NULL, "http://e.com/"));
}

/* Prefetches and pings are speculative; ordinary requests are not. */
static void
test_rules_speculative(void)
{
	g_assert_true(gsurf_hardening_is_speculative("prefetch", NULL, NULL, NULL));
	g_assert_true(gsurf_hardening_is_speculative("Prefetch", NULL, NULL, NULL));
	g_assert_true(gsurf_hardening_is_speculative(NULL, "prefetch;prerender", NULL, NULL));
	g_assert_true(gsurf_hardening_is_speculative(NULL, NULL, "https://e.com/", NULL));
	g_assert_true(gsurf_hardening_is_speculative(NULL, NULL, NULL, "text/ping"));
	g_assert_true(gsurf_hardening_is_speculative(NULL, NULL, NULL, " Text/Ping; charset=x"));
	g_assert_false(gsurf_hardening_is_speculative(NULL, NULL, NULL, NULL));
	g_assert_false(gsurf_hardening_is_speculative("navigate", "", NULL, "text/html"));
}

/* Build `a(sa{sv})` exemption rules from YAML via GsurfHostRules, the way
 * the module does. */
static GVariant *
rules_variant(const gchar *yaml)
{
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(GsurfHostRules) rules = gsurf_host_rules_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(yaml_parser_load_from_data(parser, yaml, -1, &error));
	g_assert_no_error(error);
	g_assert_true(gsurf_host_rules_load(rules, yaml_parser_get_root(parser), "match", &error));
	g_assert_no_error(error);
	return g_variant_ref_sink(gsurf_host_rules_to_variant(rules));
}

/* Exemptions: union of matching rules, a rule without skip exempts
 * everything, regexes are caseless and unanchored. */
static void
test_rules_exemptions(void)
{
	g_autoptr(GVariant) rules = rules_variant(
		"- match: '^(localhost|127\\.0\\.0\\.1|.*\\.lan)$'\n"
		"  skip: [https_only]\n"
		"- match: '(^|\\.)mybank\\.example$'\n"
		"  skip: [referer, strip_params]\n"
		"- match: 'bank'\n"
		"  skip: gpc\n"
		"- match: '^everything\\.example$'\n"
		"- '^bare\\.example$'\n"
		"- match: '^nothing\\.example$'\n"
		"  skip: []\n");
	g_autoptr(GsurfHardeningExemptions) ex = gsurf_hardening_exemptions_new(rules);
	g_autoptr(GsurfHardeningExemptions) none = gsurf_hardening_exemptions_new(NULL);
	g_autoptr(GVariant) not_rules = g_variant_ref_sink(g_variant_new_string("not rules"));
	g_autoptr(GsurfHardeningExemptions) wrong = gsurf_hardening_exemptions_new(not_rules);

	g_assert_cmpuint(gsurf_hardening_exemptions_get_size(ex), ==, 6);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(ex, "localhost"), ==,
		GSURF_HARDENING_HTTPS_ONLY);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(ex, "NAS.LAN"), ==,
		GSURF_HARDENING_HTTPS_ONLY);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(ex, "www.mybank.example"), ==,
		GSURF_HARDENING_REFERER | GSURF_HARDENING_STRIP_PARAMS | GSURF_HARDENING_GPC);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(ex, "notmybank.example"), ==,
		GSURF_HARDENING_GPC);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(ex, "everything.example"), ==,
		GSURF_HARDENING_ALL);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(ex, "bare.example"), ==,
		GSURF_HARDENING_ALL);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(ex, "nothing.example"), ==, 0);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(ex, "example.com"), ==, 0);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(ex, NULL), ==, 0);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(ex, ""), ==, 0);
	g_assert_cmpuint(gsurf_hardening_exemptions_get_size(none), ==, 0);
	g_assert_cmpuint(gsurf_hardening_exemptions_get_size(wrong), ==, 0);

	/* A pattern that does not compile is dropped, the rest kept. */
	{
		GVariantBuilder b;
		g_autoptr(GVariant) bad = NULL;
		g_autoptr(GsurfHardeningExemptions) partial = NULL;

		g_variant_builder_init(&b, G_VARIANT_TYPE("a(sa{sv})"));
		g_variant_builder_add(&b, "(s@a{sv})", "([", g_variant_new_array(
			G_VARIANT_TYPE("{sv}"), NULL, 0));
		g_variant_builder_add(&b, "(s@a{sv})", "^ok$", g_variant_new_array(
			G_VARIANT_TYPE("{sv}"), NULL, 0));
		bad = g_variant_ref_sink(g_variant_builder_end(&b));
		partial = gsurf_hardening_exemptions_new(bad);
		g_assert_cmpuint(gsurf_hardening_exemptions_get_size(partial), ==, 1);
		g_assert_cmpuint(gsurf_hardening_exemptions_lookup(partial, "ok"), ==,
			GSURF_HARDENING_ALL);
	}
}

/* `skip:` validation reports unknown and session-wide names. */
static void
test_rules_skip_mask(void)
{
	g_autoptr(GPtrArray) unknown = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) session = g_ptr_array_new_with_free_func(g_free);
	const gchar *names[] = { "dnt", "itp", "bogus", "webrtc", "third_party_cookies", NULL };
	g_autoptr(GVariant) list = g_variant_ref_sink(g_variant_new_strv(names, -1));
	g_autoptr(GVariant) one = g_variant_ref_sink(g_variant_new_string("no_prefetch"));
	g_autoptr(GVariant) blank = g_variant_ref_sink(g_variant_new_string(""));
	g_autoptr(GVariant) number = g_variant_ref_sink(g_variant_new_int64(3));

	g_assert_cmpuint(gsurf_hardening_skip_mask(list, unknown, session), ==,
		GSURF_HARDENING_DNT | GSURF_HARDENING_WEBRTC);
	g_assert_cmpuint(unknown->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(unknown, 0), ==, "bogus");
	g_assert_cmpuint(session->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(session, 0), ==, "itp");
	g_assert_cmpstr(g_ptr_array_index(session, 1), ==, "third_party_cookies");

	g_assert_cmpuint(gsurf_hardening_skip_mask(one, NULL, NULL), ==, GSURF_HARDENING_NO_PREFETCH);
	g_assert_cmpuint(gsurf_hardening_skip_mask(blank, NULL, NULL), ==, 0);
	g_assert_cmpuint(gsurf_hardening_skip_mask(NULL, NULL, NULL), ==, GSURF_HARDENING_ALL);
	g_ptr_array_set_size(unknown, 0);
	g_assert_cmpuint(gsurf_hardening_skip_mask(number, unknown, NULL), ==, 0);
	g_assert_cmpuint(unknown->len, ==, 1);
}

/* The policy survives the trip to the web process unchanged; bad
 * dictionaries leave everything off. */
static void
test_rules_policy_variant(void)
{
	g_autoptr(GsurfHardeningPolicy) policy = gsurf_hardening_policy_new();
	g_autoptr(GsurfHardeningPolicy) copy = NULL;
	g_autoptr(GsurfHardeningPolicy) empty = NULL;
	g_autoptr(GsurfHardeningPolicy) junk = NULL;
	g_autoptr(GVariant) dict = NULL;
	g_autoptr(GVariant) wrong_types = NULL;
	const gchar *params[] = { "utm_*", "fbclid", NULL };

	g_assert_cmpuint(gsurf_hardening_policy_get_features(policy), ==, 0);

	policy->https_only = TRUE;
	policy->https_only_local = TRUE;
	g_strfreev(policy->strip_params);
	policy->strip_params = g_strdupv((gchar **)params);
	policy->referer = GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN;
	policy->gpc = TRUE;
	policy->dnt = TRUE;
	policy->no_prefetch = TRUE;
	policy->webrtc_off = TRUE;
	gsurf_hardening_policy_set_exemptions(policy,
		rules_variant("- match: '^a\\.example$'\n  skip: [gpc, dnt]\n"));
	g_assert_cmpuint(gsurf_hardening_policy_get_features(policy), ==, GSURF_HARDENING_ALL);

	dict = g_variant_ref_sink(gsurf_hardening_policy_to_variant(policy));
	copy = gsurf_hardening_policy_new_from_variant(dict);
	g_assert_true(copy->https_only);
	g_assert_true(copy->https_only_local);
	g_assert_true(g_strv_equal((const gchar * const *)copy->strip_params, params));
	g_assert_cmpint(copy->referer, ==, GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN);
	g_assert_true(copy->gpc && copy->dnt && copy->no_prefetch && copy->webrtc_off);
	g_assert_cmpuint(gsurf_hardening_exemptions_get_size(copy->exempt), ==, 1);
	g_assert_cmpuint(gsurf_hardening_policy_features_for_host(copy, "a.example"), ==,
		GSURF_HARDENING_ALL & ~(GSURF_HARDENING_GPC | GSURF_HARDENING_DNT));
	g_assert_cmpuint(gsurf_hardening_policy_features_for_host(copy, "b.example"), ==,
		GSURF_HARDENING_ALL);
	g_assert_cmpuint(gsurf_hardening_policy_features_for_host(copy, NULL), ==,
		GSURF_HARDENING_ALL);

	/* No dictionary, or every key mistyped: all off. */
	empty = gsurf_hardening_policy_new_from_variant(NULL);
	g_assert_cmpuint(gsurf_hardening_policy_get_features(empty), ==, 0);
	wrong_types = g_variant_ref_sink(g_variant_new_parsed(
		"{'https-only': <'yes'>, 'gpc': <1>, 'strip-params': <'utm_*'>, "
		"'referer': <'sideways'>, 'exempt': <['x']>}"));
	junk = gsurf_hardening_policy_new_from_variant(wrong_types);
	g_assert_cmpuint(gsurf_hardening_policy_get_features(junk), ==, 0);
	g_assert_cmpuint(gsurf_hardening_exemptions_get_size(junk->exempt), ==, 0);

	/* An empty strip list is no stripping. */
	g_strfreev(policy->strip_params);
	policy->strip_params = g_new0(gchar *, 1);
	g_assert_cmpuint(gsurf_hardening_policy_get_features(policy) & GSURF_HARDENING_STRIP_PARAMS,
		==, 0);
}

/* ---------------------------------------------------------------------
 * The module (real hardening.so)
 * ------------------------------------------------------------------- */

/* A fake session backend records what the registry applies. */
static struct {
	guint    policies;
	gboolean itp;
	gint     cookie_policy;
} fake;

static gpointer
fake_create(const gchar *id, const gchar *data_dir, const gchar *cache_dir)
{
	return g_object_new(G_TYPE_OBJECT, NULL);
}

static void
fake_destroy(gpointer native)
{
	g_object_unref(native);
}

static void
fake_apply_policy(gpointer native, gboolean itp, gint cookie_policy)
{
	fake.policies++;
	fake.itp = itp;
	fake.cookie_policy = cookie_policy;
}

static const GsurfSessionBackend fake_backend = {
	fake_create, fake_destroy, fake_apply_policy
};

static gchar *
module_so_path(void)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);

	return g_build_filename(dir, "modules", "hardening.so", NULL);
}

static GsurfConfig *
config_from(const gchar *yaml)
{
	g_autoptr(GError) error = NULL;
	GsurfConfig *config = gsurf_config_new();

	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	return config;
}

/* Load hardening.so into a fresh manager with @yaml (mgr_with() of
 * test-manager.c). The fake backend is installed first so the policy
 * the module applies is observable. NULL (test skipped) if not built. */
static GsurfModuleManager *
mgr_with(const gchar *yaml)
{
	g_autofree gchar *so = module_so_path();
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = NULL;
	GsurfModuleManager *mgr;

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("hardening.so not built");
		return NULL;
	}
	/* Installing the backend re-applies a policy an earlier test left;
	 * count only what this module does from here on. */
	gsurf_container_registry_set_session_backend(gsurf_container_registry_get_default(),
		&fake_backend);
	memset(&fake, 0, sizeof(fake));

	config = config_from(yaml);
	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(mgr);
	return mgr;
}

/* Drop the manager and check the module is gone and let go of the
 * extension and the session policy. */
static void
mgr_free(GsurfModuleManager *mgr)
{
	GsurfModule *module = gsurf_module_manager_get_module(mgr, "hardening");
	g_autoptr(GVariant) data = NULL;

	g_assert_nonnull(module);
	g_object_add_weak_pointer(G_OBJECT(module), (gpointer *)&module);
	g_object_unref(mgr);
	g_assert_null(module);

	data = gsurf_web_process_extension_get_user_data();
	g_assert_false(g_variant_lookup(data, "hardening", "@a{sv}", NULL));
	gsurf_container_registry_set_session_backend(gsurf_container_registry_get_default(), NULL);
}

/* The policy the module registered for the extension, or NULL. */
static GsurfHardeningPolicy *
registered_policy(void)
{
	g_autoptr(GVariant) data = gsurf_web_process_extension_get_user_data();
	g_autoptr(GVariant) mine = g_variant_lookup_value(data, "hardening", G_VARIANT_TYPE_VARDICT);

	return mine != NULL ? gsurf_hardening_policy_new_from_variant(mine) : NULL;
}

/* Run the setting provider for @uri on settings with everything on. */
static GsurfSettings *
settings_for(GsurfModuleManager *mgr, const gchar *uri)
{
	GsurfSettings *s = gsurf_settings_new();

	s->webrtc = TRUE;
	s->dns_prefetch = TRUE;
	s->hyperlink_auditing = TRUE;
	gsurf_module_manager_dispatch_apply_settings(mgr, NULL, s, uri);
	return s;
}

/* `enabled: true` alone gives the LibreWolf-like defaults. */
static void
test_module_defaults(void)
{
	GsurfModuleManager *mgr = mgr_with("modules:\n  hardening:\n    enabled: true\n");
	g_autoptr(GsurfHardeningPolicy) policy = NULL;
	g_autoptr(GsurfSettings) s = NULL;
	g_autofree gchar *dir = NULL;
	const gchar *defaults[] = {
		"utm_*", "fbclid", "gclid", "dclid", "mc_eid", "igshid", "_hsenc", "yclid", "msclkid", NULL
	};

	if (mgr == NULL)
		return;
	policy = registered_policy();
	g_assert_nonnull(policy);
	g_assert_true(policy->https_only);
	g_assert_false(policy->https_only_local);
	g_assert_true(g_strv_equal((const gchar * const *)policy->strip_params, defaults));
	g_assert_cmpint(policy->referer, ==, GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN);
	g_assert_true(policy->gpc);
	g_assert_false(policy->dnt);       /* core network.do_not_track defaults off */
	g_assert_true(policy->no_prefetch);
	g_assert_true(policy->webrtc_off);
	g_assert_cmpuint(gsurf_hardening_exemptions_get_size(policy->exempt), ==, 0);

	/* The extension registered is the one built next to hardening.so. */
	dir = gsurf_web_process_extension_get_directory();
	g_assert_nonnull(dir);
	g_assert_true(g_str_has_suffix(dir, "hardening-webext"));

	/* Session policy: ITP on, third-party cookies blocked. */
	g_assert_cmpuint(fake.policies, >, 0);
	g_assert_true(fake.itp);
	g_assert_cmpint(fake.cookie_policy, ==, 2);

	/* Setting provider: WebRTC and prefetching off. */
	s = settings_for(mgr, "https://example.com/");
	g_assert_false(s->webrtc);
	g_assert_false(s->dns_prefetch);
	g_assert_false(s->hyperlink_auditing);

	mgr_free(mgr);
	/* Letting go restores WebKit's own session defaults. */
	g_assert_false(fake.itp);
	g_assert_cmpint(fake.cookie_policy, ==, 2);
}

static const gchar *FULL_YAML =
	"modules:\n"
	"  hardening:\n"
	"    enabled: true\n"
	"    https_only: false\n"
	"    https_only_local: true\n"
	"    strip_params: [utm_*, ref_src]\n"
	"    referer: strip_cross_origin\n"
	"    gpc: false\n"
	"    dnt: true\n"
	"    no_prefetch: false\n"
	"    third_party_cookies: block_all\n"
	"    itp: false\n"
	"    webrtc: true\n"
	"    exempt:\n"
	"      - match: '^(localhost|127\\.0\\.0\\.1|.*\\.lan)$'\n"
	"        skip: [https_only]\n"
	"      - match: '(^|\\.)mybank\\.example$'\n"
	"        skip: [referer, strip_params]\n";

/* Every option is read; webrtc: true leaves the core setting alone. */
static void
test_module_full_config(void)
{
	GsurfModuleManager *mgr = mgr_with(FULL_YAML);
	g_autoptr(GsurfHardeningPolicy) policy = NULL;
	g_autoptr(GsurfSettings) s = NULL;
	const gchar *params[] = { "utm_*", "ref_src", NULL };

	if (mgr == NULL)
		return;
	policy = registered_policy();
	g_assert_nonnull(policy);
	g_assert_false(policy->https_only);
	g_assert_true(policy->https_only_local);
	g_assert_true(g_strv_equal((const gchar * const *)policy->strip_params, params));
	g_assert_cmpint(policy->referer, ==, GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN);
	g_assert_false(policy->gpc);
	g_assert_true(policy->dnt);
	g_assert_false(policy->no_prefetch);
	g_assert_false(policy->webrtc_off);
	g_assert_cmpuint(gsurf_hardening_exemptions_get_size(policy->exempt), ==, 2);
	g_assert_cmpuint(gsurf_hardening_policy_features_for_host(policy, "www.mybank.example"),
		==, GSURF_HARDENING_DNT);
	g_assert_cmpuint(gsurf_hardening_policy_features_for_host(policy, "example.com"), ==,
		GSURF_HARDENING_STRIP_PARAMS | GSURF_HARDENING_REFERER | GSURF_HARDENING_DNT);

	g_assert_false(fake.itp);
	g_assert_cmpint(fake.cookie_policy, ==, 1);

	s = settings_for(mgr, "https://example.com/");
	g_assert_true(s->webrtc);
	g_assert_true(s->dns_prefetch);
	mgr_free(mgr);
}

/* Exemptions reach the setting provider (by the top-level URI's host). */
static void
test_module_settings_exempt(void)
{
	GsurfModuleManager *mgr = mgr_with(
		"modules:\n  hardening:\n    enabled: true\n    exempt:\n"
		"      - match: '^meet\\.example$'\n        skip: [webrtc]\n"
		"      - match: '^all\\.example$'\n");
	g_autoptr(GsurfSettings) meet = NULL;
	g_autoptr(GsurfSettings) all = NULL;
	g_autoptr(GsurfSettings) other = NULL;
	g_autoptr(GsurfSettings) blank = NULL;

	if (mgr == NULL)
		return;
	meet = settings_for(mgr, "https://MEET.example/room");
	g_assert_true(meet->webrtc);
	g_assert_false(meet->dns_prefetch);
	all = settings_for(mgr, "https://all.example/");
	g_assert_true(all->webrtc);
	g_assert_true(all->dns_prefetch);
	other = settings_for(mgr, "https://other.example/");
	g_assert_false(other->webrtc);
	/* No host (about:blank): nothing is exempt. */
	blank = settings_for(mgr, "about:blank");
	g_assert_false(blank->webrtc);
	mgr_free(mgr);
}

/* A disabled module changes nothing outside itself. */
static void
test_module_disabled(void)
{
	GsurfModuleManager *mgr = mgr_with("modules:\n  hardening:\n    enabled: false\n");
	g_autoptr(GVariant) data = NULL;

	if (mgr == NULL)
		return;
	data = gsurf_web_process_extension_get_user_data();
	g_assert_false(g_variant_lookup(data, "hardening", "@a{sv}", NULL));
	g_assert_cmpuint(fake.policies, ==, 0);
	mgr_free(mgr);
	g_assert_cmpuint(fake.policies, ==, 0);
}

/* DNT follows network.do_not_track unless the module says otherwise. */
static void
test_module_dnt_from_core(void)
{
	GsurfModuleManager *mgr = mgr_with(
		"network:\n  do_not_track: true\nmodules:\n  hardening:\n    enabled: true\n");
	g_autoptr(GsurfHardeningPolicy) inherited = NULL;
	g_autoptr(GsurfHardeningPolicy) overridden = NULL;
	g_autoptr(GsurfConfig) config = NULL;

	if (mgr == NULL)
		return;
	inherited = registered_policy();
	g_assert_true(inherited->dnt);

	config = config_from("network:\n  do_not_track: true\n"
		"modules:\n  hardening:\n    enabled: true\n    dnt: false\n");
	gsurf_module_configure(gsurf_module_manager_get_module(mgr, "hardening"), config);
	overridden = registered_policy();
	g_assert_false(overridden->dnt);
	mgr_free(mgr);
}

/* A second configure() replaces everything: options, exemptions, the
 * extension data and the session policy, back to defaults and off. */
static void
test_module_reconfigure(void)
{
	GsurfModuleManager *mgr = mgr_with(FULL_YAML);
	GsurfModule *module;
	g_autoptr(GsurfConfig) second = NULL;
	g_autoptr(GsurfConfig) bare = NULL;
	g_autoptr(GsurfConfig) off = NULL;
	g_autoptr(GsurfConfig) on = NULL;
	g_autoptr(GsurfHardeningPolicy) policy = NULL;
	g_autoptr(GsurfSettings) s = NULL;

	if (mgr == NULL)
		return;
	module = gsurf_module_manager_get_module(mgr, "hardening");

	/* Different options, one different exemption. */
	second = config_from("modules:\n  hardening:\n    enabled: true\n"
		"    referer: off\n    strip_params: []\n    exempt:\n"
		"      - match: '^only\\.example$'\n        skip: [gpc]\n");
	gsurf_module_configure(module, second);
	policy = registered_policy();
	g_assert_cmpint(policy->referer, ==, GSURF_HARDENING_REFERER_OFF);
	g_assert_null(policy->strip_params[0]);
	g_assert_true(policy->https_only);          /* back to its default */
	g_assert_cmpuint(gsurf_hardening_exemptions_get_size(policy->exempt), ==, 1);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(policy->exempt, "www.mybank.example"),
		==, 0);
	g_assert_true(fake.itp);
	g_assert_cmpint(fake.cookie_policy, ==, 2);
	g_clear_pointer(&policy, gsurf_hardening_policy_free);

	/* A config without a hardening section: defaults (still enabled). */
	bare = config_from("modules: {}\n");
	gsurf_module_configure(module, bare);
	policy = registered_policy();
	g_assert_nonnull(policy);
	g_assert_cmpint(policy->referer, ==, GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN);
	g_assert_cmpuint(gsurf_hardening_exemptions_get_size(policy->exempt), ==, 0);
	g_clear_pointer(&policy, gsurf_hardening_policy_free);

	/* Switched off: no extension data, WebKit's session defaults back,
	 * and the setting provider no longer touches anything. */
	off = config_from("modules:\n  hardening:\n    enabled: false\n");
	gsurf_module_configure(module, off);
	g_assert_null(registered_policy());
	g_assert_false(fake.itp);
	g_assert_cmpint(fake.cookie_policy, ==, 2);
	g_assert_false(gsurf_module_get_enabled(module));
	s = settings_for(mgr, "https://example.com/");
	g_assert_true(s->webrtc);
	g_clear_object(&s);

	/* And on again. */
	on = config_from("modules:\n  hardening:\n    enabled: true\n    itp: false\n");
	gsurf_module_configure(module, on);
	policy = registered_policy();
	g_assert_nonnull(policy);
	g_assert_false(fake.itp);
	mgr_free(mgr);
}

/* Bad input is reported and replaced by safe values. */
static void
test_module_bad_input(void)
{
	GsurfModuleManager *mgr;
	g_autoptr(GsurfHardeningPolicy) policy = NULL;
	g_autoptr(GsurfConfig) config = NULL;
	GsurfModule *module;

	/* An invalid exemption regex drops the whole list. */
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*hardening: exempt:*no site exceptions*");
	mgr = mgr_with("modules:\n  hardening:\n    enabled: true\n    exempt:\n"
		"      - match: '^ok\\.example$'\n      - match: '(['\n");
	if (mgr == NULL) {
		g_test_assert_expected_messages();
		return;
	}
	g_test_assert_expected_messages();
	policy = registered_policy();
	g_assert_cmpuint(gsurf_hardening_exemptions_get_size(policy->exempt), ==, 0);
	g_clear_pointer(&policy, gsurf_hardening_policy_free);
	module = gsurf_module_manager_get_module(mgr, "hardening");

	/* Unknown, session-wide and empty skip lists; bad enum values; a
	 * strip_params that is not a list, and a non-scalar element. */
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*unknown referer policy 'sideways'*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*exempt[0]*unknown skip name(s) bogus*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*exempt[1]*itp*cannot be exempted*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*exempt[1]*skips nothing*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*exempt[2]*skips nothing*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*unknown third_party_cookies value 'some'*");
	config = config_from("modules:\n  hardening:\n    enabled: true\n"
		"    referer: sideways\n    third_party_cookies: some\n"
		"    exempt:\n"
		"      - match: '^a\\.example$'\n        skip: [gpc, bogus]\n"
		"      - match: '^b\\.example$'\n        skip: [itp]\n"
		"      - match: '^c\\.example$'\n        skip: []\n");
	gsurf_module_configure(module, config);
	g_test_assert_expected_messages();
	policy = registered_policy();
	g_assert_cmpint(policy->referer, ==, GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN);
	g_assert_cmpint(fake.cookie_policy, ==, 2);
	g_assert_cmpuint(gsurf_hardening_exemptions_lookup(policy->exempt, "a.example"), ==,
		GSURF_HARDENING_GPC);
	g_clear_pointer(&policy, gsurf_hardening_policy_free);
	g_clear_object(&config);

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*strip_params must be a list*");
	config = config_from("modules:\n  hardening:\n    enabled: true\n    strip_params: fbclid\n");
	gsurf_module_configure(module, config);
	g_test_assert_expected_messages();
	policy = registered_policy();
	g_assert_cmpstr(policy->strip_params[0], ==, "utm_*");
	g_clear_pointer(&policy, gsurf_hardening_policy_free);
	g_clear_object(&config);

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*strip_params[1] is not a parameter name*");
	config = config_from("modules:\n  hardening:\n    enabled: true\n"
		"    strip_params: [fbclid, {a: b}, gclid]\n");
	gsurf_module_configure(module, config);
	g_test_assert_expected_messages();
	policy = registered_policy();
	g_assert_cmpstr(policy->strip_params[0], ==, "fbclid");
	g_assert_cmpstr(policy->strip_params[1], ==, "gclid");
	g_assert_null(policy->strip_params[2]);
	g_clear_pointer(&policy, gsurf_hardening_policy_free);
	mgr_free(mgr);
}

/* The third_party_cookies vocabulary. */
static void
test_module_cookie_values(void)
{
	static const struct {
		const gchar *value;
		gint         policy;
	} cases[] = {
		{ "allow", 0 }, { "block", 2 }, { "block_all", 1 }, { "true", 2 }, { "false", 0 },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *yaml = g_strdup_printf(
			"modules:\n  hardening:\n    enabled: true\n    third_party_cookies: %s\n",
			cases[i].value);
		GsurfModuleManager *mgr = mgr_with(yaml);

		if (mgr == NULL)
			return;
		g_assert_cmpint(fake.cookie_policy, ==, cases[i].policy);
		mgr_free(mgr);
	}
}

/* A missing extension (bad web_process_extension path) leaves the
 * request features off but the settings and session policy in force. */
static void
test_module_missing_webext(void)
{
	GsurfModuleManager *mgr = mgr_with("modules:\n  hardening:\n    enabled: true\n"
		"    web_process_extension: /nonexistent/gsurf-hardening-webext.so\n");
	g_autoptr(GsurfSettings) s = NULL;

	if (mgr == NULL)
		return;
	g_assert_null(registered_policy());
	g_assert_true(fake.itp);
	s = settings_for(mgr, "https://example.com/");
	g_assert_false(s->webrtc);
	mgr_free(mgr);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/gsurf/hardening/rules/names", test_rules_names);
	g_test_add_func("/gsurf/hardening/rules/local-hosts", test_rules_local_hosts);
	g_test_add_func("/gsurf/hardening/rules/upgrade", test_rules_upgrade);
	g_test_add_func("/gsurf/hardening/rules/downgrade", test_rules_downgrade);
	g_test_add_func("/gsurf/hardening/rules/strip-params", test_rules_strip_params);
	g_test_add_func("/gsurf/hardening/rules/referer", test_rules_referer);
	g_test_add_func("/gsurf/hardening/rules/speculative", test_rules_speculative);
	g_test_add_func("/gsurf/hardening/rules/exemptions", test_rules_exemptions);
	g_test_add_func("/gsurf/hardening/rules/skip-mask", test_rules_skip_mask);
	g_test_add_func("/gsurf/hardening/rules/policy-variant", test_rules_policy_variant);

	g_test_add_func("/gsurf/hardening/module/defaults", test_module_defaults);
	g_test_add_func("/gsurf/hardening/module/full-config", test_module_full_config);
	g_test_add_func("/gsurf/hardening/module/settings-exempt", test_module_settings_exempt);
	g_test_add_func("/gsurf/hardening/module/disabled", test_module_disabled);
	g_test_add_func("/gsurf/hardening/module/dnt-from-core", test_module_dnt_from_core);
	g_test_add_func("/gsurf/hardening/module/reconfigure", test_module_reconfigure);
	g_test_add_func("/gsurf/hardening/module/bad-input", test_module_bad_input);
	g_test_add_func("/gsurf/hardening/module/cookie-values", test_module_cookie_values);
	g_test_add_func("/gsurf/hardening/module/missing-webext", test_module_missing_webext);
	return g_test_run();
}
