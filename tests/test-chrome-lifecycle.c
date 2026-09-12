/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gsurf.h>

#ifdef GSURF_BACKEND_GTK3
#include <gtk/gtk.h>

/* Model-only views avoid web processes and network access in chrome tests. */
typedef struct { GsurfView parent; const gchar *uri; gdouble progress; } TestChromeView;
typedef struct { GsurfViewClass parent; } TestChromeViewClass;
typedef struct { GsurfWindow parent; GtkWidget *box; } TestChromeWindow;
typedef struct { GsurfWindowClass parent; } TestChromeWindowClass;

G_DEFINE_TYPE(TestChromeView, test_chrome_view, GSURF_TYPE_VIEW)
G_DEFINE_TYPE(TestChromeWindow, test_chrome_window, GSURF_TYPE_WINDOW)

/* Return stable, controllable page state through the public view contract. */
static const gchar *
test_uri(GsurfView *view)
{
	return ((TestChromeView *)view)->uri;
}

static gdouble
test_progress(GsurfView *view)
{
	return ((TestChromeView *)view)->progress;
}

static void
test_chrome_view_class_init(TestChromeViewClass *klass)
{
	GSURF_VIEW_CLASS(klass)->get_uri = test_uri;
	GSURF_VIEW_CLASS(klass)->get_estimated_load_progress = test_progress;
}

static void
test_chrome_view_init(TestChromeView *self)
{
	self->uri = "https://example.org/";
	self->progress = 1.0;
}

/* A real GTK container exercises widget ownership without showing a window. */
static void
add_chrome(GsurfWindow *window, gpointer widget, gboolean top)
{
	(void) top;
	gtk_container_add(GTK_CONTAINER(((TestChromeWindow *)window)->box), GTK_WIDGET(widget));
}

static void
test_chrome_window_dispose(GObject *object)
{
	TestChromeWindow *self = (TestChromeWindow *)object;

	if (self->box != NULL) {
		gtk_widget_destroy(self->box);
		g_clear_object(&self->box);
	}
	G_OBJECT_CLASS(test_chrome_window_parent_class)->dispose(object);
}

static void
test_chrome_window_class_init(TestChromeWindowClass *klass)
{
	GSURF_WINDOW_CLASS(klass)->add_chrome_widget = add_chrome;
	G_OBJECT_CLASS(klass)->dispose = test_chrome_window_dispose;
}

static void
test_chrome_window_init(TestChromeWindow *self)
{
	self->box = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
}

/* Assert the container holds exactly the activation's single chrome widget. */
static GtkWidget *
only_child(GsurfWindow *window)
{
	GList *children;
	GtkWidget *child;

	children = gtk_container_get_children(GTK_CONTAINER(((TestChromeWindow *)window)->box));
	g_assert_cmpuint(g_list_length(children), ==, 1);
	child = GTK_WIDGET(children->data);
	g_list_free(children);
	return child;
}

/* No signal emitter may retain a disposed or deactivated module as data. */
static void
assert_disconnected(gpointer emitter, gpointer module)
{
	g_assert_cmpuint(g_signal_handler_find(emitter, G_SIGNAL_MATCH_DATA,
		0, 0, NULL, NULL, module), ==, 0);
}

static void
test_chrome_lifetime(gconstpointer data)
{
	const gchar *name = data;
	g_autoptr(GsurfApplication) app = NULL;
	g_autoptr(GsurfWindow) window = NULL;
	g_autoptr(GsurfView) first = NULL;
	g_autoptr(GsurfView) second = NULL;
	g_autoptr(GsurfModuleManager) manager = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *exe = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *filename = NULL;
	g_autofree gchar *path = NULL;
	GsurfModule *module;
	GtkWidget *widget;
	GList *children;

	if (g_getenv("GSURF_TEST_GUI") == NULL) {
		g_test_skip("set GSURF_TEST_GUI=1 with a display to test chrome lifetimes");
		return;
	}
	g_assert_true(gtk_init_check(NULL, NULL));
	app = gsurf_application_new(NULL);
	window = g_object_new(test_chrome_window_get_type(), NULL);
	first = g_object_new(test_chrome_view_get_type(), NULL);
	second = g_object_new(test_chrome_view_get_type(), NULL);
	((TestChromeView *)first)->progress = 0.25;
	((TestChromeView *)second)->uri = "https://example.net/";
	gsurf_window_add_view(window, first);
	gsurf_window_add_view(window, second);
	gsurf_application_add_window(app, window);
	gsurf_module_manager_set_application(gsurf_module_manager_get_default(), app);

	/* Load the actual shared module rather than copying its implementation. */
	exe = g_file_read_link("/proc/self/exe", &error);
	g_assert_no_error(error);
	dir = g_path_get_dirname(exe);
	filename = g_strconcat(name, ".so", NULL);
	path = g_build_filename(dir, "modules", filename, NULL);
	manager = gsurf_module_manager_new();
	module = gsurf_module_manager_load_module(manager, path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(module);
	g_assert_true(gsurf_module_activate(module));
	widget = only_child(window);
	if (GTK_IS_LABEL(widget))
		g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(widget)), ==, "https://example.org/  [25%]");

	/* Completed tabs must not inherit the previous tab's loading percentage. */
	gsurf_window_set_active_view(window, second);
	if (GTK_IS_LABEL(widget))
		g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(widget)), ==, "https://example.net/");
	else
		g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(widget)), ==, "https://example.net/");
	gsurf_window_remove_view(window, first);
	gsurf_window_remove_view(window, second);
	if (GTK_IS_LABEL(widget))
		g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(widget)), ==, "");
	else
		g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(widget)), ==, "");
	gsurf_window_add_view(window, first);

	/* Keep native objects alive to catch callbacks surviving deactivation. */
	g_object_ref(widget);
	gsurf_module_deactivate(module);
	assert_disconnected(window, module);
	assert_disconnected(first, module);
	assert_disconnected(widget, module);
	children = gtk_container_get_children(GTK_CONTAINER(((TestChromeWindow *)window)->box));
	g_assert_null(children);
	g_object_unref(widget);
	g_assert_true(gsurf_module_activate(module));
	widget = g_object_ref(only_child(window));

	/* Repeated disposal must release all resources even without deactivate. */
	g_object_run_dispose(G_OBJECT(module));
	g_object_run_dispose(G_OBJECT(module));
	assert_disconnected(window, module);
	assert_disconnected(first, module);
	assert_disconnected(widget, module);
	g_object_unref(widget);
	g_clear_object(&manager);

	/* The reverse lifetime is valid too: the host container dies first. */
	manager = gsurf_module_manager_new();
	module = gsurf_module_manager_load_module(manager, path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(module);
	g_assert_true(gsurf_module_activate(module));
	only_child(window);
	gsurf_module_manager_set_application(gsurf_module_manager_get_default(), NULL);
	g_clear_object(&app);
	g_clear_object(&window);
	gsurf_view_emit_uri_changed(first, "https://example.org/");
	gsurf_view_emit_progress_changed(first, 0.5);
	gsurf_module_deactivate(module);
	assert_disconnected(first, module);
}
#endif

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#ifdef GSURF_BACKEND_GTK3
	g_test_add_data_func("/gsurf/chrome/status-bar", "status_bar", test_chrome_lifetime);
	g_test_add_data_func("/gsurf/chrome/address-bar", "chromebar", test_chrome_lifetime);
#endif
	return g_test_run();
}
