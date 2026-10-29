/*
 * test-keynav-webkit.c - keyboard focus between the page and the address bar
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Drives a real GTK3 window, WebKit view, chromebar and modal module:
 * the page owns focus once the window is presented, Escape leaves a field
 * the page autofocused, the open-prompt action focuses the address bar,
 * and Escape in the address bar restores the URI and hands focus back to
 * the page. Needs a display and a web process, so it only runs with
 * GSURF_TEST_GUI=1 (make test-gui).
 */

#include <gsurf.h>

#ifdef GSURF_BACKEND_GTK3
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

static const gchar *PAGE =
	"<!doctype html><html><head><title>keynav</title></head>"
	"<body><input id='q' autofocus></body></html>";

static gboolean
deadline(gpointer data)
{
	g_error("keynav WebKit test timed out: %s", (const gchar *)data);
	return G_SOURCE_REMOVE;
}

static gboolean
quit_loop(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

/* Let pending work (web-process messages, focus changes) run. */
static void
spin(guint ms)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

	g_timeout_add(ms, quit_loop, loop);
	g_main_loop_run(loop);
}

static void
loaded(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
	if (event == WEBKIT_LOAD_FINISHED)
		g_main_loop_quit(data);
}

static void
wait_load_html(GsurfView *view, const gchar *html, const gchar *base)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	WebKitWebView *native = WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
	gulong handler = g_signal_connect(native, "load-changed", G_CALLBACK(loaded), loop);
	guint timeout = g_timeout_add_seconds(20, deadline, (gpointer)"page load");

	gsurf_view_load_html(view, html, base);
	g_main_loop_run(loop);
	g_source_remove(timeout);
	g_signal_handler_disconnect(native, handler);
}

/* Poll (bounded) until the page reports an editable element focused. */
static void
wait_editing(GsurfView *view)
{
	guint i;

	for (i = 0; i < 100 && !gsurf_view_get_editing(view); i++)
		spin(50);
	g_assert_true(gsurf_view_get_editing(view));
}

typedef struct {
	GMainLoop *loop;
	gchar     *text;
} ScriptResult;

static void
script_done(GObject *source, GAsyncResult *result, gpointer data)
{
	ScriptResult *script = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JSCValue) value = webkit_web_view_evaluate_javascript_finish(
		WEBKIT_WEB_VIEW(source), result, &error);

	g_assert_no_error(error);
	script->text = jsc_value_to_string(value);
	g_main_loop_quit(script->loop);
}

static gchar *
evaluate(GsurfView *view, const gchar *expression)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	ScriptResult result = { loop, NULL };
	guint timeout = g_timeout_add_seconds(10, deadline, (gpointer)"script");

	webkit_web_view_evaluate_javascript(
		WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view)),
		expression, -1, NULL, NULL, NULL, script_done, &result);
	g_main_loop_run(loop);
	g_source_remove(timeout);
	return result.text;
}

/* Deliver a key press to the toplevel the way GDK would, so it runs the
 * window's key-press-event handler (and from there the focused entry). */
static void
press_key(GtkWidget *toplevel, guint keyval)
{
	GdkEvent *event = gdk_event_new(GDK_KEY_PRESS);
	GdkDisplay *display = gtk_widget_get_display(toplevel);
	GdkSeat *seat = gdk_display_get_default_seat(display);
	GdkKeymapKey *keys = NULL;
	gint n_keys = 0;

	event->key.window = g_object_ref(gtk_widget_get_window(toplevel));
	event->key.send_event = TRUE;
	event->key.time = GDK_CURRENT_TIME;
	event->key.keyval = keyval;
	if (gdk_keymap_get_entries_for_keyval(gdk_keymap_get_for_display(display),
			keyval, &keys, &n_keys) && n_keys > 0)
		event->key.hardware_keycode = (guint16)keys[0].keycode;
	g_free(keys);
	gdk_event_set_device(event, gdk_seat_get_keyboard(seat));

	gtk_widget_event(toplevel, event);
	gdk_event_free(event);
}

/* Stand-in for the binary's keymap layer: modules first (modal). */
static gboolean
on_key_press(GsurfWindow *window, guint keyval, guint keycode, guint state,
             gpointer user_data)
{
	return gsurf_module_manager_dispatch_key_event(gsurf_module_manager_get_default(),
		gsurf_window_get_active_view(window), keyval, keycode, state,
		GSURF_MODE_NORMAL);
}

static gchar *
module_so_path(const gchar *name)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);
	g_autofree gchar *base = g_strconcat(name, ".so", NULL);

	return g_build_filename(dir, "modules", base, NULL);
}

static void
test_focus_round_trip(void)
{
	static const gchar *yaml =
		"modules:\n"
		"  chromebar:\n    enabled: true\n"
		"  modal:\n    enabled: true\n";
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = NULL;
	g_autoptr(GsurfApplication) app = NULL;
	g_autoptr(GsurfWindow) window = NULL;
	g_autoptr(GsurfView) view = NULL;
	g_autofree gchar *chromebar = module_so_path("chromebar");
	g_autofree gchar *modal = module_so_path("modal");
	g_autofree gchar *active = NULL;
	GsurfModuleManager *mgr;
	GtkWidget *toplevel, *webview, *focus;

	if (g_getenv("GSURF_TEST_GUI") == NULL) {
		g_test_skip("set GSURF_TEST_GUI=1 to exercise keyboard focus in WebKit");
		return;
	}
	if (!g_file_test(chromebar, G_FILE_TEST_EXISTS) || !g_file_test(modal, G_FILE_TEST_EXISTS)) {
		g_test_skip("chromebar.so / modal.so not built");
		return;
	}
	g_assert_true(gtk_init_check(NULL, NULL));

	/* App, manager and modules wired as the gsurf binary does. */
	config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	gsurf_config_set_default(config);
	app = gsurf_application_new(config);
	mgr = gsurf_module_manager_get_default();
	gsurf_module_manager_set_application(mgr, app);
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, chromebar, &error));
	g_assert_no_error(error);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, modal, &error));
	g_assert_no_error(error);

	window = gsurf_window_new();
	g_signal_connect(window, "key-press", G_CALLBACK(on_key_press), NULL);
	view = gsurf_view_new();
	gsurf_window_add_view(window, view);
	gsurf_application_add_window(app, window);
	gsurf_module_manager_activate_all(mgr);
	gsurf_window_present(window);

	toplevel = gsurf_window_get_native_widget(window);
	webview = gsurf_view_get_native_widget(view);

	/* 1. The page, not the address bar, has focus from the start. */
	g_assert_true(gtk_window_get_focus(GTK_WINDOW(toplevel)) == webview);

	/* 2. Escape leaves the field the page autofocused. */
	wait_load_html(view, PAGE, "http://keynav.test/");
	wait_editing(view);
	press_key(toplevel, GDK_KEY_Escape);
	g_assert_false(gsurf_view_get_editing(view));
	spin(200);
	active = evaluate(view, "document.activeElement === document.body");
	g_assert_cmpstr(active, ==, "true");

	/* 3. open-prompt (`o`) focuses the address bar, text selected. */
	g_assert_true(gsurf_module_manager_dispatch_action(mgr, view, GSURF_ACTION_OPEN_PROMPT));
	focus = gtk_window_get_focus(GTK_WINDOW(toplevel));
	g_assert_true(GTK_IS_ENTRY(focus));
	g_assert_true(gtk_editable_get_selection_bounds(GTK_EDITABLE(focus), NULL, NULL));

	/* 4. Escape in the bar drops the edit and returns to the page. */
	gtk_entry_set_text(GTK_ENTRY(focus), "half-typed");
	press_key(toplevel, GDK_KEY_Escape);
	g_assert_true(gtk_window_get_focus(GTK_WINDOW(toplevel)) == webview);
	g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(focus)), ==, gsurf_view_get_uri(view));

	gsurf_module_manager_deactivate_all(mgr);
	gsurf_config_set_default(NULL);
}
#endif

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
#ifdef GSURF_BACKEND_GTK3
	g_test_add_func("/gsurf/keynav/focus-round-trip", test_focus_round_trip);
#endif
	return g_test_run();
}
