/*
 * test-manager.c - Module manager mechanics and hook dispatch
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exercises the manager's loading, enabled gating, priority, input
 * passthrough, default verdicts, and every hook-dispatch path through
 * the real compiled module .so files (with a NULL view, which each hook
 * tolerates).
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
	g_free(exe); g_free(dir); g_free(base);
	return path;
}

/* Build a manager with one module loaded + activated from inline YAML.
 * Returns NULL if the module .so is not present (caller skips). */
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

/* ---- manager mechanics ---- */

static void
test_defaults_no_modules(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_new();
	gchar *path;

	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL, "https://x"), ==, GSURF_POLICY_USE);
	g_assert_cmpint(gsurf_module_manager_dispatch_permission(mgr, NULL, "geolocation", NULL), ==, GSURF_PERMISSION_PROMPT);
	g_assert_cmpint(gsurf_module_manager_dispatch_verify_cert(mgr, "h", 0), ==, GSURF_TLS_PROMPT);
	path = gsurf_module_manager_dispatch_decide_destination(mgr, "u", "f");
	g_assert_null(path);
	g_assert_null(gsurf_module_manager_dispatch_rewrite_uri(mgr, "x"));
	/* These must not crash with no modules / no view. */
	gsurf_module_manager_dispatch_after_navigate(mgr, NULL, "https://x");
	gsurf_module_manager_dispatch_inject_scripts(mgr, NULL, NULL);

	g_object_unref(mgr);
}

/* The newer dispatch hooks (request-filter, status, context-menu, render
 * overlay) return pass-through defaults and never crash with no modules. */
static void
test_new_hook_defaults(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_new();
	g_autoptr(GsurfHitTest) hit = gsurf_hit_test_new();
	GPtrArray *items = g_ptr_array_new();
	gchar *redirect = NULL;

	g_assert_cmpint(gsurf_module_manager_dispatch_filter_request(mgr, NULL, "https://x", &redirect),
		==, GSURF_FILTER_ALLOW);
	g_assert_null(gsurf_module_manager_dispatch_status_text(mgr, NULL));
	gsurf_module_manager_dispatch_populate_menu(mgr, hit, items);
	g_assert_cmpuint(items->len, ==, 0);
	gsurf_module_manager_dispatch_render_overlay(mgr, NULL, NULL);

	g_ptr_array_unref(items);
	g_object_unref(mgr);
}

static void
test_passthrough_flag(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_new();
	g_assert_false(gsurf_module_manager_get_input_passthrough(mgr));
	gsurf_module_manager_set_input_passthrough(mgr, TRUE);
	g_assert_true(gsurf_module_manager_get_input_passthrough(mgr));
	gsurf_module_manager_set_input_passthrough(mgr, FALSE);
	g_assert_false(gsurf_module_manager_get_input_passthrough(mgr));
	g_object_unref(mgr);
}

static void
test_load_bad_path(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_new();
	g_autoptr(GError) error = NULL;

	g_assert_null(gsurf_module_manager_load_module(mgr, "/nonexistent/x.so", &error));
	g_assert_nonnull(error);
	g_object_unref(mgr);
}

/* Programmatic search-path management (the embedding API). */
static void
test_search_paths(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_new();
	GPtrArray *paths;
	const gchar *set[] = { "/one", "/two", "/three", NULL };

	paths = gsurf_module_manager_get_search_paths(mgr);
	g_assert_nonnull(paths);
	g_assert_cmpuint(paths->len, ==, 0);

	/* Append + dedup + NULL/empty are ignored. */
	gsurf_module_manager_add_search_path(mgr, "/a");
	gsurf_module_manager_add_search_path(mgr, "/a");   /* duplicate */
	gsurf_module_manager_add_search_path(mgr, NULL);
	gsurf_module_manager_add_search_path(mgr, "");
	gsurf_module_manager_add_search_path(mgr, "/b");
	g_assert_cmpuint(paths->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(paths, 0), ==, "/a");
	g_assert_cmpstr(g_ptr_array_index(paths, 1), ==, "/b");

	/* set replaces wholesale and preserves order. */
	gsurf_module_manager_set_search_paths(mgr, set);
	g_assert_cmpuint(paths->len, ==, 3);
	g_assert_cmpstr(g_ptr_array_index(paths, 2), ==, "/three");

	/* clear + set(NULL) both empty it. */
	gsurf_module_manager_clear_search_paths(mgr);
	g_assert_cmpuint(paths->len, ==, 0);
	gsurf_module_manager_set_search_paths(mgr, set);
	gsurf_module_manager_set_search_paths(mgr, NULL);
	g_assert_cmpuint(paths->len, ==, 0);

	g_assert_nonnull(gsurf_module_manager_get_system_module_dir());

	/* load_modules with no search paths loads nothing and does not crash. */
	g_assert_cmpuint(gsurf_module_manager_load_modules(mgr), ==, 0);

	g_object_unref(mgr);
}

/* load_modules pulls .so files from a configured search path. */
static void
test_load_modules_from_path(void)
{
	g_autofree char *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree char *dir = g_path_get_dirname(exe);
	g_autofree char *moddir = g_build_filename(dir, "modules", NULL);
	GsurfModuleManager *mgr;
	guint n;

	if (!g_file_test(moddir, G_FILE_TEST_IS_DIR)) {
		g_test_skip("modules dir not built");
		return;
	}

	mgr = gsurf_module_manager_new();
	gsurf_module_manager_add_search_path(mgr, moddir);
	n = gsurf_module_manager_load_modules(mgr);
	g_assert_cmpuint(n, >, 0);
	/* A known module resolved by name. */
	g_assert_nonnull(gsurf_module_manager_get_module(mgr, "search_engines"));

	g_object_unref(mgr);
}

static void
test_priority_api(void)
{
	GsurfModuleManager *mgr = mgr_with("search_engines",
		"modules:\n  search_engines:\n    enabled: true\n");
	GsurfModule *m;

	if (mgr == NULL) { g_test_skip("search_engines.so not built"); return; }
	m = gsurf_module_manager_get_module(mgr, "search_engines");
	g_assert_nonnull(m);
	g_assert_cmpint(gsurf_module_get_priority(m), ==, GSURF_PRIORITY_DEFAULT);
	gsurf_module_set_priority(m, 10);
	g_assert_cmpint(gsurf_module_get_priority(m), ==, 10);
	g_assert_true(gsurf_module_get_enabled(m));
	g_assert_true(gsurf_module_is_active(m));
	g_object_unref(mgr);
}

static void
test_enabled_gating(void)
{
	GsurfModuleManager *mgr = mgr_with("search_engines",
		"modules:\n  search_engines:\n    enabled: false\n"
		"    engines:\n      g: { prefix: \"g \", url: \"https://g/?q=%s\" }\n");
	GsurfModule *m;
	gchar *out;

	if (mgr == NULL) { g_test_skip("search_engines.so not built"); return; }
	m = gsurf_module_manager_get_module(mgr, "search_engines");
	g_assert_nonnull(m);
	g_assert_false(gsurf_module_get_enabled(m));
	g_assert_false(gsurf_module_is_active(m));
	/* Inactive module must not participate in dispatch. */
	out = gsurf_module_manager_dispatch_rewrite_uri(mgr, "g hi");
	g_assert_null(out);
	g_object_unref(mgr);
}

/* ---- hook dispatch through real modules ---- */

static void
test_uri_params(void)
{
	GsurfModuleManager *mgr = mgr_with("uri_params",
		"modules:\n  uri_params:\n    enabled: true\n"
		"    rules:\n      - regex: \"example\"\n        settings:\n          javascript: false\n");
	g_autoptr(GsurfSettings) match = NULL;
	g_autoptr(GsurfSettings) nomatch = NULL;

	if (mgr == NULL) { g_test_skip("uri_params.so not built"); return; }
	match = gsurf_settings_new();
	nomatch = gsurf_settings_new();
	g_assert_true(match->javascript);

	gsurf_module_manager_dispatch_apply_settings(mgr, NULL, match, "https://example.com/");
	g_assert_false(match->javascript);                 /* rule matched -> overridden */

	gsurf_module_manager_dispatch_apply_settings(mgr, NULL, nomatch, "https://other.org/");
	g_assert_true(nomatch->javascript);                /* no match -> unchanged */
	g_object_unref(mgr);
}

static void
test_useragent(void)
{
	GsurfModuleManager *mgr = mgr_with("useragent",
		"modules:\n  useragent:\n    enabled: true\n    default: \"DefaultUA\"\n"
		"    presets:\n      firefox: \"FF/1\"\n"
		"    rules:\n      - regex: \"google\"\n        ua: firefox\n");
	g_autoptr(GsurfSettings) a = NULL;
	g_autoptr(GsurfSettings) b = NULL;

	if (mgr == NULL) { g_test_skip("useragent.so not built"); return; }
	a = gsurf_settings_new();
	b = gsurf_settings_new();

	gsurf_module_manager_dispatch_apply_settings(mgr, NULL, a, "https://google.com/");
	g_assert_cmpstr(a->user_agent, ==, "FF/1");        /* preset resolved */

	gsurf_module_manager_dispatch_apply_settings(mgr, NULL, b, "https://elsewhere.net/");
	g_assert_cmpstr(b->user_agent, ==, "DefaultUA");   /* default fallback */
	g_object_unref(mgr);
}

static void
test_downloads(void)
{
	g_autofree char *dir = g_build_filename(g_get_tmp_dir(), "gsurf-dltest", NULL);
	g_autofree char *yaml = g_strdup_printf(
		"modules:\n  downloads:\n    enabled: true\n    dir: \"%s\"\n", dir);
	GsurfModuleManager *mgr = mgr_with("downloads", yaml);
	g_autofree char *expected = NULL;
	gchar *dest;

	if (mgr == NULL) { g_test_skip("downloads.so not built"); return; }
	dest = gsurf_module_manager_dispatch_decide_destination(mgr, "https://x/file.bin", "file.bin");
	expected = g_build_filename(dir, "file.bin", NULL);
	g_assert_cmpstr(dest, ==, expected);
	g_free(dest);
	g_rmdir(dir);
	g_object_unref(mgr);
}

static void
test_cert_manager(void)
{
	GsurfModuleManager *mgr = mgr_with("cert_manager",
		"modules:\n  cert_manager:\n    enabled: true\n"
		"    pins:\n      \"self.example\": \"sha256//AAAA\"\n");

	if (mgr == NULL) { g_test_skip("cert_manager.so not built"); return; }
	g_assert_cmpint(gsurf_module_manager_dispatch_verify_cert(mgr, "self.example", 0), ==, GSURF_TLS_PROCEED);
	g_assert_cmpint(gsurf_module_manager_dispatch_verify_cert(mgr, "other.example", 0), ==, GSURF_TLS_PROMPT);
	g_object_unref(mgr);
}

static void
test_geolocation(void)
{
	GsurfModuleManager *mgr = mgr_with("geolocation",
		"modules:\n  geolocation:\n    enabled: true\n    default: deny\n");

	if (mgr == NULL) { g_test_skip("geolocation.so not built"); return; }
	g_assert_cmpint(gsurf_module_manager_dispatch_permission(mgr, NULL, "geolocation", NULL), ==, GSURF_PERMISSION_DENY);
	/* It only governs geolocation; other types fall through to PROMPT. */
	g_assert_cmpint(gsurf_module_manager_dispatch_permission(mgr, NULL, "notification", NULL), ==, GSURF_PERMISSION_PROMPT);
	g_object_unref(mgr);
}

static void
test_notifications(void)
{
	GsurfModuleManager *mgr = mgr_with("notifications",
		"modules:\n  notifications:\n    enabled: true\n    allow_web_notifications: deny\n");

	if (mgr == NULL) { g_test_skip("notifications.so not built"); return; }
	g_assert_cmpint(gsurf_module_manager_dispatch_permission(mgr, NULL, "notification", NULL), ==, GSURF_PERMISSION_DENY);
	g_object_unref(mgr);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/manager/defaults-no-modules", test_defaults_no_modules);
	g_test_add_func("/gsurf/manager/new-hook-defaults", test_new_hook_defaults);
	g_test_add_func("/gsurf/manager/passthrough-flag", test_passthrough_flag);
	g_test_add_func("/gsurf/manager/load-bad-path", test_load_bad_path);
	g_test_add_func("/gsurf/manager/search-paths", test_search_paths);
	g_test_add_func("/gsurf/manager/load-modules-from-path", test_load_modules_from_path);
	g_test_add_func("/gsurf/manager/priority-api", test_priority_api);
	g_test_add_func("/gsurf/manager/enabled-gating", test_enabled_gating);
	g_test_add_func("/gsurf/manager/uri-params", test_uri_params);
	g_test_add_func("/gsurf/manager/useragent", test_useragent);
	g_test_add_func("/gsurf/manager/downloads", test_downloads);
	g_test_add_func("/gsurf/manager/cert-manager", test_cert_manager);
	g_test_add_func("/gsurf/manager/geolocation", test_geolocation);
	g_test_add_func("/gsurf/manager/notifications", test_notifications);
	return g_test_run();
}
