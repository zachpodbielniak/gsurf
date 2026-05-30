/*
 * test-settings.c - GsurfSettings defaults, copy, setters
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <glib.h>

static void
test_defaults(void)
{
	g_autoptr(GsurfSettings) s = gsurf_settings_new();

	/* Browser baseline (mirrors surf defconfig). */
	g_assert_true(s->javascript);
	g_assert_true(s->images);
	g_assert_false(s->webgl);
	g_assert_false(s->webrtc);
	g_assert_false(s->media_stream);
	g_assert_true(s->site_quirks);
	g_assert_false(s->developer_extras);
	g_assert_cmpuint(s->default_font_size, ==, 16);
	g_assert_cmpuint(s->default_monospace_font_size, ==, 13);
	g_assert_cmpstr(s->default_charset, ==, "UTF-8");
	g_assert_null(s->user_agent);
}

static void
test_setters(void)
{
	g_autoptr(GsurfSettings) s = gsurf_settings_new();

	gsurf_settings_set_user_agent(s, "Agent/1.0");
	g_assert_cmpstr(s->user_agent, ==, "Agent/1.0");

	/* Empty string clears it back to the engine default (NULL). */
	gsurf_settings_set_user_agent(s, "");
	g_assert_null(s->user_agent);

	gsurf_settings_set_default_charset(s, "ISO-8859-1");
	g_assert_cmpstr(s->default_charset, ==, "ISO-8859-1");

	/* NULL charset resets to UTF-8. */
	gsurf_settings_set_default_charset(s, NULL);
	g_assert_cmpstr(s->default_charset, ==, "UTF-8");
}

static void
test_copy_independence(void)
{
	g_autoptr(GsurfSettings) a = gsurf_settings_new();
	GsurfSettings *b;

	a->webgl = TRUE;
	gsurf_settings_set_user_agent(a, "A");
	b = gsurf_settings_copy(a);

	g_assert_true(b->webgl);
	g_assert_cmpstr(b->user_agent, ==, "A");
	/* Deep copy: distinct string storage. */
	g_assert_true(a->user_agent != b->user_agent);

	/* Mutating the copy must not affect the original. */
	b->webgl = FALSE;
	gsurf_settings_set_user_agent(b, "B");
	g_assert_true(a->webgl);
	g_assert_cmpstr(a->user_agent, ==, "A");

	g_object_unref(b);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/settings/defaults", test_defaults);
	g_test_add_func("/gsurf/settings/setters", test_setters);
	g_test_add_func("/gsurf/settings/copy-independence", test_copy_independence);
	return g_test_run();
}
