/*
 * test-version.c - Version macros + library init
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <glib.h>
#include <string.h>

static void
test_version_string(void)
{
	const char *s = gsurf_get_version_string();
	g_assert_nonnull(s);
	g_assert_cmpstr(s, ==, GSURF_VERSION_STRING);
	/* Looks like "N.N.N". */
	g_assert_true(g_regex_match_simple("^[0-9]+\\.[0-9]+\\.[0-9]+$", s, 0, 0));
}

static void
test_version_components(void)
{
	guint major = 99, minor = 99, micro = 99;

	gsurf_get_version(&major, &minor, &micro);
	g_assert_cmpuint(major, ==, GSURF_VERSION_MAJOR);
	g_assert_cmpuint(minor, ==, GSURF_VERSION_MINOR);
	g_assert_cmpuint(micro, ==, GSURF_VERSION_MICRO);

	/* NULL out-args must be tolerated. */
	gsurf_get_version(NULL, NULL, NULL);
}

static void
test_version_check(void)
{
	g_assert_true(GSURF_CHECK_VERSION(0, 0, 0));
	g_assert_true(GSURF_CHECK_VERSION(GSURF_VERSION_MAJOR, GSURF_VERSION_MINOR, GSURF_VERSION_MICRO));
	g_assert_false(GSURF_CHECK_VERSION(GSURF_VERSION_MAJOR + 1, 0, 0));
	g_assert_false(GSURF_CHECK_VERSION(GSURF_VERSION_MAJOR, GSURF_VERSION_MINOR, GSURF_VERSION_MICRO + 1));
}

static void
test_init(void)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(gsurf_init_check(NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(gsurf_is_initialized());
	/* Idempotent. */
	g_assert_true(gsurf_init_check(NULL, NULL, &error));
	g_assert_no_error(error);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/version/string", test_version_string);
	g_test_add_func("/gsurf/version/components", test_version_components);
	g_test_add_func("/gsurf/version/check", test_version_check);
	g_test_add_func("/gsurf/version/init", test_init);
	return g_test_run();
}
