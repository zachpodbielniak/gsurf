/*
 * test-window-layout-webkit.c - Real backend windows: view routing, layouts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Runs on a real GsurfGtk3Window / GsurfGtk4Window with real WebKit
 * views and checks what the headless suites (test-window-layout,
 * test-window-lifecycle) cannot:
 *
 *   - mouse buttons pressed on a view widget reach
 *     GsurfWindow::button-press on both toolkits, once per press.
 *
 * Runs only with GSURF_TEST_GUI=1 (make test-gui).
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>

#include "test-util.h"
#include "webkit-test-util.h"

/* --- helpers --- */

typedef struct {
	guint count;
	guint button;
	gboolean consume;
} ButtonLog;

static gboolean
on_button_press(GsurfWindow *window, guint button, guint state, gpointer user_data)
{
	ButtonLog *log = user_data;

	(void)window;
	(void)state;
	log->count++;
	log->button = button;
	return log->consume;
}

#ifdef GSURF_BACKEND_GTK3
static gboolean
stop_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	(void)widget;
	(void)event;
	(void)user_data;
	return TRUE;
}
#endif

/* Press mouse @button on @widget the way the backend sees it: GTK3 runs
 * the widget's button-press-event, GTK4 the window's click gesture on
 * the widget (GTK4 cannot synthesize a pointer event, so the gesture's
 * "pressed" is emitted directly and reports button 0). */
static void
press_button(GtkWidget *widget, guint button)
{
#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(widget);
	GtkEventController *found = NULL;
	guint i;

	(void)button;
	for (i = 0; i < g_list_model_get_n_items(controllers); i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (g_strcmp0(gtk_event_controller_get_name(c), "gsurf-view-click") != 0)
			continue;
		/* One gesture per window: a second would emit every press twice. */
		g_assert_null(found);
		found = c; /* still owned by the widget */
	}
	g_assert_nonnull(found);
	g_assert_true(GTK_IS_GESTURE_CLICK(found));
	g_assert_cmpint(gtk_event_controller_get_propagation_phase(found), ==,
		GTK_PHASE_CAPTURE);
	g_assert_cmpuint(gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(found)), ==, 0);
	g_signal_emit_by_name(found, "pressed", 1, 10.0, 10.0);
#else
	GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS);
	gboolean handled = FALSE;
	gulong id;

	event->button.button = button;
	/* The window's handler runs first (connected earlier); this one then
	 * keeps a device-less synthetic event away from WebKit's own. */
	id = g_signal_connect(widget, "button-press-event", G_CALLBACK(stop_event), NULL);
	g_signal_emit_by_name(widget, "button-press-event", event, &handled);
	g_signal_handler_disconnect(widget, id);
	gdk_event_free(event);
#endif
}

/* --- tests --- */

/* A press on a view widget emits GsurfWindow::button-press exactly once
 * (GTK4 used to route none at all), also after the view widget left the
 * stack and came back (wiring is once per widget and window). */
static void
test_button_routing(void)
{
	g_autoptr(GsurfWindow) window = NULL;
	g_autoptr(GsurfView) view = NULL;
	ButtonLog log = { 0, 0, FALSE };
	GtkWidget *widget;

	if (!gsurf_webkit_test_init())
		return;
	window = gsurf_window_new();
	view = gsurf_view_new();
	gsurf_window_add_view(window, view);
	g_signal_connect(window, "button-press", G_CALLBACK(on_button_press), &log);
	widget = gsurf_view_get_native_widget(view);
	g_assert_nonnull(widget);

	press_button(widget, 8);
	g_assert_cmpuint(log.count, ==, 1);
#ifdef GSURF_BACKEND_GTK3
	g_assert_cmpuint(log.button, ==, 8);
#endif

	/* Out of the window and back: still one emission per press. */
	g_object_ref(view);
	gsurf_window_remove_view(window, view);
	gsurf_window_add_view(window, view);
	g_object_unref(view);
	log.consume = TRUE;
	press_button(widget, 9);
	g_assert_cmpuint(log.count, ==, 2);
#ifdef GSURF_BACKEND_GTK3
	g_assert_cmpuint(log.button, ==, 9);
#endif
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *tmp = NULL;
	int ret;

	g_test_init(&argc, &argv, NULL);

	/* Private XDG dirs: nothing touches the user's real profile. */
	tmp = g_dir_make_tmp("gsurf-window-layout-XXXXXX", NULL);
	g_assert_nonnull(tmp);
	g_setenv("XDG_DATA_HOME", tmp, TRUE);
	g_setenv("XDG_CACHE_HOME", tmp, TRUE);
	g_setenv("XDG_CONFIG_HOME", tmp, TRUE);

	g_test_add_func("/gsurf/window-webkit/button-routing", test_button_routing);

	ret = g_test_run();
	gsurf_test_rmtree(tmp);
	return ret;
}
