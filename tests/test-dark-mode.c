/*
 * test-dark-mode.c - dark_mode module configuration, state and hooks
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Headless tests of the real dark_mode.so: defaults, the legacy mode
 * mapping, every key and its validation (bad values are dropped with a
 * warning, the rest of the section still applies), the site rules as the
 * page engine receives them, reconfiguration, the per-site state file
 * (load, save, permissions, malformed input), the toggles through the
 * action, key and keybind-help hooks, and script injection into a
 * recording headless GsurfView (worlds, live updates, on-demand Dark
 * Reader, view and module lifetimes), and the toggles module forwarding
 * its legacy dark_mode key. Every test case runs with its own
 * temporary XDG directories (G_TEST_OPTION_ISOLATE_DIRS).
 */

#include <gsurf.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>

#define DOMAIN "gsurf-dark-mode"
#define WORLD  "gsurf-dark-mode"

/* --- recording headless view --------------------------------------- */

typedef struct {
	GsurfView  parent;
	gchar     *uri;
	GPtrArray *scripts;    /* "world|source" added as user scripts */
	GPtrArray *evaluated;  /* "world|source" run in the main frame */
	GPtrArray *messages;   /* "world|name" registered handlers */
} TestView;
typedef struct { GsurfViewClass parent; } TestViewClass;

GType test_view_get_type(void);
G_DEFINE_TYPE(TestView, test_view, GSURF_TYPE_VIEW)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(TestView, g_object_unref)

static const gchar *
test_view_get_uri(GsurfView *view)
{
	return ((TestView *)view)->uri;
}

static void
test_view_add_user_script_in_world(GsurfView *view, const gchar *source, gboolean at_end,
                                   const gchar *const *allow, const gchar *world)
{
	TestView *self = (TestView *)view;

	g_assert_false(at_end);
	g_assert_null(allow);
	g_ptr_array_add(self->scripts, g_strconcat(world, "|", source, NULL));
}

static void
test_view_register_script_message(GsurfView *view, const gchar *name, const gchar *world)
{
	TestView *self = (TestView *)view;

	g_ptr_array_add(self->messages, g_strconcat(world, "|", name, NULL));
}

static void
test_view_run_in_world_async(GsurfView *view, const gchar *script, const gchar *world,
                             GCancellable *cancellable, GAsyncReadyCallback callback,
                             gpointer user_data)
{
	TestView *self = (TestView *)view;
	GTask *task = g_task_new(view, cancellable, callback, user_data);

	g_ptr_array_add(self->evaluated, g_strconcat(world, "|", script, NULL));
	g_task_return_pointer(task, g_strdup("null"), g_free);
	g_object_unref(task);
}

static gchar *
test_view_run_in_world_finish(GsurfView *view, GAsyncResult *result, GError **error)
{
	(void)view;
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
test_view_finalize(GObject *object)
{
	TestView *self = (TestView *)object;

	g_free(self->uri);
	g_ptr_array_unref(self->scripts);
	g_ptr_array_unref(self->evaluated);
	g_ptr_array_unref(self->messages);
	G_OBJECT_CLASS(test_view_parent_class)->finalize(object);
}

static void
test_view_class_init(TestViewClass *klass)
{
	GsurfViewClass *view_class = GSURF_VIEW_CLASS(klass);

	G_OBJECT_CLASS(klass)->finalize = test_view_finalize;
	view_class->get_uri = test_view_get_uri;
	view_class->add_user_script_in_world = test_view_add_user_script_in_world;
	view_class->register_script_message = test_view_register_script_message;
	view_class->run_javascript_in_world_async = test_view_run_in_world_async;
	view_class->run_javascript_in_world_finish = test_view_run_in_world_finish;
}

static void
test_view_init(TestView *self)
{
	self->scripts = g_ptr_array_new_with_free_func(g_free);
	self->evaluated = g_ptr_array_new_with_free_func(g_free);
	self->messages = g_ptr_array_new_with_free_func(g_free);
}

static TestView *
new_view(const gchar *uri)
{
	TestView *view = g_object_new(test_view_get_type(), NULL);

	view->uri = g_strdup(uri);
	return view;
}

/* Let GTask callbacks (live updates) run. */
static void
drain(void)
{
	while (g_main_context_iteration(NULL, FALSE))
		;
}

/* --- module helpers -------------------------------------------------- */

static gchar *
module_so_path(const gchar *name)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = NULL;
	g_autofree gchar *base = g_strconcat(name, ".so", NULL);

	g_assert_nonnull(exe);
	dir = g_path_get_dirname(exe);
	return g_build_filename(dir, "modules", base, NULL);
}

static GsurfConfig *
config_for(const gchar *section)
{
	g_autofree gchar *yaml = g_strdup_printf("modules:\n  dark_mode:\n    enabled: true\n%s",
		section != NULL ? section : "");
	g_autoptr(GError) error = NULL;
	GsurfConfig *config = gsurf_config_new();

	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	return config;
}

/* A manager with the real dark_mode.so loaded, configured from @section
 * (lines indented under modules: dark_mode:) and activated. */
static GsurfModuleManager *
mgr_with(const gchar *section)
{
	g_autofree gchar *so = module_so_path("dark_mode");
	g_autoptr(GsurfConfig) config = config_for(section);
	g_autoptr(GError) error = NULL;
	GsurfModuleManager *mgr;

	g_assert_true(g_file_test(so, G_FILE_TEST_EXISTS));
	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(mgr);
	return mgr;
}

static GsurfModule *
module_of(GsurfModuleManager *mgr)
{
	GsurfModule *module = gsurf_module_manager_get_module(mgr, "dark_mode");

	g_assert_nonnull(module);
	return module;
}

static void
reconfigure(GsurfModule *module, const gchar *section)
{
	g_autoptr(GsurfConfig) config = config_for(section);

	gsurf_module_configure(module, config);
}

/* The state JSON without its "version" counter, for exact comparison. */
static gchar *
state_of(GsurfModule *module)
{
	g_autofree gchar *json = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonGenerator) gen = json_generator_new();
	JsonNode *root;

	g_object_get(module, "state-json", &json, NULL);
	g_assert_nonnull(json);
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	root = json_parser_get_root(parser);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(root));
	g_assert_true(json_object_has_member(json_node_get_object(root), "version"));
	json_object_remove_member(json_node_get_object(root), "version");
	json_generator_set_root(gen, root);
	return json_generator_to_data(gen, NULL);
}

static gint64
version_of(GsurfModule *module)
{
	g_autofree gchar *json = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();

	g_object_get(module, "state-json", &json, NULL);
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	return json_object_get_int_member(json_node_get_object(json_parser_get_root(parser)),
		"version");
}

#define NEUTRAL "\"brightness\":100,\"contrast\":100,\"sepia\":0,\"grayscale\":0"

/* A whole state with the given global and rules/overrides JSON. */
static gchar *
expect_state(const gchar *global, const gchar *rules, const gchar *overrides)
{
	return g_strdup_printf("{\"enabled\":true,\"fallback\":\"filter\",\"global\":%s,"
		"\"rules\":%s,\"overrides\":%s}", global, rules, overrides);
}

static gchar *
state_path(void)
{
	return g_build_filename(g_get_user_data_dir(), "gsurf", "dark_mode", "sites.yaml", NULL);
}

static void
write_state(const gchar *contents, gint mode)
{
	g_autofree gchar *path = state_path();
	g_autofree gchar *dir = g_path_get_dirname(path);

	g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
	g_assert_true(g_file_set_contents(path, contents, -1, NULL));
	g_assert_cmpint(g_chmod(path, mode), ==, 0);
}

static gint
file_mode(const gchar *path)
{
	GStatBuf st;

	g_assert_cmpint(g_stat(path, &st), ==, 0);
	return (gint)(st.st_mode & 0777);
}

/* --- configuration -------------------------------------------------- */

static void
test_defaults(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with(NULL);
	GsurfModule *module = module_of(mgr);
	g_autofree gchar *state = state_of(module);
	g_autofree gchar *want = expect_state(
		"{\"mode\":\"native\",\"scheme\":\"light dark\"," NEUTRAL "}", "[]", "{}");
	g_autofree gchar *key = NULL;
	g_autofree gchar *key_site = NULL;
	g_autofree gchar *want_key = gsurf_keys_normalize("Ctrl+Alt+d");
	g_autofree gchar *want_site = gsurf_keys_normalize("Ctrl+Alt+Shift+d");
	g_autofree gchar *file = NULL;
	g_autofree gchar *want_file = state_path();
	gboolean on = FALSE, preload = TRUE;

	/* Missing mode keeps the historical default: legacy auto. */
	g_assert_cmpstr(state, ==, want);
	g_object_get(module, "key-toggle", &key, "key-toggle-site", &key_site,
		"state-file", &file, "global-on", &on, "preload-engine", &preload, NULL);
	g_assert_cmpstr(key, ==, want_key);
	g_assert_cmpstr(key_site, ==, want_site);
	g_assert_cmpstr(file, ==, want_file);
	g_assert_true(on);
	g_assert_false(preload);
	g_assert_cmpstr(gsurf_module_get_name(module), ==, "dark_mode");
	/* Nothing is written until a site is toggled. */
	g_assert_false(g_file_test(file, G_FILE_TEST_EXISTS));
}

static void
check_mode(const gchar *section, const gchar *global)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with(section);
	g_autofree gchar *state = state_of(module_of(mgr));
	g_autofree gchar *want = expect_state(global, "[]", "{}");

	g_assert_cmpstr(state, ==, want);
}

static void
test_legacy_mapping(void)
{
	check_mode("    mode: dark\n", "{\"mode\":\"native\",\"scheme\":\"dark\"," NEUTRAL "}");
	check_mode("    mode: dark\n    force_dark_css: true\n",
		"{\"mode\":\"filter\",\"scheme\":\"dark\"," NEUTRAL "}");
	/* Order does not matter: force_dark_css is read first. */
	check_mode("    force_dark_css: yes\n    mode: dark\n",
		"{\"mode\":\"filter\",\"scheme\":\"dark\"," NEUTRAL "}");
	check_mode("    mode: dark\n    force_dark_css: false\n",
		"{\"mode\":\"native\",\"scheme\":\"dark\"," NEUTRAL "}");
	check_mode("    mode: auto\n", "{\"mode\":\"native\",\"scheme\":\"light dark\"," NEUTRAL "}");
	check_mode("    mode: light\n", "{\"mode\":\"off\",\"scheme\":\"dark\"," NEUTRAL "}");
	/* force_dark_css only matters for legacy dark. */
	check_mode("    mode: native\n    force_dark_css: true\n",
		"{\"mode\":\"native\",\"scheme\":\"dark\"," NEUTRAL "}");
	/* Mode names are case-insensitive. */
	check_mode("    mode: DARK\n", "{\"mode\":\"native\",\"scheme\":\"dark\"," NEUTRAL "}");
}

static void
test_modes_and_adjustments(void)
{
	check_mode("    mode: off\n", "{\"mode\":\"off\",\"scheme\":\"dark\"," NEUTRAL "}");
	check_mode("    mode: native\n", "{\"mode\":\"native\",\"scheme\":\"dark\"," NEUTRAL "}");
	check_mode("    mode: Filter\n", "{\"mode\":\"filter\",\"scheme\":\"dark\"," NEUTRAL "}");
	check_mode("    mode: filter\n    brightness: 90\n    contrast: 110\n"
		"    sepia: 10\n    grayscale: 20\n",
		"{\"mode\":\"filter\",\"scheme\":\"dark\",\"brightness\":90,\"contrast\":110,"
		"\"sepia\":10,\"grayscale\":20}");
	/* Range edges are accepted. */
	check_mode("    mode: filter\n    brightness: 0\n    contrast: 200\n"
		"    sepia: 100\n    grayscale: 0\n",
		"{\"mode\":\"filter\",\"scheme\":\"dark\",\"brightness\":0,\"contrast\":200,"
		"\"sepia\":100,\"grayscale\":0}");

	/* Only dynamic preloads Dark Reader. */
	{
		g_autoptr(GsurfModuleManager) mgr = mgr_with("    mode: dynamic\n");
		gboolean preload = FALSE;

		g_object_get(module_of(mgr), "preload-engine", &preload, NULL);
		g_assert_true(preload);
	}
}

static void
expect_warning(const gchar *pattern)
{
	g_test_expect_message(DOMAIN, G_LOG_LEVEL_WARNING, pattern);
}

static void
test_validation(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *want = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *key_site = NULL;
	g_autofree gchar *want_key = gsurf_keys_normalize("Ctrl+Alt+d");

	/* Expected in the order the module reads the section: force_dark_css
	 * first, then every key in file order, then the rules. */
	expect_warning("dark_mode.force_dark_css: expected a boolean");
	expect_warning("dark_mode.mode: unknown mode 'bogus'*");
	expect_warning("dark_mode.brightness: 201 is outside 0-200");
	expect_warning("dark_mode.contrast: -1 is outside 0-200");
	expect_warning("dark_mode.sepia: 101 is outside 0-100");
	expect_warning("dark_mode.grayscale: expected an integer percentage");
	expect_warning("dark_mode: unknown key 'colour'");
	expect_warning("dark_mode.key_toggle: expected a key string*");
	expect_warning("dark_mode.sites[0]: expected a mapping with a \"match\"*");
	expect_warning("dark_mode.sites[1]: expected a mapping with a \"match\"*");
	expect_warning("dark_mode.sites[2]: *");          /* bad regex */
	expect_warning("dark_mode.sites[3].brightness: expected an integer percentage");
	expect_warning("dark_mode.sites[3]: unknown key 'colour'");
	expect_warning("dark_mode.sites[3].mode: expected one of*");
	mgr = mgr_with(
		"    force_dark_css: maybe\n"
		"    mode: bogus\n"
		"    brightness: 201\n"
		"    contrast: -1\n"
		"    sepia: 101\n"
		"    grayscale: abc\n"
		"    colour: blue\n"
		"    key_toggle: [Ctrl+x]\n"
		"    key_toggle_site: ''\n"
		"    sites:\n"
		"      - [a, b]\n"
		"      - mode: off\n"
		"      - match: '('\n"
		"        mode: off\n"
		"      - match: 'news'\n"
		"        brightness: 1.5\n"
		"        sepia: 10\n"
		"        colour: red\n"
		"        mode: [x]\n"
		"      - 'bare\\.example'\n");
	g_test_assert_expected_messages();

	/* Every bad value fell back to its default; the good parts stayed:
	 * the "news" rule keeps sepia, the bare pattern is a rule too. */
	state = state_of(module_of(mgr));
	want = expect_state("{\"mode\":\"native\",\"scheme\":\"light dark\"," NEUTRAL "}",
		"[{\"pattern\":\"news\",\"sepia\":10},{\"pattern\":\"bare\\\\.example\"}]", "{}");
	g_assert_cmpstr(state, ==, want);
	g_object_get(module_of(mgr), "key-toggle", &key, "key-toggle-site", &key_site, NULL);
	g_assert_cmpstr(key, ==, want_key);   /* bad value: default kept */
	g_assert_null(key_site);              /* "": unbound */
}

static void
test_bad_shapes(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *want = expect_state(
		"{\"mode\":\"native\",\"scheme\":\"light dark\"," NEUTRAL "}", "[]", "{}");
	g_autoptr(GsurfConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	/* sites must be a list. */
	expect_warning("dark_mode.sites: expected a list of rules");
	mgr = mgr_with("    sites: '(^|\\.)example\\.org$'\n");
	g_test_assert_expected_messages();
	state = state_of(module_of(mgr));
	g_assert_cmpstr(state, ==, want);

	/* The section itself must be a mapping. */
	config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(config,
		"modules:\n  dark_mode: on\n", -1, &error));
	g_assert_no_error(error);
	expect_warning("dark_mode: expected a mapping; using the defaults");
	gsurf_module_configure(module_of(mgr), config);
	g_test_assert_expected_messages();
	g_clear_pointer(&state, g_free);
	state = state_of(module_of(mgr));
	g_assert_cmpstr(state, ==, want);

	/* A null key binding unbinds it too. */
	reconfigure(module_of(mgr), "    key_toggle: null\n    key_toggle_site: ~\n");
	{
		g_autofree gchar *key = NULL;
		g_autofree gchar *key_site = NULL;

		g_object_get(module_of(mgr), "key-toggle", &key, "key-toggle-site", &key_site, NULL);
		g_assert_null(key);
		g_assert_null(key_site);
	}
}

static void
test_site_rules_json(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with(
		"    mode: filter\n"
		"    brightness: 100\n"
		"    contrast: 100\n"
		"    sepia: 0\n"
		"    grayscale: 0\n"
		"    key_toggle: Ctrl+Alt+d\n"
		"    key_toggle_site: Ctrl+Alt+Shift+d\n"
		"    sites:\n"
		"      - match: '(^|\\.)github\\.com$'\n"
		"        mode: native\n"
		"      - match: '(^|\\.)example\\.org$'\n"
		"        mode: off\n"
		"      - match: 'news'\n"
		"        brightness: 90\n"
		"        sepia: 10\n"
		"      - match: '^old\\.example$'\n"
		"        mode: dark\n"
		"        force_dark_css: true\n"
		"      - match: '^sys\\.example$'\n"
		"        mode: auto\n"
		"      - match: '^dr\\.example$'\n"
		"        mode: dynamic\n"
		"        grayscale: 100\n");
	g_autofree gchar *state = state_of(module_of(mgr));
	g_autofree gchar *want = expect_state(
		"{\"mode\":\"filter\",\"scheme\":\"dark\"," NEUTRAL "}",
		"[{\"pattern\":\"(^|\\\\.)github\\\\.com$\",\"mode\":\"native\",\"scheme\":\"dark\"},"
		"{\"pattern\":\"(^|\\\\.)example\\\\.org$\",\"mode\":\"off\",\"scheme\":\"dark\"},"
		"{\"pattern\":\"news\",\"brightness\":90,\"sepia\":10},"
		"{\"pattern\":\"^old\\\\.example$\",\"mode\":\"filter\",\"scheme\":\"dark\"},"
		"{\"pattern\":\"^sys\\\\.example$\",\"mode\":\"native\",\"scheme\":\"light dark\"},"
		"{\"pattern\":\"^dr\\\\.example$\",\"mode\":\"dynamic\",\"scheme\":\"dark\","
		"\"grayscale\":100}]",
		"{}");
	gboolean preload = TRUE;

	g_assert_cmpstr(state, ==, want);
	/* A dynamic rule alone does not preload (loaded on demand). */
	g_object_get(module_of(mgr), "preload-engine", &preload, NULL);
	g_assert_false(preload);
}

static void
test_reconfigure(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with(
		"    mode: dynamic\n"
		"    sepia: 30\n"
		"    key_toggle: Ctrl+Alt+F1\n"
		"    key_toggle_site: ''\n"
		"    sites:\n"
		"      - match: 'one'\n"
		"        mode: off\n");
	GsurfModule *module = module_of(mgr);
	g_autofree gchar *defaults = expect_state(
		"{\"mode\":\"native\",\"scheme\":\"light dark\"," NEUTRAL "}", "[]", "{}");
	g_autofree gchar *want_key = gsurf_keys_normalize("Ctrl+Alt+d");
	g_autofree gchar *want_site = gsurf_keys_normalize("Ctrl+Alt+Shift+d");
	gint64 before = version_of(module);
	gchar *state, *key = NULL, *key_site = NULL;
	gboolean preload = FALSE;

	/* 1. A second configure() with another section replaces everything. */
	reconfigure(module, "    mode: filter\n    sites:\n      - match: 'two'\n");
	state = state_of(module);
	g_assert_cmpstr(state, ==,
		"{\"enabled\":true,\"fallback\":\"filter\",\"global\":{\"mode\":\"filter\","
		"\"scheme\":\"dark\"," NEUTRAL "},\"rules\":[{\"pattern\":\"two\"}],\"overrides\":{}}");
	g_free(state);
	g_assert_cmpint(version_of(module), >, before);

	/* 2. And an empty one goes back to the defaults, keys included. */
	reconfigure(module, NULL);
	state = state_of(module);
	g_assert_cmpstr(state, ==, defaults);
	g_free(state);
	g_object_get(module, "key-toggle", &key, "key-toggle-site", &key_site,
		"preload-engine", &preload, NULL);
	g_assert_cmpstr(key, ==, want_key);
	g_assert_cmpstr(key_site, ==, want_site);
	g_assert_false(preload);
	g_free(key);
	g_free(key_site);

	/* 3. So does a config without a dark_mode section at all. */
	reconfigure(module, "    mode: off\n");
	{
		g_autoptr(GsurfConfig) config = gsurf_config_new();

		g_assert_true(gsurf_config_load_from_data(config, "modules: {}\n", -1, NULL));
		gsurf_module_configure(module, config);
	}
	state = state_of(module);
	g_assert_cmpstr(state, ==, defaults);
	g_free(state);

	/* 4. The global switch is runtime state and survives configure(). */
	g_object_set(module, "global-on", FALSE, NULL);
	reconfigure(module, "    mode: filter\n");
	state = state_of(module);
	g_assert_nonnull(strstr(state, "\"enabled\":false"));
	g_free(state);
}

/* --- state file ------------------------------------------------------ */

static void
test_state_file_load(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *want = NULL;

	/* Hosts are normalized; bad entries are skipped one by one. */
	write_state(
		"version: 1\n"
		"sites:\n"
		"  Example.ORG.: false\n"
		"  github.com: true\n"
		"  '[::1]': on\n"
		"  maybe.example: perhaps\n"
		"  list.example: [1]\n"
		"  '': true\n", 0600);
	expect_warning("*sites.yaml: maybe.example: expected true or false");
	expect_warning("*sites.yaml: list.example: expected true or false");
	expect_warning("*sites.yaml: empty host name skipped");
	mgr = mgr_with("    mode: filter\n");
	g_test_assert_expected_messages();
	state = state_of(module_of(mgr));
	want = expect_state("{\"mode\":\"filter\",\"scheme\":\"dark\"," NEUTRAL "}", "[]",
		"{\"::1\":true,\"example.org\":false,\"github.com\":true}");
	g_assert_cmpstr(state, ==, want);

	/* Reconfigure re-reads the file (edited behind our back). */
	write_state("sites:\n  other.example: true\n", 0600);
	reconfigure(module_of(mgr), "    mode: filter\n");
	g_clear_pointer(&state, g_free);
	state = state_of(module_of(mgr));
	g_assert_nonnull(strstr(state, "\"overrides\":{\"other.example\":true}"));

	/* An empty file, or one without sites, means no overrides. */
	write_state("", 0600);
	reconfigure(module_of(mgr), "    mode: filter\n");
	g_clear_pointer(&state, g_free);
	state = state_of(module_of(mgr));
	g_assert_nonnull(strstr(state, "\"overrides\":{}"));
	write_state("version: 1\n", 0600);
	reconfigure(module_of(mgr), "    mode: filter\n");
	g_clear_pointer(&state, g_free);
	state = state_of(module_of(mgr));
	g_assert_nonnull(strstr(state, "\"overrides\":{}"));
}

static void
test_state_file_permissions(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autofree gchar *path = state_path();
	g_autofree gchar *state = NULL;
	g_autoptr(TestView) view = new_view("https://loose.example/");

	/* A world-readable file is still used, with a warning... */
	write_state("sites:\n  loose.example: false\n", 0644);
	expect_warning("*sites.yaml: *; it is rewritten with mode 0600 on the next save");
	mgr = mgr_with("    mode: filter\n");
	g_test_assert_expected_messages();
	state = state_of(module_of(mgr));
	g_assert_nonnull(strstr(state, "\"overrides\":{\"loose.example\":false}"));

	/* ...and the next save tightens it. */
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module_of(mgr)),
		GSURF_VIEW(view), GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	g_assert_cmpint(file_mode(path), ==, 0600);
	drain();
}

static void
test_state_file_malformed(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autofree gchar *path = state_path();
	g_autofree gchar *bad = g_strconcat(path, ".bad", NULL);
	g_autofree gchar *contents = NULL;
	g_autofree gchar *state = NULL;
	g_autoptr(TestView) view = new_view("https://x.example/");

	/* Wrong shapes are ignored as a whole... */
	write_state("sites: [a, b]\n", 0600);
	expect_warning("*sites.yaml: \"sites\" must map host names to true or false; ignoring it");
	mgr = mgr_with("    mode: filter\n");
	g_test_assert_expected_messages();
	write_state("- just\n- a list\n", 0600);
	expect_warning("*sites.yaml: expected a mapping with a \"sites\" member; ignoring it");
	reconfigure(module_of(mgr), "    mode: filter\n");
	g_test_assert_expected_messages();

	/* ...as is broken YAML, */
	write_state("sites: {a: [\n", 0600);
	expect_warning("*sites.yaml: *; ignoring it");
	reconfigure(module_of(mgr), "    mode: filter\n");
	g_test_assert_expected_messages();
	state = state_of(module_of(mgr));
	g_assert_nonnull(strstr(state, "\"overrides\":{}"));

	/* and the next save keeps the unreadable file aside, not lost. */
	g_test_expect_message(DOMAIN, G_LOG_LEVEL_MESSAGE, "dark_mode: kept the unreadable*");
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module_of(mgr)),
		GSURF_VIEW(view), GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	g_assert_true(g_file_get_contents(bad, &contents, NULL, NULL));
	g_assert_cmpstr(contents, ==, "sites: {a: [\n");
	g_clear_pointer(&contents, g_free);
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "  \"x.example\": false\n"));
	g_assert_cmpint(file_mode(path), ==, 0600);
	drain();
}

static void
test_state_file_unwritable(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with("    mode: filter\n");
	g_autofree gchar *path = state_path();
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree gchar *state = NULL;
	g_autoptr(TestView) view = new_view("https://x.example/");

	/* A directory in place of the file: the toggle still applies in
	 * memory, the failure is reported. */
	g_assert_cmpint(g_mkdir_with_parents(path, 0700), ==, 0);
	expect_warning("*sites.yaml: *");
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module_of(mgr)),
		GSURF_VIEW(view), GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	g_test_assert_expected_messages();
	state = state_of(module_of(mgr));
	g_assert_nonnull(strstr(state, "\"overrides\":{\"x.example\":false}"));
	g_assert_cmpint(g_rmdir(path), ==, 0);
	(void)dir;
	drain();
}

/* --- toggles --------------------------------------------------------- */

static void
test_toggle_site(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with(
		"    mode: filter\n"
		"    sites:\n"
		"      - match: '(^|\\.)off\\.example$'\n"
		"        mode: off\n");
	GsurfModule *module = module_of(mgr);
	GsurfActionHandler *handler = GSURF_ACTION_HANDLER(module);
	g_autofree gchar *path = state_path();
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autoptr(TestView) dark = new_view("https://WWW.Example.org./page?q=1");
	g_autoptr(TestView) off = new_view("https://a.off.example/");
	g_autoptr(TestView) blank = new_view("about:blank");
	gchar *contents = NULL, *state;

	/* 1. A dark host: the override "false" is stored and written. */
	g_assert_true(gsurf_action_handler_handle_action(handler, GSURF_VIEW(dark),
		GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	state = state_of(module);
	g_assert_nonnull(strstr(state, "\"overrides\":{\"www.example.org\":false}"));
	g_free(state);
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "sites:\n  \"www.example.org\": false\n"));
	g_free(contents);
	g_assert_cmpint(file_mode(path), ==, 0600);
	g_assert_cmpint(file_mode(dir), ==, 0700);

	/* 2. A host the config turns off: the override "true". */
	g_assert_true(gsurf_action_handler_handle_action(handler, GSURF_VIEW(off),
		GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	state = state_of(module);
	g_assert_nonnull(strstr(state,
		"\"overrides\":{\"a.off.example\":true,\"www.example.org\":false}"));
	g_free(state);

	/* 3. Toggling back to what the config does drops the override, and
	 * the file says so after a reload too. */
	g_assert_true(gsurf_action_handler_handle_action(handler, GSURF_VIEW(dark),
		GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	g_assert_true(gsurf_action_handler_handle_action(handler, GSURF_VIEW(off),
		GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "sites: {}\n"));
	g_free(contents);
	reconfigure(module, "    mode: filter\n");
	state = state_of(module);
	g_assert_nonnull(strstr(state, "\"overrides\":{}"));
	g_free(state);

	/* 4. No host, no view: nothing changes. */
	g_test_expect_message(DOMAIN, G_LOG_LEVEL_MESSAGE, "*no host to toggle");
	g_assert_true(gsurf_action_handler_handle_action(handler, GSURF_VIEW(blank),
		GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	g_test_assert_expected_messages();
	g_assert_false(gsurf_action_handler_handle_action(handler, NULL,
		GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	/* Other actions are not ours. */
	g_assert_false(gsurf_action_handler_handle_action(handler, GSURF_VIEW(dark),
		GSURF_ACTION_RELOAD));

	/* 5. Legacy auto counts as "not dark": toggling turns it dark. */
	reconfigure(module, "    mode: auto\n");
	g_assert_true(gsurf_action_handler_handle_action(handler, GSURF_VIEW(dark),
		GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	state = state_of(module);
	g_assert_nonnull(strstr(state, "\"overrides\":{\"www.example.org\":true}"));
	g_free(state);
	drain();
}

static void
test_toggle_global(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with("    mode: filter\n");
	GsurfModule *module = module_of(mgr);
	g_autofree gchar *path = state_path();
	gboolean on = TRUE;
	gchar *state;

	/* Through the action (no view needed) and through the dispatcher. */
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), NULL,
		GSURF_ACTION_DARK_MODE_TOGGLE));
	g_object_get(module, "global-on", &on, NULL);
	g_assert_false(on);
	state = state_of(module);
	g_assert_nonnull(strstr(state, "\"enabled\":false"));
	g_free(state);
	g_assert_true(gsurf_module_manager_dispatch_action(mgr, NULL,
		GSURF_ACTION_DARK_MODE_TOGGLE));
	g_object_get(module, "global-on", &on, NULL);
	g_assert_true(on);
	/* The global switch is not persisted. */
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
}

static void
test_keys_and_help(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with("    mode: filter\n");
	GsurfModule *module = module_of(mgr);
	GsurfInputHandler *input = GSURF_INPUT_HANDLER(module);
	g_autoptr(TestView) view = new_view("https://keys.example/");
	g_autoptr(GPtrArray) help = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gsurf_keybind_help_free);
	g_autofree gchar *state = NULL;
	gboolean on = TRUE;
	guint i, found = 0;

	/* Ctrl+Alt+d: global; Ctrl+Alt+Shift+d (keyval 'D'): this site. */
	g_assert_true(gsurf_input_handler_handle_key_event(input, GSURF_VIEW(view), 'd', 0,
		GSURF_MOD_CTRL | GSURF_MOD_ALT, GSURF_MODE_NORMAL));
	g_object_get(module, "global-on", &on, NULL);
	g_assert_false(on);
	g_assert_true(gsurf_input_handler_handle_key_event(input, GSURF_VIEW(view), 'D', 0,
		GSURF_MOD_CTRL | GSURF_MOD_ALT | GSURF_MOD_SHIFT, GSURF_MODE_NORMAL));
	state = state_of(module);
	g_assert_nonnull(strstr(state, "\"overrides\":{\"keys.example\":false}"));
	/* Ctrl+Shift+d stays with bookmarks. */
	g_assert_false(gsurf_input_handler_handle_key_event(input, GSURF_VIEW(view), 'D', 0,
		GSURF_MOD_CTRL | GSURF_MOD_SHIFT, GSURF_MODE_NORMAL));

	/* Both keys are listed for the help overlay. */
	gsurf_keybind_provider_list_keybinds(GSURF_KEYBIND_PROVIDER(module), help);
	for (i = 0; i < help->len; i++) {
		GsurfKeybindHelp *row = g_ptr_array_index(help, i);

		g_assert_cmpstr(gsurf_keybind_help_get_source(row), ==, "dark_mode");
		if (g_strcmp0(gsurf_keybind_help_get_action(row), "dark-mode-toggle") == 0 ||
		    g_strcmp0(gsurf_keybind_help_get_action(row), "dark-mode-toggle-site") == 0)
			found++;
	}
	g_assert_cmpuint(found, ==, 2);

	/* Unbound keys do nothing and are not listed. */
	reconfigure(module, "    key_toggle: ''\n    key_toggle_site: ''\n");
	g_assert_false(gsurf_input_handler_handle_key_event(input, GSURF_VIEW(view), 'd', 0,
		GSURF_MOD_CTRL | GSURF_MOD_ALT, GSURF_MODE_NORMAL));
	g_ptr_array_set_size(help, 0);
	gsurf_keybind_provider_list_keybinds(GSURF_KEYBIND_PROVIDER(module), help);
	g_assert_cmpuint(help->len, ==, 0);
	drain();
}

/* --- injection, live updates and lifetimes --------------------------- */

static guint
count_prefix(GPtrArray *array, const gchar *prefix)
{
	guint i, n = 0;

	for (i = 0; i < array->len; i++)
		if (g_str_has_prefix(g_ptr_array_index(array, i), prefix))
			n++;
	return n;
}

/* A world script that is the vendored bundle (its banner comes first;
 * the file keeps upstream's CRLF line ends). */
static gboolean
starts_with_dark_reader(const gchar *script)
{
	g_autofree gchar *head = NULL;

	if (!g_str_has_prefix(script, WORLD "|/**"))
		return FALSE;
	head = g_strndup(script, 64);
	return strstr(head, " * Dark Reader v4.9.133") != NULL;
}

static void
test_inject_and_live_updates(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with("    mode: filter\n");
	GsurfModule *module = module_of(mgr);
	TestView *view = new_view("https://live.example/");
	TestView *other = new_view("https://other.example/");
	gpointer weak = view;
	const gchar *script;

	/* 1. One engine script in our world, with the state baked in, and
	 * the message handler registered in the same world. */
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL); /* no-op */
	g_assert_cmpuint(view->scripts->len, ==, 1);
	script = g_ptr_array_index(view->scripts, 0);
	g_assert_true(g_str_has_prefix(script, WORLD "|"));
	g_assert_nonnull(strstr(script, "globalThis.__gsurf_dark = Object.freeze("));
	g_assert_nonnull(strstr(script, "__gsurf_dark.init({\"version\":"));
	g_assert_nonnull(strstr(script, "\"global\":{\"mode\":\"filter\""));
	g_assert_null(strstr(script, "Dark Reader v"));
	g_assert_cmpuint(view->messages->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(view->messages, 0), ==, WORLD "|gsurf_dark_mode");
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(other), NULL);

	/* 2. A toggle reaches every open view twice: appended for later
	 * documents and evaluated in the current one. */
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module),
		GSURF_VIEW(view), GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
	drain();
	g_assert_cmpuint(view->scripts->len, ==, 2);
	g_assert_cmpuint(view->evaluated->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(view->scripts, 1), ==,
		g_ptr_array_index(view->evaluated, 0));
	script = g_ptr_array_index(view->evaluated, 0);
	g_assert_true(g_str_has_prefix(script, WORLD "|globalThis.__gsurf_dark !== undefined"));
	g_assert_nonnull(strstr(script, "\"overrides\":{\"live.example\":false}"));
	g_assert_cmpuint(other->scripts->len, ==, 2);
	g_assert_cmpuint(other->evaluated->len, ==, 1);

	/* 3. So does a reconfigure. */
	reconfigure(module, "    mode: native\n");
	drain();
	g_assert_cmpuint(view->scripts->len, ==, 3);
	g_assert_nonnull(strstr(g_ptr_array_index(view->scripts, 2),
		"\"global\":{\"mode\":\"native\""));

	/* 4. Dark Reader on demand: only a need-engine message from our
	 * world loads it, into the main frame. */
	gsurf_view_emit_script_message(GSURF_VIEW(view), "gsurf_dark_mode", "{\"type\":\"hello\"}");
	gsurf_view_emit_script_message(GSURF_VIEW(view), "gsurf_dark_mode", "not json");
	gsurf_view_emit_script_message(GSURF_VIEW(view), "gsurf_dark_mode", "[1]");
	gsurf_view_emit_script_message(GSURF_VIEW(view), "gsurf_dark_mode", NULL);
	gsurf_view_emit_script_message(GSURF_VIEW(view), "other", "{\"type\":\"need-engine\"}");
	drain();
	g_assert_cmpuint(view->evaluated->len, ==, 2);
	gsurf_view_emit_script_message(GSURF_VIEW(view), "gsurf_dark_mode",
		"{\"type\":\"need-engine\",\"host\":\"live.example\"}");
	drain();
	g_assert_cmpuint(view->evaluated->len, ==, 3);
	script = g_ptr_array_index(view->evaluated, 2);
	g_assert_true(starts_with_dark_reader(script));
	g_assert_nonnull(strstr(script, "__gsurf_dark.engine_ready()"));

	/* 5. The module holds views weakly: a dropped view is finalized and
	 * forgotten (a later toggle does not touch it). */
	g_object_add_weak_pointer(G_OBJECT(view), &weak);
	g_object_unref(view);
	g_assert_null(weak);
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), NULL,
		GSURF_ACTION_DARK_MODE_TOGGLE));
	drain();
	g_assert_cmpuint(other->scripts->len, ==, 4);

	/* 6. And a view may outlive the module. */
	weak = mgr;
	g_object_add_weak_pointer(G_OBJECT(mgr), &weak);
	g_clear_object(&mgr);
	g_assert_null(weak);
	g_object_unref(other);
}

static void
test_inject_preloads_dark_reader(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with("    mode: dynamic\n");
	g_autoptr(TestView) view = new_view("https://dr.example/");

	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);
	g_assert_cmpuint(view->scripts->len, ==, 2);
	/* Dark Reader first, so the engine finds it at document start. */
	g_assert_true(starts_with_dark_reader(g_ptr_array_index(view->scripts, 0)));
	g_assert_nonnull(strstr(g_ptr_array_index(view->scripts, 1), "__gsurf_dark.init("));
	g_assert_cmpuint(count_prefix(view->scripts, WORLD "|"), ==, 2);
}

static void
test_disabled_module_is_inert(void)
{
	g_autofree gchar *so = module_so_path("dark_mode");
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfModuleManager) mgr = gsurf_module_manager_new();
	g_autoptr(TestView) view = new_view("https://x.example/");
	g_autoptr(GError) error = NULL;

	g_assert_true(gsurf_config_load_from_data(config,
		"modules:\n  dark_mode:\n    enabled: false\n    mode: filter\n", -1, &error));
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(mgr);

	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);
	g_assert_cmpuint(view->scripts->len, ==, 0);
	g_assert_false(gsurf_module_manager_dispatch_action(mgr, GSURF_VIEW(view),
		GSURF_ACTION_DARK_MODE_TOGGLE_SITE));
}

/* The toggles module's legacy dark_mode key is forwarded, through the
 * process-wide manager, to dark_mode's global toggle. The only test case
 * here that loads modules into the default manager. */
static void
test_toggles_module_forwards(void)
{
	g_autofree gchar *dark_so = module_so_path("dark_mode");
	g_autofree gchar *toggles_so = module_so_path("toggles");
	g_autoptr(GsurfConfig) config = config_for(
		"  toggles:\n"
		"    enabled: true\n"
		"    keys:\n"
		"      dark_mode: \"Ctrl+Alt+F5\"\n");
	g_autoptr(TestView) view = new_view("https://fwd.example/");
	g_autoptr(GError) error = NULL;
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfModule *dark, *toggles;
	gboolean on = FALSE;

	if (!g_file_test(toggles_so, G_FILE_TEST_EXISTS)) {
		g_test_skip("toggles.so not built");
		return;
	}
	gsurf_module_manager_set_config(mgr, config);
	dark = gsurf_module_manager_load_module(mgr, dark_so, &error);
	g_assert_no_error(error);
	toggles = gsurf_module_manager_load_module(mgr, toggles_so, &error);
	g_assert_no_error(error);
	g_assert_nonnull(dark);
	g_assert_nonnull(toggles);
	gsurf_module_manager_activate_all(mgr);

	g_assert_true(gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(toggles),
		GSURF_VIEW(view), 0xffc2 /* F5 */, 0, GSURF_MOD_CTRL | GSURF_MOD_ALT,
		GSURF_MODE_NORMAL));
	g_object_get(dark, "global-on", &on, NULL);
	g_assert_false(on);

	/* With dark_mode disabled the key is not consumed. */
	gsurf_module_deactivate(dark);
	g_assert_false(gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(toggles),
		GSURF_VIEW(view), 0xffc2, 0, GSURF_MOD_CTRL | GSURF_MOD_ALT, GSURF_MODE_NORMAL));
	g_object_get(dark, "global-on", &on, NULL);
	g_assert_false(on);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	g_test_add_func("/gsurf/dark-mode/defaults", test_defaults);
	g_test_add_func("/gsurf/dark-mode/legacy-mapping", test_legacy_mapping);
	g_test_add_func("/gsurf/dark-mode/modes-and-adjustments", test_modes_and_adjustments);
	g_test_add_func("/gsurf/dark-mode/validation", test_validation);
	g_test_add_func("/gsurf/dark-mode/bad-shapes", test_bad_shapes);
	g_test_add_func("/gsurf/dark-mode/site-rules-json", test_site_rules_json);
	g_test_add_func("/gsurf/dark-mode/reconfigure", test_reconfigure);
	g_test_add_func("/gsurf/dark-mode/state-file/load", test_state_file_load);
	g_test_add_func("/gsurf/dark-mode/state-file/permissions", test_state_file_permissions);
	g_test_add_func("/gsurf/dark-mode/state-file/malformed", test_state_file_malformed);
	g_test_add_func("/gsurf/dark-mode/state-file/unwritable", test_state_file_unwritable);
	g_test_add_func("/gsurf/dark-mode/toggle-site", test_toggle_site);
	g_test_add_func("/gsurf/dark-mode/toggle-global", test_toggle_global);
	g_test_add_func("/gsurf/dark-mode/keys-and-help", test_keys_and_help);
	g_test_add_func("/gsurf/dark-mode/inject-and-live-updates", test_inject_and_live_updates);
	g_test_add_func("/gsurf/dark-mode/inject-preloads-dark-reader",
		test_inject_preloads_dark_reader);
	g_test_add_func("/gsurf/dark-mode/disabled-module", test_disabled_module_is_inert);
	g_test_add_func("/gsurf/dark-mode/toggles-module-forwards", test_toggles_module_forwards);
	return g_test_run();
}
