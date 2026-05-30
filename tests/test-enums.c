/*
 * test-enums.c - Enum/flags registration and action string mapping
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <glib.h>

static void
test_action_from_string(void)
{
	g_assert_cmpint(gsurf_action_from_string("reload"), ==, GSURF_ACTION_RELOAD);
	g_assert_cmpint(gsurf_action_from_string("scroll-down"), ==, GSURF_ACTION_SCROLL_DOWN);
	g_assert_cmpint(gsurf_action_from_string("tab-new"), ==, GSURF_ACTION_TAB_NEW);
	g_assert_cmpint(gsurf_action_from_string("quit"), ==, GSURF_ACTION_QUIT);

	/* Underscores are accepted as equivalent to hyphens. */
	g_assert_cmpint(gsurf_action_from_string("reload_nocache"), ==, GSURF_ACTION_RELOAD_NOCACHE);
	g_assert_cmpint(gsurf_action_from_string("scroll_bottom"), ==, GSURF_ACTION_SCROLL_BOTTOM);

	/* Unknown / empty / NULL all map to NONE. */
	g_assert_cmpint(gsurf_action_from_string("does-not-exist"), ==, GSURF_ACTION_NONE);
	g_assert_cmpint(gsurf_action_from_string(""), ==, GSURF_ACTION_NONE);
	g_assert_cmpint(gsurf_action_from_string(NULL), ==, GSURF_ACTION_NONE);
}

static void
test_action_to_string(void)
{
	g_assert_cmpstr(gsurf_action_to_string(GSURF_ACTION_RELOAD), ==, "reload");
	g_assert_cmpstr(gsurf_action_to_string(GSURF_ACTION_NONE), ==, "none");
	g_assert_cmpstr(gsurf_action_to_string(GSURF_ACTION_FOLLOW_HINTS), ==, "follow-hints");
}

static void
test_action_roundtrip(void)
{
	GsurfAction a;
	for (a = GSURF_ACTION_NONE; a < GSURF_ACTION_LAST; a++) {
		const char *nick = gsurf_action_to_string(a);
		if (nick != NULL)
			g_assert_cmpint(gsurf_action_from_string(nick), ==, a);
	}
}

static void
test_enum_types(void)
{
	/* Enums are registered as GTypes (needed for properties/signals/GIR). */
	g_assert_true(G_TYPE_IS_ENUM(GSURF_TYPE_ACTION));
	g_assert_true(G_TYPE_IS_ENUM(GSURF_TYPE_LOAD_EVENT));
	g_assert_true(G_TYPE_IS_ENUM(GSURF_TYPE_POLICY_DECISION));
	g_assert_true(G_TYPE_IS_ENUM(GSURF_TYPE_MODE_POLICY));
	g_assert_true(G_TYPE_IS_FLAGS(GSURF_TYPE_KEY_MOD));
}

static void
test_enum_values(void)
{
	GEnumClass *klass = g_type_class_ref(GSURF_TYPE_LOAD_EVENT);
	GEnumValue *v;

	v = g_enum_get_value_by_nick(klass, "finished");
	g_assert_nonnull(v);
	g_assert_cmpint(v->value, ==, GSURF_LOAD_FINISHED);

	v = g_enum_get_value(klass, GSURF_LOAD_STARTED);
	g_assert_nonnull(v);
	g_assert_cmpstr(v->value_nick, ==, "started");

	g_type_class_unref(klass);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/enums/action-from-string", test_action_from_string);
	g_test_add_func("/gsurf/enums/action-to-string", test_action_to_string);
	g_test_add_func("/gsurf/enums/action-roundtrip", test_action_roundtrip);
	g_test_add_func("/gsurf/enums/types", test_enum_types);
	g_test_add_func("/gsurf/enums/values", test_enum_values);
	return g_test_run();
}
