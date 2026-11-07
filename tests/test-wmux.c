/*
 * test-wmux.c - The wmux module's configuration, headless
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the real wmux.so through a module manager (as gsurf does) and
 * checks its configuration without a display:
 *
 *   - every default, read back from the module's struct (the private
 *     header describes it; the struct is plain data, so no symbol of the
 *     module is needed) and from its keybind help;
 *   - every key with a valid non-default value, and with invalid values
 *     (each warns and keeps the default);
 *   - binding overrides: rebinding, new keys, unbinding with "" and null,
 *     bad keys and bad commands (warned and skipped, the default kept);
 *   - a new prefix moving send-prefix along; extra repeatable keys;
 *   - a second configure() going back to the defaults;
 *   - the keybind help entries ("C-b %" -> split-window -h, C-h, ...);
 *   - activation without a window, in kiosk mode and on a window that
 *     cannot host a layout: the module stays inactive (logged once).
 *
 * The behaviour on real windows is tests/test-wmux-webkit.c (make
 * test-gui); docs/wmux.org's YAML blocks are tests/test-wmux-docs.c.
 */

#include <gsurf.h>
#include <string.h>

#include "modules/wmux/gsurf-wmux-private.h"

/* --- harness --- */

static gchar *
module_so_path(const gchar *name)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);
	g_autofree gchar *base = g_strconcat(name, ".so", NULL);

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

/* A manager with wmux.so loaded and configured from @yaml (NULL when the
 * module is not built; the caller skips). */
static GsurfModuleManager *
mgr_with(const gchar *yaml, GsurfModule **module)
{
	g_autofree gchar *so = module_so_path("wmux");
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = NULL;
	GsurfModuleManager *mgr;

	if (!g_file_test(so, G_FILE_TEST_EXISTS))
		return NULL;
	config = config_from(yaml);
	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);
	*module = gsurf_module_manager_get_module(mgr, "wmux");
	g_assert_nonnull(*module);
	return mgr;
}

#define WMUX(m) ((GsurfWmuxModule *)(m))

/* Configure @module again from @yaml. */
static void
reconfigure(GsurfModule *module, const gchar *yaml)
{
	g_autoptr(GsurfConfig) config = config_from(yaml);

	gsurf_module_configure(module, config);
}

/* The module's keybind help, as "key\tdescription" lines. */
static GHashTable *
help_of(GsurfModule *module)
{
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gsurf_keybind_help_free);
	GHashTable *map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	guint i;

	gsurf_keybind_provider_list_keybinds(GSURF_KEYBIND_PROVIDER(module), entries);
	for (i = 0; i < entries->len; i++) {
		GsurfKeybindHelp *h = g_ptr_array_index(entries, i);

		g_assert_cmpstr(gsurf_keybind_help_get_source(h), ==, "wmux");
		g_assert_false(g_hash_table_contains(map, gsurf_keybind_help_get_key(h)));
		g_hash_table_insert(map, g_strdup(gsurf_keybind_help_get_key(h)),
			g_strdup(gsurf_keybind_help_get_description(h)));
	}
	return map;
}

#define ASSERT_HELP(map, key, desc) \
	g_assert_cmpstr(g_hash_table_lookup((map), (key)), ==, (desc))

static void
assert_defaults(GsurfModule *module)
{
	GsurfWmuxModule *w = WMUX(module);
	g_autoptr(GHashTable) help = help_of(module);

	g_assert_cmpstr(w->prefix, ==, "C-b");
	g_assert_cmpint(w->prefix_timeout, ==, 0);
	g_assert_cmpint(w->repeat_time, ==, 500);
	g_assert_cmpint(w->display_panes_time, ==, 1000);
	g_assert_cmpint(w->split_ratio, ==, 50);
	g_assert_false(w->keep_tab);
	g_assert_false(w->keep_pane);
	g_assert_false(w->select_active_pane);
	g_assert_true(w->confirm_kill);
	g_assert_true(w->mouse);
	g_assert_true(w->remember_layout);
	g_assert_cmpint(w->min_pane_pct, ==, 10);
	g_assert_cmpint(w->gap, ==, 2);
	g_assert_cmpint(w->border_width, ==, 1);
	g_assert_cmpint(w->active_border_width, ==, 2);
	g_assert_cmpstr(w->border_color, ==, "#45475a");
	g_assert_cmpstr(w->active_border_color, ==, "#89b4fa");
	g_assert_true(w->container_colors);
	g_assert_cmpfloat_with_epsilon(w->inactive_opacity, 0.55, 1e-9);
	g_assert_cmpfloat_with_epsilon(w->active_brighten, 0.25, 1e-9);
	g_assert_true(w->status);
	g_assert_false(w->status_top);
	g_assert_cmpstr(w->status_bg, ==, "#181825");
	g_assert_cmpstr(w->status_fg, ==, "#cdd6f4");
	g_assert_cmpstr(w->status_current_fg, ==, "#89b4fa");
	g_assert_cmpint(w->window_name_max, ==, 20);
	g_assert_true(w->mode_indicator);
	g_assert_false(w->mode_hide_normal);
	g_assert_cmpstr(w->mode_normal_color, ==, "#89b4fa");
	g_assert_cmpstr(w->mode_insert_color, ==, "#a6e3a1");
	g_assert_cmpstr(w->mode_follow_color, ==, "#f9e2af");

	/* The tables: 53 prefix bindings plus the prefix itself, 4 root. */
	g_assert_cmpuint(g_hash_table_size(help), ==, 1 + 53 + 4);
	ASSERT_HELP(help, "C-b", "wmux prefix: the next key runs a prefix-table command");
	ASSERT_HELP(help, "C-b %", "split-window -h");
	ASSERT_HELP(help, "C-b \"", "split-window -v");
	ASSERT_HELP(help, "C-b C-%", "split-window -h -c");
	ASSERT_HELP(help, "C-b x", "confirm-before kill-pane");
	ASSERT_HELP(help, "C-b ,", "command-prompt -I #W rename-window");
	ASSERT_HELP(help, "C-b Up", "select-pane -U (repeatable)");
	ASSERT_HELP(help, "C-b M-Left", "resize-pane -L 5 (repeatable)");
	ASSERT_HELP(help, "C-b C-b", "send-prefix");
	ASSERT_HELP(help, "C-b 7", "select-window -t 7");
	ASSERT_HELP(help, "C-h", "select-pane -L");
	ASSERT_HELP(help, "C-j", "select-pane -D");
	ASSERT_HELP(help, "C-k", "select-pane -U");
	ASSERT_HELP(help, "C-l", "select-pane -R");
}

/* --- tests --- */

static void
test_defaults(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	GsurfModule *m = NULL;

	/* No wmux block at all, then an empty one, then just enabled. */
	mgr = mgr_with("modules: {}\n", &m);
	if (mgr == NULL) {
		g_test_skip("wmux.so not built");
		return;
	}
	g_assert_cmpstr(gsurf_module_get_name(m), ==, "wmux");
	g_assert_false(gsurf_module_get_enabled(m));
	assert_defaults(m);
	reconfigure(m, "modules:\n  wmux: {}\n");
	assert_defaults(m);
	reconfigure(m, "modules:\n  wmux:\n    enabled: true\n");
	assert_defaults(m);

	/* activate() without a window: active, the priority before modal. */
	g_assert_true(gsurf_module_activate(m));
	g_assert_cmpint(gsurf_module_get_priority(m), ==, -10);
	g_assert_false(WMUX(m)->installed);
	gsurf_module_deactivate(m);
	gsurf_module_deactivate(m);

	/* The manager owns the module: it goes with it. */
	{
		gpointer weak = m;

		g_object_add_weak_pointer(G_OBJECT(m), &weak);
		g_clear_object(&mgr);
		g_assert_null(weak);
	}
}

static const gchar *every_key_yaml =
	"modules:\n"
	"  wmux:\n"
	"    enabled: true\n"
	"    prefix: \"Ctrl+a\"\n"
	"    prefix_timeout: 1500\n"
	"    repeat_time: 800\n"
	"    display_panes_time: 2500\n"
	"    split_ratio: 30\n"
	"    on_pane_kill: keep_tab\n"
	"    on_tab_close: keep_pane\n"
	"    select_background: active_pane\n"
	"    confirm_kill: false\n"
	"    mouse: off\n"
	"    remember_layout: no\n"
	"    min_pane_pct: 5\n"
	"    gap: 0\n"
	"    border_width: 3\n"
	"    active_border_width: 6\n"
	"    border_color: \"red\"\n"
	"    active_border_color: \"rgb(0,255,0)\"\n"
	"    container_colors: false\n"
	"    inactive_opacity: 0.3\n"
	"    active_brighten: 1\n"
	"    status: false\n"
	"    status_position: top\n"
	"    status_bg: \"#1e1e2e\"\n"
	"    status_fg: \"white\"\n"
	"    status_current_fg: \"#f38ba8\"\n"
	"    window_name_max: 8\n"
	"    mode_indicator: false\n"
	"    mode_hide_normal: true\n"
	"    mode_normal_color: \"#ffffff\"\n"
	"    mode_insert_color: red\n"
	"    mode_follow_color: \"rgb(0,0,255)\"\n"
	"    bindings: {}\n"
	"    root_bindings: {}\n"
	"    repeatable: []\n";

static void
test_every_key(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	GsurfModule *m = NULL;
	GsurfWmuxModule *w;

	mgr = mgr_with(every_key_yaml, &m);
	if (mgr == NULL) {
		g_test_skip("wmux.so not built");
		return;
	}
	w = WMUX(m);
	g_assert_cmpstr(w->prefix, ==, "C-a");
	g_assert_cmpint(w->prefix_timeout, ==, 1500);
	g_assert_cmpint(w->repeat_time, ==, 800);
	g_assert_cmpint(w->display_panes_time, ==, 2500);
	g_assert_cmpint(w->split_ratio, ==, 30);
	g_assert_true(w->keep_tab);
	g_assert_true(w->keep_pane);
	g_assert_true(w->select_active_pane);
	g_assert_false(w->confirm_kill);
	g_assert_false(w->mouse);
	g_assert_false(w->remember_layout);
	g_assert_cmpint(w->min_pane_pct, ==, 5);
	g_assert_cmpint(w->gap, ==, 0);
	g_assert_cmpint(w->border_width, ==, 3);
	g_assert_cmpint(w->active_border_width, ==, 6);
	g_assert_cmpstr(w->border_color, ==, "rgb(255,0,0)");
	g_assert_cmpstr(w->active_border_color, ==, "rgb(0,255,0)");
	g_assert_false(w->container_colors);
	g_assert_cmpfloat_with_epsilon(w->inactive_opacity, 0.3, 1e-9);
	g_assert_cmpfloat_with_epsilon(w->active_brighten, 1.0, 1e-9);
	g_assert_false(w->status);
	g_assert_true(w->status_top);
	g_assert_cmpstr(w->status_bg, ==, "rgb(30,30,46)");
	g_assert_cmpstr(w->status_fg, ==, "rgb(255,255,255)");
	g_assert_cmpstr(w->status_current_fg, ==, "rgb(243,139,168)");
	g_assert_cmpint(w->window_name_max, ==, 8);
	g_assert_false(w->mode_indicator);
	g_assert_true(w->mode_hide_normal);
	g_assert_cmpstr(w->mode_normal_color, ==, "rgb(255,255,255)");
	g_assert_cmpstr(w->mode_insert_color, ==, "rgb(255,0,0)");
	g_assert_cmpstr(w->mode_follow_color, ==, "rgb(0,0,255)");

	/* A second configure() with nothing set: every default is back. */
	reconfigure(m, "modules:\n  wmux:\n    enabled: true\n");
	assert_defaults(m);
}

/* Every key with a bad value: one warning each, the default kept. */
static void
test_invalid_values(void)
{
	static const struct {
		const gchar *line;
		const gchar *warning;
	} bad[] = {
		{ "prefix: \"Super+b\"", "*wmux.prefix*Super*" },
		{ "prefix: \"\"", "*wmux.prefix*" },
		{ "prefix_timeout: -1", "*wmux.prefix_timeout must be an integer from 0 to 60000*" },
		{ "repeat_time: lots", "*wmux.repeat_time must be an integer*" },
		{ "display_panes_time: 5", "*wmux.display_panes_time must be an integer from 100*" },
		{ "split_ratio: 100", "*wmux.split_ratio must be an integer from 1 to 99*" },
		{ "on_pane_kill: explode", "*wmux.on_pane_kill must be 'close_tab' or 'keep_tab'*" },
		{ "on_tab_close: 3", "*wmux.on_tab_close must be 'close_pane' or 'keep_pane'*" },
		{ "select_background: pane", "*wmux.select_background must be 'solo' or 'active_pane'*" },
		{ "confirm_kill: maybe", "*wmux.confirm_kill must be true or false*" },
		{ "mouse: [1]", "*wmux.mouse must be true or false*" },
		{ "remember_layout: 2", "*wmux.remember_layout must be true or false*" },
		{ "min_pane_pct: 51", "*wmux.min_pane_pct must be an integer from 0 to 50*" },
		{ "gap: 65", "*wmux.gap must be an integer from 0 to 64*" },
		{ "border_width: 1.5", "*wmux.border_width must be an integer*" },
		{ "active_border_width: 33", "*wmux.active_border_width must be an integer*" },
		{ "border_color: \"not-a-colour\"", "*wmux.border_color*not a CSS colour*" },
		{ "active_border_color: 12", "*wmux.active_border_color*not a CSS colour*" },
		{ "container_colors: yesno", "*wmux.container_colors must be true or false*" },
		{ "inactive_opacity: 1.5", "*wmux.inactive_opacity must be a number from 0 to 1*" },
		{ "active_brighten: half", "*wmux.active_brighten must be a number*" },
		{ "status: {a: 1}", "*wmux.status must be true or false*" },
		{ "status_position: left", "*wmux.status_position must be 'bottom' or 'top'*" },
		{ "status_bg: \"#12\"", "*wmux.status_bg*not a CSS colour*" },
		{ "status_fg: \"\"", "*wmux.status_fg*not a CSS colour*" },
		{ "status_current_fg: nope", "*wmux.status_current_fg*not a CSS colour*" },
		{ "window_name_max: 0", "*wmux.window_name_max must be an integer from 1 to 200*" },
		{ "mode_indicator: maybe", "*wmux.mode_indicator must be true or false*" },
		{ "mode_hide_normal: 2", "*wmux.mode_hide_normal must be true or false*" },
		{ "mode_normal_color: nope", "*wmux.mode_normal_color*not a CSS colour*" },
		{ "mode_insert_color: \"#1\"", "*wmux.mode_insert_color*not a CSS colour*" },
		{ "mode_follow_color: []", "*wmux.mode_follow_color*not a CSS colour*" },
		{ "bindings: [a, b]", "*wmux.bindings must be a mapping*" },
		{ "root_bindings: \"C-h\"", "*wmux.root_bindings must be a mapping*" },
		{ "repeatable: o", "*wmux.repeatable must be a list*" },
		{ "colour: red", "*unknown option 'colour'*" },
	};
	g_autoptr(GsurfModuleManager) mgr = NULL;
	GsurfModule *m = NULL;
	guint i;

	mgr = mgr_with("modules: {}\n", &m);
	if (mgr == NULL) {
		g_test_skip("wmux.so not built");
		return;
	}
	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autofree gchar *yaml = g_strdup_printf(
			"modules:\n  wmux:\n    enabled: true\n    %s\n", bad[i].line);

		g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, bad[i].warning);
		reconfigure(m, yaml);
		g_test_assert_expected_messages();
		assert_defaults(m);
	}
}

static void
test_bindings(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GHashTable) help = NULL;
	GsurfModule *m = NULL;

	mgr = mgr_with("modules: {}\n", &m);
	if (mgr == NULL) {
		g_test_skip("wmux.so not built");
		return;
	}

	/* 1. Rebind, add, unbind with "" and null; tmux and gsurf key
	 *    notation; a root override and a new root key. */
	reconfigure(m,
		"modules:\n  wmux:\n    enabled: true\n"
		"    bindings:\n"
		"      \"%\": \"split-window -v\"\n"
		"      \"g\": \"split-window -h https://github.com\"\n"
		"      \"Shift+apostrophe\": \"\"\n"
		"      \"x\": null\n"
		"      \"M-d\": \"select-layout main-vertical\"\n"
		"    root_bindings:\n"
		"      \"C-l\": \"\"\n"
		"      \"Alt+l\": \"select-pane -R\"\n");
	help = help_of(m);
	ASSERT_HELP(help, "C-b %", "split-window -v");
	ASSERT_HELP(help, "C-b g", "split-window -h https://github.com");
	ASSERT_HELP(help, "C-b M-d", "select-layout main-vertical");
	g_assert_false(g_hash_table_contains(help, "C-b \""));
	g_assert_false(g_hash_table_contains(help, "C-b x"));
	g_assert_false(g_hash_table_contains(help, "C-l"));
	ASSERT_HELP(help, "M-l", "select-pane -R");
	ASSERT_HELP(help, "C-h", "select-pane -L");
	g_clear_pointer(&help, g_hash_table_unref);

	/* 2. Bad commands and keys warn and are skipped; the defaults for
	 *    those keys stay. */
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*wmux.bindings*Foo*skipped*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
		"*wmux.bindings.o: the command must be a string*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
		"*wmux.bindings*%*unknown command \"spilt-window\"*skipped*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
		"*wmux.bindings*z*has no flag -x*skipped*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*wmux.root_bindings*C-h*skipped*");
	reconfigure(m,
		"modules:\n  wmux:\n    enabled: true\n"
		"    bindings:\n"
		"      \"Foo+b\": \"kill-pane\"\n"
		"      \"%\": \"spilt-window -h\"\n"
		"      \"z\": \"resize-pane -x\"\n"
		"      \"o\": [1, 2]\n"
		"    root_bindings:\n"
		"      \"C-h\": \"select-pane -Q\"\n");
	g_test_assert_expected_messages();
	help = help_of(m);
	ASSERT_HELP(help, "C-b %", "split-window -h");
	ASSERT_HELP(help, "C-b z", "resize-pane -Z");
	ASSERT_HELP(help, "C-b o", "select-pane -t +1");
	ASSERT_HELP(help, "C-h", "select-pane -L");
	g_clear_pointer(&help, g_hash_table_unref);

	/* 3. Extra repeatable keys; an unbound one warns. */
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*wmux.repeatable*F7*skipped*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*wmux.repeatable[2] must be a key*");
	reconfigure(m,
		"modules:\n  wmux:\n    enabled: true\n"
		"    bindings:\n      \"H\": \"resize-pane -L 10\"\n"
		"    repeatable: [\"o\", \"F7\", [x], \"H\"]\n");
	g_test_assert_expected_messages();
	help = help_of(m);
	ASSERT_HELP(help, "C-b o", "select-pane -t +1 (repeatable)");
	ASSERT_HELP(help, "C-b H", "resize-pane -L 10 (repeatable)");
	ASSERT_HELP(help, "C-b ;", "last-pane");
	g_clear_pointer(&help, g_hash_table_unref);

	/* 4. Back to the defaults. */
	reconfigure(m, "modules:\n  wmux: {}\n");
	assert_defaults(m);
}

/* A new prefix takes send-prefix with it (C-a C-a), unless the user bound
 * the new prefix key themselves. */
static void
test_prefix_moves_send_prefix(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GHashTable) help = NULL;
	GsurfModule *m = NULL;

	mgr = mgr_with("modules:\n  wmux:\n    prefix: C-a\n", &m);
	if (mgr == NULL) {
		g_test_skip("wmux.so not built");
		return;
	}
	help = help_of(m);
	ASSERT_HELP(help, "C-a", "wmux prefix: the next key runs a prefix-table command");
	ASSERT_HELP(help, "C-a C-a", "send-prefix");
	ASSERT_HELP(help, "C-a %", "split-window -h");
	g_assert_false(g_hash_table_contains(help, "C-a C-b"));
	g_assert_false(g_hash_table_contains(help, "C-b"));
	g_clear_pointer(&help, g_hash_table_unref);

	/* The user's own binding for the new prefix key wins. */
	reconfigure(m, "modules:\n  wmux:\n    prefix: \"Ctrl+a\"\n"
		"    bindings:\n      \"C-a\": last-window\n");
	help = help_of(m);
	ASSERT_HELP(help, "C-a C-a", "last-window");
	g_clear_pointer(&help, g_hash_table_unref);

	/* M-Space as the prefix (gsurf notation), C-b back to the defaults. */
	reconfigure(m, "modules:\n  wmux:\n    prefix: \"Alt+space\"\n");
	help = help_of(m);
	ASSERT_HELP(help, "M-Space M-Space", "send-prefix");
	g_clear_pointer(&help, g_hash_table_unref);
	reconfigure(m, "modules:\n  wmux:\n    prefix: C-b\n");
	assert_defaults(m);
}

/* --- refusals (headless windows) --- */

typedef struct { GsurfWindow parent; } TestWindow;
typedef struct { GsurfWindowClass parent; } TestWindowClass;
GType test_window_get_type(void);
G_DEFINE_TYPE(TestWindow, test_window, GSURF_TYPE_WINDOW)

static void w_set_content(GsurfWindow *w, gpointer widget) { (void)w; (void)widget; }
static void
test_window_class_init(TestWindowClass *klass)
{
	GSURF_WINDOW_CLASS(klass)->set_content = w_set_content;
}
static void test_window_init(TestWindow *self) { (void)self; }

/* Like libregnum: no native content to swap. */
typedef struct { GsurfWindow parent; } TestPlainWindow;
typedef struct { GsurfWindowClass parent; } TestPlainWindowClass;
GType test_plain_window_get_type(void);
G_DEFINE_TYPE(TestPlainWindow, test_plain_window, GSURF_TYPE_WINDOW)
static void test_plain_window_class_init(TestPlainWindowClass *klass) { (void)klass; }
static void test_plain_window_init(TestPlainWindow *self) { (void)self; }

/* Activate wmux for a window of @type (kiosk when @kiosk): it must stay
 * inactive and say so once, however often it is activated. */
static void
check_refusal(GType type, gboolean kiosk, const gchar *message)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfApplication) app = NULL;
	g_autoptr(GsurfWindow) window = NULL;
	GsurfModuleManager *def = gsurf_module_manager_get_default();
	GsurfModule *m = NULL;

	mgr = mgr_with("modules:\n  wmux:\n    enabled: true\n", &m);
	if (mgr == NULL) {
		g_test_skip("wmux.so not built");
		return;
	}
	config->kiosk = kiosk;
	gsurf_config_set_default(config);
	app = gsurf_application_new(config);
	window = g_object_new(type, NULL);
	gsurf_application_add_window(app, window);
	gsurf_module_manager_set_application(def, app);

	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, message);
	g_assert_true(gsurf_module_activate(m));
	g_test_assert_expected_messages();
	g_assert_null(gsurf_window_get_layout(window));
	g_assert_false(WMUX(m)->installed);
	g_assert_null(WMUX(m)->root);
	/* Logged once: a second activation is silent. */
	g_assert_true(gsurf_module_activate(m));
	g_assert_null(gsurf_window_get_layout(window));
	gsurf_module_deactivate(m);

	gsurf_module_manager_set_application(def, NULL);
	gsurf_config_set_default(NULL);
}

static void
test_kiosk_refusal(void)
{
	check_refusal(test_window_get_type(), TRUE, "*kiosk mode shows a single view*");
}

static void
test_no_content_refusal(void)
{
	check_refusal(test_plain_window_get_type(), FALSE, "*this window cannot host panes*");
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/wmux/defaults", test_defaults);
	g_test_add_func("/wmux/every-key", test_every_key);
	g_test_add_func("/wmux/invalid-values", test_invalid_values);
	g_test_add_func("/wmux/bindings", test_bindings);
	g_test_add_func("/wmux/prefix-moves-send-prefix", test_prefix_moves_send_prefix);
	g_test_add_func("/wmux/kiosk-refusal", test_kiosk_refusal);
	g_test_add_func("/wmux/no-content-refusal", test_no_content_refusal);
	return g_test_run();
}
