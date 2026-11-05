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
 *
 * Without a window layout it also checks the sidebar's own drag and drop
 * (a row dropped on another row: before it, after it, under it; never
 * into its own subtree; a plain click still activates) through the rows'
 * real drag source and drop target handlers, and that a tab opened from
 * the page's context menu ("Open Link in New Tab") is a background child
 * of its page in the tree.
 *
 * Runs only with GSURF_TEST_GUI=1 (make test-gui).
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>

#include "test-util.h"
#include "webkit-test-util.h"
#include "modules/include/gsurf-chrome-compat.h"

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

/* ------------------------------------------------------------------ */
/* Sidebar drag and drop, context-menu children (no window layout)    */
/* ------------------------------------------------------------------ */

typedef struct {
	gchar              *tmp;
	GsurfConfig        *config;
	GsurfApplication   *app;
	GsurfModuleManager *mgr;
	GsurfWindow        *window;
	GtkWidget          *toplevel;
} T;

/* A window with tab "a", the tabs module (sidebar shown) active. */
static gboolean
t_init(T *t)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *so = gsurf_webkit_test_module_path("tabs");
	GsurfModule *module;

	memset(t, 0, sizeof(*t));
	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("tabs.so not built");
		return FALSE;
	}
	t->tmp = g_dir_make_tmp("gsurf-tabs-webkit-XXXXXX", &error);
	g_assert_no_error(error);
	g_setenv("XDG_DATA_HOME", t->tmp, TRUE);
	g_setenv("XDG_CACHE_HOME", t->tmp, TRUE);
	g_setenv("XDG_CONFIG_HOME", t->tmp, TRUE);

	t->config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(t->config,
		"browser:\n  homepage: about:blank\n"
		"modules:\n  tabs:\n    enabled: true\n    sidebar: true\n", -1, &error));
	gsurf_config_set_default(t->config);
	t->app = gsurf_application_new(t->config);
	t->mgr = gsurf_module_manager_get_default();
	gsurf_module_manager_set_application(t->mgr, t->app);
	gsurf_module_manager_set_config(t->mgr, t->config);
	module = gsurf_module_manager_get_module(t->mgr, "tabs");
	if (module == NULL)
		module = gsurf_module_manager_load_module(t->mgr, so, &error);
	else
		gsurf_module_configure(module, t->config);
	g_assert_no_error(error);
	gsurf_module_set_enabled(module, TRUE);

	t->window = gsurf_window_new();
	t->toplevel = gsurf_window_get_native_widget(t->window);
	gsurf_application_add_window(t->app, t->window);
	return TRUE;
}

static void
t_activate(T *t)
{
	gsurf_module_manager_activate_all(t->mgr);
	gsurf_window_present(t->window);
}

static void
t_free(T *t)
{
	gsurf_module_manager_deactivate_all(t->mgr);
	gsurf_module_manager_set_application(t->mgr, NULL);
	gsurf_module_manager_set_config(t->mgr, NULL);
	gsurf_config_set_default(NULL);
	gtk_widget_set_visible(t->toplevel, FALSE);
	g_clear_object(&t->window);
	g_clear_object(&t->app);
	g_clear_object(&t->config);
	gsurf_webkit_test_spin(50);
	gsurf_test_rmtree(t->tmp);
	g_free(t->tmp);
}

/* A tab titled @name opened from @opener (or none), added in the
 * background. */
static GsurfView *
t_tab(T *t, GsurfView *opener, const gchar *name)
{
	g_autofree gchar *uri = g_strdup_printf("data:text/html,<title>%s</title>%s", name, name);
	GsurfView *view = gsurf_view_new_full(opener, NULL, uri, FALSE);

	g_object_set_data_full(G_OBJECT(view), "name", g_strdup(name), g_free);
	gsurf_window_add_view(t->window, view);
	g_object_unref(view);
	gsurf_view_load_uri(view, uri);
	return view;
}

/* The window order as "a b c". */
static gchar *
t_order(T *t)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < gsurf_window_get_n_views(t->window); i++)
		g_string_append_printf(out, "%s%s", i > 0 ? " " : "",
			(const gchar *)g_object_get_data(G_OBJECT(gsurf_window_get_nth_view(t->window, i)),
				"name"));
	return g_string_free(out, FALSE);
}

#define ASSERT_ORDER(t, expected) G_STMT_START { \
	g_autofree gchar *got_ = t_order(t); \
	g_assert_cmpstr(got_, ==, (expected)); \
} G_STMT_END

/* The sidebar row of @view (a tab row: class gsurf-tab-row, its id). */
static GtkWidget *
row_for(GtkWidget *root, guint64 id)
{
	guint64 *wid = g_object_get_data(G_OBJECT(root), "gsurf-tab-view-id");
	GtkWidget *found = NULL;

#ifdef GSURF_BACKEND_GTK4
	GtkWidget *child;

	if (wid != NULL && *wid == id && gtk_widget_has_css_class(root, "gsurf-tab-row"))
		return root;
	for (child = gtk_widget_get_first_child(root); child != NULL && found == NULL;
	     child = gtk_widget_get_next_sibling(child))
		found = row_for(child, id);
#else
	if (wid != NULL && *wid == id &&
	    gtk_style_context_has_class(gtk_widget_get_style_context(root), "gsurf-tab-row"))
		return root;
	if (GTK_IS_CONTAINER(root)) {
		g_autoptr(GList) children = gtk_container_get_children(GTK_CONTAINER(root));
		GList *l;

		for (l = children; l != NULL && found == NULL; l = l->next)
			found = row_for(l->data, id);
	}
#endif
	return found;
}

#ifdef GSURF_BACKEND_GTK4
static GtkEventController *
controller_named(GtkWidget *widget, const gchar *name)
{
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(widget);
	GtkEventController *found = NULL;
	guint i;

	for (i = 0; i < g_list_model_get_n_items(controllers) && found == NULL; i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (g_strcmp0(gtk_event_controller_get_name(c), name) == 0)
			found = c;
	}
	g_assert_nonnull(found);
	return found;
}
#endif

/* Drag @view's row onto @target's row at height fraction @frac (0.1:
 * upper quarter, 0.5 middle, 0.9 lower quarter) through the real
 * handlers. Returns whether the hover was accepted. */
static gboolean
t_drag_row(T *t, GsurfView *view, GsurfView *target, gdouble frac)
{
	GtkWidget *src = row_for(t->toplevel, gsurf_view_get_id(view));
	GtkWidget *dst = row_for(t->toplevel, gsurf_view_get_id(target));
	gboolean accepted;
	gint height;
	gint y;
	guint i;

	g_assert_nonnull(src);
	g_assert_nonnull(dst);
	for (i = 0; i < 200 && gsurf_chrome_widget_get_height(dst) == 0; i++)
		gsurf_webkit_test_spin(25);
	height = gsurf_chrome_widget_get_height(dst);
	g_assert_cmpint(height, >, 0);
	y = (gint)(height * frac);
#ifdef GSURF_BACKEND_GTK4
	{
		GtkEventController *source = controller_named(src, "gsurf-chrome-drag-source");
		GtkEventController *drop = controller_named(dst, "gsurf-chrome-drop-target");
		g_autoptr(GdkContentProvider) content = NULL;
		GdkDragAction action = 0;
		gboolean taken = FALSE;

		/* The drag carries the view id in the private target. */
		g_signal_emit_by_name(source, "prepare", 1.0, 1.0, &content);
		g_assert_nonnull(content);
		{
			g_autoptr(GdkContentFormats) formats = gdk_content_provider_ref_formats(content);

			g_assert_true(gdk_content_formats_contain_mime_type(formats,
				GSURF_LAYOUT_VIEW_MIME_TYPE));
		}
		g_signal_emit_by_name(source, "drag-begin", NULL);
		g_signal_emit_by_name(drop, "drag-motion", NULL, 4.0, (gdouble)y, &action);
		accepted = (action != 0);
		if (accepted)
			g_signal_emit_by_name(drop, "drop", NULL, 4.0, (gdouble)y, &taken);
		g_signal_emit_by_name(source, "drag-end", NULL, FALSE);
	}
#else
	{
		gboolean ok = FALSE;

		g_signal_emit_by_name(src, "drag-begin", NULL);
		g_signal_emit_by_name(dst, "drag-motion", NULL, 4, y, 0u, &accepted);
		if (accepted)
			g_signal_emit_by_name(dst, "drag-drop", NULL, 4, y, 0u, &ok);
		g_signal_emit_by_name(src, "drag-end", NULL);
	}
#endif
	return accepted;
}

/* Rows re-parent by drag and drop: upper quarter before, lower quarter
 * after, middle under; never into the dragged tab's own subtree. */
static void
test_row_drag_drop(void)
{
	T t;
	GsurfView *a, *b, *c, *d;

	if (!gsurf_webkit_test_init() || !t_init(&t))
		return;
	a = t_tab(&t, NULL, "a");
	b = t_tab(&t, a, "b");
	c = t_tab(&t, NULL, "c");
	d = t_tab(&t, c, "d");
	t_activate(&t);
	gsurf_window_set_active_view(t.window, a);
	ASSERT_ORDER(&t, "a b c d");

	/* 1. c (with d) under b: a [ b [ c [ d ] ] ]. */
	g_assert_true(t_drag_row(&t, c, b, 0.5));
	ASSERT_ORDER(&t, "a b c d");
	g_assert_cmpuint(gsurf_view_get_opener_id(c), ==, 0);
	/* 2. a into its own subtree (onto d) is refused. */
	g_assert_false(t_drag_row(&t, a, d, 0.5));
	ASSERT_ORDER(&t, "a b c d");
	/* 3. d before a: a root first. */
	g_assert_true(t_drag_row(&t, d, a, 0.1));
	ASSERT_ORDER(&t, "d a b c");
	/* 4. d after a: after a's whole subtree. */
	g_assert_true(t_drag_row(&t, d, a, 0.9));
	ASSERT_ORDER(&t, "a b c d");
	/* 5. b under d: b and c move with it. */
	g_assert_true(t_drag_row(&t, b, d, 0.5));
	ASSERT_ORDER(&t, "a d b c");
	/* No drop indicator stays behind. */
	{
		GtkWidget *row = row_for(t.toplevel, gsurf_view_get_id(d));

#ifdef GSURF_BACKEND_GTK4
		g_assert_false(gtk_widget_has_css_class(row, "gsurf-tab-drop-child"));
#else
		g_assert_false(gtk_style_context_has_class(gtk_widget_get_style_context(row),
			"gsurf-tab-drop-child"));
#endif
	}
	g_assert_true(gsurf_window_get_active_view(t.window) == a);
	t_free(&t);
}

static gboolean
on_new_view_requested(GsurfView *source, const gchar *uri, gboolean background,
                      gpointer user_data)
{
	T *t = user_data;
	GsurfView *view = gsurf_view_new_full(source, NULL, uri, FALSE);

	g_object_set_data_full(G_OBJECT(view), "name", g_strdup("new"), g_free);
	gsurf_window_add_view(t->window, view);
	if (!background)
		gsurf_window_set_active_view(t->window, view);
	gsurf_view_load_uri(view, uri);
	g_object_unref(view);
	return TRUE;
}

/* "Open Link in New Tab" from a page's context menu: a background tab,
 * placed as the page's last child (after its existing children). */
static void
test_context_menu_child(void)
{
	T t;
	GsurfView *a, *b, *c;
	WebKitContextMenu *menu;
	g_autoptr(WebKitHitTestResult) hit = NULL;
	gboolean handled = FALSE;
	GList *l;
	GAction *action = NULL;

	if (!gsurf_webkit_test_init() || !t_init(&t))
		return;
	a = t_tab(&t, NULL, "a");
	b = t_tab(&t, a, "b");
	c = t_tab(&t, NULL, "c");
	t_activate(&t);
	gsurf_window_set_active_view(t.window, a);
	g_signal_connect(a, "new-view-requested", G_CALLBACK(on_new_view_requested), &t);
	gsurf_webkit_test_wait_finished(gsurf_webkit_test_native(a));

	menu = webkit_context_menu_new();
	webkit_context_menu_append(menu, webkit_context_menu_item_new_from_stock_action(
		WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WINDOW));
	hit = g_object_new(WEBKIT_TYPE_HIT_TEST_RESULT,
		"context", WEBKIT_HIT_TEST_RESULT_CONTEXT_LINK, "link-uri", "about:blank", NULL);
#ifdef GSURF_BACKEND_GTK4
	g_signal_emit_by_name(gsurf_webkit_test_native(a), "context-menu", menu, hit, &handled);
#else
	g_signal_emit_by_name(gsurf_webkit_test_native(a), "context-menu", menu, NULL, hit,
		&handled);
#endif
	for (l = webkit_context_menu_get_items(menu); l != NULL; l = l->next) {
		GAction *candidate = webkit_context_menu_item_get_gaction(l->data);

		if (candidate != NULL &&
		    g_strcmp0(g_action_get_name(candidate), "gsurf-open-link-new-tab") == 0)
			action = candidate;
	}
	g_assert_nonnull(action);
	g_action_activate(action, NULL);
	g_object_unref(menu);
	ASSERT_ORDER(&t, "a b new c");
	g_assert_true(gsurf_window_get_active_view(t.window) == a);
	(void)b;
	(void)c;
	t_free(&t);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/tabs/webkit/sidebar-width", test_sidebar_width);
	g_test_add_func("/gsurf/tabs/webkit/row-drag-drop", test_row_drag_drop);
	g_test_add_func("/gsurf/tabs/webkit/context-menu-child", test_context_menu_child);
	return g_test_run();
}
