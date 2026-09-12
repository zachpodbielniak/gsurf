/*
 * test-keybind-help.c - Dynamic keybinding help collection
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The `?` overlay is assembled at display time from the core keybind
 * table plus every *active* GsurfKeybindProvider module. These tests
 * cover that collection, including enabled-gating and live reconfigure.
 */

#include <gsurf.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static char *
module_so_path(const char *name)
{
	char *exe = g_file_read_link("/proc/self/exe", NULL);
	char *dir, *base, *path;

	g_assert_nonnull(exe);
	dir = g_path_get_dirname(exe);
	base = g_strconcat(name, ".so", NULL);
	path = g_build_filename(dir, "modules", base, NULL);
	g_free(exe);
	g_free(dir);
	g_free(base);
	return path;
}

static GsurfModuleManager *
mgr_with(const char *modname, const char *yaml)
{
	g_autofree char *so = module_so_path(modname);
	g_autoptr(GError) error = NULL;
	GsurfConfig *config;
	GsurfModuleManager *mgr;

	if (!g_file_test(so, G_FILE_TEST_EXISTS))
		return NULL;

	config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);

	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(mgr);

	g_object_unref(config);
	return mgr;
}

static const GsurfKeybindHelp *
find_row(GPtrArray *entries, const char *key, const char *source)
{
	guint i;

	for (i = 0; i < entries->len; i++) {
		const GsurfKeybindHelp *h = g_ptr_array_index(entries, i);

		if (g_strcmp0(h->key, key) == 0 && g_strcmp0(h->source, source) == 0)
			return h;
	}
	return NULL;
}

static void
test_core_only(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_new();
	GsurfConfig *config = gsurf_config_new();
	g_autoptr(GPtrArray) entries = NULL;

	g_assert_true(gsurf_config_load_from_data(config,
		"keybinds:\n"
		"  \"Ctrl+r\": reload\n"
		"  \"question\": show-keybinds\n",
		-1, NULL));
	gsurf_module_manager_set_config(mgr, config);

	entries = gsurf_module_manager_collect_keybinds(mgr);
	g_assert_nonnull(find_row(entries, "Ctrl+r", "core"));
	g_assert_nonnull(find_row(entries, "question", "core"));
	g_assert_cmpstr(find_row(entries, "question", "core")->action, ==, "show-keybinds");
	g_assert_null(find_row(entries, "Ctrl+t", "tabs"));

	g_object_unref(config);
	g_object_unref(mgr);
}

static void
test_question_alias(void)
{
	g_autoptr(GsurfConfig) config = gsurf_config_new();

	gsurf_config_set_keybind(config, "?", GSURF_ACTION_SHOW_KEYBINDS);
	g_assert_cmpint(gsurf_config_get_keybind_action(config, "question"),
		==, GSURF_ACTION_SHOW_KEYBINDS);
	g_assert_cmpint(gsurf_config_get_keybind_action(config, "?"),
		==, GSURF_ACTION_SHOW_KEYBINDS);
}

static void
test_active_module_keys(void)
{
	GsurfModuleManager *mgr = mgr_with("homepage",
		"keybinds:\n  \"Ctrl+r\": reload\n"
		"modules:\n  homepage:\n    enabled: true\n    key_home: \"Alt+h\"\n");
	g_autoptr(GPtrArray) entries = NULL;
	const GsurfKeybindHelp *row;

	if (mgr == NULL) {
		g_test_skip("homepage.so not built");
		return;
	}

	entries = gsurf_module_manager_collect_keybinds(mgr);
	g_assert_nonnull(find_row(entries, "Ctrl+r", "core"));
	row = find_row(entries, "Alt+h", "homepage");
	g_assert_nonnull(row);
	g_assert_cmpstr(row->description, ==, "Go to the homepage");
	/* Default Ctrl+Home must not appear after reconfigure. */
	g_assert_null(find_row(entries, "Ctrl+Home", "homepage"));

	g_object_unref(mgr);
}

static void
test_disabled_module_omitted(void)
{
	GsurfModuleManager *mgr = mgr_with("homepage",
		"modules:\n  homepage:\n    enabled: false\n    key_home: \"Alt+h\"\n");
	g_autoptr(GPtrArray) entries = NULL;

	if (mgr == NULL) {
		g_test_skip("homepage.so not built");
		return;
	}

	entries = gsurf_module_manager_collect_keybinds(mgr);
	g_assert_null(find_row(entries, "Alt+h", "homepage"));
	g_assert_null(find_row(entries, "Ctrl+Home", "homepage"));

	g_object_unref(mgr);
}

static void
test_tabs_and_modal_together(void)
{
	g_autofree char *tabs_so = module_so_path("tabs");
	g_autofree char *modal_so = module_so_path("modal");
	g_autoptr(GError) error = NULL;
	GsurfConfig *config;
	GsurfModuleManager *mgr;
	g_autoptr(GPtrArray) entries = NULL;

	if (!g_file_test(tabs_so, G_FILE_TEST_EXISTS) ||
	    !g_file_test(modal_so, G_FILE_TEST_EXISTS)) {
		g_test_skip("tabs.so or modal.so not built");
		return;
	}

	config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(config,
		"keybinds:\n  \"question\": show-keybinds\n"
		"modules:\n"
		"  tabs:\n    enabled: true\n    key_new: \"Ctrl+n\"\n"
		"  modal:\n    enabled: true\n    hint_key: \"f\"\n",
		-1, NULL));

	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, tabs_so, &error));
	g_assert_no_error(error);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, modal_so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(mgr);

	entries = gsurf_module_manager_collect_keybinds(mgr);
	g_assert_nonnull(find_row(entries, "question", "core"));
	g_assert_nonnull(find_row(entries, "Ctrl+n", "tabs"));
	g_assert_null(find_row(entries, "Ctrl+t", "tabs"));
	g_assert_nonnull(find_row(entries, "f", "modal"));
	g_assert_nonnull(find_row(entries, "j", "modal"));

	g_object_unref(config);
	g_object_unref(mgr);
}

static void
test_empty_manager(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_new();
	g_autoptr(GPtrArray) entries = gsurf_module_manager_collect_keybinds(mgr);

	g_assert_nonnull(entries);
	g_assert_cmpuint(entries->len, ==, 0);
	g_object_unref(mgr);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/keybind-help/core-only", test_core_only);
	g_test_add_func("/gsurf/keybind-help/question-alias", test_question_alias);
	g_test_add_func("/gsurf/keybind-help/active-module-keys", test_active_module_keys);
	g_test_add_func("/gsurf/keybind-help/disabled-module-omitted", test_disabled_module_omitted);
	g_test_add_func("/gsurf/keybind-help/tabs-and-modal", test_tabs_and_modal_together);
	g_test_add_func("/gsurf/keybind-help/empty-manager", test_empty_manager);
	return g_test_run();
}
