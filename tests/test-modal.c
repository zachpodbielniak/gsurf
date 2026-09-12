/*
 * test-modal.c - Link-hint configuration through the real modal module
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>

/* A headless view captures scripts at the public backend boundary. Complete
 * asynchronously so modal's nested loop follows the real backend contract. */
typedef struct {
	GsurfView parent_instance;
	gchar *script;
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
	/* Zero matches leaves modal in normal mode for the next f/F check. */
	g_task_return_pointer(task, g_strdup("0"), g_free);
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

/* Isolate crispy's cache from the user's real browser configuration. */
int
main(int argc, char **argv)
{
	g_autofree gchar *cache = NULL;
	g_autofree gchar *compiled_dir = NULL;
	g_autofree gchar *browser_dir = NULL;
	g_autoptr(GDir) dir = NULL;
	const gchar *name;
	gint result;

	g_test_init(&argc, &argv, NULL);
	cache = g_dir_make_tmp("gsurf-hint-cache-XXXXXX", NULL);
	g_assert_nonnull(cache);
	g_setenv("XDG_CACHE_HOME", cache, TRUE);
	g_test_add_func("/gsurf/modal/hint-yaml", test_hint_yaml);
	g_test_add_func("/gsurf/modal/hint-invalid", test_hint_invalid);
	g_test_add_func("/gsurf/modal/hint-c-config", test_hint_c_config);
	result = g_test_run();
	browser_dir = g_build_filename(cache, "gsurf", NULL);
	compiled_dir = g_build_filename(browser_dir, "cconfig", NULL);
	dir = g_dir_open(compiled_dir, 0, NULL);
	if (dir != NULL) {
		while ((name = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *path = g_build_filename(compiled_dir, name, NULL);
			g_assert_cmpint(g_unlink(path), ==, 0);
		}
		g_clear_pointer(&dir, g_dir_close);
		g_assert_cmpint(g_rmdir(compiled_dir), ==, 0);
		g_assert_cmpint(g_rmdir(browser_dir), ==, 0);
	}
	g_assert_cmpint(g_rmdir(cache), ==, 0);
	return result;
}
