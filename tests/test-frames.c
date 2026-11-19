/*
 * test-frames.c - A page's frames, without a web engine
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   - the frames module: registers its web-process extension while
 *     active and only then; `web_process_extension:` points elsewhere; a
 *     path that is not there registers nothing; a reconfigure replaces it;
 *   - the view API over a fake view: what the extension's replies become
 *     (a value with the frame's place, no such frame, a script error, an
 *     answer of the wrong shape), the main frame without the extension,
 *     and a view that cannot reach one;
 *   - input into a frame on a backend without native input runs in the
 *     frame, at the frame's own point.
 * The extension itself, against WebKit and a cross-origin frame, is in
 * test-control-webkit.c and test-mcp-webkit.c.
 */

#include <gsurf.h>
#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include "core/gsurf-frames-protocol.h"

static gchar *
module_so_path(const gchar *name)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);
	g_autofree gchar *base = g_strconcat(name, ".so", NULL);

	return g_build_filename(dir, "modules", base, NULL);
}

/* A manager with the frames module loaded from @yaml and activated, or
 * NULL when it is not built. */
static GsurfModuleManager *
frames_manager(const gchar *yaml)
{
	g_autofree gchar *so = module_so_path("frames");
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GError) error = NULL;
	GsurfModuleManager *mgr;

	if (!g_file_test(so, G_FILE_TEST_EXISTS))
		return NULL;
	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(mgr);
	return mgr;
}

/* ------------------------------------------------------------------ */
/* The module                                                          */
/* ------------------------------------------------------------------ */

static void
test_module_registers(void)
{
	g_autoptr(GsurfModuleManager) mgr = frames_manager("modules:\n  frames:\n    enabled: true\n");
	g_autofree gchar *dir = NULL;
	GsurfModule *module;

	if (mgr == NULL) {
		g_test_skip("frames.so is not built");
		return;
	}
	module = gsurf_module_manager_get_module(mgr, "frames");
	g_assert_nonnull(module);
	g_assert_cmpstr(gsurf_module_get_name(module), ==, "frames");
	g_assert_true(gsurf_module_is_active(module));

	/* the only extension: WebKit is pointed at its own directory */
	dir = gsurf_web_process_extension_get_directory();
	g_assert_nonnull(dir);
	g_assert_true(g_str_has_suffix(dir, "frames-webext"));

	/* deactivated: gone for every web process started after */
	gsurf_module_manager_deactivate_all(mgr);
	g_clear_pointer(&dir, g_free);
	dir = gsurf_web_process_extension_get_directory();
	g_assert_null(dir);
}

static void
test_module_disabled(void)
{
	g_autoptr(GsurfModuleManager) mgr =
		frames_manager("modules:\n  frames:\n    enabled: false\n");
	g_autofree gchar *dir = NULL;

	if (mgr == NULL) {
		g_test_skip("frames.so is not built");
		return;
	}
	g_assert_false(gsurf_module_is_active(gsurf_module_manager_get_module(mgr, "frames")));
	dir = gsurf_web_process_extension_get_directory();
	g_assert_null(dir);
}

static void
test_module_path(void)
{
	g_autofree gchar *so = module_so_path("frames");
	g_autofree gchar *moddir = g_path_get_dirname(so);
	g_autofree gchar *webext = g_build_filename(moddir, "frames-webext",
		"gsurf-frames-webext.so", NULL);
	g_autofree gchar *yaml = g_strdup_printf("modules:\n  frames:\n    enabled: true\n"
		"    web_process_extension: %s\n", webext);
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GsurfConfig) other = gsurf_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	GsurfModule *module;

	if (!g_file_test(webext, G_FILE_TEST_EXISTS)) {
		g_test_skip("the frames web-process extension is not built");
		return;
	}

	/* 1. An explicit path that is there. */
	mgr = frames_manager(yaml);
	module = gsurf_module_manager_get_module(mgr, "frames");
	dir = gsurf_web_process_extension_get_directory();
	{
		g_autofree gchar *expected = g_path_get_dirname(webext);

		g_assert_cmpstr(dir, ==, expected);
	}
	g_clear_pointer(&dir, g_free);

	/* 2. Reconfigured to a path that is not there: nothing registered,
	 *    and nothing left from before. */
	gsurf_module_deactivate(module);
	g_assert_true(gsurf_config_load_from_data(other, "modules:\n  frames:\n    enabled: true\n"
		"    web_process_extension: /nonexistent/gsurf-frames-webext.so\n", -1, &error));
	g_assert_no_error(error);
	gsurf_module_configure(module, other);
	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*web-process extension not found at "
		"/nonexistent/gsurf-frames-webext.so*");
	g_assert_true(gsurf_module_activate(module));
	g_test_assert_expected_messages();
	dir = gsurf_web_process_extension_get_directory();
	g_assert_null(dir);
	gsurf_module_manager_deactivate_all(mgr);
}

/* ------------------------------------------------------------------ */
/* The view API, over a fake view                                      */
/* ------------------------------------------------------------------ */

/* A view whose "page" answers with a canned reply, and whose main frame
 * answers every script with "main:<script>". */
typedef struct {
	GsurfView  parent;
	GVariant  *reply;     /* NULL: no extension answers */
	gchar     *last_name;
	GVariant  *last_params;
	GPtrArray *scripts;   /* every main-frame script */
} FakeView;
typedef struct { GsurfViewClass parent; } FakeViewClass;

GType fake_view_get_type(void);
G_DEFINE_TYPE(FakeView, fake_view, GSURF_TYPE_VIEW)

static void
fake_send_page_message(GsurfView *view, const gchar *name, GVariant *params, GTask *task_in)
{
	FakeView *self = (FakeView *)view;
	g_autoptr(GTask) task = task_in;

	g_free(self->last_name);
	self->last_name = g_strdup(name);
	g_clear_pointer(&self->last_params, g_variant_unref);
	self->last_params = params != NULL ? g_variant_ref(params) : NULL;
	if (self->reply == NULL)
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"no web-process extension answers '%s'", name);
	else
		g_task_return_pointer(task, g_variant_ref(self->reply),
			(GDestroyNotify)g_variant_unref);
}

static void
fake_run_javascript_async(GsurfView *view, const gchar *script, GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
	FakeView *self = (FakeView *)view;
	g_autoptr(GTask) task = g_task_new(view, cancellable, callback, user_data);

	g_ptr_array_add(self->scripts, g_strdup(script));
	/* "undefined" has no JSON form: no text and no error, as WebKit's */
	g_task_return_pointer(task, g_strcmp0(script, "undefined") == 0 ? NULL
		: g_strconcat("main:", script, NULL), g_free);
}

static gchar *
fake_run_javascript_finish(GsurfView *view, GAsyncResult *result, GError **error)
{
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
fake_view_finalize(GObject *object)
{
	FakeView *self = (FakeView *)object;

	g_clear_pointer(&self->reply, g_variant_unref);
	g_clear_pointer(&self->last_params, g_variant_unref);
	g_free(self->last_name);
	g_ptr_array_unref(self->scripts);
	G_OBJECT_CLASS(fake_view_parent_class)->finalize(object);
}

static void
fake_view_class_init(FakeViewClass *klass)
{
	GsurfViewClass *view_class = GSURF_VIEW_CLASS(klass);

	G_OBJECT_CLASS(klass)->finalize = fake_view_finalize;
	view_class->send_page_message = fake_send_page_message;
	view_class->run_javascript_async = fake_run_javascript_async;
	view_class->run_javascript_finish = fake_run_javascript_finish;
}

static void
fake_view_init(FakeView *self)
{
	self->scripts = g_ptr_array_new_with_free_func(g_free);
}

/* A view with neither web processes nor scripts. */
typedef struct { GsurfView parent; } BareView;
typedef struct { GsurfViewClass parent; } BareViewClass;

GType bare_view_get_type(void);
G_DEFINE_TYPE(BareView, bare_view, GSURF_TYPE_VIEW)

static void bare_view_class_init(BareViewClass *klass) { (void)klass; }
static void bare_view_init(BareView *self) { (void)self; }

typedef struct {
	GAsyncResult *result;
} Wait;

static void
on_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Wait *w = user_data;

	(void)source;
	w->result = g_object_ref(result);
}

static GAsyncResult *
wait_for(Wait *w)
{
	while (w->result == NULL)
		g_main_context_iteration(NULL, TRUE);
	return w->result;
}

/* Run @script in @frame of @view; the value, or NULL with @error. */
static gchar *
run_in(GsurfView *view, const gchar *frame, const gchar *require, const gchar *script,
       gdouble *x, gdouble *y, guint64 *id, GError **error)
{
	Wait w = { NULL };
	gchar *text;

	gsurf_view_run_javascript_in_frame_async(view, frame, require, script, NULL, on_done, &w);
	text = gsurf_view_run_javascript_in_frame_finish(view, wait_for(&w), x, y, id, error);
	g_object_unref(w.result);
	return text;
}

static void
set_reply(FakeView *view, GVariant *reply)
{
	g_clear_pointer(&view->reply, g_variant_unref);
	view->reply = reply != NULL ? g_variant_ref_sink(reply) : NULL;
}

static void
test_view_frame_replies(void)
{
	g_autoptr(GsurfView) view = g_object_new(fake_view_get_type(), NULL);
	FakeView *fake = (FakeView *)view;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	const gchar *spec;
	const gchar *script;
	const gchar *require;
	gdouble x = -1;
	gdouble y = -1;
	guint64 id = 0;

	/* 1. A value, with where the frame is and which frame it was; the
	 *    request carries the frame, the script and the selector. */
	set_reply(fake, g_variant_new(GSURF_FRAMES_EVAL_REPLY, (guint)GSURF_FRAMES_STATUS_OK,
		(guint64)42, "inside", 94.5, 160.0));
	text = run_in(view, "js.stripe.com", "#card", "document.title", &x, &y, &id, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(text, ==, "inside");
	g_assert_cmpfloat(x, ==, 94.5);
	g_assert_cmpfloat(y, ==, 160.0);
	g_assert_cmpuint(id, ==, 42);
	g_assert_cmpstr(fake->last_name, ==, GSURF_FRAMES_MSG_EVAL);
	g_variant_get(fake->last_params, "(&s&s&s)", &spec, &script, &require);
	g_assert_cmpstr(spec, ==, "js.stripe.com");
	g_assert_cmpstr(script, ==, "document.title");
	g_assert_cmpstr(require, ==, "#card");
	g_clear_pointer(&text, g_free);

	/* 2. No selector is an empty one. */
	text = run_in(view, "7", NULL, "1", NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_variant_get(fake->last_params, "(&s&s&s)", &spec, &script, &require);
	g_assert_cmpstr(require, ==, "");
	g_clear_pointer(&text, g_free);

	/* 3. No such frame: NOT_FOUND, in the extension's words. */
	set_reply(fake, g_variant_new(GSURF_FRAMES_EVAL_REPLY,
		(guint)GSURF_FRAMES_STATUS_NO_FRAME, (guint64)0, "no frame's URI contains 'x'", 0.0,
		0.0));
	text = run_in(view, "x", NULL, "1", NULL, NULL, NULL, &error);
	g_assert_null(text);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	g_assert_cmpstr(error->message, ==, "no frame's URI contains 'x'");
	g_clear_error(&error);

	/* 4. The script threw: FAILED, with the exception. */
	set_reply(fake, g_variant_new(GSURF_FRAMES_EVAL_REPLY, (guint)GSURF_FRAMES_STATUS_SCRIPT,
		(guint64)42, "TypeError: nope", 0.0, 0.0));
	text = run_in(view, "42", NULL, "nope()", NULL, NULL, NULL, &error);
	g_assert_null(text);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
	g_assert_cmpstr(error->message, ==, "TypeError: nope");
	g_clear_error(&error);

	/* 5. An answer of another shape is not trusted. */
	set_reply(fake, g_variant_new("(s)", "?"));
	text = run_in(view, "42", NULL, "1", NULL, NULL, NULL, &error);
	g_assert_null(text);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error(&error);

	/* 6. No extension: say which module to enable. */
	set_reply(fake, NULL);
	text = run_in(view, "42", NULL, "1", NULL, NULL, NULL, &error);
	g_assert_null(text);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
	g_assert_nonnull(strstr(error->message, "frames module"));
	g_clear_error(&error);

	/* 7. The main frame needs no extension: the view's own scripts. */
	text = run_in(view, NULL, "#ignored", "document.title", &x, &y, &id, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(text, ==, "main:document.title");
	g_assert_cmpfloat(x, ==, 0);
	g_assert_cmpfloat(y, ==, 0);
	g_assert_cmpuint(id, ==, 0);
	g_clear_pointer(&text, g_free);

	/* 8. ... where undefined is null, as the extension answers it */
	text = run_in(view, NULL, NULL, "undefined", NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(text, ==, "null");
}

static void
test_view_frame_list(void)
{
	g_autoptr(GsurfView) view = g_object_new(fake_view_get_type(), NULL);
	g_autoptr(GsurfView) bare = g_object_new(bare_view_get_type(), NULL);
	g_autoptr(GError) error = NULL;
	gchar *json;
	Wait w = { NULL };

	/* 1. The list, as the extension wrote it. */
	set_reply((FakeView *)view, g_variant_new("(s)", "[{\"id\":1,\"main\":true}]"));
	gsurf_view_list_frames_async(view, NULL, on_done, &w);
	json = gsurf_view_list_frames_finish(view, wait_for(&w), &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json, ==, "[{\"id\":1,\"main\":true}]");
	g_assert_cmpstr(((FakeView *)view)->last_name, ==, GSURF_FRAMES_MSG_LIST);
	g_free(json);
	g_clear_object(&w.result);

	/* 2. A view that has no web processes at all. */
	gsurf_view_list_frames_async(bare, NULL, on_done, &w);
	json = gsurf_view_list_frames_finish(bare, wait_for(&w), &error);
	g_assert_null(json);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
	g_assert_nonnull(strstr(error->message, "frames module"));
	g_clear_error(&error);
	g_clear_object(&w.result);

	/* 3. The bare transport says so in its own words. */
	gsurf_view_send_page_message_async(bare, "anything", g_variant_new("(s)", "x"), NULL,
		on_done, &w);
	g_assert_null(gsurf_view_send_page_message_finish(bare, wait_for(&w), &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
	g_clear_object(&w.result);
}

/*
 * Without native input (this fake has none) a click in a frame is the
 * DOM events dispatched inside the frame, at the frame's own point, and
 * typing goes to the field there.
 */
static void
test_view_frame_input(void)
{
	g_autoptr(GsurfView) view = g_object_new(fake_view_get_type(), NULL);
	FakeView *fake = (FakeView *)view;
	g_autoptr(GError) error = NULL;
	const gchar *spec;
	const gchar *script;
	const gchar *require;
	Wait w = { NULL };

	set_reply(fake, g_variant_new(GSURF_FRAMES_EVAL_REPLY, (guint)GSURF_FRAMES_STATUS_OK,
		(guint64)42, "true", 100.0, 200.0));

	/* 1. The view's point 130, 260 is the frame's 30, 60. */
	g_assert_true(gsurf_view_send_click_in_frame(view, "42", 130, 260, 100, 200, 1, 0, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(fake->last_name, ==, GSURF_FRAMES_MSG_EVAL);
	g_variant_get(fake->last_params, "(&s&s&s)", &spec, &script, &require);
	g_assert_cmpstr(spec, ==, "42");
	g_assert_nonnull(strstr(script, "elementFromPoint"));
	g_assert_nonnull(strstr(script, "(30,60,0,"));
	g_assert_cmpuint(fake->scripts->len, ==, 0);

	/* 2. A point outside the frame is refused. */
	g_assert_false(gsurf_view_send_click_in_frame(view, "42", 10, 260, 100, 200, 1, 0, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* 3. No frame: the main frame, as gsurf_view_send_click(). */
	g_assert_true(gsurf_view_send_click_in_frame(view, NULL, 5, 6, 0, 0, 1, 0, &error));
	g_assert_cmpuint(fake->scripts->len, ==, 1);
	g_assert_nonnull(strstr(g_ptr_array_index(fake->scripts, 0), "(5,6,0,"));

	/* 4. Typing: into the field by its selector, inside the frame; the
	 *    selector and the text are data, quoted. */
	gsurf_view_type_in_frame_async(view, "42", "#card\"", 130, 260, 100, 200, "4242'\"",
		NULL, on_done, &w);
	g_assert_true(gsurf_view_type_in_frame_finish(view, wait_for(&w), &error));
	g_assert_no_error(error);
	g_variant_get(fake->last_params, "(&s&s&s)", &spec, &script, &require);
	g_assert_cmpstr(spec, ==, "42");
	g_assert_nonnull(strstr(script, "(\"#card\\\"\",\"4242'\\\"\")"));
	g_assert_nonnull(strstr(script, "insertText"));
	g_clear_object(&w.result);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/gsurf/frames/module/registers", test_module_registers);
	g_test_add_func("/gsurf/frames/module/disabled", test_module_disabled);
	g_test_add_func("/gsurf/frames/module/path", test_module_path);
	g_test_add_func("/gsurf/frames/view/replies", test_view_frame_replies);
	g_test_add_func("/gsurf/frames/view/list", test_view_frame_list);
	g_test_add_func("/gsurf/frames/view/input", test_view_frame_input);

	return g_test_run();
}
