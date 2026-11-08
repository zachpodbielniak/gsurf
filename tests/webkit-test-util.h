/*
 * webkit-test-util.h - Shared helpers for the WebKit GUI test suites
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Header-only (static inline) so any tests/test-*-webkit.c can include it
 * without Makefile changes. Every wait is bounded: a hung web process
 * fails the test instead of hanging `make test-gui`.
 */

#ifndef GSURF_WEBKIT_TEST_UTIL_H
#define GSURF_WEBKIT_TEST_UTIL_H

#include <gsurf.h>
#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <string.h>
#ifdef GSURF_BACKEND_GTK4
#include <webkit/webkit.h>
#else
#include <webkit2/webkit2.h>
#endif

#ifdef GSURF_BACKEND_GTK4
/* The one toolkit diagnostic gsurf_webkit_test_init() tolerates. */
#define GSURF_WEBKIT_TEST_FRAME_TIMINGS "Duplicate call to gdk_frame_timings_presented()"

/*
 * gsurf_webkit_test_field_is:
 * Whether log field @field is @key and its value starts with @prefix
 * (values are NUL-terminated when length < 0, else @length bytes).
 */
static inline gboolean
gsurf_webkit_test_field_is(const GLogField *field, const gchar *key, const gchar *prefix)
{
	gsize n = strlen(prefix);

	if (!g_str_equal(field->key, key))
		return FALSE;
	if (field->length >= 0 && (gsize)field->length < n)
		return FALSE;
	return strncmp((const gchar *)field->value, prefix, n) == 0;
}

/*
 * gsurf_webkit_test_log_writer:
 * GTK 4 logs a Gdk-WARNING "Duplicate call to gdk_frame_timings_presented()
 * with different values" when a compositor's presentation feedback for a
 * frame disagrees with an earlier one -- seen under headless mutter's
 * virtual monitor (the Wayland stand-in for xvfb-run). It is the
 * toolkit's frame-clock bookkeeping, outside anything gsurf does, yet
 * g_test_init() makes it fatal and it aborts whichever test happens to
 * spin the main loop then. Drop exactly that message (Gdk domain plus
 * message prefix); every other log keeps the default, fatal handling.
 */
static inline GLogWriterOutput
gsurf_webkit_test_log_writer(GLogLevelFlags log_level, const GLogField *fields,
                             gsize n_fields, gpointer user_data)
{
	gboolean gdk = FALSE;
	gboolean timings = FALSE;
	gsize i;

	if ((log_level & G_LOG_LEVEL_WARNING) != 0) {
		for (i = 0; i < n_fields; i++) {
			/* The domain exactly "Gdk" (not "GdkPixbuf"). */
			if (gsurf_webkit_test_field_is(&fields[i], "GLIB_DOMAIN", "Gdk") &&
			    (fields[i].length < 0
			     ? ((const gchar *)fields[i].value)[3] == '\0'
			     : fields[i].length == 3))
				gdk = TRUE;
			else if (gsurf_webkit_test_field_is(&fields[i], "MESSAGE",
					GSURF_WEBKIT_TEST_FRAME_TIMINGS))
				timings = TRUE;
		}
		if (gdk && timings) {
			g_test_message("ignored a GTK frame-timing warning (compositor feedback)");
			return G_LOG_WRITER_HANDLED;
		}
	}
	return g_log_writer_default(log_level, fields, n_fields, user_data);
}
#endif

/*
 * gsurf_webkit_test_init:
 *
 * Returns %FALSE (after marking the test skipped) unless GSURF_TEST_GUI is
 * set; otherwise initializes GTK for the compiled backend. Under GTK 4 it
 * first installs gsurf_webkit_test_log_writer() (once per process: GLib
 * allows a single writer).
 */
static inline gboolean
gsurf_webkit_test_init(void)
{
	if (g_getenv("GSURF_TEST_GUI") == NULL) {
		g_test_skip("set GSURF_TEST_GUI=1 (make test-gui) to run WebKit tests");
		return FALSE;
	}
#ifdef GSURF_BACKEND_GTK3
	g_assert_true(gtk_init_check(NULL, NULL));
#else
	{
		static gsize writer_set = 0;

		if (g_once_init_enter(&writer_set)) {
			g_log_set_writer_func(gsurf_webkit_test_log_writer, NULL, NULL);
			g_once_init_leave(&writer_set, 1);
		}
	}
	g_assert_true(gtk_init_check());
#endif
	return TRUE;
}

static inline gboolean
gsurf_webkit_test_deadline(gpointer data)
{
	g_error("WebKit test timed out: %s", (const gchar *)data);
	return G_SOURCE_REMOVE;
}

static inline gboolean
gsurf_webkit_test_quit(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

/* Run the main loop for @ms milliseconds. */
static inline void
gsurf_webkit_test_spin(guint ms)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

	g_timeout_add(ms, gsurf_webkit_test_quit, loop);
	g_main_loop_run(loop);
}

static inline void
gsurf_webkit_test_on_load(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
	(void)view;
	if (event == WEBKIT_LOAD_FINISHED)
		g_main_loop_quit(data);
}

/* Wait (bounded) for the next load of @view to finish. */
static inline void
gsurf_webkit_test_wait_finished(WebKitWebView *view)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	gulong handler = g_signal_connect(view, "load-changed",
		G_CALLBACK(gsurf_webkit_test_on_load), loop);
	guint timeout = g_timeout_add_seconds(20, gsurf_webkit_test_deadline, "page load");

	g_main_loop_run(loop);
	g_source_remove(timeout);
	g_signal_handler_disconnect(view, handler);
}

/* Load @uri into @view and wait for it to finish. */
static inline void
gsurf_webkit_test_load(WebKitWebView *view, const gchar *uri)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	gulong handler = g_signal_connect(view, "load-changed",
		G_CALLBACK(gsurf_webkit_test_on_load), loop);
	guint timeout = g_timeout_add_seconds(20, gsurf_webkit_test_deadline, (gpointer)uri);

	webkit_web_view_load_uri(view, uri);
	g_main_loop_run(loop);
	g_source_remove(timeout);
	g_signal_handler_disconnect(view, handler);
}

typedef struct {
	GMainLoop *loop;
	gchar     *text;
	GError    *error;
} GsurfWebkitTestResult;

static inline void
gsurf_webkit_test_eval_done(GObject *source, GAsyncResult *result, gpointer data)
{
	GsurfWebkitTestResult *out = data;
	JSCValue *value = webkit_web_view_evaluate_javascript_finish(
		WEBKIT_WEB_VIEW(source), result, &out->error);

	if (value != NULL) {
		out->text = jsc_value_is_string(value) ? jsc_value_to_string(value)
		                                       : jsc_value_to_json(value, 0);
		g_object_unref(value);
	}
	g_main_loop_quit(out->loop);
}

/* Evaluate @expression in @world (NULL = main world) and return its value
 * as a string (strings as-is, anything else as JSON). Fails the test on a
 * JavaScript error. */
static inline gchar *
gsurf_webkit_test_eval_in(WebKitWebView *view, const gchar *world, const gchar *expression)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	GsurfWebkitTestResult result = { loop, NULL, NULL };
	guint timeout = g_timeout_add_seconds(10, gsurf_webkit_test_deadline, "JavaScript");

	webkit_web_view_evaluate_javascript(view, expression, -1, world, NULL, NULL,
		gsurf_webkit_test_eval_done, &result);
	g_main_loop_run(loop);
	g_source_remove(timeout);
	g_assert_no_error(result.error);
	return result.text;
}

static inline gchar *
gsurf_webkit_test_eval(WebKitWebView *view, const gchar *expression)
{
	return gsurf_webkit_test_eval_in(view, NULL, expression);
}

/* The native WebKitWebView of a GsurfView. */
static inline WebKitWebView *
gsurf_webkit_test_native(GsurfView *view)
{
	return WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
}

/* Start @server on an ephemeral loopback port; returns the port. */
static inline guint16
gsurf_webkit_test_listen(SoupServer *server)
{
	g_autoptr(GError) error = NULL;
	GSList *uris;
	guint16 port;

	g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(server);
	port = (guint16)g_uri_get_port(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	return port;
}

/* Path of a built module next to the test binary (build/<mode>/modules). */
static inline gchar *
gsurf_webkit_test_module_path(const gchar *name)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);
	g_autofree gchar *base = g_strconcat(name, ".so", NULL);

	return g_build_filename(dir, "modules", base, NULL);
}

#endif /* GSURF_WEBKIT_TEST_UTIL_H */
