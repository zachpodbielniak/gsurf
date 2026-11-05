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
	g_assert_cmpint(gsurf_action_from_string("pane-split-right"), ==,
		GSURF_ACTION_PANE_SPLIT_RIGHT);
	g_assert_cmpint(gsurf_action_from_string("pane_split_down"), ==,
		GSURF_ACTION_PANE_SPLIT_DOWN);
	g_assert_cmpint(gsurf_action_from_string("open-new-view-background"), ==,
		GSURF_ACTION_OPEN_NEW_VIEW_BACKGROUND);

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
	g_assert_cmpstr(gsurf_action_to_string(GSURF_ACTION_SHOW_KEYBINDS), ==, "show-keybinds");
	g_assert_cmpstr(gsurf_action_to_string(GSURF_ACTION_PANE_SPLIT_RIGHT), ==, "pane-split-right");
	g_assert_cmpstr(gsurf_action_to_string(GSURF_ACTION_PANE_SPLIT_DOWN), ==, "pane-split-down");
	g_assert_cmpstr(gsurf_action_to_string(GSURF_ACTION_OPEN_NEW_VIEW_BACKGROUND), ==,
		"open-new-view-background");
	/* Appended last: the values before it keep their numbers. */
	g_assert_cmpint(GSURF_ACTION_OPEN_NEW_VIEW_BACKGROUND, ==, GSURF_ACTION_PANE_SPLIT_DOWN + 1);
}

static void
test_action_description(void)
{
	g_assert_cmpstr(gsurf_action_get_description(GSURF_ACTION_RELOAD), ==,
		"Reload the page");
	g_assert_cmpstr(gsurf_action_get_description(GSURF_ACTION_SHOW_KEYBINDS), ==,
		"Show all keybindings");
	g_assert_nonnull(gsurf_action_get_description(GSURF_ACTION_QUIT));
	g_assert_cmpstr(gsurf_action_get_description(GSURF_ACTION_PANE_SPLIT_RIGHT), ==,
		"Split the pane, the new pane on the right");
	g_assert_cmpstr(gsurf_action_get_description(GSURF_ACTION_PANE_SPLIT_DOWN), ==,
		"Split the pane, the new pane below");
	g_assert_cmpstr(gsurf_action_get_description(GSURF_ACTION_OPEN_NEW_VIEW_BACKGROUND), ==,
		"Open in a new background tab");
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
	g_assert_true(G_TYPE_IS_ENUM(GSURF_TYPE_LAYOUT_DROP_ZONE));
	g_assert_true(G_TYPE_IS_ENUM(GSURF_TYPE_TAB_TREE_POSITION));

	/* The mode indicator labels. */
	g_assert_cmpstr(gsurf_mode_policy_get_label(GSURF_MODE_NORMAL), ==, "NORMAL");
	g_assert_cmpstr(gsurf_mode_policy_get_label(GSURF_MODE_INSERT), ==, "INSERT");
	g_assert_cmpstr(gsurf_mode_policy_get_label(GSURF_MODE_FOLLOW), ==, "FOLLOW");
	g_assert_cmpstr(gsurf_mode_policy_get_label(GSURF_MODE_FIND), ==, "FIND");
	g_assert_cmpstr(gsurf_mode_policy_get_label(GSURF_MODE_PASSTHROUGH), ==, "PASSTHROUGH");
	g_assert_null(gsurf_mode_policy_get_label((GsurfModePolicy)99));
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

	/* The drop zones, by nick. */
	klass = g_type_class_ref(GSURF_TYPE_LAYOUT_DROP_ZONE);
	v = g_enum_get_value_by_nick(klass, "center");
	g_assert_nonnull(v);
	g_assert_cmpint(v->value, ==, GSURF_LAYOUT_DROP_CENTER);
	v = g_enum_get_value_by_nick(klass, "bottom");
	g_assert_nonnull(v);
	g_assert_cmpint(v->value, ==, GSURF_LAYOUT_DROP_BOTTOM);
	g_assert_cmpuint(klass->n_values, ==, 5);
	g_type_class_unref(klass);

	/* The tab tree positions (0.4.4), by nick. */
	klass = g_type_class_ref(GSURF_TYPE_TAB_TREE_POSITION);
	v = g_enum_get_value_by_nick(klass, "before");
	g_assert_nonnull(v);
	g_assert_cmpint(v->value, ==, GSURF_TAB_TREE_POSITION_BEFORE);
	v = g_enum_get_value_by_nick(klass, "child");
	g_assert_nonnull(v);
	g_assert_cmpint(v->value, ==, GSURF_TAB_TREE_POSITION_CHILD);
	g_assert_cmpuint(klass->n_values, ==, 3);
	g_type_class_unref(klass);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/enums/action-from-string", test_action_from_string);
	g_test_add_func("/gsurf/enums/action-to-string", test_action_to_string);
	g_test_add_func("/gsurf/enums/action-description", test_action_description);
	g_test_add_func("/gsurf/enums/action-roundtrip", test_action_roundtrip);
	g_test_add_func("/gsurf/enums/types", test_enum_types);
	g_test_add_func("/gsurf/enums/values", test_enum_values);
	return g_test_run();
}
