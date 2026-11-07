/*
 * test-languages.c - The languages pages see: locale names to BCP 47
 * tags, browser.languages, and the C-locale fallback
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>

/* Assert @got is exactly the NULL-terminated @want list. */
static void
assert_strv(gchar **got, const gchar * const *want)
{
	g_autofree gchar *a = g_strjoinv(",", got);
	g_autofree gchar *b = g_strjoinv(",", (gchar **)want);

	g_assert_cmpstr(a, ==, b);
}

/* POSIX locale names and tags normalize; C / POSIX and junk are not
 * languages. */
static void
test_normalize(void)
{
	static const struct { const gchar *in; const gchar *out; } cases[] = {
		{ "en_US.UTF-8", "en-US" },
		{ "de_DE@euro", "de-DE" },
		{ "sr_RS.UTF-8@latin", "sr-RS" },
		{ "en", "en" },
		{ "en-GB", "en-GB" },
		{ " fr_CA ", "fr-CA" },
		{ "zh-Hant-TW", "zh-Hant-TW" },
		{ "C", NULL },
		{ "C.UTF-8", NULL },
		{ "c.utf8", NULL },
		{ "POSIX", NULL },
		{ "", NULL },
		{ "e", NULL },
		{ "english", NULL },
		{ "12-US", NULL },
		{ "en--US", NULL },
		{ "en_US;q=0.9", NULL },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *got = gsurf_languages_normalize_tag(cases[i].in);

		g_assert_cmpstr(got, ==, cases[i].out);
	}
	g_assert_null(gsurf_languages_normalize_tag(NULL));
}

/* A locale's language list: tags in order, no repeats; the C locale (or
 * nothing at all) falls back to en-US. */
static void
test_from_names(void)
{
	static const gchar *const en_us[] = { "en_US.UTF-8", "en_US", "en.UTF-8", "en", "C", NULL };
	static const gchar *const c_only[] = { "C.UTF-8", "C", NULL };
	static const gchar *const mixed[] = { "de_DE.UTF-8", "en", "de-DE", "bogus!", NULL };
	g_auto(GStrv) a = gsurf_languages_from_names(en_us);
	g_auto(GStrv) b = gsurf_languages_from_names(c_only);
	g_auto(GStrv) c = gsurf_languages_from_names(mixed);
	g_auto(GStrv) d = gsurf_languages_from_names(NULL);

	assert_strv(a, (const gchar *const[]){ "en-US", "en", NULL });
	assert_strv(b, (const gchar *const[]){ GSURF_LANGUAGES_FALLBACK, NULL });
	assert_strv(c, (const gchar *const[]){ "de-DE", "en", NULL });
	assert_strv(d, (const gchar *const[]){ "en-US", NULL });
}

/* browser.languages wins when it names a language; otherwise the locale
 * (a configured list of junk warns). */
static void
test_resolve(void)
{
	static const gchar *const configured[] = { "fr_FR", "en", NULL };
	static const gchar *const junk[] = { "C", NULL };
	g_auto(GStrv) a = gsurf_languages_resolve(configured);
	g_auto(GStrv) b = NULL;
	g_auto(GStrv) c = NULL;
	g_auto(GStrv) locale = gsurf_languages_from_names(g_get_language_names());

	assert_strv(a, (const gchar *const[]){ "fr-FR", "en", NULL });
	b = gsurf_languages_resolve(NULL);
	assert_strv(b, (const gchar *const *)locale);
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*browser.languages names no language*");
	c = gsurf_languages_resolve(junk);
	g_test_assert_expected_messages();
	assert_strv(c, (const gchar *const *)locale);
	/* Never "C", whatever the test's locale. */
	g_assert_false(g_strv_contains((const gchar *const *)locale, "C"));
}

/* The YAML: a list, a comma string, an empty list back to the locale. */
static void
test_config(void)
{
	g_autoptr(GsurfConfig) c = gsurf_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_null(c->languages);
	g_assert_true(gsurf_config_load_from_data(c,
		"browser:\n  languages: [en-US, de]\n", -1, &error));
	g_assert_no_error(error);
	assert_strv(c->languages, (const gchar *const[]){ "en-US", "de", NULL });
	g_assert_true(gsurf_config_load_from_data(c,
		"browser:\n  languages: \"fr_CA, en\"\n", -1, &error));
	assert_strv(c->languages, (const gchar *const[]){ "fr_CA", "en", NULL });
	g_assert_true(gsurf_config_load_from_data(c,
		"browser:\n  homepage: about:blank\n", -1, &error));
	assert_strv(c->languages, (const gchar *const[]){ "fr_CA", "en", NULL });
	g_assert_true(gsurf_config_load_from_data(c,
		"browser:\n  languages: []\n", -1, &error));
	g_assert_null(c->languages);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/languages/normalize", test_normalize);
	g_test_add_func("/gsurf/languages/from-names", test_from_names);
	g_test_add_func("/gsurf/languages/resolve", test_resolve);
	g_test_add_func("/gsurf/languages/config", test_config);
	return g_test_run();
}
