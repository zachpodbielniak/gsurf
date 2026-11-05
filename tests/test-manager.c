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
#include <gio/gio.h>
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

/* ---- in-test action-handler module ----
 *
 * Records the last action it saw and answers TRUE only for the action it
 * was built to handle. `claim_highest` sets the highest priority from
 * activate(), as the modal module does. */

#define TEST_TYPE_ACTION_MODULE (test_action_module_get_type())
G_DECLARE_FINAL_TYPE(TestActionModule, test_action_module, TEST, ACTION_MODULE, GsurfModule)

struct _TestActionModule
{
	GsurfModule parent_instance;
	const gchar *name;
	GsurfAction handles;
	gboolean    claim_highest;
	guint       calls;
};

static void test_action_module_iface_init(GsurfActionHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(TestActionModule, test_action_module, GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_ACTION_HANDLER, test_action_module_iface_init))

static gboolean
test_action_module_handle(GsurfActionHandler *handler, GsurfView *view, GsurfAction action)
{
	TestActionModule *self = TEST_ACTION_MODULE(handler);

	self->calls++;
	return action == self->handles;
}

static void
test_action_module_iface_init(GsurfActionHandlerInterface *iface)
{
	iface->handle_action = test_action_module_handle;
}

static const gchar *
test_action_module_get_name(GsurfModule *module)
{
	return TEST_ACTION_MODULE(module)->name;
}

static gboolean
test_action_module_activate(GsurfModule *module)
{
	if (TEST_ACTION_MODULE(module)->claim_highest)
		gsurf_module_set_priority(module, GSURF_PRIORITY_HIGHEST);
	return TRUE;
}

static void
test_action_module_class_init(TestActionModuleClass *klass)
{
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	module_class->get_name = test_action_module_get_name;
	module_class->activate = test_action_module_activate;
}

static void
test_action_module_init(TestActionModule *self)
{
}

static TestActionModule *
new_action_module(const gchar *name, GsurfAction handles, gboolean claim_highest)
{
	TestActionModule *m = g_object_new(TEST_TYPE_ACTION_MODULE, NULL);

	m->name = name;
	m->handles = handles;
	m->claim_highest = claim_highest;
	gsurf_module_set_enabled(GSURF_MODULE(m), TRUE);
	return m;
}

/* ---- in-test navigation-hook module ----
 *
 * Answers the main-frame response hook with a fixed verdict and counts
 * the calls; `response_hook` FALSE leaves the vfunc unset so the
 * interface default (USE) is exercised. */

#define TEST_TYPE_NAV_MODULE (test_nav_module_get_type())
G_DECLARE_FINAL_TYPE(TestNavModule, test_nav_module, TEST, NAV_MODULE, GsurfModule)

struct _TestNavModule
{
	GsurfModule         parent_instance;
	GsurfPolicyDecision verdict;
	guint               calls;
	gchar              *last_uri;
};

static void test_nav_module_iface_init(GsurfNavigationHookInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(TestNavModule, test_nav_module, GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_NAVIGATION_HOOK, test_nav_module_iface_init))

static GsurfPolicyDecision
test_nav_module_response(GsurfNavigationHook *hook, GsurfView *view, const gchar *uri)
{
	TestNavModule *self = TEST_NAV_MODULE(hook);

	(void)view;
	self->calls++;
	g_free(self->last_uri);
	self->last_uri = g_strdup(uri);
	return self->verdict;
}

static void
test_nav_module_iface_init(GsurfNavigationHookInterface *iface)
{
	iface->decide_main_response = test_nav_module_response;
}

static void
test_nav_module_finalize(GObject *object)
{
	g_free(TEST_NAV_MODULE(object)->last_uri);
	G_OBJECT_CLASS(test_nav_module_parent_class)->finalize(object);
}

static void
test_nav_module_class_init(TestNavModuleClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = test_nav_module_finalize;
}

static void
test_nav_module_init(TestNavModule *self)
{
	(void)self;
}

/* ---- manager mechanics ---- */

static void
test_defaults_no_modules(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_new();
	gchar *path;

	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL, "https://x"), ==, GSURF_POLICY_USE);
	g_assert_cmpint(gsurf_module_manager_dispatch_main_response(mgr, NULL, "https://x"), ==, GSURF_POLICY_USE);
	g_assert_cmpint(gsurf_module_manager_dispatch_permission(mgr, NULL, "geolocation", NULL), ==, GSURF_PERMISSION_PROMPT);
	g_assert_cmpint(gsurf_module_manager_dispatch_verify_cert(mgr, "h", 0), ==, GSURF_TLS_PROMPT);
	path = gsurf_module_manager_dispatch_decide_destination(mgr, "u", "f");
	g_assert_null(path);
	g_assert_null(gsurf_module_manager_dispatch_rewrite_uri(mgr, "x"));
	/* These must not crash with no modules / no view. */
	gsurf_module_manager_dispatch_after_navigate(mgr, NULL, "https://x");
	gsurf_module_manager_dispatch_inject_scripts(mgr, NULL, NULL);
	/* No action handler: the host's key falls through to the page. */
	g_assert_false(gsurf_module_manager_dispatch_action(mgr, NULL, GSURF_ACTION_OPEN_PROMPT));

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

/* The main-frame response hook: first non-USE verdict in priority
 * order wins; inactive modules and modules without the vfunc say USE. */
static void
test_main_response_dispatch(void)
{
	g_autoptr(GsurfModuleManager) mgr = gsurf_module_manager_new();
	g_autoptr(TestNavModule) allow = g_object_new(TEST_TYPE_NAV_MODULE, NULL);
	g_autoptr(TestNavModule) veto = g_object_new(TEST_TYPE_NAV_MODULE, NULL);
	g_autoptr(TestNavModule) off = g_object_new(TEST_TYPE_NAV_MODULE, NULL);

	allow->verdict = GSURF_POLICY_USE;
	veto->verdict = GSURF_POLICY_IGNORE;
	off->verdict = GSURF_POLICY_IGNORE;
	gsurf_module_set_enabled(GSURF_MODULE(allow), TRUE);
	gsurf_module_set_enabled(GSURF_MODULE(veto), TRUE);
	gsurf_module_set_priority(GSURF_MODULE(allow), GSURF_PRIORITY_HIGH);
	gsurf_module_manager_add_module(mgr, GSURF_MODULE(off));
	gsurf_module_manager_add_module(mgr, GSURF_MODULE(veto));
	gsurf_module_manager_add_module(mgr, GSURF_MODULE(allow));
	gsurf_module_manager_activate_all(mgr);

	g_assert_cmpint(gsurf_module_manager_dispatch_main_response(mgr, NULL, "https://a/"),
		==, GSURF_POLICY_IGNORE);
	g_assert_cmpuint(allow->calls, ==, 1);
	g_assert_cmpuint(veto->calls, ==, 1);
	g_assert_cmpuint(off->calls, ==, 0);
	g_assert_cmpstr(veto->last_uri, ==, "https://a/");

	/* before_navigate is a separate hook: unset here, so USE. */
	g_assert_cmpint(gsurf_module_manager_dispatch_before_navigate(mgr, NULL, "https://a/"),
		==, GSURF_POLICY_USE);
	g_assert_cmpuint(veto->calls, ==, 1);

	gsurf_module_manager_deactivate_all(mgr);
}

/* Actions go to the first handler (by priority) that performs them;
 * handlers that decline pass it on, inactive modules never see it. */
static void
test_action_dispatch(void)
{
	g_autoptr(GsurfModuleManager) mgr = gsurf_module_manager_new();
	g_autoptr(TestActionModule) find = new_action_module("t-find", GSURF_ACTION_FIND, FALSE);
	g_autoptr(TestActionModule) prompt = new_action_module("t-prompt", GSURF_ACTION_OPEN_PROMPT, FALSE);
	g_autoptr(TestActionModule) off = new_action_module("t-off", GSURF_ACTION_OPEN_PROMPT, FALSE);

	gsurf_module_set_priority(GSURF_MODULE(find), GSURF_PRIORITY_HIGH);
	gsurf_module_set_enabled(GSURF_MODULE(off), FALSE);
	gsurf_module_manager_add_module(mgr, GSURF_MODULE(off));
	gsurf_module_manager_add_module(mgr, GSURF_MODULE(prompt));
	gsurf_module_manager_add_module(mgr, GSURF_MODULE(find));
	gsurf_module_manager_activate_all(mgr);

	g_assert_true(gsurf_module_manager_dispatch_action(mgr, NULL, GSURF_ACTION_OPEN_PROMPT));
	g_assert_cmpuint(find->calls, ==, 1);    /* asked first, declined */
	g_assert_cmpuint(prompt->calls, ==, 1);  /* performed it */
	g_assert_cmpuint(off->calls, ==, 0);     /* disabled: never active */

	g_assert_true(gsurf_module_manager_dispatch_action(mgr, NULL, GSURF_ACTION_FIND));
	g_assert_cmpuint(prompt->calls, ==, 1);  /* find consumed it first */

	g_assert_false(gsurf_module_manager_dispatch_action(mgr, NULL, GSURF_ACTION_TAB_NEW));
}

/* ---- execute_action (0.4.4) ---- */

typedef struct { GsurfWindow parent; } ExecWindow;
typedef struct { GsurfWindowClass parent; } ExecWindowClass;

GType exec_window_get_type(void);
G_DEFINE_TYPE(ExecWindow, exec_window, GSURF_TYPE_WINDOW)

static void exec_window_class_init(ExecWindowClass *klass) { (void)klass; }
static void exec_window_init(ExecWindow *self) { (void)self; }

typedef struct {
	guint        calls;
	GsurfWindow *window;
	GsurfAction  action;
	gboolean     answer;
	guint        destroyed;
} ExecLog;

static gboolean
exec_record(GsurfWindow *window, GsurfView *view, GsurfAction action, gpointer user_data)
{
	ExecLog *log = user_data;

	(void)view;
	log->calls++;
	log->window = window;
	log->action = action;
	return log->answer;
}

static void
exec_destroy(gpointer user_data)
{
	((ExecLog *)user_data)->destroyed++;
}

/* gsurf_module_manager_execute_action(): bad actions, modules alone, the
 * host's executor (its destroy notify on replace and dispose), kiosk
 * refusals before anything runs. */
static void
test_execute_action(void)
{
	g_autoptr(GsurfModuleManager) mgr = gsurf_module_manager_new();
	g_autoptr(TestActionModule) find = new_action_module("t-find", GSURF_ACTION_FIND, FALSE);
	g_autoptr(GsurfWindow) window = g_object_new(exec_window_get_type(), NULL);
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GError) error = NULL;
	ExecLog log = { 0 };

	gsurf_module_manager_add_module(mgr, GSURF_MODULE(find));
	gsurf_module_manager_activate_all(mgr);

	/* 1. Not an action. */
	g_assert_false(gsurf_module_manager_execute_action(mgr, NULL, NULL, GSURF_ACTION_NONE,
		&error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(gsurf_module_manager_execute_action(mgr, NULL, NULL, GSURF_ACTION_LAST,
		&error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* 2. No executor: the modules only. */
	g_assert_true(gsurf_module_manager_execute_action(mgr, NULL, NULL, GSURF_ACTION_FIND,
		&error));
	g_assert_no_error(error);
	g_assert_cmpuint(find->calls, ==, 1);
	g_assert_false(gsurf_module_manager_execute_action(mgr, NULL, NULL, GSURF_ACTION_ZOOM_IN,
		&error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
	g_assert_nonnull(strstr(error->message, "zoom-in"));
	g_clear_error(&error);

	/* 3. The executor gets the window and the action; FALSE is an error.
	 *    Without a window (no application, none given) it is skipped. */
	gsurf_module_manager_set_action_executor(mgr, exec_record, &log, exec_destroy);
	log.answer = TRUE;
	g_assert_true(gsurf_module_manager_execute_action(mgr, window, NULL, GSURF_ACTION_ZOOM_IN,
		&error));
	g_assert_no_error(error);
	g_assert_cmpuint(log.calls, ==, 1);
	g_assert_true(log.window == window);
	g_assert_cmpint(log.action, ==, GSURF_ACTION_ZOOM_IN);
	log.answer = FALSE;
	g_assert_false(gsurf_module_manager_execute_action(mgr, window, NULL, GSURF_ACTION_RELOAD,
		&error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
	g_clear_error(&error);
	g_assert_true(gsurf_module_manager_execute_action(mgr, NULL, NULL, GSURF_ACTION_FIND, NULL));
	g_assert_cmpuint(log.calls, ==, 2);

	/* 4. Kiosk refuses forbidden actions before the executor runs. */
	config->kiosk = TRUE;
	gsurf_config_set_default(config);
	log.answer = TRUE;
	g_assert_false(gsurf_module_manager_execute_action(mgr, window, NULL,
		GSURF_ACTION_OPEN_PROMPT, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "open-prompt"));
	g_clear_error(&error);
	g_assert_cmpuint(log.calls, ==, 2);
	g_assert_true(gsurf_module_manager_execute_action(mgr, window, NULL, GSURF_ACTION_ZOOM_IN,
		&error));
	g_assert_cmpuint(log.calls, ==, 3);
	gsurf_config_set_default(NULL);

	/* 5. Replacing the executor frees the old data; so does dispose. */
	gsurf_module_manager_set_action_executor(mgr, exec_record, &log, exec_destroy);
	g_assert_cmpuint(log.destroyed, ==, 1);
	gsurf_module_manager_set_action_executor(mgr, NULL, NULL, NULL);
	g_assert_cmpuint(log.destroyed, ==, 2);
	gsurf_module_manager_set_action_executor(mgr, exec_record, &log, exec_destroy);
	g_clear_object(&mgr);
	g_assert_cmpuint(log.destroyed, ==, 3);
}

/* A priority claimed inside activate() takes effect for dispatch: the
 * manager sorts again after activating (modal relies on this). */
static void
test_priority_set_in_activate(void)
{
	g_autoptr(GsurfModuleManager) mgr = gsurf_module_manager_new();
	g_autoptr(TestActionModule) early = new_action_module("t-early", GSURF_ACTION_FIND, FALSE);
	g_autoptr(TestActionModule) late = new_action_module("t-late", GSURF_ACTION_FIND, TRUE);

	gsurf_module_set_priority(GSURF_MODULE(early), GSURF_PRIORITY_HIGH);
	gsurf_module_manager_add_module(mgr, GSURF_MODULE(early));
	gsurf_module_manager_add_module(mgr, GSURF_MODULE(late));
	gsurf_module_manager_activate_all(mgr);

	g_assert_true(gsurf_module_manager_dispatch_action(mgr, NULL, GSURF_ACTION_FIND));
	g_assert_cmpuint(late->calls, ==, 1);
	g_assert_cmpuint(early->calls, ==, 0);
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
count_mode(GsurfModuleManager *mgr, GsurfModePolicy mode, gpointer user_data)
{
	GString *log = user_data;

	(void)mgr;
	g_string_append_printf(log, "%s,", gsurf_mode_policy_get_label(mode));
}

static void
count_notify(GObject *object, GParamSpec *pspec, gpointer user_data)
{
	(void)object;
	(void)pspec;
	(*(guint *)user_data)++;
}

/* The published input mode: NORMAL at first; a change notifies
 * GsurfModuleManager:mode and emits mode-changed once; the same mode
 * again is silent; the property sets it too; it is independent of the
 * passthrough flag. */
static void
test_mode(void)
{
	g_autoptr(GsurfModuleManager) mgr = gsurf_module_manager_new();
	g_autoptr(GString) log = g_string_new(NULL);
	GsurfModePolicy read = GSURF_MODE_FIND;
	guint notifies = 0;

	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
	g_signal_connect(mgr, "mode-changed", G_CALLBACK(count_mode), log);
	g_signal_connect(mgr, "notify::mode", G_CALLBACK(count_notify), &notifies);

	gsurf_module_manager_set_mode(mgr, GSURF_MODE_INSERT);
	gsurf_module_manager_set_mode(mgr, GSURF_MODE_INSERT);
	gsurf_module_manager_set_mode(mgr, GSURF_MODE_FOLLOW);
	g_object_set(mgr, "mode", GSURF_MODE_NORMAL, NULL);
	g_assert_cmpstr(log->str, ==, "INSERT,FOLLOW,NORMAL,");
	g_assert_cmpuint(notifies, ==, 3);
	g_object_get(mgr, "mode", &read, NULL);
	g_assert_cmpint(read, ==, GSURF_MODE_NORMAL);
	g_assert_false(gsurf_module_manager_get_input_passthrough(mgr));

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*GSURF_IS_MODULE_MANAGER*");
	g_assert_cmpint(gsurf_module_manager_get_mode(NULL), ==, GSURF_MODE_NORMAL);
	g_test_assert_expected_messages();
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

/* Priorities are public gint values: comparisons must not overflow at
 * either endpoint, both when registering and when activating modules. */
static void
test_priority_extremes(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = module_so_path("uri_params");
	GsurfModule *first, *second;
	GPtrArray *modules;

	mgr = mgr_with("useragent", "modules:\n  useragent:\n    enabled: true\n");
	if (mgr == NULL || !g_file_test(path, G_FILE_TEST_EXISTS)) {
		g_test_skip("useragent.so and uri_params.so must be built");
		return;
	}
	first = gsurf_module_manager_get_module(mgr, "useragent");
	gsurf_module_set_priority(first, G_MININT);
	second = gsurf_module_manager_load_module(mgr, path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(second);
	modules = gsurf_module_manager_get_modules(mgr);
	g_assert_true(g_ptr_array_index(modules, 0) == first);

	gsurf_module_set_priority(second, G_MAXINT);
	gsurf_module_manager_activate_all(mgr);
	g_assert_true(g_ptr_array_index(modules, 0) == first);
	g_assert_true(g_ptr_array_index(modules, 1) == second);

	/* Also exercise the opposite comparator argument order. */
	gsurf_module_set_priority(first, G_MAXINT);
	gsurf_module_set_priority(second, G_MININT);
	gsurf_module_manager_activate_all(mgr);
	g_assert_true(g_ptr_array_index(modules, 0) == second);
}

/* Supply a new configuration object just as an embedding host does. */
static void
reconfigure_module(GsurfModuleManager *mgr, const gchar *name, const gchar *yaml)
{
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	gsurf_module_configure(gsurf_module_manager_get_module(mgr, name), config);
}

/* Removing a site from the replacement rules must restore baseline settings. */
static void
test_uri_params_reconfigure(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GsurfSettings) settings = NULL;

	mgr = mgr_with("uri_params",
		"modules:\n  uri_params:\n    enabled: true\n"
		"    rules:\n      - regex: old\n        settings: {javascript: false}\n");
	if (mgr == NULL) {
		g_test_skip("uri_params.so not built");
		return;
	}
	reconfigure_module(mgr, "uri_params",
		"modules:\n  uri_params:\n"
		"    rules:\n      - regex: new\n        settings: {images: false}\n");
	settings = gsurf_settings_new();
	gsurf_module_manager_dispatch_apply_settings(mgr, NULL, settings, "https://old.test");
	g_assert_true(settings->javascript);
	gsurf_module_manager_dispatch_apply_settings(mgr, NULL, settings, "https://new.test");
	g_assert_false(settings->images);

	reconfigure_module(mgr, "uri_params", "modules:\n  uri_params:\n    rules: []\n");
	settings->images = TRUE;
	gsurf_module_manager_dispatch_apply_settings(mgr, NULL, settings, "https://new.test");
	g_assert_true(settings->images);
}

/* Replaced rules must win immediately, and empty collections remove both
 * cached rules and named presets instead of retaining earlier values. */
static void
test_useragent_reconfigure(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GsurfSettings) settings = NULL;

	mgr = mgr_with("useragent",
		"modules:\n  useragent:\n    enabled: true\n    default: Baseline\n"
		"    presets: {desktop: OldAgent}\n"
		"    rules:\n      - {regex: example, ua: desktop}\n");
	if (mgr == NULL) {
		g_test_skip("useragent.so not built");
		return;
	}
	settings = gsurf_settings_new();
	reconfigure_module(mgr, "useragent",
		"modules:\n  useragent:\n    presets: {desktop: NewAgent}\n"
		"    rules:\n      - {regex: example, ua: desktop}\n");
	gsurf_module_manager_dispatch_apply_settings(mgr, NULL, settings, "https://example.test");
	g_assert_cmpstr(settings->user_agent, ==, "NewAgent");

	reconfigure_module(mgr, "useragent",
		"modules:\n  useragent:\n    presets: {}\n"
		"    rules:\n      - {regex: example, ua: desktop}\n");
	gsurf_module_manager_dispatch_apply_settings(mgr, NULL, settings, "https://example.test");
	/* Unknown preset names are documented literal user-agent strings. */
	g_assert_cmpstr(settings->user_agent, ==, "desktop");

	reconfigure_module(mgr, "useragent", "modules:\n  useragent:\n    rules: []\n");
	gsurf_module_manager_dispatch_apply_settings(mgr, NULL, settings, "https://example.test");
	g_assert_cmpstr(settings->user_agent, ==, "Baseline");
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
	g_test_add_func("/gsurf/manager/mode", test_mode);
	g_test_add_func("/gsurf/manager/action-dispatch", test_action_dispatch);
	g_test_add_func("/gsurf/manager/execute-action", test_execute_action);
	g_test_add_func("/gsurf/manager/main-response-dispatch", test_main_response_dispatch);
	g_test_add_func("/gsurf/manager/priority-set-in-activate", test_priority_set_in_activate);
	g_test_add_func("/gsurf/manager/load-bad-path", test_load_bad_path);
	g_test_add_func("/gsurf/manager/search-paths", test_search_paths);
	g_test_add_func("/gsurf/manager/load-modules-from-path", test_load_modules_from_path);
	g_test_add_func("/gsurf/manager/priority-api", test_priority_api);
	g_test_add_func("/gsurf/manager/priority-extremes", test_priority_extremes);
	g_test_add_func("/gsurf/manager/uri-params-reconfigure", test_uri_params_reconfigure);
	g_test_add_func("/gsurf/manager/useragent-reconfigure", test_useragent_reconfigure);
	g_test_add_func("/gsurf/manager/enabled-gating", test_enabled_gating);
	g_test_add_func("/gsurf/manager/uri-params", test_uri_params);
	g_test_add_func("/gsurf/manager/useragent", test_useragent);
	g_test_add_func("/gsurf/manager/downloads", test_downloads);
	g_test_add_func("/gsurf/manager/cert-manager", test_cert_manager);
	g_test_add_func("/gsurf/manager/geolocation", test_geolocation);
	g_test_add_func("/gsurf/manager/notifications", test_notifications);
	return g_test_run();
}
