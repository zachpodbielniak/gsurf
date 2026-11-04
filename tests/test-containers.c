/*
 * test-containers.c - The containers module: config, placement policy,
 *                     host assignment decisions, reconfigure
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Headless: loads the real containers.so through a module manager (as
 * test-manager.c does) and builds views from a minimal GsurfView
 * subclass, so the registry resolver runs at view construction without
 * WebKit. No host application is set, so a scheduled reopen never
 * creates a (WebKit) view here; test-containers-module-webkit drives
 * the reopen itself in real windows.
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>

/* --- headless view: remembers the URI it was asked to load --- */

typedef struct {
	GsurfView  parent;
	gchar     *uri;
	gboolean   back;
} TestView;
typedef struct { GsurfViewClass parent; } TestViewClass;

GType test_view_get_type(void);
G_DEFINE_TYPE(TestView, test_view, GSURF_TYPE_VIEW)

static void
test_view_load_uri(GsurfView *view, const gchar *uri)
{
	TestView *self = (TestView *)view;

	g_free(self->uri);
	self->uri = g_strdup(uri);
}

static const gchar *
test_view_get_uri(GsurfView *view)
{
	return ((TestView *)view)->uri;
}

static gboolean
test_view_can_go_back(GsurfView *view)
{
	return ((TestView *)view)->back;
}

static void
test_view_finalize(GObject *object)
{
	g_free(((TestView *)object)->uri);
	G_OBJECT_CLASS(test_view_parent_class)->finalize(object);
}

static void
test_view_class_init(TestViewClass *klass)
{
	GsurfViewClass *view_class = GSURF_VIEW_CLASS(klass);

	G_OBJECT_CLASS(klass)->finalize = test_view_finalize;
	view_class->load_uri = test_view_load_uri;
	view_class->get_uri = test_view_get_uri;
	view_class->can_go_back = test_view_can_go_back;
}

static void test_view_init(TestView *self) { (void)self; }

static GsurfView *
new_view(GsurfView *opener, const gchar *container, const gchar *uri, gboolean related)
{
	return g_object_new(test_view_get_type(),
		"opener", opener,
		"container-id", container,
		"initial-uri", uri,
		"related", related,
		NULL);
}

/* --- module loading (cf. test-manager.c) --- */

static gchar *
module_so_path(const gchar *name)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = NULL;
	g_autofree gchar *base = NULL;

	g_assert_nonnull(exe);
	dir = g_path_get_dirname(exe);
	base = g_strconcat(name, ".so", NULL);
	return g_build_filename(dir, "modules", base, NULL);
}

static GsurfConfig *
config_from(const gchar *yaml)
{
	g_autoptr(GError) error = NULL;
	GsurfConfig *config = gsurf_config_new();

	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	return config;
}

/* A manager with the real containers module loaded, configured from
 * @yaml and activated. NULL (test skipped) when the .so is missing. */
static GsurfModuleManager *
mgr_with(const gchar *yaml, GsurfModule **module)
{
	g_autofree gchar *so = module_so_path("containers");
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = NULL;
	GsurfModuleManager *mgr;

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("containers.so not built");
		return NULL;
	}
	config = config_from(yaml);
	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	*module = gsurf_module_manager_load_module(mgr, so, &error);
	g_assert_no_error(error);
	g_assert_nonnull(*module);
	gsurf_module_manager_activate_all(mgr);
	return mgr;
}

static void
reconfigure(GsurfModule *module, const gchar *yaml)
{
	g_autoptr(GsurfConfig) config = config_from(yaml);

	gsurf_module_configure(module, config);
}

/* Tear a manager down and check the module object really went away. */
static void
mgr_free(GsurfModuleManager *mgr, GsurfModule *module)
{
	gpointer weak = module;

	g_object_add_weak_pointer(G_OBJECT(module), &weak);
	g_object_unref(mgr);
	g_assert_null(weak);
}

/* The container a fresh unrelated view lands in. */
static gchar *
placed(GsurfView *opener, const gchar *requested, const gchar *uri)
{
	g_autoptr(GsurfView) view = new_view(opener, requested, uri, FALSE);

	return g_strdup(gsurf_view_get_container_id(view));
}

static const gchar *const full_yaml =
	"modules:\n"
	"  containers:\n"
	"    enabled: true\n"
	"    new_tab: temporary\n"
	"    temp_cleanup_delay: 0\n"
	"    temp_color: \"#123456\"\n"
	"    key_new_temp: Ctrl+Alt+n\n"
	"    containers:\n"
	"      - name: Media\n"
	"        color: \"#f9e2af\"\n"
	"        hosts: ['(^|\\.)youtube\\.com$', '(^|\\.)twitch\\.tv$']\n"
	"      - name: Work Stuff\n"
	"        hosts: ['^intranet\\.example$']\n"
	"      - name: Bank\n"
	"        id: money\n"
	"        color: \"#a6e3a1\"\n";

/* ===== config ===== */

static void
test_config(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	GsurfModule *module = NULL;
	GsurfModuleManager *mgr = mgr_with(full_yaml, &module);
	g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gsurf_keybind_help_free);
	GsurfContainer *c;

	if (mgr == NULL)
		return;
	g_assert_true(gsurf_module_is_active(module));
	g_assert_cmpstr(gsurf_module_get_name(module), ==, "containers");

	/* Names become ids: lowercase, other characters -> '-'; an explicit
	 * id wins. Name and colour reach the registry. */
	c = gsurf_container_registry_lookup(registry, "media");
	g_assert_nonnull(c);
	g_assert_cmpstr(gsurf_container_get_name(c), ==, "Media");
	g_assert_cmpstr(gsurf_container_get_color(c), ==, "#f9e2af");
	g_assert_false(gsurf_container_get_temporary(c));
	c = gsurf_container_registry_lookup(registry, "work-stuff");
	g_assert_nonnull(c);
	g_assert_cmpstr(gsurf_container_get_name(c), ==, "Work Stuff");
	g_assert_null(gsurf_container_get_color(c));
	c = gsurf_container_registry_lookup(registry, "money");
	g_assert_nonnull(c);
	g_assert_cmpstr(gsurf_container_get_name(c), ==, "Bank");
	g_assert_null(gsurf_container_registry_lookup(registry, "bank"));

	/* Keys: a configured one and a default one. */
	gsurf_keybind_provider_list_keybinds(GSURF_KEYBIND_PROVIDER(module), keys);
	g_assert_cmpuint(keys->len, ==, 2);
	g_assert_cmpstr(gsurf_keybind_help_get_key(g_ptr_array_index(keys, 0)), ==, "Ctrl+Alt+n");
	g_assert_cmpstr(gsurf_keybind_help_get_action(g_ptr_array_index(keys, 0)), ==,
		"container-new-temp");
	g_assert_cmpstr(gsurf_keybind_help_get_key(g_ptr_array_index(keys, 1)), ==, "Ctrl+Alt+c");
	g_assert_cmpstr(gsurf_keybind_help_get_action(g_ptr_array_index(keys, 1)), ==,
		"container-reopen-next");

	/* The module owns no key it was not given: other keys pass. */
	g_assert_false(gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(module), NULL,
		'x', 0, 0, GSURF_MODE_NORMAL));
	/* Unrelated actions are not claimed. */
	g_assert_false(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), NULL,
		GSURF_ACTION_RELOAD));

	mgr_free(mgr, module);
	/* Unloading undefines everything the module defined. */
	g_assert_null(gsurf_container_registry_lookup(registry, "media"));
	g_assert_null(gsurf_container_registry_lookup(registry, "work-stuff"));
	g_assert_null(gsurf_container_registry_lookup(registry, "money"));
}

/* ===== resolver ===== */

static void
test_resolver(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	GsurfModule *module = NULL;
	GsurfModuleManager *mgr = mgr_with(full_yaml, &module);
	g_autoptr(GsurfView) in_money = NULL;
	g_autoptr(GsurfView) in_default = NULL;
	g_autofree gchar *id = NULL;
	GsurfContainer *temp;

	if (mgr == NULL)
		return;

	/* 1. An explicit request wins over everything (even a host rule). */
	id = placed(NULL, "money", "https://www.youtube.com/watch");
	g_assert_cmpstr(id, ==, "money");
	g_clear_pointer(&id, g_free);
	id = placed(NULL, GSURF_CONTAINER_DEFAULT, "https://youtube.com/");
	g_assert_null(id);
	/* "temporary" is a new temporary container in temp_color. */
	id = placed(NULL, GSURF_CONTAINER_TEMPORARY, "https://youtube.com/");
	g_assert_nonnull(id);
	temp = gsurf_container_registry_lookup(registry, id);
	g_assert_nonnull(temp);
	g_assert_true(gsurf_container_get_temporary(temp));
	g_assert_cmpstr(gsurf_container_get_color(temp), ==, "#123456");
	g_clear_pointer(&id, g_free);

	/* 2. A host rule for the URI hint; matching is per host, anchored
	 *    by the pattern, case-insensitive. */
	id = placed(NULL, NULL, "https://www.YouTube.com/watch?v=1");
	g_assert_cmpstr(id, ==, "media");
	g_clear_pointer(&id, g_free);
	id = placed(NULL, NULL, "https://twitch.tv/");
	g_assert_cmpstr(id, ==, "media");
	g_clear_pointer(&id, g_free);
	id = placed(NULL, NULL, "http://intranet.example:8080/x");
	g_assert_cmpstr(id, ==, "work-stuff");
	g_clear_pointer(&id, g_free);

	/* 3. The opener's container (also beats new_tab), but a host rule
	 *    beats the opener. */
	in_money = new_view(NULL, "money", NULL, FALSE);
	id = placed(in_money, NULL, "https://example.org/");
	g_assert_cmpstr(id, ==, "money");
	g_clear_pointer(&id, g_free);
	id = placed(in_money, NULL, "https://youtube.com/");
	g_assert_cmpstr(id, ==, "media");
	g_clear_pointer(&id, g_free);
	in_default = new_view(NULL, GSURF_CONTAINER_DEFAULT, NULL, FALSE);
	id = placed(in_default, NULL, NULL);
	g_assert_null(id);

	/* A related popup shares its opener's container (core rule). */
	{
		g_autoptr(GsurfView) popup = new_view(in_money, NULL, "https://youtube.com/", TRUE);

		g_assert_cmpstr(gsurf_view_get_container_id(popup), ==, "money");
	}

	/* 4. No request, no rule, no opener: new_tab (temporary here). */
	id = placed(NULL, NULL, "https://example.org/");
	g_assert_nonnull(id);
	g_assert_true(g_str_has_prefix(id, "tmp-"));
	g_clear_pointer(&id, g_free);
	id = placed(NULL, NULL, NULL);
	g_assert_true(g_str_has_prefix(id, "tmp-"));
	g_clear_pointer(&id, g_free);

	/* new_tab: default / a container id; inherit_opener off. */
	reconfigure(module,
		"modules:\n  containers:\n    enabled: true\n    new_tab: default\n"
		"    inherit_opener: false\n"
		"    containers:\n      - name: Bank\n        id: money\n");
	id = placed(NULL, NULL, "https://example.org/");
	g_assert_null(id);
	id = placed(in_money, NULL, "https://example.org/");
	g_assert_null(id);
	reconfigure(module,
		"modules:\n  containers:\n    enabled: true\n    new_tab: money\n"
		"    containers:\n      - name: Bank\n        id: money\n");
	id = placed(NULL, NULL, NULL);
	g_assert_cmpstr(id, ==, "money");
	g_clear_pointer(&id, g_free);

	g_clear_object(&in_money);
	g_clear_object(&in_default);
	mgr_free(mgr, module);
	/* The resolver went with the module: a plain view is default again. */
	id = placed(NULL, NULL, "https://youtube.com/");
	g_assert_null(id);
}

/* ===== disabled ===== */

static void
test_disabled(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	GsurfModule *module = NULL;
	GsurfModuleManager *mgr = mgr_with(
		"modules:\n  containers:\n    enabled: false\n    new_tab: temporary\n"
		"    containers:\n      - name: Media\n        hosts: ['youtube']\n", &module);
	g_autofree gchar *id = NULL;

	if (mgr == NULL)
		return;
	g_assert_false(gsurf_module_is_active(module));
	/* Nothing defined, no resolver: placement is the core default. */
	g_assert_null(gsurf_container_registry_lookup(registry, "media"));
	id = placed(NULL, NULL, "https://youtube.com/");
	g_assert_null(id);
	/* A missing section is the same as disabled (modules default off). */
	reconfigure(module, "browser:\n  homepage: about:blank\n");
	g_assert_null(gsurf_container_registry_lookup(registry, "media"));
	mgr_free(mgr, module);
}

/* ===== bad input ===== */

static void
test_bad_input(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	GsurfModule *module = NULL;
	GsurfModuleManager *mgr;
	g_autofree gchar *id = NULL;

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*temp_cleanup_delay -5 is out of range*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*entry 0 is not a mapping*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*entry 1 has no `name`*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*entry 2 ('Temporary')*reserved*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*entry 3 ('Escape')*may only use*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*entry 4 ('Default')*reserved*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*'good': bad host pattern '('*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*'good': host pattern 2 is empty*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*duplicate container id 'good'*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*'listless': `hosts` must be a list*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*`new_tab` names unknown container 'nowhere'*");
	mgr = mgr_with(
		"modules:\n  containers:\n    enabled: true\n"
		"    temp_cleanup_delay: -5\n"
		"    new_tab: nowhere\n"
		"    containers:\n"
		"      - just-a-string\n"
		"      - color: \"#fff\"\n"
		"      - name: Temporary\n"
		"      - name: Escape\n        id: ../etc\n"
		"      - name: Default\n"
		"      - name: Good\n        hosts: ['(', '^good\\.example$', '']\n"
		"      - name: good\n"
		"      - name: Listless\n        hosts: 'not-a-list'\n",
		&module);
	if (mgr == NULL)
		return;
	g_test_assert_expected_messages();

	/* The valid parts still work. */
	g_assert_nonnull(gsurf_container_registry_lookup(registry, "good"));
	g_assert_nonnull(gsurf_container_registry_lookup(registry, "listless"));
	g_assert_null(gsurf_container_registry_lookup(registry, "temporary"));
	id = placed(NULL, NULL, "https://good.example/");
	g_assert_cmpstr(id, ==, "good");
	g_clear_pointer(&id, g_free);
	/* new_tab fell back to temporary. */
	id = placed(NULL, NULL, "https://elsewhere.example/");
	g_assert_true(g_str_has_prefix(id, "tmp-"));
	g_clear_pointer(&id, g_free);

	/* A non-list `containers` and an empty new_tab are reported too. */
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*`containers` must be a list*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*empty `new_tab`*");
	reconfigure(module,
		"modules:\n  containers:\n    enabled: true\n    new_tab: ''\n"
		"    containers: {name: Media}\n");
	g_test_assert_expected_messages();
	g_assert_null(gsurf_container_registry_lookup(registry, "good"));

	mgr_free(mgr, module);
}

/* ===== reconfigure ===== */

static void
test_reconfigure(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	GsurfModule *module = NULL;
	GsurfModuleManager *mgr = mgr_with(
		"modules:\n  containers:\n    enabled: true\n    new_tab: default\n"
		"    containers:\n"
		"      - name: A\n        color: red\n        hosts: ['^a\\.example$']\n"
		"      - name: B\n        hosts: ['^b\\.example$']\n", &module);
	g_autoptr(GsurfView) holder = NULL;
	g_autofree gchar *id = NULL;
	GsurfContainer *b;
	gpointer weak_b;

	if (mgr == NULL)
		return;
	g_assert_nonnull(gsurf_container_registry_lookup(registry, "a"));
	b = gsurf_container_registry_lookup(registry, "b");
	g_assert_nonnull(b);

	/* Keep B in use across the reconfigure. */
	holder = new_view(NULL, NULL, "https://b.example/", FALSE);
	g_assert_cmpstr(gsurf_view_get_container_id(holder), ==, "b");

	/* The same config twice changes nothing. */
	reconfigure(module,
		"modules:\n  containers:\n    enabled: true\n    new_tab: default\n"
		"    containers:\n"
		"      - name: A\n        color: red\n        hosts: ['^a\\.example$']\n"
		"      - name: B\n        hosts: ['^b\\.example$']\n");
	g_assert_true(gsurf_container_registry_lookup(registry, "b") == b);

	/* A new config replaces the old: A is restyled and loses its rule
	 * to C, B (in use) stays until its last view closes but no longer
	 * gets new tabs. */
	reconfigure(module,
		"modules:\n  containers:\n    enabled: true\n    new_tab: default\n"
		"    containers:\n"
		"      - name: Alpha\n        id: a\n        color: blue\n"
		"      - name: C\n        hosts: ['^a\\.example$', '^b\\.example$']\n");
	g_assert_cmpstr(gsurf_container_get_name(gsurf_container_registry_lookup(registry, "a")),
		==, "Alpha");
	g_assert_cmpstr(gsurf_container_get_color(gsurf_container_registry_lookup(registry, "a")),
		==, "blue");
	g_assert_nonnull(gsurf_container_registry_lookup(registry, "c"));
	g_assert_true(gsurf_container_registry_lookup(registry, "b") == b);
	id = placed(NULL, NULL, "https://a.example/");
	g_assert_cmpstr(id, ==, "c");
	g_clear_pointer(&id, g_free);
	id = placed(NULL, NULL, "https://b.example/");
	g_assert_cmpstr(id, ==, "c");
	g_clear_pointer(&id, g_free);

	/* B goes once its last view does. */
	weak_b = b;
	g_object_add_weak_pointer(G_OBJECT(b), &weak_b);
	g_clear_object(&holder);
	g_assert_null(gsurf_container_registry_lookup(registry, "b"));
	g_assert_null(weak_b);

	/* Back to defaults: the section without options drops every
	 * container; the resolver (enabled) uses new_tab: temporary. */
	reconfigure(module, "modules:\n  containers:\n    enabled: true\n");
	g_assert_null(gsurf_container_registry_lookup(registry, "a"));
	g_assert_null(gsurf_container_registry_lookup(registry, "c"));
	id = placed(NULL, NULL, "https://a.example/");
	g_assert_true(g_str_has_prefix(id, "tmp-"));
	g_clear_pointer(&id, g_free);

	/* Disabling removes the resolver. */
	reconfigure(module,
		"modules:\n  containers:\n    enabled: false\n"
		"    containers:\n      - name: A\n");
	g_assert_null(gsurf_container_registry_lookup(registry, "a"));
	id = placed(NULL, NULL, "https://a.example/");
	g_assert_null(id);

	/* Deactivate / activate round trip restores names and colours. */
	reconfigure(module,
		"modules:\n  containers:\n    enabled: true\n"
		"    containers:\n      - name: Alpha\n        id: a\n        color: blue\n");
	gsurf_module_deactivate(module);
	g_assert_null(gsurf_container_registry_lookup(registry, "a"));
	id = placed(NULL, NULL, NULL);
	g_assert_null(id);
	g_assert_true(gsurf_module_activate(module));
	g_assert_cmpstr(gsurf_container_get_color(gsurf_container_registry_lookup(registry, "a")),
		==, "blue");
	id = placed(NULL, NULL, NULL);
	g_assert_true(g_str_has_prefix(id, "tmp-"));
	g_clear_pointer(&id, g_free);

	mgr_free(mgr, module);
	g_assert_null(gsurf_container_registry_lookup(registry, "a"));
}

/* ===== host assignment decisions ===== */

static GsurfPolicyDecision
before(GsurfModule *module, GsurfView *view, const gchar *uri)
{
	return gsurf_navigation_hook_before_navigate(GSURF_NAVIGATION_HOOK(module), view, uri);
}

static GsurfPolicyDecision
response(GsurfModule *module, GsurfView *view, const gchar *uri)
{
	return gsurf_navigation_hook_decide_main_response(GSURF_NAVIGATION_HOOK(module),
		view, uri);
}

/* Load @uri in a headless view and report it committed. */
static void
commit(GsurfView *view, const gchar *uri)
{
	gsurf_view_load_uri(view, uri);
	gsurf_view_emit_load_changed(view, GSURF_LOAD_COMMITTED);
	gsurf_view_emit_load_changed(view, GSURF_LOAD_FINISHED);
}

static void
test_assignment_decisions(void)
{
	GsurfModule *module = NULL;
	GsurfModuleManager *mgr = mgr_with(full_yaml, &module);
	g_autoptr(GsurfConfig) config = NULL;
	g_autoptr(GsurfView) tab = NULL;
	g_autoptr(GsurfView) media = NULL;
	g_autoptr(GsurfView) explicit_tab = NULL;
	g_autoptr(GsurfView) popup = NULL;

	if (mgr == NULL)
		return;
	config = gsurf_config_new();
	gsurf_config_set_default(config);

	tab = new_view(NULL, GSURF_CONTAINER_DEFAULT, NULL, FALSE);
	commit(tab, "https://example.org/");

	/* An address the view loads itself (typed, bookmark) to an assigned
	 * host moves before the request is sent. */
	gsurf_view_load_uri(tab, "https://www.youtube.com/");
	g_assert_cmpint(before(module, tab, "https://www.youtube.com/"), ==, GSURF_POLICY_IGNORE);
	/* A second hit before the reopen ran is still blocked (once). */
	g_assert_cmpint(before(module, tab, "https://www.youtube.com/"), ==, GSURF_POLICY_IGNORE);

	/* Sub-frame (or popup) navigations look like any navigation the
	 * view did not request: never moved here ... */
	commit(tab, "https://example.org/");
	g_assert_cmpint(before(module, tab, "https://www.youtube.com/embed/1"), ==,
		GSURF_POLICY_USE);
	/* ... only the main frame's response is. */
	g_assert_cmpint(response(module, tab, "https://www.youtube.com/watch"), ==,
		GSURF_POLICY_IGNORE);

	/* Unassigned hosts and non-network URIs pass. */
	g_assert_cmpint(response(module, tab, "https://example.net/"), ==, GSURF_POLICY_USE);
	g_assert_cmpint(response(module, tab, "about:blank"), ==, GSURF_POLICY_USE);
	g_assert_cmpint(response(module, tab, "file:///tmp/x"), ==, GSURF_POLICY_USE);

	/* Already in the right container. */
	media = new_view(NULL, NULL, "https://youtube.com/", FALSE);
	g_assert_cmpstr(gsurf_view_get_container_id(media), ==, "media");
	g_assert_cmpint(response(module, media, "https://youtube.com/"), ==, GSURF_POLICY_USE);
	/* ... and it is not pulled back out for an unassigned host. */
	g_assert_cmpint(response(module, media, "https://example.org/"), ==, GSURF_POLICY_USE);
	/* Another container's host does move it. */
	commit(media, "https://youtube.com/");
	g_assert_cmpint(response(module, media, "https://intranet.example/"), ==,
		GSURF_POLICY_IGNORE);

	/* Popups stay with their opener whatever they load. */
	popup = new_view(tab, NULL, NULL, TRUE);
	g_assert_cmpint(response(module, popup, "https://youtube.com/"), ==, GSURF_POLICY_USE);

	/* A view explicitly placed for a host keeps its first load (and its
	 * redirects within that host)... */
	explicit_tab = new_view(NULL, GSURF_CONTAINER_DEFAULT, "https://youtube.com/", FALSE);
	gsurf_view_load_uri(explicit_tab, "https://youtube.com/");
	g_assert_cmpint(before(module, explicit_tab, "https://youtube.com/"), ==,
		GSURF_POLICY_USE);
	g_assert_cmpint(response(module, explicit_tab, "https://youtube.com/"), ==,
		GSURF_POLICY_USE);
	/* ... but once it committed, later navigations are assigned. */
	commit(explicit_tab, "https://youtube.com/");
	g_assert_cmpint(response(module, explicit_tab, "https://youtube.com/next"), ==,
		GSURF_POLICY_IGNORE);

	/* Kiosk mode never opens views. */
	{
		g_autoptr(GsurfView) kiosk_tab = new_view(NULL, GSURF_CONTAINER_DEFAULT, NULL, FALSE);

		config->kiosk = TRUE;
		commit(kiosk_tab, "https://example.org/");
		g_assert_cmpint(response(module, kiosk_tab, "https://youtube.com/"), ==,
			GSURF_POLICY_USE);
		g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module),
			kiosk_tab, GSURF_ACTION_CONTAINER_NEW_TEMP));
		g_assert_false(gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(module),
			kiosk_tab, 'n', 0, GSURF_MOD_CTRL | GSURF_MOD_ALT, GSURF_MODE_NORMAL));
		config->kiosk = FALSE;
	}

	/* Without a host window the scheduled reopens do nothing. */
	while (g_main_context_iteration(NULL, FALSE))
		;
	/* No window either: the actions report "not handled". */
	g_assert_false(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), tab,
		GSURF_ACTION_CONTAINER_NEW_TEMP));
	g_assert_false(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), tab,
		GSURF_ACTION_CONTAINER_REOPEN_NEXT));

	gsurf_config_set_default(NULL);
	g_clear_object(&popup);
	g_clear_object(&explicit_tab);
	g_clear_object(&media);
	g_clear_object(&tab);
	mgr_free(mgr, module);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	g_test_add_func("/gsurf/containers-module/config", test_config);
	g_test_add_func("/gsurf/containers-module/resolver", test_resolver);
	g_test_add_func("/gsurf/containers-module/disabled", test_disabled);
	g_test_add_func("/gsurf/containers-module/bad-input", test_bad_input);
	g_test_add_func("/gsurf/containers-module/reconfigure", test_reconfigure);
	g_test_add_func("/gsurf/containers-module/assignment-decisions", test_assignment_decisions);
	return g_test_run();
}
