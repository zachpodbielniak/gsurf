/*
 * test-tabs-webkit.c - The tabs module's tree sidebar in a real window
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the real tabs.so into the default module manager, gives it a host
 * application with a real backend window (GsurfGtk3Window or
 * GsurfGtk4Window) and checks what the headless chrome tests cannot: the
 * laid-out size of the sidebar. The sidebar must keep its configured
 * sidebar_width and leave the rest of the window to the page. GTK4
 * propagates a child's hexpand up to its parents, so a tab title that
 * expands inside its row once made the sidebar take half the window.
 * Runs only with GSURF_TEST_GUI=1 (make test-gui).
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>

#include "test-util.h"
#include "webkit-test-util.h"

#define SIDEBAR_WIDTH  (200)
#define WINDOW_WIDTH   (1200)
#define WINDOW_HEIGHT  (800)
#define LONG_TITLE \
	"A very long page title that would push a naive sidebar far wider " \
	"than its configured width if the label were allowed to ask for it"

/* Depth-first search of the widget tree under @root for a widget whose
 * name (gtk_widget_set_name) is @name. */
static GtkWidget *
find_named(GtkWidget *root, const gchar *name)
{
	GtkWidget *found = NULL;

	if (g_strcmp0(gtk_widget_get_name(root), name) == 0)
		return root;
#ifdef GSURF_BACKEND_GTK4
	{
		GtkWidget *child;

		for (child = gtk_widget_get_first_child(root);
		     child != NULL && found == NULL;
		     child = gtk_widget_get_next_sibling(child))
			found = find_named(child, name);
	}
#else
	if (GTK_IS_CONTAINER(root)) {
		g_autoptr(GList) children = gtk_container_get_children(GTK_CONTAINER(root));
		GList *l;

		for (l = children; l != NULL && found == NULL; l = l->next)
			found = find_named(l->data, name);
	}
#endif
	return found;
}

/* The allocated width of @widget on either toolkit. */
static gint
width_of(GtkWidget *widget)
{
#ifdef GSURF_BACKEND_GTK4
	return gtk_widget_get_width(widget);
#else
	return gtk_widget_get_allocated_width(widget);
#endif
}

/* Spin (bounded, about 5 s) until @widget is visible and allocated. */
static void
wait_allocated(GtkWidget *widget)
{
	guint i;

	for (i = 0; i < 100; i++) {
		if (gtk_widget_get_visible(widget) && width_of(widget) > 0)
			return;
		gsurf_webkit_test_spin(50);
	}
	g_error("widget %s was never allocated", gtk_widget_get_name(widget));
}

/* Toggled open, the sidebar is exactly sidebar_width wide however long a
 * tab title is, and the page keeps the rest of the window; toggled shut
 * it disappears. Both toolkits share the expectation. */
static void
test_sidebar_width(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *so = NULL;
	g_autofree gchar *tmp = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *page = NULL;
	GsurfConfig *config;
	GsurfApplication *app;
	GsurfModuleManager *mgr;
	GsurfWindow *window;
	GsurfView *view;
	GtkWidget *toplevel;
	GtkWidget *sidebar;

	if (!gsurf_webkit_test_init())
		return;
	so = gsurf_webkit_test_module_path("tabs");
	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("tabs.so not built");
		return;
	}

	/* 1. Private XDG dirs: nothing touches the user's real profile. */
	tmp = g_dir_make_tmp("gsurf-tabs-webkit-XXXXXX", &error);
	g_assert_no_error(error);
	g_setenv("XDG_DATA_HOME", tmp, TRUE);
	g_setenv("XDG_CACHE_HOME", tmp, TRUE);
	g_setenv("XDG_CONFIG_HOME", tmp, TRUE);

	/* 2. App, manager and the tabs module wired as the gsurf binary
	 *    does; the sidebar starts hidden, as by default. */
	yaml = g_strdup_printf(
		"browser:\n  homepage: about:blank\n"
		"modules:\n  tabs:\n    enabled: true\n"
		"    sidebar_width: %d\n", SIDEBAR_WIDTH);
	config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	gsurf_config_set_default(config);
	app = gsurf_application_new(config);
	mgr = gsurf_module_manager_get_default();
	gsurf_module_manager_set_application(mgr, app);
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);

	window = gsurf_window_new();
	view = gsurf_view_new();
	gsurf_window_add_view(window, view);
	gsurf_application_add_window(app, window);
	gsurf_module_manager_activate_all(mgr);
	toplevel = gsurf_window_get_native_widget(window);
#ifdef GSURF_BACKEND_GTK4
	gtk_window_set_default_size(GTK_WINDOW(toplevel), WINDOW_WIDTH, WINDOW_HEIGHT);
#else
	gtk_window_resize(GTK_WINDOW(toplevel), WINDOW_WIDTH, WINDOW_HEIGHT);
#endif
	gsurf_window_present(window);

	/* 3. A page with a long title, so the row's title label has a wide
	 *    natural size. */
	page = g_strdup_printf("data:text/html,<title>%s</title>hello", LONG_TITLE);
	gsurf_view_load_uri(view, page);
	gsurf_webkit_test_wait_finished(gsurf_webkit_test_native(view));

	sidebar = find_named(toplevel, "gsurf-tab-sidebar");
	g_assert_nonnull(sidebar);
	g_assert_false(gtk_widget_get_visible(sidebar));

	/* 4. Toggle it open: it gets its configured width, no more, and the
	 *    page keeps the rest of the window. */
	g_assert_true(gsurf_module_manager_dispatch_action(mgr, view,
		GSURF_ACTION_TAB_SIDEBAR_TOGGLE));
	wait_allocated(sidebar);
	gsurf_webkit_test_spin(200);
	g_assert_false(gtk_widget_compute_expand(sidebar, GTK_ORIENTATION_HORIZONTAL));
	g_assert_cmpint(width_of(sidebar), ==, SIDEBAR_WIDTH);
	g_assert_cmpint(width_of(toplevel), >=, 2 * SIDEBAR_WIDTH);
	g_assert_cmpint(width_of(gsurf_view_get_native_widget(view)), >=,
		width_of(toplevel) - SIDEBAR_WIDTH - 2);

	/* 5. Toggle it shut again. */
	g_assert_true(gsurf_module_manager_dispatch_action(mgr, view,
		GSURF_ACTION_TAB_SIDEBAR_TOGGLE));
	g_assert_false(gtk_widget_get_visible(sidebar));

	gsurf_module_manager_deactivate_all(mgr);
	gsurf_config_set_default(NULL);
	g_object_unref(config);
	gsurf_test_rmtree(tmp);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/tabs/webkit/sidebar-width", test_sidebar_width);
	return g_test_run();
}
