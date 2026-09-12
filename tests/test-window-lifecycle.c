/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gsurf.h>

/* Minimal backends expose model/native synchronization without a display. */
typedef struct { GsurfView parent; } TestLifetimeView;
typedef struct { GsurfViewClass parent; } TestLifetimeViewClass;
typedef struct { GsurfWindow parent; GsurfView *visible; guint switches; } TestLifetimeWindow;
typedef struct { GsurfWindowClass parent; } TestLifetimeWindowClass;

G_DEFINE_TYPE(TestLifetimeView, test_lifetime_view, GSURF_TYPE_VIEW)
G_DEFINE_TYPE(TestLifetimeWindow, test_lifetime_window, GSURF_TYPE_WINDOW)

static void
test_lifetime_view_class_init(TestLifetimeViewClass *klass)
{
	(void) klass;
}

static void
test_lifetime_view_init(TestLifetimeView *self)
{
	(void) self;
}

/* Record exactly which view the base class asks the backend to display. */
static void
set_visible(GsurfWindow *window, GsurfView *view)
{
	TestLifetimeWindow *self = (TestLifetimeWindow *)window;

	self->visible = view;
	self->switches++;
}

static void
test_lifetime_window_class_init(TestLifetimeWindowClass *klass)
{
	GSURF_WINDOW_CLASS(klass)->set_active_view = set_visible;
}

static void
test_lifetime_window_init(TestLifetimeWindow *self)
{
	self->visible = NULL;
	self->switches = 0;
}

/* Observers must see the same active view as the window's public getter. */
static void
active_changed(GsurfWindow *window, GsurfView *view, gpointer data)
{
	guint *count = data;

	g_assert_true(gsurf_window_get_active_view(window) == view);
	(*count)++;
}

static void
test_remove_active(void)
{
	g_autoptr(GsurfWindow) window = g_object_new(test_lifetime_window_get_type(), NULL);
	g_autoptr(GsurfView) first = g_object_new(test_lifetime_view_get_type(), NULL);
	g_autoptr(GsurfView) second = g_object_new(test_lifetime_view_get_type(), NULL);
	g_autoptr(GsurfView) third = g_object_new(test_lifetime_view_get_type(), NULL);
	TestLifetimeWindow *native = (TestLifetimeWindow *)window;
	guint changes = 0;

	g_signal_connect(window, "active-view-changed", G_CALLBACK(active_changed), &changes);
	gsurf_window_add_view(window, first);
	gsurf_window_add_view(window, second);
	gsurf_window_add_view(window, third);
	gsurf_window_set_active_view(window, second);
	gsurf_window_remove_view(window, second);
	g_assert_true(gsurf_window_get_active_view(window) == third);
	g_assert_true(native->visible == third);
	g_assert_cmpuint(native->switches, ==, 3);
	g_assert_cmpuint(changes, ==, 3);
	gsurf_window_remove_view(window, third);
	g_assert_true(native->visible == first);
	g_assert_cmpuint(changes, ==, 4);
	gsurf_window_remove_view(window, first);
	g_assert_null(gsurf_window_get_active_view(window));
	g_assert_cmpuint(changes, ==, 5);
}

static void
test_application_dispose(void)
{
	g_autoptr(GsurfApplication) app = gsurf_application_new(NULL);
	g_autoptr(GsurfWindow) window = g_object_new(test_lifetime_window_get_type(), NULL);

	/* A host can retain its window after disposing the application. */
	gsurf_application_add_window(app, window);
	g_assert_cmpuint(g_signal_handler_find(window, G_SIGNAL_MATCH_DATA,
		0, 0, NULL, NULL, app), !=, 0);
	g_object_run_dispose(G_OBJECT(app));
	g_assert_cmpuint(g_signal_handler_find(window, G_SIGNAL_MATCH_DATA,
		0, 0, NULL, NULL, app), ==, 0);
	g_clear_object(&app);
	g_assert_false(gsurf_window_emit_close_request(window));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/window/remove-active", test_remove_active);
	g_test_add_func("/gsurf/window/application-dispose", test_application_dispose);
	return g_test_run();
}
