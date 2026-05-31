/*
 * test-config.c - GsurfConfig YAML parsing and lookups
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <glib.h>

static void
test_defaults(void)
{
	g_autoptr(GsurfConfig) c = gsurf_config_new();

	g_assert_cmpstr(c->homepage, ==, "about:blank");
	g_assert_cmpint(c->window_width, ==, 1024);
	g_assert_cmpint(c->window_height, ==, 768);
	g_assert_cmpfloat(c->default_zoom, ==, 1.0);
	g_assert_nonnull(c->settings);
	g_assert_nonnull(c->keybinds);
}

static void
test_load_core(void)
{
	g_autoptr(GsurfConfig) c = gsurf_config_new();
	g_autoptr(GError) error = NULL;
	static const char *yaml =
		"browser:\n"
		"  homepage: \"https://example.org\"\n"
		"  default_zoom: 1.5\n"
		"  smooth_scroll: false\n"
		"user_agent: \"Agent/2\"\n"
		"webkit:\n"
		"  javascript: false\n"
		"  webgl: true\n"
		"  default_font_size: 20\n"
		"window:\n"
		"  title: \"My gsurf\"\n"
		"  geometry: 800x600\n";

	g_assert_true(gsurf_config_load_from_data(c, yaml, -1, &error));
	g_assert_no_error(error);

	g_assert_cmpstr(c->homepage, ==, "https://example.org");
	g_assert_cmpfloat(c->default_zoom, ==, 1.5);
	g_assert_false(c->smooth_scroll);
	g_assert_cmpstr(c->user_agent, ==, "Agent/2");
	g_assert_cmpstr(c->settings->user_agent, ==, "Agent/2");
	g_assert_false(c->settings->javascript);
	g_assert_true(c->settings->webgl);
	g_assert_cmpuint(c->settings->default_font_size, ==, 20);
	g_assert_cmpstr(c->window_title, ==, "My gsurf");
	g_assert_cmpint(c->window_width, ==, 800);
	g_assert_cmpint(c->window_height, ==, 600);
}

static void
test_keybinds(void)
{
	g_autoptr(GsurfConfig) c = gsurf_config_new();
	g_autoptr(GError) error = NULL;
	static const char *yaml =
		"keybinds:\n"
		"  \"ctrl+r\": reload\n"          /* lower-case + reordered on load */
		"  \"shift+ctrl+g\": find-prev\n"
		"  \"j\": scroll-down\n";

	g_assert_true(gsurf_config_load_from_data(c, yaml, -1, &error));
	g_assert_no_error(error);

	/* Lookups use the canonical form produced by gsurf_keys_to_string. */
	g_assert_cmpint(gsurf_config_get_keybind_action(c, "Ctrl+r"), ==, GSURF_ACTION_RELOAD);
	g_assert_cmpint(gsurf_config_get_keybind_action(c, "Ctrl+Shift+g"), ==, GSURF_ACTION_FIND_PREV);
	g_assert_cmpint(gsurf_config_get_keybind_action(c, "j"), ==, GSURF_ACTION_SCROLL_DOWN);

	g_assert_cmpint(gsurf_config_get_keybind_action(c, "Ctrl+z"), ==, GSURF_ACTION_NONE);
	g_assert_cmpint(gsurf_config_get_keybind_action(c, NULL), ==, GSURF_ACTION_NONE);
}

static void
test_modules_node(void)
{
	g_autoptr(GsurfConfig) c = gsurf_config_new();
	g_autoptr(GError) error = NULL;
	static const char *yaml =
		"modules:\n"
		"  foo:\n"
		"    enabled: true\n"
		"    n: 3\n";

	g_assert_true(gsurf_config_load_from_data(c, yaml, -1, &error));
	g_assert_no_error(error);

	g_assert_nonnull(gsurf_config_get_module_node(c, "foo"));
	g_assert_null(gsurf_config_get_module_node(c, "missing"));
}

static void
test_bad_geometry_ignored(void)
{
	g_autoptr(GsurfConfig) c = gsurf_config_new();
	g_autoptr(GError) error = NULL;
	static const char *yaml = "window:\n  geometry: not-a-size\n";

	g_assert_true(gsurf_config_load_from_data(c, yaml, -1, &error));
	g_assert_no_error(error);
	/* Defaults preserved when the geometry can't be parsed. */
	g_assert_cmpint(c->window_width, ==, 1024);
	g_assert_cmpint(c->window_height, ==, 768);
}

static void
test_override(void)
{
	g_autoptr(GsurfConfig) c = gsurf_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(gsurf_config_load_from_data(c, "browser:\n  homepage: \"https://a\"\n", -1, &error));
	g_assert_cmpstr(c->homepage, ==, "https://a");
	/* A second load merges over the first. */
	g_assert_true(gsurf_config_load_from_data(c, "browser:\n  homepage: \"https://b\"\n", -1, &error));
	g_assert_cmpstr(c->homepage, ==, "https://b");
	g_assert_no_error(error);
}

static void
test_ignore_yaml(void)
{
	g_autoptr(GsurfConfig) c = gsurf_config_new();
	g_autoptr(GError) error = NULL;

	/* Establish a baseline value. */
	g_assert_true(gsurf_config_load_from_data(c, "browser:\n  homepage: \"https://base\"\n", -1, &error));
	g_assert_cmpstr(c->homepage, ==, "https://base");

	/* A document with ignore_yaml: true is skipped entirely — its homepage
	 * must NOT take effect. */
	g_assert_true(gsurf_config_load_from_data(c,
		"ignore_yaml: true\nbrowser:\n  homepage: \"https://ignored\"\n", -1, &error));
	g_assert_cmpstr(c->homepage, ==, "https://base");

	/* ignore_yaml: false applies normally. */
	g_assert_true(gsurf_config_load_from_data(c,
		"ignore_yaml: false\nbrowser:\n  homepage: \"https://applied\"\n", -1, &error));
	g_assert_cmpstr(c->homepage, ==, "https://applied");
	g_assert_no_error(error);
}

static void
test_set_binds(void)
{
	g_autoptr(GsurfConfig) c = gsurf_config_new();

	/* Programmatic setters (the C-config path). */
	gsurf_config_set_keybind(c, "Ctrl+r", GSURF_ACTION_RELOAD);
	g_assert_cmpint(gsurf_config_get_keybind_action(c, "Ctrl+r"), ==, GSURF_ACTION_RELOAD);

	/* The binding string is normalized: modifier order/case is canonicalized,
	 * so this resolves to the same "Ctrl+Shift+t" the runtime looks up. */
	gsurf_config_set_keybind(c, "shift+ctrl+t", GSURF_ACTION_TAB_NEW);
	g_assert_cmpint(gsurf_config_get_keybind_action(c, "Ctrl+Shift+t"), ==, GSURF_ACTION_TAB_NEW);

	/* GSURF_ACTION_NONE removes the binding. */
	gsurf_config_set_keybind(c, "Ctrl+r", GSURF_ACTION_NONE);
	g_assert_cmpint(gsurf_config_get_keybind_action(c, "Ctrl+r"), ==, GSURF_ACTION_NONE);

	/* Mousebinds likewise. */
	gsurf_config_set_mousebind(c, "Button8", GSURF_ACTION_BACK);
	g_assert_cmpint(gsurf_config_get_mousebind_action(c, "Button8"), ==, GSURF_ACTION_BACK);
	gsurf_config_set_mousebind(c, "Button8", GSURF_ACTION_NONE);
	g_assert_cmpint(gsurf_config_get_mousebind_action(c, "Button8"), ==, GSURF_ACTION_NONE);
}

static void
test_non_mapping_root(void)
{
	g_autoptr(GsurfConfig) c = gsurf_config_new();
	g_autoptr(GError) error = NULL;

	/* A sequence at the root is not a valid config. */
	g_assert_false(gsurf_config_load_from_data(c, "- a\n- b\n", -1, &error));
	g_assert_nonnull(error);
	/* Defaults remain intact after a failed load. */
	g_assert_cmpstr(c->homepage, ==, "about:blank");
}

static void
test_default_singleton(void)
{
	g_autoptr(GsurfConfig) c = gsurf_config_new();

	gsurf_config_set_default(c);
	g_assert_true(gsurf_config_get_default() == c);
	gsurf_config_set_default(NULL);
	g_assert_null(gsurf_config_get_default());
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/config/defaults", test_defaults);
	g_test_add_func("/gsurf/config/load-core", test_load_core);
	g_test_add_func("/gsurf/config/keybinds", test_keybinds);
	g_test_add_func("/gsurf/config/modules-node", test_modules_node);
	g_test_add_func("/gsurf/config/bad-geometry", test_bad_geometry_ignored);
	g_test_add_func("/gsurf/config/override", test_override);
	g_test_add_func("/gsurf/config/ignore-yaml", test_ignore_yaml);
	g_test_add_func("/gsurf/config/set-binds", test_set_binds);
	g_test_add_func("/gsurf/config/non-mapping-root", test_non_mapping_root);
	g_test_add_func("/gsurf/config/default-singleton", test_default_singleton);
	return g_test_run();
}
