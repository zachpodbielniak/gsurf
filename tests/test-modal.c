/*
 * test-modal.c - The real modal module: link hints, modes and insert keys
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>

#include "test-util.h"

/* A headless view captures scripts at the public backend boundary. Complete
 * asynchronously so modal's nested loop follows the real backend contract. */
typedef struct {
	GsurfView parent_instance;
	gchar *script;      /* the last script run */
	GString *all;       /* every script run, newline separated */
	const gchar *reply; /* what a script returns ("0" unless set) */
} HintView;
typedef GsurfViewClass HintViewClass;

GType hint_view_get_type(void);
G_DEFINE_TYPE(HintView, hint_view, GSURF_TYPE_VIEW)

static void
hint_view_run_async(GsurfView *view, const gchar *script,
                    GCancellable *cancellable, GAsyncReadyCallback callback,
                    gpointer user_data)
{
	HintView *self = (HintView *)view;
	g_autoptr(GTask) task = g_task_new(view, cancellable, callback, user_data);

	g_free(self->script);
	self->script = g_strdup(script);
	if (self->all == NULL)
		self->all = g_string_new(NULL);
	g_string_append_printf(self->all, "%s\n", script);
	/* Zero matches leaves modal in normal mode for the next f/F check. */
	g_task_return_pointer(task, g_strdup(self->reply != NULL ? self->reply : "0"), g_free);
}

/* Transfer the result exactly as a real JavaScript backend would. */
static gchar *
hint_view_run_finish(GsurfView *view, GAsyncResult *result, GError **error)
{
	(void)view;
	return (gchar *)g_task_propagate_pointer(G_TASK(result), error);
}

/* Release the captured script before chaining to the base view. */
static void
hint_view_finalize(GObject *object)
{
	g_free(((HintView *)object)->script);
	if (((HintView *)object)->all != NULL)
		g_string_free(((HintView *)object)->all, TRUE);
	G_OBJECT_CLASS(hint_view_parent_class)->finalize(object);
}

/* Install only the vfuncs needed by link hinting; no GTK/display is needed. */
static void
hint_view_class_init(HintViewClass *klass)
{
	klass->run_javascript_async = hint_view_run_async;
	klass->run_javascript_finish = hint_view_run_finish;
	G_OBJECT_CLASS(klass)->finalize = hint_view_finalize;
}

/* GObject zero-initializes the captured script pointer. */
static void
hint_view_init(HintView *self)
{
	(void)self;
}

/* Load the built module once and keep its registered GType resident. Each
 * assertion uses a fresh instance so mode/config state cannot leak. */
static GsurfModule *
new_modal(void)
{
	static GType module_type = G_TYPE_INVALID;

	if (module_type == G_TYPE_INVALID) {
		g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
		g_autofree gchar *dir = g_path_get_dirname(exe);
		g_autofree gchar *path = g_build_filename(dir, "modules", "modal.so", NULL);
		GModule *library = g_module_open(path, G_MODULE_BIND_LAZY);
		GType (*register_type)(void) = NULL;

		g_assert_nonnull(library);
		g_assert_true(g_module_symbol(library, "gsurf_module_register", (gpointer *)&register_type));
		g_module_make_resident(library);
		module_type = register_type();
		g_module_close(library);
	}
	return g_object_new(module_type, NULL);
}

/* Dispatch both actual keys and inspect the backend's resulting CSS. */
static void
assert_hint_size(GsurfModule *module, gint size)
{
	g_autoptr(GsurfView) view = g_object_new(hint_view_get_type(), NULL);
	g_autofree gchar *css = g_strdup_printf("font:bold %dpx monospace", size);
	const guint keys[] = { 'f', 'F' };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		g_assert_true(gsurf_input_handler_handle_key_event(
			GSURF_INPUT_HANDLER(module), view, keys[i], 0, 0, GSURF_MODE_NORMAL));
		g_assert_nonnull(strstr(((HintView *)view)->script, css));
		g_assert_nonnull(strstr(((HintView *)view)->script,
			i == 0 ? "window.__gsurfNV=false" : "window.__gsurfNV=true"));
	}
}

/* Missing values preserve the old appearance; a YAML override changes both
 * follow modes, and reconfiguration without the option retains that value. */
static void
test_hint_yaml(void)
{
	g_autoptr(GsurfModule) module = new_modal();
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GError) error = NULL;

	assert_hint_size(module, 11);
	g_assert_true(gsurf_config_load_from_data(config,
		"modules:\n  modal:\n    hint_font_size: 15\n", -1, &error));
	g_assert_no_error(error);
	gsurf_module_configure(module, config);
	assert_hint_size(module, 15);
	g_assert_true(gsurf_config_load_from_data(config,
		"modules:\n  modal:\n    enabled: true\n", -1, &error));
	g_assert_no_error(error);
	gsurf_module_configure(module, config);
	assert_hint_size(module, 15);
}

/* Invalid scalars and containers must not generate unusable CSS or silently
 * truncate a fractional/unit-suffixed size to an accepted integer. */
static void
test_hint_invalid(void)
{
	const gchar *values[] = { "0", "-1", "1.5", "15px", "true", "null",
		"\"\"", "[]", "{}", "2147483648", "999999999999999999999999" };
	g_autoptr(GsurfModule) module = new_modal();
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	guint i;

	for (i = 0; i < G_N_ELEMENTS(values); i++) {
		g_autofree gchar *yaml = g_strdup_printf(
			"modules:\n  modal:\n    hint_font_size: %s\n", values[i]);
		g_autoptr(GError) error = NULL;

		g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
		g_assert_no_error(error);
		g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*hint_font_size*positive integer*");
		gsurf_module_configure(module, config);
		g_test_assert_expected_messages();
		assert_hint_size(module, 11);
	}
}

/* Exercise the actual crispy compile/load path after a disabled YAML file.
 * Embedded YAML in C is still parsed: ignore_yaml applies per document. */
static void
test_hint_c_config(void)
{
	const gchar *source =
		"#include <gsurf/gsurf.h>\n"
		"G_MODULE_EXPORT gboolean gsurf_config_init(void)\n"
		"{\n"
		" GsurfConfig *config = gsurf_config_get_default();\n"
		" return gsurf_config_load_from_data(config,\n"
		"  \"modules:\\n  modal:\\n    hint_font_size: 19\\n\", -1, NULL);\n"
		"}\n";
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfigCompiler) compiler = gsurf_config_compiler_new(&error);
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfModule) module = new_modal();
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;

	g_assert_no_error(error);
	g_assert_nonnull(compiler);
	dir = g_dir_make_tmp("gsurf-hint-config-XXXXXX", &error);
	g_assert_no_error(error);
	path = g_build_filename(dir, "config.c", NULL);
	g_assert_true(g_file_set_contents(path, source, -1, &error));
	g_assert_no_error(error);
	g_assert_true(gsurf_config_load_from_data(config,
		"ignore_yaml: true\nmodules:\n  modal:\n    hint_font_size: 30\n", -1, &error));
	g_assert_no_error(error);
	gsurf_module_configure(module, config);
	assert_hint_size(module, 11);
	gsurf_config_set_default(config);
	g_assert_true(gsurf_config_compiler_compile_and_load(compiler, path, &error));
	g_assert_no_error(error);
	gsurf_module_configure(module, config);
	assert_hint_size(module, 19);
	gsurf_config_set_default(NULL);
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* --- modes and insert keys --- */

#define KEY_ESCAPE (0xff1b)

static gboolean
key(GsurfModule *module, GsurfView *view, guint keyval)
{
	return gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(module), view,
		keyval, 0, 0, GSURF_MODE_NORMAL);
}

/* INSERT publishes passthrough to the manager (the host drops bare keys),
 * and the mode itself for mode indicators: the two always agree. */
static gboolean
inserting(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	gboolean passthrough = gsurf_module_manager_get_input_passthrough(mgr);

	g_assert_cmpint(passthrough, ==,
		gsurf_module_manager_get_mode(mgr) == GSURF_MODE_INSERT);
	return passthrough;
}

static void
configure_yaml(GsurfModule *module, const gchar *yaml)
{
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	gsurf_module_configure(module, config);
}

/* Both default insert keys enter INSERT; Escape leaves it and blurs the
 * focused field (the blur script ran and the editing state is cleared). */
static void
test_insert_keys_default(void)
{
	g_autoptr(GsurfModule) module = new_modal();
	g_autoptr(GsurfView) view = g_object_new(hint_view_get_type(), NULL);
	HintView *hv = (HintView *)view;
	const guint keys[] = { 'i', 'a' };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		g_assert_false(inserting());
		g_assert_true(key(module, view, keys[i]));
		g_assert_true(inserting());
		/* In INSERT every bare key belongs to the page. */
		g_assert_false(key(module, view, 'o'));
		g_assert_false(key(module, view, 'j'));
		g_assert_true(inserting());
		gsurf_view_set_editing(view, TRUE);
		g_clear_pointer(&hv->script, g_free);
		g_assert_true(key(module, view, KEY_ESCAPE));
		g_assert_false(inserting());
		g_assert_false(gsurf_view_get_editing(view));
		g_assert_nonnull(hv->script);
		g_assert_nonnull(strstr(hv->script, ".blur()"));
		g_assert_nonnull(strstr(hv->script, "shadowRoot"));
	}

	/* The actions do the same. */
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), view,
		GSURF_ACTION_ENTER_INSERT_MODE));
	g_assert_true(inserting());
	g_clear_pointer(&hv->script, g_free);
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), view,
		GSURF_ACTION_ENTER_NORMAL_MODE));
	g_assert_false(inserting());
	g_assert_nonnull(strstr(hv->script, ".blur()"));
}

/* The published mode follows every switch (FOLLOW included would need
 * a page; INSERT and NORMAL here), and a deactivated modal module leaves
 * NORMAL behind, passthrough off. */
static void
test_mode_published(void)
{
	g_autoptr(GsurfModule) module = new_modal();
	g_autoptr(GsurfView) view = g_object_new(hint_view_get_type(), NULL);
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();

	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
	g_assert_true(gsurf_module_activate(module));
	g_assert_true(key(module, view, 'i'));
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_INSERT);
	gsurf_module_deactivate(module);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
	g_assert_false(inserting());
	/* start_mode: insert publishes INSERT at configuration. */
	configure_yaml(module, "modules:\n  modal:\n    start_mode: insert\n");
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_INSERT);
	g_assert_true(key(module, view, KEY_ESCAPE));
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
}

/* With a page field focused (NORMAL), bare keys are typing, never
 * commands or mode switches; Escape blurs and stays in NORMAL. */
static void
test_normal_with_field(void)
{
	g_autoptr(GsurfModule) module = new_modal();
	g_autoptr(GsurfView) view = g_object_new(hint_view_get_type(), NULL);
	HintView *hv = (HintView *)view;
	const guint keys[] = { 'o', 'a', 'i', 'f', 'g', 'j' };
	guint i;

	gsurf_view_set_editing(view, TRUE);
	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		g_assert_false(key(module, view, keys[i]));
		g_assert_false(inserting());
	}
	g_assert_null(hv->script);
	g_assert_true(key(module, view, KEY_ESCAPE));
	g_assert_false(gsurf_view_get_editing(view));
	g_assert_false(inserting());
	g_assert_nonnull(strstr(hv->script, ".blur()"));
	/* Unfocused, `a` is a command again. */
	g_assert_true(key(module, view, 'a'));
	g_assert_true(inserting());
	g_assert_true(key(module, view, KEY_ESCAPE));
}

static void
test_insert_keys_config(void)
{
	g_autoptr(GsurfModule) module = new_modal();
	g_autoptr(GsurfView) view = g_object_new(hint_view_get_type(), NULL);

	/* A list replaces the defaults. */
	configure_yaml(module, "modules:\n  modal:\n    insert_keys: [i, I]\n");
	g_assert_false(key(module, view, 'a'));
	g_assert_false(inserting());
	g_assert_true(key(module, view, 'I'));
	g_assert_true(inserting());
	g_assert_true(key(module, view, KEY_ESCAPE));

	/* A reconfigure without the option keeps them. */
	configure_yaml(module, "modules:\n  modal:\n    scroll_step: 10\n");
	g_assert_true(key(module, view, 'I'));
	g_assert_true(inserting());
	g_assert_true(key(module, view, KEY_ESCAPE));

	/* A single name is accepted too. */
	configure_yaml(module, "modules:\n  modal:\n    insert_keys: a\n");
	g_assert_false(key(module, view, 'I'));
	g_assert_true(key(module, view, 'a'));
	g_assert_true(inserting());
	g_assert_true(key(module, view, KEY_ESCAPE));

	/* Bad values are reported and change nothing. */
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*insert_keys must be a list*");
	configure_yaml(module, "modules:\n  modal:\n    insert_keys: {x: 1}\n");
	g_test_assert_expected_messages();
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*insert_keys: entry 1 is not a key name*");
	configure_yaml(module, "modules:\n  modal:\n    insert_keys: [i, [x]]\n");
	g_test_assert_expected_messages();
	g_assert_true(key(module, view, 'a'));
	g_assert_true(inserting());
	g_assert_true(key(module, view, KEY_ESCAPE));

	/* An empty list leaves only the action. */
	configure_yaml(module, "modules:\n  modal:\n    insert_keys: []\n");
	g_assert_false(key(module, view, 'a'));
	g_assert_false(key(module, view, 'i'));
	g_assert_false(inserting());
}

/* gi focuses the first field (the page script) and enters INSERT only
 * when there was one. */
static void
test_gi(void)
{
	g_autoptr(GsurfModule) module = new_modal();
	g_autoptr(GsurfView) view = g_object_new(hint_view_get_type(), NULL);
	HintView *hv = (HintView *)view;

	hv->reply = "none";
	g_assert_true(key(module, view, 'g'));
	g_assert_true(key(module, view, 'i'));
	g_assert_nonnull(strstr(hv->script, "querySelectorAll"));
	g_assert_nonnull(strstr(hv->script, "focus()"));
	g_assert_false(inserting());

	hv->reply = "ok";
	g_assert_true(key(module, view, 'g'));
	g_assert_true(key(module, view, 'i'));
	g_assert_true(inserting());
	g_assert_true(key(module, view, KEY_ESCAPE));
	g_assert_false(inserting());
}

/* The help list follows the configured insert keys and lists gi. */
static void
test_keybind_list(void)
{
	g_autoptr(GsurfModule) module = new_modal();
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gsurf_keybind_help_free);
	gboolean seen_i = FALSE, seen_a = FALSE, seen_gi = FALSE;
	guint i;

	gsurf_keybind_provider_list_keybinds(GSURF_KEYBIND_PROVIDER(module), entries);
	for (i = 0; i < entries->len; i++) {
		GsurfKeybindHelp *e = g_ptr_array_index(entries, i);
		const gchar *k = gsurf_keybind_help_get_key(e);

		seen_i |= g_strcmp0(k, "i") == 0;
		seen_a |= g_strcmp0(k, "a") == 0;
		seen_gi |= g_strcmp0(k, "gi") == 0;
	}
	g_assert_true(seen_i);
	g_assert_true(seen_a);
	g_assert_true(seen_gi);
}

/* Isolate crispy's cache from the user's real browser configuration. */
int
main(int argc, char **argv)
{
	g_autofree gchar *cache = NULL;
	gint result;

	g_test_init(&argc, &argv, NULL);
	cache = g_dir_make_tmp("gsurf-hint-cache-XXXXXX", NULL);
	g_assert_nonnull(cache);
	g_setenv("XDG_CACHE_HOME", cache, TRUE);
	g_test_add_func("/gsurf/modal/hint-yaml", test_hint_yaml);
	g_test_add_func("/gsurf/modal/hint-invalid", test_hint_invalid);
	g_test_add_func("/gsurf/modal/hint-c-config", test_hint_c_config);
	g_test_add_func("/gsurf/modal/insert-keys-default", test_insert_keys_default);
	g_test_add_func("/gsurf/modal/normal-with-field", test_normal_with_field);
	g_test_add_func("/gsurf/modal/insert-keys-config", test_insert_keys_config);
	g_test_add_func("/gsurf/modal/gi", test_gi);
	g_test_add_func("/gsurf/modal/mode-published", test_mode_published);
	g_test_add_func("/gsurf/modal/keybind-list", test_keybind_list);
	result = g_test_run();
	/* The C config compiler may leave more than gsurf/cconfig behind (a
	 * ccache-wrapped gcc writes $XDG_CACHE_HOME/ccache), so remove the
	 * whole temporary tree instead of assuming its layout. */
	g_assert_true(gsurf_test_rmtree(cache));
	return result;
}
