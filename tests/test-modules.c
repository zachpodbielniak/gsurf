/*
 * test-modules.c - Module system + search_engines module
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Validates loading a .so module, configuring it from YAML, and
 * dispatching the GsurfUriHandler hook through the module manager.
 */

#include <gsurf.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static const char *TEST_YAML =
	"modules:\n"
	"  search_engines:\n"
	"    enabled: true\n"
	"    default: ddg\n"
	"    default_is_search: true\n"
	"    engines:\n"
	"      ddg:    { prefix: \"d \", url: \"https://duckduckgo.com/?q=%s\" }\n"
	"      google: { prefix: \"g \", url: \"https://www.google.com/search?q=%s\" }\n";

static char *
module_so_path(const char *name)
{
	char *exe = g_file_read_link("/proc/self/exe", NULL);
	char *dir, *path, *base;

	g_assert_nonnull(exe);
	dir = g_path_get_dirname(exe);
	base = g_strconcat(name, ".so", NULL);
	path = g_build_filename(dir, "modules", base, NULL);
	g_free(exe);
	g_free(dir);
	g_free(base);
	return path;
}

static void
test_search_engines(void)
{
	g_autoptr(GError) error = NULL;
	GsurfConfig *config;
	GsurfModuleManager *mgr;
	GsurfModule *mod;
	g_autofree char *so = module_so_path("search_engines");
	char *out;

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("search_engines.so not built");
		return;
	}

	config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(config, TEST_YAML, -1, &error));
	g_assert_no_error(error);

	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);

	mod = gsurf_module_manager_load_module(mgr, so, &error);
	g_assert_no_error(error);
	g_assert_nonnull(mod);
	g_assert_cmpstr(gsurf_module_get_name(mod), ==, "search_engines");

	gsurf_module_manager_activate_all(mgr);

	/* Prefix match -> google search. */
	out = gsurf_module_manager_dispatch_rewrite_uri(mgr, "g hello world");
	g_assert_nonnull(out);
	g_assert_true(g_str_has_prefix(out, "https://www.google.com/search?q="));
	g_assert_nonnull(strstr(out, "hello"));
	g_free(out);

	/* Bare words -> default (ddg) search. */
	out = gsurf_module_manager_dispatch_rewrite_uri(mgr, "open source browsers");
	g_assert_nonnull(out);
	g_assert_true(g_str_has_prefix(out, "https://duckduckgo.com/?q="));
	g_free(out);

	/* A real URL is left alone. */
	out = gsurf_module_manager_dispatch_rewrite_uri(mgr, "example.com");
	g_assert_null(out);

	out = gsurf_module_manager_dispatch_rewrite_uri(mgr, "https://suckless.org");
	g_assert_null(out);

	g_object_unref(mgr);
	g_object_unref(config);
}

static void
test_history(void)
{
	g_autoptr(GError) error = NULL;
	GsurfConfig *config;
	GsurfModuleManager *mgr;
	g_autofree char *so = module_so_path("history");
	g_autofree char *histfile = NULL;
	g_autofree char *yaml = NULL;
	g_autofree char *contents = NULL;

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("history.so not built");
		return;
	}

	histfile = g_build_filename(g_get_tmp_dir(), "gsurf-test-history.txt", NULL);
	g_unlink(histfile);
	yaml = g_strdup_printf(
		"modules:\n"
		"  history:\n"
		"    enabled: true\n"
		"    log_titles: false\n"
		"    file: \"%s\"\n", histfile);

	config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);

	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(mgr);

	gsurf_module_manager_dispatch_after_navigate(mgr, NULL, "https://suckless.org");
	gsurf_module_manager_dispatch_after_navigate(mgr, NULL, "about:blank");

	g_assert_true(g_file_get_contents(histfile, &contents, NULL, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(contents, "https://suckless.org"));
	/* about: URIs are skipped. */
	g_assert_null(strstr(contents, "about:blank"));

	g_unlink(histfile);
	g_object_unref(mgr);
	g_object_unref(config);
}

static void
test_adblock(void)
{
	g_autoptr(GError) error = NULL;
	GsurfConfig *config;
	GsurfModuleManager *mgr;
	g_autofree char *so = module_so_path("adblock");
	g_autofree char *hosts = NULL;
	g_autofree char *yaml = NULL;

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("adblock.so not built");
		return;
	}

	hosts = g_build_filename(g_get_tmp_dir(), "gsurf-test-hosts.txt", NULL);
	/* Mixed format: "0.0.0.0 host", bare host, a comment, blank line, and
	 * "localhost" (which must be ignored). */
	g_assert_true(g_file_set_contents(hosts,
		"# test hosts\n\n0.0.0.0 ads.example.com\nbadsite.net\n"
		"127.0.0.1 localhost\ntracker.net\n", -1, &error));
	g_assert_no_error(error);

	yaml = g_strdup_printf(
		"modules:\n"
		"  adblock:\n"
		"    enabled: true\n"
		"    host_files:\n"
		"      - \"%s\"\n"
		"    block_lists:\n"
		"      - \"inline-block.example\"\n"   /* a non-existent path: must be skipped gracefully */
		"    whitelist:\n"
		"      - \"tracker.net\"\n", hosts);

	config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);

	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(mgr);

	/* Exact host blocked. */
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL,
		"https://ads.example.com/banner.png"), ==, GSURF_POLICY_IGNORE);
	/* Subdomain of a blocked host blocked (parent-domain match). */
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL,
		"https://cdn.badsite.net/x"), ==, GSURF_POLICY_IGNORE);
	/* Whitelisted host allowed even though it is in the blocklist. */
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL,
		"https://tracker.net/t"), ==, GSURF_POLICY_USE);
	/* "localhost" must never have been added. */
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL,
		"http://localhost:8080/"), ==, GSURF_POLICY_USE);
	/* Unrelated host allowed. */
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL,
		"https://good.example.org/"), ==, GSURF_POLICY_USE);
	/* A host that merely CONTAINS a blocked host as a substring is allowed
	 * (matching is by domain label, not substring). */
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL,
		"https://notbadsite.net/x"), ==, GSURF_POLICY_USE);
	/* Edge cases: NULL, empty, non-http, and host-less URIs must not crash
	 * and default to allowing. */
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL, NULL),
		==, GSURF_POLICY_USE);
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL, ""),
		==, GSURF_POLICY_USE);
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL, "about:blank"),
		==, GSURF_POLICY_USE);
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL, "not a uri"),
		==, GSURF_POLICY_USE);

	g_unlink(hosts);
	g_object_unref(mgr);
	g_object_unref(config);
}

/* The content_filters: path: pre-built WebKit JSON files are loaded (valid,
 * missing, and empty) without disrupting host-based blocking or crashing.
 * The per-view application needs a WebKit view (covered by the GUI smoke
 * test); here we exercise the load/parse/configure path headlessly. */
static void
test_adblock_content_filters(void)
{
	g_autoptr(GError) error = NULL;
	GsurfConfig *config;
	GsurfModuleManager *mgr;
	g_autofree char *so = module_so_path("adblock");
	g_autofree char *valid = NULL, *empty = NULL, *missing = NULL;
	g_autofree char *hosts = NULL, *yaml = NULL;

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("adblock.so not built");
		return;
	}

	valid = g_build_filename(g_get_tmp_dir(), "gsurf-cf-valid.json", NULL);
	empty = g_build_filename(g_get_tmp_dir(), "gsurf-cf-empty.json", NULL);
	missing = g_build_filename(g_get_tmp_dir(), "gsurf-cf-does-not-exist.json", NULL);
	hosts = g_build_filename(g_get_tmp_dir(), "gsurf-cf-hosts.txt", NULL);

	g_assert_true(g_file_set_contents(valid,
		"[{\"trigger\":{\"url-filter\":\"tracker\\\\.example\"},"
		"\"action\":{\"type\":\"block\"}}]\n", -1, &error));
	g_assert_no_error(error);
	g_assert_true(g_file_set_contents(empty, "", -1, &error));
	g_assert_no_error(error);
	g_assert_true(g_file_set_contents(hosts, "ads.example.com\n", -1, &error));
	g_assert_no_error(error);
	g_unlink(missing);   /* ensure it does not exist */

	yaml = g_strdup_printf(
		"modules:\n"
		"  adblock:\n"
		"    enabled: true\n"
		"    host_files:\n"
		"      - \"%s\"\n"
		"    content_filters:\n"
		"      - \"%s\"\n"   /* valid */
		"      - \"%s\"\n"   /* empty -> skipped */
		"      - \"%s\"\n",  /* missing -> skipped */
		hosts, valid, empty, missing);

	config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);

	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);
	/* configure() runs load_content_filter on all three entries here. */
	gsurf_module_manager_activate_all(mgr);

	/* Host-based blocking still works alongside content_filters. */
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL,
		"https://ads.example.com/x"), ==, GSURF_POLICY_IGNORE);

	/* The script-injector dispatch with a NULL view is a no-op (does not
	 * crash) — the real application path is exercised by make test-gui. */
	gsurf_module_manager_dispatch_inject_scripts(mgr, NULL, NULL);

	g_unlink(valid);
	g_unlink(empty);
	g_unlink(hosts);
	g_object_unref(mgr);
	g_object_unref(config);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/modules/search-engines", test_search_engines);
	g_test_add_func("/gsurf/modules/history", test_history);
	g_test_add_func("/gsurf/modules/adblock", test_adblock);
	g_test_add_func("/gsurf/modules/adblock-content-filters", test_adblock_content_filters);
	return g_test_run();
}
