/*
 * test-wmux-webkit.c - The wmux module on real windows and real WebKit views
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the real wmux.so (plus tabs, containers, chromebar and session
 * where a case needs them) into a module manager, gives it an application
 * with a real backend window (GsurfGtk3Window / GsurfGtk4Window) and
 * loopback pages, and drives it the way a user does:
 *
 *   - keys go through the backend's key handling (GTK3: the toplevel's
 *     key-press-event with the focused entry behind it; GTK4: the window's
 *     capture-phase key controller), then GsurfWindow::key-press, then a
 *     copy of the gsurf binary's typing guard (main.c is not linked into
 *     tests): a bare key is dropped while a page field has focus unless the
 *     window's key grab is set. So the prefix-then-% case really exercises
 *     the key grab; everything else dispatches to the modules as gsurf
 *     does;
 *   - the prompt entry gets its keys through the entry's own key handler;
 *   - border drags go through the grid's pointer handlers (GTK3 button
 *     and motion events on the grid's window; GTK4 the grid's drag
 *     gesture signals);
 *   - tab rows are clicked through their click handlers; the tab menu's
 *     "Open in Split" actions are activated from its action group.
 *
 * The module's internals are read through its private header (the struct
 * is plain data) and the layout model compiled in here, to check the panes
 * against the widgets: every view is in exactly one pane or a background
 * tab, the grid's children are exactly the current window's panes, the
 * window's active view is the active pane's.
 *
 * The tab sidebar's side of wmux is covered too: tabs dragged from the
 * sidebar onto panes (drag source and drop target handlers driven
 * directly: centre, edges, swap, join, a pane's own tab, the highlight),
 * the sidebar grouped by wmux window (split, background children, new
 * window, rename, a heading click selecting its window, collapse, kill,
 * wmux off and wmux_groups: false back to the opener tree), and every way
 * of opening a tab from a tab making it a child (the context menu's
 * "Open Link in New Tab", foreground link tabs, popups, the modal F
 * hints, a background child replaced in another container), with the
 * gsurf binary's host handlers copied here like its key guard.
 *
 * Runs only with GSURF_TEST_GUI=1 (make test-gui), on both toolkits.
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>

#include "test-util.h"
#include "webkit-test-util.h"
#include "modules/include/gsurf-chrome-compat.h"

#include "modules/wmux/gsurf-wmux-private.h"
/* The layout model, to inspect the module's panes. */
#include "modules/wmux/gsurf-wmux-layout.c"

#define WINDOW_WIDTH  (1000)
#define WINDOW_HEIGHT (700)

/* ------------------------------------------------------------------ */
/* Loopback pages                                                      */
/* ------------------------------------------------------------------ */

static SoupServer *server;
static guint16     port;

/* /p/<name>: a page titled <name>. /input: a page with a focused field. */
static void
serve(SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *query,
      gpointer user_data)
{
	g_autofree gchar *body = NULL;

	(void)srv;
	(void)query;
	(void)user_data;
	if (g_strcmp0(path, "/input") == 0)
		body = g_strdup("<!doctype html><html><head><title>input</title></head>"
			"<body><input id=\"f\" autofocus></body></html>");
	else if (g_strcmp0(path, "/links") == 0)
		body = g_strdup_printf("<!doctype html><html><head><title>links</title></head>"
			"<body><a href=\"http://127.0.0.1:%u/p/hinted\" style=\"font-size:40px\">go</a>"
			"</body></html>", port);
	else
		body = g_strdup_printf("<!doctype html><html><head><title>%s</title></head>"
			"<body>%s</body></html>", g_str_has_prefix(path, "/p/") ? path + 3 : "page", path);
	soup_server_message_set_status(msg, 200, NULL);
	soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_COPY, body, strlen(body));
}

static gchar *
page(const gchar *name)
{
	return g_strdup_printf("http://127.0.0.1:%u/p/%s", port, name);
}

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
	GsurfConfig        *config;
	GsurfApplication   *app;
	GsurfModuleManager *mgr;
	GsurfWindow        *window;
	GtkWidget          *toplevel;
	GsurfModule        *module;
	GsurfWmuxModule    *w;
	GsurfView          *first;
} H;

#define WAIT_UNTIL(cond) G_STMT_START { \
	guint wait_i_; \
	for (wait_i_ = 0; wait_i_ < 400 && !(cond); wait_i_++) \
		gsurf_webkit_test_spin(25); \
	g_assert_true(cond); \
} G_STMT_END

/* A copy of main.c's on_key_press guard and module dispatch. */
static gboolean
on_key_press(GsurfWindow *window, guint keyval, guint keycode, guint state, gpointer user_data)
{
	H *h = user_data;
	GsurfView *view = gsurf_window_get_active_view(window);
	gboolean has_mod = (state & (GSURF_MOD_CTRL | GSURF_MOD_ALT | GSURF_MOD_SUPER)) != 0;
	gboolean typing = gsurf_module_manager_is_typing(h->mgr, view) &&
	                  !has_mod && keyval != GDK_KEY_Escape;

	if (typing && !gsurf_window_get_key_grab(window))
		return FALSE;
	/* (main.c then never runs a core binding for a typed key.) */
	return gsurf_module_manager_dispatch_key_event(h->mgr, view, keyval, keycode, state,
		GSURF_MODE_NORMAL);
}

static gchar *
so_path(const gchar *name)
{
	return gsurf_webkit_test_module_path(name);
}

/* Load module @name into the manager, or configure the loaded one again
 * from the case's config; either way enabled. */
static void
use_module(H *h, const gchar *name)
{
	g_autofree gchar *path = so_path(name);
	g_autoptr(GError) error = NULL;
	GsurfModule *module = gsurf_module_manager_get_module(h->mgr, name);

	if (module == NULL) {
		module = gsurf_module_manager_load_module(h->mgr, path, &error);
		g_assert_no_error(error);
		g_assert_nonnull(module);
	} else {
		gsurf_module_configure(module, h->config);
	}
	gsurf_module_set_enabled(module, TRUE);
}

/*
 * h_init:
 * @wmux_yaml: extra lines for the wmux section (indented 4)
 * @extra_yaml: (nullable): more top-level module sections
 * @modules: (nullable): other modules to load
 *
 * A config, an application with one real window holding one view on
 * /p/first, the modules loaded and activated (wmux among them), the
 * window presented. FALSE (skipped) when a module is not built.
 */
static gboolean
h_init(H *h, const gchar *wmux_yaml, const gchar *extra_yaml, const gchar *const *modules)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *first = page("first");
	g_autofree gchar *so = so_path("wmux");
	guint i;

	memset(h, 0, sizeof(*h));
	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("wmux.so not built");
		return FALSE;
	}
	for (i = 0; modules != NULL && modules[i] != NULL; i++) {
		g_autofree gchar *other = so_path(modules[i]);

		if (!g_file_test(other, G_FILE_TEST_EXISTS)) {
			g_test_skip("a module is not built");
			return FALSE;
		}
	}

	/* 1. Config: new tabs open /p/new. */
	yaml = g_strdup_printf("browser:\n  homepage: about:blank\n"
		"  new_view_uri: http://127.0.0.1:%u/p/new\n"
		"modules:\n  wmux:\n    enabled: true\n%s%s",
		port, wmux_yaml != NULL ? wmux_yaml : "", extra_yaml != NULL ? extra_yaml : "");
	h->config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(h->config, yaml, -1, &error));
	g_assert_no_error(error);
	gsurf_config_set_default(h->config);

	/* 2. Application and window (one startup view), as main.c builds
	 *    them before activating the modules. */
	h->app = gsurf_application_new(h->config);
	gsurf_module_manager_set_application(gsurf_module_manager_get_default(), h->app);
	h->window = gsurf_window_new();
	h->toplevel = gsurf_window_get_native_widget(h->window);
#ifdef GSURF_BACKEND_GTK4
	gtk_window_set_default_size(GTK_WINDOW(h->toplevel), WINDOW_WIDTH, WINDOW_HEIGHT);
#else
	gtk_window_resize(GTK_WINDOW(h->toplevel), WINDOW_WIDTH, WINDOW_HEIGHT);
#endif
	h->first = gsurf_view_new_full(NULL, NULL, first, FALSE);
	gsurf_window_add_view(h->window, h->first);
	g_object_unref(h->first);
	gsurf_application_add_window(h->app, h->window);
	g_signal_connect(h->window, "key-press", G_CALLBACK(on_key_press), h);

	/* 3. Modules, in the default manager: modules dispatch through it
	 *    (the tabs menu's Open in Split, chromebar's active view). A
	 *    module loaded by an earlier case is configured again; loaded
	 *    modules this case does not want are disabled. */
	h->mgr = g_object_ref(gsurf_module_manager_get_default());
	gsurf_module_manager_set_application(h->mgr, h->app);
	gsurf_module_manager_set_config(h->mgr, h->config);
	{
		GPtrArray *loaded = gsurf_module_manager_get_modules(h->mgr);

		for (i = 0; i < loaded->len; i++)
			gsurf_module_set_enabled(g_ptr_array_index(loaded, i), FALSE);
	}
	use_module(h, "wmux");
	for (i = 0; modules != NULL && modules[i] != NULL; i++)
		use_module(h, modules[i]);
	gsurf_module_manager_activate_all(h->mgr);
	h->module = gsurf_module_manager_get_module(h->mgr, "wmux");
	h->w = (GsurfWmuxModule *)h->module;
	g_assert_true(h->w->installed);
	g_assert_true(gsurf_window_get_layout(h->window) == GSURF_WINDOW_LAYOUT(h->module));

	gsurf_window_present(h->window);
	gsurf_view_load_uri(h->first, first);
	return TRUE;
}

static void
h_free(H *h)
{
	GPtrArray *loaded;
	guint i;

	if (h->mgr != NULL) {
		gsurf_module_manager_deactivate_all(h->mgr);
		if (h->window != NULL)
			g_assert_null(gsurf_window_get_layout(h->window));
		loaded = gsurf_module_manager_get_modules(h->mgr);
		for (i = 0; i < loaded->len; i++)
			gsurf_module_set_enabled(g_ptr_array_index(loaded, i), FALSE);
		gsurf_module_manager_set_application(h->mgr, NULL);
		gsurf_module_manager_set_config(h->mgr, NULL);
		g_clear_object(&h->mgr);
	}
	gsurf_config_set_default(NULL);
	if (h->window != NULL) {
		g_signal_handlers_disconnect_by_data(h->window, h);
		gtk_widget_set_visible(h->toplevel, FALSE);
	}
	g_clear_object(&h->window);
	g_clear_object(&h->app);
	g_clear_object(&h->config);
	gsurf_webkit_test_spin(50);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/* A key press through the backend's key handling (see the header). */
static gboolean
press(H *h, guint keyval, GdkModifierType mods)
{
	gboolean handled = FALSE;
#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(h->toplevel);
	GtkEventController *found = NULL;
	guint i;

	for (i = 0; i < g_list_model_get_n_items(controllers) && found == NULL; i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (GTK_IS_EVENT_CONTROLLER_KEY(c) &&
		    gtk_event_controller_get_propagation_phase(c) == GTK_PHASE_CAPTURE)
			found = c; /* still owned by the toplevel */
	}
	g_assert_nonnull(found);
	g_signal_emit_by_name(found, "key-pressed", keyval, 0u, mods, &handled);
#else
	GdkEvent *event = gdk_event_new(GDK_KEY_PRESS);
	GdkDisplay *display = gtk_widget_get_display(h->toplevel);
	GdkKeymapKey *keys = NULL;
	gint n_keys = 0;

	event->key.window = g_object_ref(gtk_widget_get_window(h->toplevel));
	event->key.send_event = TRUE;
	event->key.time = GDK_CURRENT_TIME;
	event->key.keyval = keyval;
	event->key.state = mods;
	event->key.string = g_strdup("");
	if (gdk_keymap_get_entries_for_keyval(gdk_keymap_get_for_display(display),
			keyval, &keys, &n_keys) && n_keys > 0)
		event->key.hardware_keycode = (guint16)keys[0].keycode;
	g_free(keys);
	gdk_event_set_device(event, gdk_seat_get_keyboard(gdk_display_get_default_seat(display)));
	handled = gtk_widget_event(h->toplevel, event);
	gdk_event_free(event);
#endif
	return handled;
}

#define CTRL  GDK_CONTROL_MASK
#define SHIFT GDK_SHIFT_MASK
#ifdef GSURF_BACKEND_GTK4
#define ALT   GDK_ALT_MASK
#else
#define ALT   GDK_MOD1_MASK
#endif

/* Make sure no prefix is pending (Escape cancels one). */
static void
set_prefix_off(H *h)
{
	if (h->w->prefix_active)
		press(h, GDK_KEY_Escape, 0);
	g_assert_false(gsurf_window_get_key_grab(h->window));
}

/* The prefix (C-b by default), then one key. */
static void
cmd(H *h, guint keyval, GdkModifierType mods)
{
	g_assert_true(press(h, GDK_KEY_b, CTRL));
	g_assert_true(gsurf_window_get_key_grab(h->window));
	g_assert_true(press(h, keyval, mods));
}

/* Whether the window's focus widget is the prompt entry (GTK4: its
 * inner text widget). Not gtk_widget_has_focus(): the headless
 * compositor need not make the toplevel active. */
static gboolean
prompt_focused(H *h)
{
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(h->toplevel));

	return focus != NULL &&
	       (focus == h->w->prompt_entry || gtk_widget_is_ancestor(focus, h->w->prompt_entry));
}

/* A key typed into the status-line prompt entry. */
static void
prompt_key(H *h, guint keyval)
{
#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(h->w->prompt_entry);
	GtkEventController *found = NULL;
	gboolean handled = FALSE;
	guint i;

	for (i = 0; i < g_list_model_get_n_items(controllers) && found == NULL; i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (g_strcmp0(gtk_event_controller_get_name(c), "gsurf-chrome-key") == 0)
			found = c;
	}
	g_assert_nonnull(found);
	g_signal_emit_by_name(found, "key-pressed", keyval, 0u, (GdkModifierType)0, &handled);
	g_assert_true(handled);
#else
	/* The toplevel hands a bare key to the focused entry first. */
	g_assert_true(prompt_focused(h));
	g_assert_true(press(h, keyval, 0));
#endif
}

static void
prompt_set_text(H *h, const gchar *text)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_editable_set_text(GTK_EDITABLE(h->w->prompt_entry), text);
#else
	gtk_entry_set_text(GTK_ENTRY(h->w->prompt_entry), text);
#endif
}

/* ------------------------------------------------------------------ */
/* Inspection                                                          */
/* ------------------------------------------------------------------ */

static GsurfWmuxWin *
cur(H *h)
{
	return g_ptr_array_index(h->w->wins, h->w->current);
}

static guint
n_panes(H *h)
{
	return gsurf_wmux_layout_get_n_panes(cur(h)->layout);
}

static GsurfView *
active_view(H *h)
{
	return gsurf_window_get_active_view(h->window);
}

/* The view in the current window's active pane. */
static GsurfView *
active_pane_view(H *h)
{
	GsurfWmuxLayout *layout = cur(h)->layout;
	guint64 id = gsurf_wmux_layout_get_payload(layout, gsurf_wmux_layout_get_active(layout));

	return id != 0 ? gsurf_view_lookup(id) : NULL;
}

static GsurfWmuxFrame *
frame_of(H *h, GsurfView *view)
{
	guint64 id = gsurf_view_get_id(view);

	return g_hash_table_lookup(h->w->frames, &id);
}

static GPtrArray *
grid_children(H *h)
{
	GPtrArray *out = g_ptr_array_new();
#ifdef GSURF_BACKEND_GTK4
	GtkWidget *child;

	for (child = gtk_widget_get_first_child(h->w->grid); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		g_ptr_array_add(out, child);
#else
	g_autoptr(GList) children = gtk_container_get_children(GTK_CONTAINER(h->w->grid));
	GList *l;

	for (l = children; l != NULL; l = l->next)
		g_ptr_array_add(out, l->data);
#endif
	return out;
}

static guint
child_pane(GtkWidget *child)
{
	return GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(child), "gsurf-wmux-grid-pane")) - 1;
}

static gint
width_of(GtkWidget *widget)
{
#ifdef GSURF_BACKEND_GTK4
	return gtk_widget_get_width(widget);
#else
	return gtk_widget_get_allocated_width(widget);
#endif
}

/* @view's frame rectangle in grid coordinates (waits for allocation). */
static GdkRectangle
rect_of(H *h, GsurfView *view)
{
	GsurfWmuxFrame *f;
	GdkRectangle r;

	WAIT_UNTIL((f = frame_of(h, view)) != NULL && gtk_widget_get_mapped(f->frame) &&
	           width_of(f->frame) > 1);
#ifdef GSURF_BACKEND_GTK4
	{
		graphene_rect_t b;

		g_assert_true(gtk_widget_compute_bounds(f->frame, h->w->grid, &b));
		r.x = (gint)b.origin.x;
		r.y = (gint)b.origin.y;
		r.width = (gint)b.size.width;
		r.height = (gint)b.size.height;
	}
#else
	gtk_widget_get_allocation(f->frame, &r);
#endif
	return r;
}

/* Let pending allocations run. */
static void
settle(void)
{
	gsurf_webkit_test_spin(120);
}

static gboolean
has_class(GtkWidget *widget, const gchar *css_class)
{
#ifdef GSURF_BACKEND_GTK4
	return gtk_widget_has_css_class(widget, css_class);
#else
	return gtk_style_context_has_class(gtk_widget_get_style_context(widget), css_class);
#endif
}

/* The status line as shown: "[gsurf]", each window entry, then (two
 * spaces) the pane counter. */
static gchar *
status_text(H *h)
{
	GString *out = g_string_new("[gsurf]");
#ifdef GSURF_BACKEND_GTK4
	GtkWidget *child;

	for (child = gtk_widget_get_first_child(h->w->status_windows); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		g_string_append_printf(out, " %s", gtk_button_get_label(GTK_BUTTON(child)));
#else
	g_autoptr(GList) children = gtk_container_get_children(GTK_CONTAINER(h->w->status_windows));
	GList *l;

	for (l = children; l != NULL; l = l->next)
		g_string_append_printf(out, " %s", gtk_button_get_label(GTK_BUTTON(l->data)));
#endif
	g_string_append_printf(out, "  %s", gtk_label_get_text(GTK_LABEL(h->w->status_right)));
	return g_string_free(out, FALSE);
}

/* The colour maths of the stylesheet, redone here. */
static gchar *
with_alpha(const gchar *color, gdouble opacity)
{
	GdkRGBA rgba;

	g_assert_true(gdk_rgba_parse(&rgba, color));
	rgba.alpha *= opacity;
	return gdk_rgba_to_string(&rgba);
}

static gchar *
brighten(const gchar *color, gdouble amount)
{
	GdkRGBA rgba;

	g_assert_true(gdk_rgba_parse(&rgba, color));
	rgba.red += (1.0 - rgba.red) * amount;
	rgba.green += (1.0 - rgba.green) * amount;
	rgba.blue += (1.0 - rgba.blue) * amount;
	return gdk_rgba_to_string(&rgba);
}

/*
 * check_invariants:
 *
 * The model, the window and the widgets agree:
 *   - every pane names a live view of the window, each view at most once
 *     (the others are background tabs);
 *   - the grid's children are the frames; those tagged with a pane are
 *     exactly the current window's panes with a view, the rest parked;
 *   - every view has a frame holding its widget; an off-screen view's
 *     frame is parked;
 *   - visibility as the model says; the window's active view is the
 *     active pane's; every layout passes its own check.
 */
static void
check_invariants(H *h)
{
	GsurfWmuxModule *w = h->w;
	GPtrArray *views = gsurf_window_get_views(h->window);
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GPtrArray) children = grid_children(h);
	/* While a solo view is shown, its own one-pane layout is on screen. */
	GsurfWmuxLayout *layout = w->solo_active ? w->solo->layout : cur(h)->layout;
	guint shown = 0;
	guint i;
	guint j;

	/* 1. Panes -> views, unique. */
	for (i = 0; i < w->wins->len; i++) {
		GsurfWmuxWin *win = g_ptr_array_index(w->wins, i);
		g_autoptr(GArray) panes = gsurf_wmux_layout_list_panes(win->layout);
		g_autofree gchar *problem = NULL;

		if (!gsurf_wmux_layout_check(win->layout, &problem))
			g_error("window %u layout: %s", i, problem);
		for (j = 0; j < panes->len; j++) {
			guint64 id = gsurf_wmux_layout_get_payload(win->layout, g_array_index(panes, guint, j));
			GsurfView *view;

			if (id == 0)
				continue;
			view = gsurf_view_lookup(id);
			g_assert_nonnull(view);
			g_assert_cmpint(gsurf_window_get_view_index(h->window, view), >=, 0);
			g_assert_false(g_hash_table_contains(seen, &id));
			g_hash_table_add(seen, g_memdup2(&id, sizeof(id)));
			if (i == w->current && !w->solo_active)
				shown++;
		}
	}
	if (w->solo_active) {
		guint64 solo = gsurf_wmux_layout_get_payload(layout, gsurf_wmux_layout_get_active(layout));

		/* The solo tab is in no pane of any window. */
		g_assert_cmpuint(gsurf_wmux_layout_get_n_panes(layout), ==, 1);
		g_assert_false(g_hash_table_contains(seen, &solo));
		shown = 1;
	}

	/* 2. Grid children = every frame; the ones tagged with a pane are
	 *    exactly the current window's panes with a view, the others are
	 *    parked (not shown). */
	g_assert_cmpuint(children->len, ==, g_hash_table_size(w->frames));
	g_assert_cmpuint(children->len, ==, views->len);
	for (i = 0; i < children->len; i++) {
		GtkWidget *child = g_ptr_array_index(children, i);
		guint64 *id = g_object_get_data(G_OBJECT(child), GSURF_WMUX_VIEW_ID_DATA);
		GsurfWmuxFrame *f;

		g_assert_nonnull(id);
		f = g_hash_table_lookup(w->frames, id);
		g_assert_nonnull(f);
		g_assert_true(f->frame == child);
		g_assert_true(gtk_widget_get_parent(f->webview) == f->overlay);
		g_assert_true(f->webview == gsurf_view_get_native_widget(gsurf_view_lookup(*id)));
		if (child_pane(child) == GSURF_WMUX_GRID_PARKED) {
			g_assert_cmpuint(gsurf_wmux_layout_find_payload(layout, *id), ==,
				GSURF_WMUX_PANE_NONE);
			g_assert_false(has_class(child, GSURF_WMUX_PANE_ACTIVE_CLASS));
			continue;
		}
		shown--;
		g_assert_cmpuint(gsurf_wmux_layout_get_payload(layout, child_pane(child)), ==, *id);
		g_assert_cmpint(has_class(child, GSURF_WMUX_PANE_ACTIVE_CLASS), ==,
			child_pane(child) == gsurf_wmux_layout_get_active(layout));
	}
	g_assert_cmpuint(shown, ==, 0);

	/* 3. Every view: visibility and parenting. */
	for (i = 0; i < views->len; i++) {
		GsurfView *view = g_ptr_array_index(views, i);
		guint64 id = gsurf_view_get_id(view);
		guint pane = gsurf_wmux_layout_find_payload(layout, id);
		guint zoomed = gsurf_wmux_layout_get_zoomed(layout);
		gboolean visible = pane != GSURF_WMUX_PANE_NONE &&
			(zoomed == GSURF_WMUX_PANE_NONE || zoomed == pane);

		GsurfWmuxFrame *f = g_hash_table_lookup(w->frames, &id);

		g_assert_cmpint(gsurf_window_get_view_visible(h->window, view), ==, visible);
		/* Every view has its frame: off screen, parked. */
		g_assert_nonnull(f);
		if (pane == GSURF_WMUX_PANE_NONE)
			g_assert_cmpuint(child_pane(f->frame), ==, GSURF_WMUX_GRID_PARKED);
		else
			g_assert_cmpuint(child_pane(f->frame), ==, pane);
	}

	/* 4. The active view is the active pane's (the solo one's). */
	if (w->solo_active)
		g_assert_true(gsurf_view_get_id(active_view(h)) ==
			gsurf_wmux_layout_get_payload(layout, gsurf_wmux_layout_get_active(layout)));
	else if (active_pane_view(h) != NULL)
		g_assert_true(active_view(h) == active_pane_view(h));
}

/* ------------------------------------------------------------------ */
/* Splits, navigation, focus, key grab                                 */
/* ------------------------------------------------------------------ */

static void
test_split_navigate(void)
{
	H h;
	GsurfView *a, *b, *c, *d;
	GdkRectangle ra, rb, rc;
	g_autofree gchar *new_uri = page("new");
	g_autofree gchar *input = NULL;
	GsurfView *before;
	g_autofree gchar *status = NULL;
	g_autofree gchar *value = NULL;
	guint i;

	if (!gsurf_webkit_test_init() || !h_init(&h, NULL, NULL, NULL))
		return;
	a = h.first;

	/* 1. One pane showing the startup view. */
	g_assert_cmpuint(h.w->wins->len, ==, 1);
	g_assert_cmpuint(n_panes(&h), ==, 1);
	g_assert_true(active_pane_view(&h) == a);
	ra = rect_of(&h, a);
	g_assert_cmpint(ra.width, >, WINDOW_WIDTH / 2);
	check_invariants(&h);

	/* 2. C-b %: a new tab in a new pane on the right, active. */
	cmd(&h, GDK_KEY_percent, SHIFT);
	g_assert_false(gsurf_window_get_key_grab(h.window));
	g_assert_cmpuint(gsurf_window_get_n_views(h.window), ==, 2);
	b = active_view(&h);
	g_assert_true(b != a);
	g_assert_true(active_pane_view(&h) == b);
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_uri(b), new_uri) == 0);
	settle();
	ra = rect_of(&h, a);
	rb = rect_of(&h, b);
	g_assert_cmpint(ra.x, <, rb.x);
	g_assert_cmpint(ra.y, ==, rb.y);
	g_assert_cmpint(ra.height, ==, rb.height);
	g_assert_cmpint(ABS(ra.width - rb.width), <=, 1);
	g_assert_cmpint(rb.x - (ra.x + ra.width), ==, h.w->gap);
	g_assert_true(gsurf_window_get_view_visible(h.window, a));
	g_assert_true(gsurf_window_get_view_visible(h.window, b));
	check_invariants(&h);

	/* 3. C-b ": the right pane splits; the new one below is active. */
	cmd(&h, GDK_KEY_quotedbl, SHIFT);
	c = active_view(&h);
	g_assert_true(c != a && c != b);
	settle();
	rb = rect_of(&h, b);
	rc = rect_of(&h, c);
	g_assert_cmpint(rb.x, ==, rc.x);
	g_assert_cmpint(rb.y, <, rc.y);
	g_assert_cmpint(rb.width, ==, rc.width);
	check_invariants(&h);

	/* 4. C-h/j/k/l (root table, no prefix) and the active view follows. */
	g_assert_true(press(&h, GDK_KEY_h, CTRL));
	g_assert_true(active_view(&h) == a);
	g_assert_true(press(&h, GDK_KEY_l, CTRL));
	g_assert_true(active_view(&h) == c); /* most recently used on the right */
	g_assert_true(press(&h, GDK_KEY_k, CTRL));
	g_assert_true(active_view(&h) == b);
	g_assert_true(press(&h, GDK_KEY_j, CTRL));
	g_assert_true(active_view(&h) == c);
	check_invariants(&h);

	/* 5. Arrows after the prefix, repeatable without it; a non-repeatable
	 *    key after the repeat ends it. */
	cmd(&h, GDK_KEY_Up, 0);
	g_assert_true(active_view(&h) == b);
	g_assert_true(press(&h, GDK_KEY_Left, 0));
	g_assert_true(active_view(&h) == a);
	/* (The page may consume the bare keys; wmux must not act on them.) */
	press(&h, GDK_KEY_x, 0);
	press(&h, GDK_KEY_Right, 0);
	g_assert_true(active_view(&h) == a);
	/* o: next pane in order; ;: last pane. */
	cmd(&h, GDK_KEY_o, 0);
	g_assert_true(active_view(&h) == b);
	cmd(&h, GDK_KEY_semicolon, 0);
	g_assert_true(active_view(&h) == a);

	/* 6. Focus follows click: focus into another pane's page. */
	gtk_widget_grab_focus(gsurf_view_get_native_widget(c));
	WAIT_UNTIL(active_view(&h) == c);
	g_assert_true(has_class(frame_of(&h, c)->frame, GSURF_WMUX_PANE_ACTIVE_CLASS));
	g_assert_false(has_class(frame_of(&h, a)->frame, GSURF_WMUX_PANE_ACTIVE_CLASS));
	check_invariants(&h);

	/* 7. Ctrl held: C-b C-% duplicates the page (same URI, container). */
	cmd(&h, GDK_KEY_percent, CTRL | SHIFT);
	d = active_view(&h);
	g_assert_true(d != c);
	g_assert_cmpstr(gsurf_view_get_initial_uri(d), ==, gsurf_view_get_uri(c));
	g_assert_null(gsurf_view_get_container_id(d)); /* the default one, like c */
	g_assert_true(gsurf_view_get_opener(d) == c);
	g_assert_cmpuint(n_panes(&h), ==, 4);
	check_invariants(&h);
	cmd(&h, GDK_KEY_quotedbl, CTRL | SHIFT);
	g_assert_cmpuint(n_panes(&h), ==, 5);
	g_assert_cmpstr(gsurf_view_get_initial_uri(active_view(&h)), ==,
		gsurf_view_get_initial_uri(d));
	check_invariants(&h);

	/* 8. The prefix while typing in a page field: the key grab sends %
	 *    to wmux (without it the guard would hand it to the page). */
	input = g_strdup_printf("http://127.0.0.1:%u/input", port);
	gsurf_view_load_uri(a, input);
	gsurf_webkit_test_wait_finished(gsurf_webkit_test_native(a));
	gsurf_window_set_active_view(h.window, a);
	WAIT_UNTIL(gsurf_view_get_editing(a));
	press(&h, GDK_KEY_y, 0); /* typing: the page's, not a command */
	i = gsurf_window_get_n_views(h.window);
	cmd(&h, GDK_KEY_percent, SHIFT);
	g_assert_cmpuint(gsurf_window_get_n_views(h.window), ==, i + 1);
	gsurf_webkit_test_spin(100);
	value = gsurf_webkit_test_eval(gsurf_webkit_test_native(a), "document.getElementById('f').value");
	g_assert_cmpstr(value, ==, "");
	check_invariants(&h);

	/* 8b. A pending prefix never lingers: a click, another active view
	 *     or the window losing focus drops it and the key grab, so the
	 *     next letter typed into the field is the page's again. */
	before = gsurf_window_get_active_view(h.window);
	gsurf_window_set_active_view(h.window, a);
	WAIT_UNTIL(gsurf_view_get_editing(a));
	g_assert_true(press(&h, GDK_KEY_b, CTRL));
	g_assert_true(gsurf_window_get_key_grab(h.window));
	gsurf_window_emit_button_press(h.window, 1, 0);
	g_assert_false(h.w->prefix_active);
	g_assert_false(gsurf_window_get_key_grab(h.window));
	i = gsurf_window_get_n_views(h.window);
	press(&h, GDK_KEY_percent, SHIFT); /* typing again, not a split */
	g_assert_cmpuint(gsurf_window_get_n_views(h.window), ==, i);

	g_assert_true(press(&h, GDK_KEY_b, CTRL));
	g_assert_true(gsurf_window_get_key_grab(h.window));
	g_signal_emit_by_name(h.window, "active-view-changed", a);
	g_assert_false(gsurf_window_get_key_grab(h.window));

	g_assert_true(press(&h, GDK_KEY_b, CTRL));
	g_assert_true(gsurf_window_get_key_grab(h.window));
	g_object_notify(G_OBJECT(h.toplevel), "is-active"); /* headless: not active */
	if (!gtk_window_is_active(GTK_WINDOW(h.toplevel)))
		g_assert_false(gsurf_window_get_key_grab(h.window));
	set_prefix_off(&h);
	g_assert_cmpuint(gsurf_window_get_n_views(h.window), ==, i);
	check_invariants(&h);
	gsurf_window_set_active_view(h.window, before);

	/* 9. The status line: one window named after its active page, the
	 *    active pane (the second) of six. */
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(active_view(&h)), "new") == 0);
	status = status_text(&h);
	g_assert_cmpstr(status, ==, "[gsurf] 0:new*  2/6");
	g_assert_true(gtk_widget_get_visible(h.w->status_box));

	/* 10. The keybind help lists the wmux tables. */
	{
		g_autoptr(GPtrArray) entries = gsurf_module_manager_collect_keybinds(h.mgr);
		gboolean found = FALSE;

		for (i = 0; i < entries->len; i++) {
			GsurfKeybindHelp *e = g_ptr_array_index(entries, i);

			if (g_strcmp0(gsurf_keybind_help_get_key(e), "C-b %") == 0 &&
			    g_strcmp0(gsurf_keybind_help_get_description(e), "split-window -h") == 0)
				found = TRUE;
		}
		g_assert_true(found);
	}
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* Pane operations                                                     */
/* ------------------------------------------------------------------ */

#ifdef GSURF_BACKEND_GTK3
static void
grid_pointer(H *h, GdkEventType type, gint x, gint y)
{
	GdkEvent *event = gdk_event_new(type);
	GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(h->w->grid));

	if (type == GDK_MOTION_NOTIFY) {
		event->motion.window = g_object_ref(gtk_widget_get_window(h->w->grid));
		event->motion.x = x;
		event->motion.y = y;
	} else {
		event->button.window = g_object_ref(gtk_widget_get_window(h->w->grid));
		event->button.button = 1;
		event->button.x = x;
		event->button.y = y;
	}
	gdk_event_set_device(event, gdk_seat_get_pointer(seat));
	gtk_widget_event(h->w->grid, event);
	gdk_event_free(event);
}
#endif

/* Drag from (@x0, @y0) to (@x1, @y1) on the grid through its pointer
 * handlers. */
static void
drag(H *h, gint x0, gint y0, gint x1, gint y1)
{
#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(h->w->grid);
	GtkEventController *found = NULL;
	guint i;

	for (i = 0; i < g_list_model_get_n_items(controllers) && found == NULL; i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (g_strcmp0(gtk_event_controller_get_name(c), "gsurf-wmux-grid-drag") == 0)
			found = c;
	}
	g_assert_nonnull(found);
	g_signal_emit_by_name(found, "drag-begin", (gdouble)x0, (gdouble)y0);
	g_signal_emit_by_name(found, "drag-update", (gdouble)(x1 - x0), (gdouble)(y1 - y0));
	g_signal_emit_by_name(found, "drag-end", (gdouble)(x1 - x0), (gdouble)(y1 - y0));
#else
	grid_pointer(h, GDK_BUTTON_PRESS, x0, y0);
	grid_pointer(h, GDK_MOTION_NOTIFY, x1, y1);
	grid_pointer(h, GDK_BUTTON_RELEASE, x1, y1);
#endif
}

static void
test_pane_ops(void)
{
	H h;
	GsurfView *a, *b, *c;
	GdkRectangle ra, rb, rc;
	gpointer weak;
	gint before;
	guint i;

	if (!gsurf_webkit_test_init() || !h_init(&h, NULL, NULL, NULL))
		return;
	a = h.first;
	cmd(&h, GDK_KEY_percent, SHIFT);
	b = active_view(&h);
	cmd(&h, GDK_KEY_quotedbl, SHIFT);
	c = active_view(&h);
	check_invariants(&h);

	/* 1. Zoom: only the zoomed pane is allocated; status shows Z. */
	cmd(&h, GDK_KEY_z, 0);
	settle();
	g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(cur(&h)->layout), !=, GSURF_WMUX_PANE_NONE);
	rc = rect_of(&h, c);
	g_assert_cmpint(rc.width, >, WINDOW_WIDTH - 50);
	g_assert_false(gtk_widget_get_child_visible(frame_of(&h, a)->frame));
	g_assert_false(gsurf_window_get_view_visible(h.window, a));
	g_assert_true(g_str_has_suffix(gtk_label_get_text(GTK_LABEL(h.w->status_right)), " Z"));
	check_invariants(&h);
	cmd(&h, GDK_KEY_z, 0);
	settle();
	g_assert_true(gtk_widget_get_child_visible(frame_of(&h, a)->frame));
	g_assert_true(gsurf_window_get_view_visible(h.window, a));

	/* 2. display-panes: numbers shown; a digit selects that pane. */
	cmd(&h, GDK_KEY_q, 0);
	g_assert_true(h.w->displaying);
	g_assert_true(gsurf_window_get_key_grab(h.window));
	g_assert_true(gtk_widget_get_visible(frame_of(&h, a)->number));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(frame_of(&h, a)->number)), ==, "0");
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(frame_of(&h, c)->number)), ==, "2");
	g_assert_true(press(&h, GDK_KEY_0, 0));
	g_assert_false(h.w->displaying);
	g_assert_false(gsurf_window_get_key_grab(h.window));
	g_assert_false(gtk_widget_get_visible(frame_of(&h, a)->number));
	g_assert_true(active_view(&h) == a);
	/* ... and the numbers go away by themselves. */
	h.w->display_panes_time = 150;
	cmd(&h, GDK_KEY_q, 0);
	WAIT_UNTIL(!h.w->displaying);
	g_assert_false(gsurf_window_get_key_grab(h.window));

	/* 3. Swap and rotate move the tabs; the active tab moves along. */
	cmd(&h, GDK_KEY_braceright, SHIFT);
	g_assert_true(active_view(&h) == a);
	g_assert_cmpint(gsurf_wmux_layout_get_pane_index(cur(&h)->layout,
		gsurf_wmux_layout_find_payload(cur(&h)->layout, gsurf_view_get_id(a))), ==, 1);
	check_invariants(&h);
	cmd(&h, GDK_KEY_braceleft, SHIFT);
	g_assert_cmpint(gsurf_wmux_layout_get_pane_index(cur(&h)->layout,
		gsurf_wmux_layout_find_payload(cur(&h)->layout, gsurf_view_get_id(a))), ==, 0);
	cmd(&h, GDK_KEY_o, CTRL);
	settle();
	ra = rect_of(&h, a);
	rb = rect_of(&h, b);
	g_assert_cmpint(rb.x, ==, 0); /* b took the first place */
	g_assert_cmpint(ra.x, >, 0);
	check_invariants(&h);
	cmd(&h, GDK_KEY_o, ALT);
	settle();
	g_assert_cmpint(rect_of(&h, a).x, ==, 0);

	/* 4. Presets: even-vertical stacks, even-horizontal lines up, tiled,
	 *    main-vertical and Space cycling. */
	cmd(&h, GDK_KEY_2, ALT);
	settle();
	ra = rect_of(&h, a);
	rb = rect_of(&h, b);
	rc = rect_of(&h, c);
	g_assert_cmpint(ra.x, ==, rb.x);
	g_assert_cmpint(rb.x, ==, rc.x);
	g_assert_cmpint(ra.y, <, rb.y);
	g_assert_cmpint(rb.y, <, rc.y);
	cmd(&h, GDK_KEY_1, ALT);
	settle();
	ra = rect_of(&h, a);
	rc = rect_of(&h, c);
	g_assert_cmpint(ra.y, ==, rc.y);
	g_assert_cmpint(ra.x, <, rc.x);
	cmd(&h, GDK_KEY_4, ALT);
	settle();
	ra = rect_of(&h, a);
	g_assert_cmpint(ra.height, >, WINDOW_HEIGHT / 2);
	g_assert_cmpint(ra.width, >, rect_of(&h, b).width);
	cmd(&h, GDK_KEY_space, 0);
	g_assert_cmpint(gsurf_wmux_layout_get_preset(cur(&h)->layout), ==, GSURF_WMUX_PRESET_TILED);
	cmd(&h, GDK_KEY_3, ALT);
	cmd(&h, GDK_KEY_E, SHIFT);
	check_invariants(&h);

	/* 5. Resize keys: C-b C-Right grows the left pane, repeatable. */
	cmd(&h, GDK_KEY_1, ALT);
	gsurf_window_set_active_view(h.window, a);
	settle();
	before = rect_of(&h, a).width;
	cmd(&h, GDK_KEY_Right, CTRL);
	g_assert_true(press(&h, GDK_KEY_Right, CTRL));
	settle();
	g_assert_cmpint(rect_of(&h, a).width, >, before + 10);
	cmd(&h, GDK_KEY_Left, ALT);
	settle();
	g_assert_cmpint(rect_of(&h, a).width, <, before);

	/* 6. A mouse drag on the gap between a and the next pane. */
	ra = rect_of(&h, a);
	before = ra.width;
	drag(&h, ra.x + ra.width + h.w->gap / 2, ra.y + 20, ra.x + ra.width + 120, ra.y + 20);
	settle();
	g_assert_false(h.w->dragging);
	g_assert_cmpint(rect_of(&h, a).width, >, before + 60);
	/* A press on a pane is not a drag. */
	drag(&h, ra.x + 30, ra.y + 30, ra.x + 300, ra.y + 30);
	g_assert_false(h.w->dragging);
	check_invariants(&h);

	/* 7. Kill with confirm: x asks; n keeps it; y closes the pane and its
	 *    tab. */
	gsurf_window_set_active_view(h.window, c);
	cmd(&h, GDK_KEY_x, 0);
	g_assert_cmpint(h.w->prompt_kind, ==, GSURF_WMUX_PROMPT_CONFIRM);
	g_assert_true(gtk_widget_get_visible(h.w->prompt_box));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(h.w->prompt_label)), ==, "kill-pane? (y/n)");
	WAIT_UNTIL(prompt_focused(&h));
	prompt_key(&h, GDK_KEY_n);
	g_assert_cmpint(h.w->prompt_kind, ==, GSURF_WMUX_PROMPT_NONE);
	g_assert_cmpuint(n_panes(&h), ==, 3);
	cmd(&h, GDK_KEY_x, 0);
	WAIT_UNTIL(prompt_focused(&h));
	weak = c;
	g_object_add_weak_pointer(G_OBJECT(c), &weak);
	prompt_key(&h, GDK_KEY_y);
	g_assert_cmpuint(n_panes(&h), ==, 2);
	g_assert_cmpuint(gsurf_window_get_n_views(h.window), ==, 2);
	WAIT_UNTIL(weak == NULL);
	check_invariants(&h);

	/* 8. The last pane of the last window is never removed: a new blank
	 *    tab takes its place. */
	cmd(&h, GDK_KEY_x, 0);
	WAIT_UNTIL(prompt_focused(&h));
	prompt_key(&h, GDK_KEY_y);
	g_assert_cmpuint(n_panes(&h), ==, 1);
	cmd(&h, GDK_KEY_x, 0);
	WAIT_UNTIL(prompt_focused(&h));
	prompt_key(&h, GDK_KEY_y);
	g_assert_cmpuint(n_panes(&h), ==, 1);
	g_assert_cmpuint(gsurf_window_get_n_views(h.window), ==, 1);
	g_assert_true(active_view(&h) != a && active_view(&h) != b);
	check_invariants(&h);

	/* 9. A split refused for space reports it in the status line. */
	for (i = 0; i < 8; i++)
		cmd(&h, GDK_KEY_percent, SHIFT);
	g_assert_cmpuint(n_panes(&h), <, 9);
	g_assert_true(gtk_widget_get_visible(h.w->message));
	check_invariants(&h);
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* tmux windows, status line, prompt, chooser, open prompt             */
/* ------------------------------------------------------------------ */

static void
test_windows(void)
{
	static const gchar *const modules[] = { "chromebar", NULL };
	H h;
	GsurfView *a, *b, *c;
	g_autofree gchar *status = NULL;
	g_autofree gchar *docs = page("docs");
	GtkWidget *entry;

	if (!gsurf_webkit_test_init() ||
	    !h_init(&h, NULL, "  chromebar:\n    enabled: true\n", modules))
		return;
	a = h.first;
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(a), "first") == 0);

	/* 1. c: a second window with a new tab; n p l cycle; digits select. */
	cmd(&h, GDK_KEY_c, 0);
	g_assert_cmpuint(h.w->wins->len, ==, 2);
	g_assert_cmpuint(h.w->current, ==, 1);
	b = active_view(&h);
	g_assert_true(b != a);
	g_assert_false(gsurf_window_get_view_visible(h.window, a));
	g_assert_cmpuint(child_pane(frame_of(&h, a)->frame), ==, GSURF_WMUX_GRID_PARKED);
	WAIT_UNTIL(!gtk_widget_get_mapped(gsurf_view_get_native_widget(a)));
	check_invariants(&h);
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(b), "new") == 0);
	status = status_text(&h);
	g_assert_cmpstr(status, ==, "[gsurf] 0:first- 1:new*  1/1");
	g_clear_pointer(&status, g_free);
	cmd(&h, GDK_KEY_n, 0);
	g_assert_cmpuint(h.w->current, ==, 0);
	g_assert_true(active_view(&h) == a);
	cmd(&h, GDK_KEY_p, 0);
	g_assert_cmpuint(h.w->current, ==, 1);
	cmd(&h, GDK_KEY_l, 0);
	g_assert_cmpuint(h.w->current, ==, 0);
	cmd(&h, GDK_KEY_1, 0);
	g_assert_cmpuint(h.w->current, ==, 1);
	cmd(&h, GDK_KEY_7, 0); /* no such window: nothing */
	g_assert_cmpuint(h.w->current, ==, 1);
	check_invariants(&h);

	/* 2. Selecting a tab shown in another window switches to it. */
	gsurf_window_set_active_view(h.window, a);
	g_assert_cmpuint(h.w->current, ==, 0);
	check_invariants(&h);

	/* 3. A status-line window entry is clickable. */
	{
#ifdef GSURF_BACKEND_GTK4
		GtkWidget *second = gtk_widget_get_next_sibling(
			gtk_widget_get_first_child(h.w->status_windows));
#else
		g_autoptr(GList) list = gtk_container_get_children(GTK_CONTAINER(h.w->status_windows));
		GtkWidget *second = g_list_nth_data(list, 1);
#endif
		g_assert_true(GTK_IS_BUTTON(second));
		g_assert_true(has_class(second, GSURF_WMUX_STATUS_WINDOW_CLASS));
		g_signal_emit_by_name(second, "clicked");
		g_assert_cmpuint(h.w->current, ==, 1);
	}

	/* 4. , renames through the prompt (initial text: the name). */
	cmd(&h, GDK_KEY_comma, 0);
	g_assert_cmpint(h.w->prompt_kind, ==, GSURF_WMUX_PROMPT_COMMAND);
	g_assert_false(gtk_widget_get_visible(h.w->status_windows));
#ifdef GSURF_BACKEND_GTK4
	g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(h.w->prompt_entry)), ==, "new");
#else
	g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(h.w->prompt_entry)), ==, "new");
#endif
	WAIT_UNTIL(prompt_focused(&h));
	prompt_set_text(&h, "my docs");
	prompt_key(&h, GDK_KEY_Return);
	g_assert_cmpint(h.w->prompt_kind, ==, GSURF_WMUX_PROMPT_NONE);
	status = status_text(&h);
	g_assert_cmpstr(status, ==, "[gsurf] 0:first- 1:my docs*  1/1");
	g_clear_pointer(&status, g_free);

	/* 5. : runs any command; Escape cancels; a bad one is reported. */
	cmd(&h, GDK_KEY_colon, SHIFT);
	WAIT_UNTIL(prompt_focused(&h));
	prompt_set_text(&h, "new-window -n mail");
	prompt_key(&h, GDK_KEY_Return);
	g_assert_cmpuint(h.w->wins->len, ==, 3);
	c = active_view(&h);
	cmd(&h, GDK_KEY_colon, SHIFT);
	WAIT_UNTIL(prompt_focused(&h));
	prompt_set_text(&h, "kill-window");
	prompt_key(&h, GDK_KEY_Escape);
	g_assert_cmpuint(h.w->wins->len, ==, 3);
	cmd(&h, GDK_KEY_colon, SHIFT);
	WAIT_UNTIL(prompt_focused(&h));
	prompt_set_text(&h, "spilt-window");
	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*unknown command*");
	prompt_key(&h, GDK_KEY_Return);
	g_test_assert_expected_messages();
	g_assert_true(gtk_widget_get_visible(h.w->message));
	status = status_text(&h);
	g_assert_cmpstr(status, ==, "[gsurf] 0:first 1:my docs- 2:mail*  1/1");
	g_clear_pointer(&status, g_free);
	/* A command that parses but cannot run says why (0.4.4). */
	cmd(&h, GDK_KEY_colon, SHIFT);
	WAIT_UNTIL(prompt_focused(&h));
	prompt_set_text(&h, "select-window -t 9");
	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*can't find window 9*");
	prompt_key(&h, GDK_KEY_Return);
	g_test_assert_expected_messages();
	g_assert_true(gtk_widget_get_visible(h.w->message));
	g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(h.w->message)),
		"select-window: can't find window 9 (there are 3)"));
	g_assert_cmpuint(h.w->current, ==, 2);

	/* 6. o (open prompt) then Enter loads into the active pane. */
	cmd(&h, GDK_KEY_percent, SHIFT);
	g_assert_true(gsurf_module_manager_dispatch_action(h.mgr, active_view(&h),
		GSURF_ACTION_OPEN_PROMPT));
	entry = NULL;
	{
		GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(h.toplevel));

		g_assert_nonnull(focus);
		g_assert_true(GTK_IS_EDITABLE(focus));
		entry = focus;
	}
#ifdef GSURF_BACKEND_GTK4
	gtk_editable_set_text(GTK_EDITABLE(entry), docs);
	while (!GTK_IS_ENTRY(entry))
		entry = gtk_widget_get_parent(entry);
#else
	gtk_entry_set_text(GTK_ENTRY(entry), docs);
#endif
	g_signal_emit_by_name(entry, "activate");
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_uri(active_pane_view(&h)), docs) == 0);
	g_assert_true(active_view(&h) != c);
	check_invariants(&h);

	/* 7. The chooser: windows and panes; j/k move, Enter selects, x
	 *    kills, Escape closes. */
	cmd(&h, GDK_KEY_w, 0);
	g_assert_true(h.w->chooser_open);
	g_assert_true(gtk_widget_get_visible(h.w->chooser));
	g_assert_true(gsurf_window_get_key_grab(h.window));
	/* window 0, its pane, window 1, its pane, window 2, its two panes */
	g_assert_cmpuint(h.w->chooser_items->len, ==, 7);
	g_assert_cmpuint(h.w->chooser_sel, ==, 6);
	g_assert_true(press(&h, GDK_KEY_k, 0));
	g_assert_true(press(&h, GDK_KEY_k, 0));
	g_assert_true(press(&h, GDK_KEY_k, 0));
	g_assert_cmpuint(h.w->chooser_sel, ==, 3);
	g_assert_true(press(&h, GDK_KEY_j, 0));
	g_assert_true(press(&h, GDK_KEY_k, 0));
	g_assert_true(press(&h, GDK_KEY_Return, 0));
	g_assert_false(h.w->chooser_open);
	g_assert_false(gsurf_window_get_key_grab(h.window));
	g_assert_cmpuint(h.w->current, ==, 1);
	g_assert_true(active_view(&h) == b);
	cmd(&h, GDK_KEY_w, 0);
	h.w->chooser_sel = 0;
	g_assert_true(press(&h, GDK_KEY_x, 0)); /* kill window 0 (and tab a) */
	g_assert_true(h.w->chooser_open);
	g_assert_cmpuint(h.w->wins->len, ==, 2);
	g_assert_cmpint(gsurf_window_get_view_index(h.window, a), <, 0);
	g_assert_true(press(&h, GDK_KEY_Escape, 0));
	g_assert_false(h.w->chooser_open);
	check_invariants(&h);

	/* 8. & kills the current window after confirming. */
	cmd(&h, GDK_KEY_ampersand, SHIFT);
	WAIT_UNTIL(prompt_focused(&h));
	prompt_key(&h, GDK_KEY_y);
	g_assert_cmpuint(h.w->wins->len, ==, 1);
	check_invariants(&h);
	/* The last window: replaced by a fresh one with a blank tab. */
	cmd(&h, GDK_KEY_ampersand, SHIFT);
	WAIT_UNTIL(prompt_focused(&h));
	prompt_key(&h, GDK_KEY_y);
	g_assert_cmpuint(h.w->wins->len, ==, 1);
	g_assert_cmpuint(gsurf_window_get_n_views(h.window), ==, 1);
	check_invariants(&h);

	/* 9. break-pane: the active pane into a window of its own. */
	cmd(&h, GDK_KEY_percent, SHIFT);
	c = active_view(&h);
	cmd(&h, GDK_KEY_exclam, SHIFT);
	g_assert_cmpuint(h.w->wins->len, ==, 2);
	g_assert_true(active_view(&h) == c);
	g_assert_cmpuint(n_panes(&h), ==, 1);
	check_invariants(&h);
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* Tabs and containers                                                 */
/* ------------------------------------------------------------------ */

static GtkWidget *
find_row(GtkWidget *root, guint64 id)
{
	guint64 *wid = g_object_get_data(G_OBJECT(root), "gsurf-tab-view-id");
	GtkWidget *found = NULL;

	if (wid != NULL && *wid == id && has_class(root, "gsurf-tab-row"))
		return root;
#ifdef GSURF_BACKEND_GTK4
	{
		GtkWidget *child;

		for (child = gtk_widget_get_first_child(root); child != NULL && found == NULL;
		     child = gtk_widget_get_next_sibling(child))
			found = find_row(child, id);
	}
#else
	if (GTK_IS_CONTAINER(root)) {
		g_autoptr(GList) children = gtk_container_get_children(GTK_CONTAINER(root));
		GList *l;

		for (l = children; l != NULL && found == NULL; l = l->next)
			found = find_row(l->data, id);
	}
#endif
	return found;
}

/* Click @view's sidebar row with @button through its click handler. */
static void
click_row(H *h, GsurfView *view, guint button)
{
	GtkWidget *row = NULL;
	guint i;

	for (i = 0; i < 200; i++) {
		row = find_row(h->toplevel, gsurf_view_get_id(view));
		if (row != NULL && gtk_widget_get_mapped(row) && width_of(row) > 0)
			break;
		gsurf_webkit_test_spin(25);
	}
	g_assert_nonnull(row);
#ifdef GSURF_BACKEND_GTK4
	{
		g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(row);
		/* The primary button acts on release (its press may start a
		 * drag); the others on press. */
		g_autofree gchar *name = g_strdup_printf("%s%u",
			button == 1 ? "gsurf-chrome-release-" : "gsurf-chrome-click-", button);
		GtkEventController *found = NULL;
		GtkWidget *title;
		graphene_rect_t bounds;

		for (i = 0; i < g_list_model_get_n_items(controllers) && found == NULL; i++) {
			g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

			if (g_strcmp0(gtk_event_controller_get_name(c), name) == 0)
				found = c;
		}
		g_assert_nonnull(found);
		for (title = gtk_widget_get_first_child(row); title != NULL;
		     title = gtk_widget_get_next_sibling(title)) {
			if (g_strcmp0(gtk_widget_get_name(title), "gsurf-tab-title") == 0)
				break;
		}
		g_assert_nonnull(title);
		for (i = 0; i < 200 && gtk_widget_get_width(title) == 0; i++)
			gsurf_webkit_test_spin(25);
		g_assert_true(gtk_widget_compute_bounds(title, row, &bounds));
		g_signal_emit_by_name(found, button == 1 ? "released" : "pressed", 1,
			(gdouble)(bounds.origin.x + bounds.size.width / 2),
			(gdouble)(bounds.origin.y + bounds.size.height / 2));
	}
#else
	{
		GdkEvent *event = gdk_event_new(button == 1 ? GDK_BUTTON_RELEASE : GDK_BUTTON_PRESS);
		gboolean handled = FALSE;

		event->button.button = button;
		g_signal_emit_by_name(row, button == 1 ? "button-release-event" : "button-press-event",
			event, &handled);
		gdk_event_free(event);
		g_assert_true(handled);
	}
#endif
}

/* Left plus right border of @view's frame, once laid out: the frame's
 * border box (rect_of(); GTK4's gtk_widget_get_width() is the content
 * width) minus the web view inside it. */
static gint
border_px(H *h, GsurfView *view)
{
	GsurfWmuxFrame *f = frame_of(h, view);

	g_assert_nonnull(f);
	WAIT_UNTIL(width_of(f->webview) > 1);
	settle();
	return rect_of(h, view).width - width_of(f->webview);
}

/* The GTK3 border colour of @widget (GTK4 has no public getter). */
#ifdef GSURF_BACKEND_GTK3
static gchar *
border_color_of(GtkWidget *widget)
{
	GtkStyleContext *context = gtk_widget_get_style_context(widget);
	GdkRGBA *rgba = NULL;
	gchar *out;

	gtk_style_context_get(context, gtk_style_context_get_state(context),
		"border-top-color", &rgba, NULL);
	out = gdk_rgba_to_string(rgba);
	gdk_rgba_free(rgba);
	return out;
}
#endif

static void
test_integration(void)
{
	static const gchar *const modules[] = { "tabs", "containers", NULL };
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	H h;
	GsurfView *a, *b, *work, *bg, *fresh;
	GsurfWmuxFrame *f;
	GsurfModule *tabs;
	GActionGroup *actions;
	g_autofree gchar *css = NULL;
	g_autofree gchar *work_uri = page("work");
	g_autofree gchar *bg_uri = page("bg");
	gchar *dim;
	gchar *bright;
	guint pane;

	if (!gsurf_webkit_test_init())
		return;
	g_assert_nonnull(gsurf_container_registry_define(registry, "work", "Work", "#f38ba8", NULL));
	if (!h_init(&h, "    border_width: 1\n    active_border_width: 4\n",
			"  tabs:\n    enabled: true\n    sidebar: true\n"
			"  containers:\n    enabled: true\n    new_tab: default\n", modules)) {
		gsurf_container_registry_undefine(registry, "work", NULL);
		return;
	}
	tabs = gsurf_module_manager_get_module(h.mgr, "tabs");
	a = h.first;

	/* 1. A tab in the work container, split beside the first. */
	cmd(&h, GDK_KEY_percent, SHIFT);
	b = active_view(&h);
	work = gsurf_view_new_full(NULL, "work", work_uri, FALSE);
	gsurf_window_add_view(h.window, work);
	g_object_unref(work);
	gsurf_window_set_active_view(h.window, work); /* shown in the active pane */
	gsurf_view_load_uri(work, work_uri);
	g_assert_true(active_pane_view(&h) == work);
	g_assert_false(gsurf_window_get_view_visible(h.window, b));
	g_assert_cmpuint(child_pane(frame_of(&h, b)->frame), ==, GSURF_WMUX_GRID_PARKED);
	check_invariants(&h);

	/* 2. Borders: classes, the stylesheet's colours, active wider. */
	f = frame_of(&h, work);
	g_assert_true(has_class(f->frame, GSURF_WMUX_PANE_CLASS));
	g_assert_true(has_class(f->frame, "wmux-ctr-work"));
	g_assert_true(has_class(f->frame, GSURF_WMUX_PANE_ACTIVE_CLASS));
	g_assert_false(has_class(frame_of(&h, a)->frame, "wmux-ctr-work"));
	css = gtk_css_provider_to_string(h.w->css);
	dim = with_alpha("#f38ba8", h.w->inactive_opacity);
	bright = brighten("#f38ba8", h.w->active_brighten);
	{
		g_autofree gchar *want_dim = g_strdup_printf("border-top-color: %s;", dim);
		g_autofree gchar *want_bright = g_strdup_printf("border-top-color: %s;", bright);

		g_assert_nonnull(strstr(css, "box.wmux-ctr-work.wmux-pane {"));
		g_assert_nonnull(strstr(css, want_dim));
		g_assert_nonnull(strstr(css, want_bright));
	}
	g_assert_nonnull(strstr(css, "4px"));
	settle();
	/* Active border 4px each side, inactive 1px. */
	g_assert_cmpint(border_px(&h, work), ==, 8);
	g_assert_cmpint(border_px(&h, a), ==, 2);
#ifdef GSURF_BACKEND_GTK3
	{
		g_autofree gchar *got = border_color_of(f->frame);
		g_autofree gchar *got_a = border_color_of(frame_of(&h, a)->frame);
		g_autofree gchar *want_a = with_alpha("#45475a", 0.55);

		g_assert_cmpstr(got, ==, bright);
		g_assert_cmpstr(got_a, ==, want_a);
	}
#endif
	/* The other pane active: work's border dims to the container's. */
	gsurf_window_set_active_view(h.window, a);
	g_assert_false(has_class(f->frame, GSURF_WMUX_PANE_ACTIVE_CLASS));
	settle();
	g_assert_cmpint(border_px(&h, work), ==, 2);
#ifdef GSURF_BACKEND_GTK3
	{
		g_autofree gchar *got = border_color_of(f->frame);

		g_assert_cmpstr(got, ==, dim);
	}
#endif
	g_free(dim);
	g_free(bright);
	/* A colour change follows live. */
	g_assert_nonnull(gsurf_container_registry_define(registry, "work", "Work", "#a6e3a1", NULL));
	g_clear_pointer(&css, g_free);
	css = gtk_css_provider_to_string(h.w->css);
	dim = with_alpha("#a6e3a1", h.w->inactive_opacity);
	g_assert_nonnull(strstr(css, dim));
	g_free(dim);

	/* 3. A sidebar click on a background tab shows it in the active pane;
	 *    the tab shown there before becomes a background tab. */
	bg = gsurf_view_new_full(NULL, NULL, bg_uri, FALSE);
	gsurf_window_add_view(h.window, bg);
	g_object_unref(bg);
	gsurf_view_load_uri(bg, bg_uri);
	g_assert_false(gsurf_window_get_view_visible(h.window, bg));
	click_row(&h, bg, 1);
	WAIT_UNTIL(active_view(&h) == bg);
	g_assert_true(active_pane_view(&h) == bg);
	g_assert_false(gsurf_window_get_view_visible(h.window, a));
	g_assert_true(gsurf_window_get_view_visible(h.window, work));
	check_invariants(&h);
	/* A sidebar click on a tab shown in a pane focuses that pane. */
	click_row(&h, work, 1);
	WAIT_UNTIL(active_view(&h) == work);
	g_assert_cmpuint(n_panes(&h), ==, 2);

	/* 4. Open in Split Right (the tab menu) for a background tab: a new
	 *    pane split off the active one shows it. */
	click_row(&h, a, 3);
	actions = g_object_get_data(G_OBJECT(tabs), "gsurf-tabs-menu-actions");
	g_assert_nonnull(actions);
	g_assert_true(g_action_group_get_action_enabled(actions, "split-right"));
	g_action_group_activate_action(actions, "split-right", NULL);
	g_assert_cmpuint(n_panes(&h), ==, 3);
	g_assert_true(active_view(&h) == a);
	g_assert_true(gsurf_window_get_view_visible(h.window, a));
	check_invariants(&h);
	/* ... and Open in Split Below for a visible tab: its pane splits with
	 *     a new tab. */
	click_row(&h, work, 3);
	g_action_group_activate_action(actions, "split-down", NULL);
	g_assert_cmpuint(n_panes(&h), ==, 4);
	fresh = active_view(&h);
	g_assert_true(fresh != work && fresh != a && fresh != bg && fresh != b);
	pane = gsurf_wmux_layout_find_payload(cur(&h)->layout, gsurf_view_get_id(work));
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(cur(&h)->layout, pane, GSURF_WMUX_DIR_DOWN, FALSE),
		==, gsurf_wmux_layout_find_payload(cur(&h)->layout, gsurf_view_get_id(fresh)));
	check_invariants(&h);
	/* The same through the action dispatch (core keybinds). */
	g_assert_true(gsurf_module_manager_dispatch_action(h.mgr, b, GSURF_ACTION_PANE_SPLIT_DOWN));
	g_assert_true(active_view(&h) == b);
	g_assert_cmpuint(n_panes(&h), ==, 5);
	check_invariants(&h);

	/* 5. Reopen in another container keeps the pane. */
	pane = gsurf_wmux_layout_find_payload(cur(&h)->layout, gsurf_view_get_id(a));
	gsurf_window_set_active_view(h.window, a);
	fresh = gsurf_window_reopen_view(h.window, a, "work");
	g_assert_nonnull(fresh);
	g_assert_cmpuint(gsurf_wmux_layout_get_payload(cur(&h)->layout, pane), ==,
		gsurf_view_get_id(fresh));
	g_assert_true(active_view(&h) == fresh);
	g_assert_cmpuint(n_panes(&h), ==, 5);
	settle();
	g_assert_true(has_class(frame_of(&h, fresh)->frame, "wmux-ctr-work"));
	check_invariants(&h);

	h_free(&h);
	gsurf_container_registry_undefine(registry, "work", NULL);
}

/* ------------------------------------------------------------------ */
/* Lifetime, serialize/restore, reconfigure                             */
/* ------------------------------------------------------------------ */

static gint
map_index(GsurfView *view, gpointer user_data)
{
	return gsurf_window_get_view_index(GSURF_WINDOW(user_data), view);
}

/* Only the first and the fourth tab are saved. */
static gint
map_ends(GsurfView *view, gpointer user_data)
{
	gint index = gsurf_window_get_view_index(GSURF_WINDOW(user_data), view);

	return index == 0 || index == 3 ? index : -1;
}

static gint
map_none(GsurfView *view, gpointer user_data)
{
	(void)view;
	(void)user_data;
	return -1;
}

static void
test_lifetime(void)
{
	H h;
	GsurfView *v[4];
	gpointer weak_view[4];
	gpointer weak_root, weak_grid, weak_frame;
	g_autofree gchar *state = NULL;
	g_autofree gchar *again = NULL;
	g_autoptr(GsurfConfig) other = NULL;
	GsurfWindowLayout *layout;
	GtkWidget *stack;
	guint i;

	if (!gsurf_webkit_test_init() || !h_init(&h, NULL, NULL, NULL))
		return;
	layout = GSURF_WINDOW_LAYOUT(h.module);

	/* 1. Three panes in window 0, one in window 1 (renamed), zoom in 1. */
	v[0] = h.first;
	cmd(&h, GDK_KEY_percent, SHIFT);
	v[1] = active_view(&h);
	cmd(&h, GDK_KEY_quotedbl, SHIFT);
	v[2] = active_view(&h);
	cmd(&h, GDK_KEY_c, 0);
	v[3] = active_view(&h);
	gsurf_window_set_active_view(h.window, v[1]);
	g_assert_cmpuint(h.w->current, ==, 0);
	for (i = 0; i < 4; i++) {
		weak_view[i] = v[i];
		g_object_add_weak_pointer(G_OBJECT(v[i]), &weak_view[i]);
	}

	/* 2. serialize -> restore on the layout object, through the window
	 *    layout wrappers the session module uses. */
	state = gsurf_window_layout_serialize(layout, h.window, map_index, h.window);
	g_assert_nonnull(state);
	g_assert_true(g_str_has_prefix(state, "wmux1 current=0 last=1 "));
	g_assert_nonnull(strstr(state, "layout=h(50:p0=0,50:v(50:p1=1,50:p2=2))"));
	g_assert_nonnull(strstr(state, "layout=p0=3]"));
	/* Rearrange, then restore: back to the saved arrangement. */
	cmd(&h, GDK_KEY_2, ALT);
	cmd(&h, GDK_KEY_exclam, SHIFT);
	g_assert_cmpuint(h.w->wins->len, ==, 3);
	g_assert_true(gsurf_window_layout_restore(layout, h.window, state, v, 4));
	g_assert_cmpuint(h.w->wins->len, ==, 2);
	g_assert_cmpuint(h.w->current, ==, 0);
	g_assert_true(active_view(&h) == v[1]);
	again = gsurf_window_layout_serialize(layout, h.window, map_index, h.window);
	g_assert_cmpstr(again, ==, state);
	check_invariants(&h);
	/* Mismatches are refused and change nothing. */
	g_assert_false(gsurf_window_layout_restore(layout, h.window, state, v, 3));
	{
		GsurfView *dup[4] = { v[0], v[0], v[2], v[3] };
		GsurfView *gap[4] = { v[0], NULL, v[2], v[3] };

		g_assert_false(gsurf_window_layout_restore(layout, h.window, state, dup, 4));
		g_assert_false(gsurf_window_layout_restore(layout, h.window, state, gap, 4));
	}
	g_assert_false(gsurf_window_layout_restore(layout, h.window, "wmux1 [", v, 4));
	g_assert_false(gsurf_window_layout_restore(layout, h.window, "garbage", v, 4));
	g_clear_pointer(&again, g_free);
	again = gsurf_window_layout_serialize(layout, h.window, map_index, h.window);
	g_assert_cmpstr(again, ==, state);
	/* Unsaved views are left out (-1); none saved: NULL. */
	g_clear_pointer(&again, g_free);
	again = gsurf_window_layout_serialize(layout, h.window, map_ends, h.window);
	g_assert_nonnull(strstr(again, "[name=\"\" renamed=0 active=0 layout=p0=0]"));
	g_assert_nonnull(strstr(again, "layout=p0=3]"));
	g_clear_pointer(&again, g_free);
	g_assert_null(gsurf_window_layout_serialize(layout, h.window, map_none, NULL));
	check_invariants(&h);

	/* 3. Deactivate: the plain stack again, every view alive, our widgets
	 *    gone. */
	weak_root = h.w->root;
	weak_grid = h.w->grid;
	weak_frame = frame_of(&h, v[0])->frame;
	g_object_add_weak_pointer(G_OBJECT(weak_root), &weak_root);
	g_object_add_weak_pointer(G_OBJECT(weak_grid), &weak_grid);
	g_object_add_weak_pointer(G_OBJECT(weak_frame), &weak_frame);
	gsurf_module_deactivate(h.module);
	g_assert_null(gsurf_window_get_layout(h.window));
	g_assert_false(gsurf_window_get_key_grab(h.window));
	gsurf_webkit_test_spin(50);
	g_assert_null(weak_root);
	g_assert_null(weak_grid);
	g_assert_null(weak_frame);
	stack = gtk_widget_get_parent(gsurf_view_get_native_widget(v[0]));
	g_assert_true(GTK_IS_STACK(stack));
	for (i = 0; i < 4; i++) {
		g_assert_nonnull(weak_view[i]);
		g_assert_true(gtk_widget_get_parent(gsurf_view_get_native_widget(v[i])) == stack);
	}
	g_assert_true(gtk_stack_get_visible_child(GTK_STACK(stack)) ==
		gsurf_view_get_native_widget(v[1]));
	gsurf_module_deactivate(h.module); /* twice is fine */
	press(&h, GDK_KEY_b, CTRL); /* keys no longer ours: no grab */
	g_assert_false(gsurf_window_get_key_grab(h.window));

	/* 4. Reactivate: one pane with the active view, the rest background. */
	g_assert_true(gsurf_module_activate(h.module));
	g_assert_true(gsurf_window_get_layout(h.window) == layout);
	g_assert_cmpuint(h.w->wins->len, ==, 1);
	g_assert_cmpuint(n_panes(&h), ==, 1);
	g_assert_true(active_pane_view(&h) == v[1]);
	check_invariants(&h);
	/* A second activate() (the class vfunc: the manager wrapper skips an
	 * active module) replaces the state. */
	cmd(&h, GDK_KEY_percent, SHIFT);
	g_assert_true(GSURF_MODULE_GET_CLASS(h.module)->activate(h.module));
	g_assert_cmpuint(n_panes(&h), ==, 1);
	check_invariants(&h);

	/* 5. Reconfigure (prefix C-a, status on top, no status) and
	 *    activate again: the new settings apply. */
	other = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(other,
		"modules:\n  wmux:\n    enabled: true\n    prefix: C-a\n"
		"    status_position: top\n    remember_layout: false\n", -1, NULL));
	gsurf_module_configure(h.module, other);
	g_assert_true(GSURF_MODULE_GET_CLASS(h.module)->activate(h.module));
	g_assert_true(gtk_widget_get_parent(h.w->status_box) == h.w->root);
	press(&h, GDK_KEY_b, CTRL);
	g_assert_false(gsurf_window_get_key_grab(h.window));
	g_assert_true(press(&h, GDK_KEY_a, CTRL));
	g_assert_true(press(&h, GDK_KEY_percent, SHIFT));
	g_assert_cmpuint(n_panes(&h), ==, 2);
	g_assert_null(gsurf_window_layout_serialize(layout, h.window, map_index, h.window));
	check_invariants(&h);

	/* 6. The window goes first: the layout is removed with it, the views
	 *    die, the module stays usable. */
	g_signal_handlers_disconnect_by_data(h.window, &h);
	gtk_widget_set_visible(h.toplevel, FALSE);
	g_object_add_weak_pointer(G_OBJECT(h.window), (gpointer *)&h.window);
	gsurf_module_manager_set_application(h.mgr, NULL);
	g_object_unref(h.window); /* ours; the application holds the other */
	g_clear_object(&h.app);
	for (i = 0; i < 100 && h.window != NULL; i++)
		gsurf_webkit_test_spin(20);
	g_assert_null(h.window);
	for (i = 0; i < 100 && weak_view[0] != NULL; i++)
		gsurf_webkit_test_spin(20);
	for (i = 0; i < 4; i++)
		g_assert_null(weak_view[i]);
	g_assert_null(h.w->window);
	g_assert_false(h.w->installed);
	gsurf_module_deactivate(h.module);
	gsurf_module_deactivate(h.module);
	h.window = NULL;
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* Session round trip (session + tabs + wmux)                          */
/* ------------------------------------------------------------------ */

static void
test_session_round_trip(void)
{
	static const gchar *const modules[] = { "tabs", "session", NULL };
	g_autofree gchar *dir = g_dir_make_tmp("gsurf-wmux-session-XXXXXX", NULL);
	g_autofree gchar *file = g_build_filename(dir, "session.yaml", NULL);
	g_autofree gchar *extra = NULL;
	g_autofree gchar *saved = NULL;
	g_autofree gchar *restored = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *uris_before = NULL;
	GString *uris;
	GsurfModule *session;
	H h;
	guint i;

	if (!gsurf_webkit_test_init())
		return;

	/* 1. First run: three panes (one of them the duplicate of another
	 *    page), a second window, zoom; save. */
	extra = g_strdup_printf("  tabs:\n    enabled: true\n"
		"  session:\n    enabled: true\n    auto_restore: false\n    file: '%s'\n", file);
	if (!h_init(&h, NULL, extra, modules)) {
		gsurf_test_rmtree(dir);
		return;
	}
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(h.first), "first") == 0);
	cmd(&h, GDK_KEY_percent, SHIFT);
	cmd(&h, GDK_KEY_quotedbl, SHIFT);
	cmd(&h, GDK_KEY_z, 0);
	cmd(&h, GDK_KEY_c, 0);
	cmd(&h, GDK_KEY_comma, 0);
	WAIT_UNTIL(prompt_focused(&h));
	prompt_set_text(&h, "second");
	prompt_key(&h, GDK_KEY_Return);
	cmd(&h, GDK_KEY_l, 0);
	for (i = 0; i < gsurf_window_get_n_views(h.window); i++)
		WAIT_UNTIL(gsurf_view_get_uri(gsurf_window_get_nth_view(h.window, i)) != NULL &&
		           g_str_has_prefix(gsurf_view_get_uri(gsurf_window_get_nth_view(h.window, i)),
		                            "http://"));
	saved = gsurf_window_layout_serialize(GSURF_WINDOW_LAYOUT(h.module), h.window,
		map_index, h.window);
	uris = g_string_new(NULL);
	for (i = 0; i < gsurf_window_get_n_views(h.window); i++)
		g_string_append_printf(uris, "%s ",
			gsurf_view_get_uri(gsurf_window_get_nth_view(h.window, i)));
	uris_before = g_string_free(uris, FALSE);
	session = gsurf_module_manager_get_module(h.mgr, "session");
	g_assert_true(gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(session), NULL,
		'S', 0, GSURF_MOD_CTRL | GSURF_MOD_SHIFT, GSURF_MODE_NORMAL));
	g_assert_true(g_file_get_contents(file, &text, NULL, NULL));
	g_assert_nonnull(strstr(text, "layout:"));
	g_assert_nonnull(strstr(text, "wmux1"));
	h_free(&h);
	g_clear_pointer(&extra, g_free);

	/* 2. Second run: a fresh application and window with a startup view;
	 *    the session restores the tabs, then hands wmux the layout. */
	extra = g_strdup_printf("  tabs:\n    enabled: true\n"
		"  session:\n    enabled: true\n    auto_restore: true\n    file: '%s'\n", file);
	if (!h_init(&h, NULL, extra, modules)) {
		gsurf_test_rmtree(dir);
		return;
	}
	g_assert_cmpuint(gsurf_window_get_n_views(h.window), ==, 4);
	uris = g_string_new(NULL);
	for (i = 0; i < gsurf_window_get_n_views(h.window); i++) {
		GsurfView *view = gsurf_window_get_nth_view(h.window, i);
		const gchar *uri = gsurf_view_get_uri(view);

		g_string_append_printf(uris, "%s ", uri != NULL ? uri : gsurf_view_get_initial_uri(view));
	}
	text = g_string_free(uris, FALSE);
	g_assert_cmpstr(text, ==, uris_before);
	restored = gsurf_window_layout_serialize(GSURF_WINDOW_LAYOUT(h.module), h.window,
		map_index, h.window);
	g_assert_cmpstr(restored, ==, saved);
	g_assert_cmpuint(h.w->wins->len, ==, 2);
	g_assert_cmpuint(h.w->current, ==, 0);
	g_assert_cmpuint(n_panes(&h), ==, 3);
	g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(cur(&h)->layout), !=, GSURF_WMUX_PANE_NONE);
	settle();
	check_invariants(&h);
	h_free(&h);
	gsurf_test_rmtree(dir);
}

/* ------------------------------------------------------------------ */
/* Randomized stress                                                   */
/* ------------------------------------------------------------------ */

static void
test_stress(void)
{
	const gchar *seed_env = g_getenv("GSURF_WMUX_SEED");
	guint32 seed = seed_env != NULL ? (guint32)g_ascii_strtoull(seed_env, NULL, 10) : 4242u;
	g_autoptr(GRand) rand = g_rand_new_with_seed(seed);
	H h;
	guint step;

	if (!gsurf_webkit_test_init() ||
	    !h_init(&h, "    confirm_kill: false\n    min_pane_pct: 5\n", NULL, NULL))
		return;
	/* Every new tab is about:blank: no network, no titles needed. */
	g_free(h.config->new_view_uri);
	h.config->new_view_uri = g_strdup("about:blank");
	g_test_message("wmux stress seed %u (GSURF_WMUX_SEED replays it)", seed);

	for (step = 0; step < 200; step++) {
		guint n_views = gsurf_window_get_n_views(h.window);
		gint op = g_rand_int_range(rand, 0, n_views > 10 ? 15 : 13);

		switch (op) {
		case 0: cmd(&h, GDK_KEY_percent, SHIFT); break;
		case 1: cmd(&h, GDK_KEY_quotedbl, SHIFT); break;
		case 2: cmd(&h, GDK_KEY_x, 0); break;
		case 3: cmd(&h, GDK_KEY_braceright, SHIFT); break;
		case 4: cmd(&h, GDK_KEY_z, 0); break;
		case 5: cmd(&h, GDK_KEY_c, 0); break;
		case 6: cmd(&h, GDK_KEY_n, 0); break;
		case 7: cmd(&h, GDK_KEY_space, 0); break;
		case 8: press(&h, (guint[]){ GDK_KEY_h, GDK_KEY_j, GDK_KEY_k, GDK_KEY_l }
				[g_rand_int_range(rand, 0, 4)], CTRL);
			break;
		case 9: cmd(&h, GDK_KEY_exclam, SHIFT); break;
		case 10: cmd(&h, GDK_KEY_o, CTRL); break;
		case 11: {
			/* A random tab selected (sidebar click, Ctrl+Tab). */
			GsurfView *view = gsurf_window_get_nth_view(h.window,
				(guint)g_rand_int_range(rand, 0, (gint)n_views));

			gsurf_window_set_active_view(h.window, view);
			break;
		}
		case 12: cmd(&h, GDK_KEY_Right, CTRL); break;
		default: {
			/* A random tab closed (Ctrl+w on it, its close button). */
			GsurfView *view = gsurf_window_get_nth_view(h.window,
				(guint)g_rand_int_range(rand, 0, (gint)n_views));

			if (n_views > 1)
				gsurf_window_remove_view(h.window, view);
			break;
		}
		}
		g_assert_false(gsurf_window_get_key_grab(h.window));
		if (step % 10 == 0)
			gsurf_webkit_test_spin(10);
		check_invariants(&h);
	}
	h_free(&h);
}

/* ------------------------------------------------------------------ */
/* Sidebar: grouped tree, drag and drop, opened-from-tab children      */
/* ------------------------------------------------------------------ */

/* Depth-first search for a widget by name under @root. */
static GtkWidget *
find_named(GtkWidget *root, const gchar *name)
{
	GtkWidget *found = NULL;

	if (g_strcmp0(gtk_widget_get_name(root), name) == 0)
		return root;
#ifdef GSURF_BACKEND_GTK4
	{
		GtkWidget *child;

		for (child = gtk_widget_get_first_child(root); child != NULL && found == NULL;
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

/* The sidebar's visible rows as text: a heading "[0:name*]", a tab
 * "title@depth" (its indentation over the default 14 px per level). */
static gchar *
sidebar_text(H *h)
{
	GtkWidget *list = find_named(h->toplevel, "gsurf-tab-sidebar-list");
	g_autoptr(GPtrArray) rows = NULL;
	GString *out = g_string_new(NULL);
	guint i;

	g_assert_nonnull(list);
	rows = g_ptr_array_new();
#ifdef GSURF_BACKEND_GTK4
	{
		GtkWidget *child;

		for (child = gtk_widget_get_first_child(list); child != NULL;
		     child = gtk_widget_get_next_sibling(child))
			g_ptr_array_add(rows, child);
	}
#else
	{
		g_autoptr(GList) children = gtk_container_get_children(GTK_CONTAINER(list));
		GList *l;

		for (l = children; l != NULL; l = l->next)
			g_ptr_array_add(rows, l->data);
	}
#endif
	for (i = 0; i < rows->len; i++) {
		GtkWidget *row = g_ptr_array_index(rows, i);
		GtkWidget *title = find_named(row, "gsurf-tab-title");
		GtkWidget *box = find_named(row, "gsurf-tab-row-box");

		if (!gtk_widget_get_visible(row))
			continue;
		if (out->len > 0)
			g_string_append_c(out, ' ');
		if (has_class(row, "gsurf-tab-group"))
			g_string_append_printf(out, "[%s]", gtk_label_get_text(GTK_LABEL(title)));
		else
			g_string_append_printf(out, "%s@%d", gtk_label_get_text(GTK_LABEL(title)),
				gtk_widget_get_margin_start(box) / 14);
	}
	return g_string_free(out, FALSE);
}

/* Wait (bounded) until the sidebar reads @expected. */
#define ASSERT_SIDEBAR(h, expected) G_STMT_START { \
	gchar *got_ = NULL; \
	guint try_; \
	for (try_ = 0; try_ < 200; try_++) { \
		g_free(got_); \
		got_ = sidebar_text(h); \
		if (g_strcmp0(got_, (expected)) == 0) \
			break; \
		gsurf_webkit_test_spin(25); \
	} \
	g_assert_cmpstr(got_, ==, (expected)); \
	g_free(got_); \
} G_STMT_END

/* The heading row of group @index. */
static GtkWidget *
group_row(H *h, guint index)
{
	GtkWidget *row = find_row(h->toplevel, G_GUINT64_CONSTANT(0x8000000000000000) | index);

	g_assert_nonnull(row);
	return row;
}

/* A primary click (released) on a sidebar row widget. */
static void
click_row_widget(GtkWidget *row, guint button)
{
#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(row);
	g_autofree gchar *name = g_strdup_printf("gsurf-chrome-release-%u", button);
	GtkEventController *found = NULL;
	GtkWidget *title = find_named(row, "gsurf-tab-title");
	graphene_rect_t bounds;
	guint i;

	for (i = 0; i < g_list_model_get_n_items(controllers) && found == NULL; i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (g_strcmp0(gtk_event_controller_get_name(c), name) == 0)
			found = c;
	}
	g_assert_nonnull(found);
	WAIT_UNTIL(gtk_widget_get_width(title) > 0);
	g_assert_true(gtk_widget_compute_bounds(title, row, &bounds));
	g_signal_emit_by_name(found, "released", 1,
		(gdouble)(bounds.origin.x + bounds.size.width / 2),
		(gdouble)(bounds.origin.y + bounds.size.height / 2));
#else
	GdkEvent *event = gdk_event_new(GDK_BUTTON_RELEASE);
	gboolean handled = FALSE;

	event->button.button = button;
	g_signal_emit_by_name(row, "button-release-event", event, &handled);
	gdk_event_free(event);
	g_assert_true(handled);
#endif
}

/* A named controller of @widget (GTK4), still owned by it. */
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

/* Start or end a drag of @view's sidebar row through its drag source
 * handlers (GTK3: the row's drag-begin / drag-end; GTK4: its
 * GtkDragSource's). */
static void
row_drag(H *h, GsurfView *view, gboolean begin)
{
	GtkWidget *row = find_row(h->toplevel, gsurf_view_get_id(view));

	g_assert_nonnull(row);
#ifdef GSURF_BACKEND_GTK4
	if (begin)
		g_signal_emit_by_name(controller_named(row, "gsurf-chrome-drag-source"),
			"drag-begin", NULL);
	else
		g_signal_emit_by_name(controller_named(row, "gsurf-chrome-drag-source"),
			"drag-end", NULL, FALSE);
#else
	g_signal_emit_by_name(row, begin ? "drag-begin" : "drag-end", NULL);
#endif
}

/* A drag over @widget at (@x, @y), then (when @drop) a drop there,
 * through its drop target handlers. Returns what the handler answered
 * (accepted / taken). */
static gboolean
drag_over(GtkWidget *widget, gint x, gint y, gboolean drop)
{
#ifdef GSURF_BACKEND_GTK4
	GtkEventController *target = controller_named(widget, "gsurf-chrome-drop-target");
	GdkDragAction action = 0;
	gboolean taken = FALSE;

	g_signal_emit_by_name(target, "drag-motion", NULL, (gdouble)x, (gdouble)y, &action);
	if (!drop)
		return action != 0;
	g_signal_emit_by_name(target, "drop", NULL, (gdouble)x, (gdouble)y, &taken);
	return taken;
#else
	gboolean ok = FALSE;

	g_signal_emit_by_name(widget, "drag-motion", NULL, x, y, 0u, &ok);
	if (!drop)
		return ok;
	g_signal_emit_by_name(widget, "drag-drop", NULL, x, y, 0u, &ok);
	return ok;
#endif
}

/* Where to drop on a pane: its centre or 5 px inside an edge. */
typedef enum { AT_CENTER, AT_LEFT, AT_RIGHT, AT_TOP, AT_BOTTOM } At;

/* Drag @view's row and drop it on @target's pane at @at. */
static gboolean
drop_on_pane(H *h, GsurfView *view, GsurfView *target, At at)
{
	GsurfWmuxFrame *f = frame_of(h, target);
	gint w;
	gint hh;
	gint x;
	gint y;
	gboolean done;

	g_assert_nonnull(f);
	row_drag(h, view, TRUE);
	g_assert_cmpuint(h->w->drag_view, ==, gsurf_view_get_id(view));
	WAIT_UNTIL(gtk_widget_get_visible(f->drop) && width_of(f->drop) > 20);
	w = width_of(f->drop);
	hh = gsurf_chrome_widget_get_height(f->drop);
	x = at == AT_LEFT ? 5 : at == AT_RIGHT ? w - 5 : w / 2;
	y = at == AT_TOP ? 5 : at == AT_BOTTOM ? hh - 5 : hh / 2;
	/* Hovering highlights the zone the drop would use. */
	g_assert_true(drag_over(f->drop, x, y, FALSE));
	g_assert_true(gtk_widget_get_visible(f->zone));
	g_assert_cmpint(f->zone_shown, ==, (gint)at);
	done = drag_over(f->drop, x, y, TRUE);
	row_drag(h, view, FALSE);
	g_assert_cmpuint(h->w->drag_view, ==, 0);
	return done;
}

/* Every pane's drop target is hidden (no drag in progress). */
static void
assert_no_drop_targets(H *h)
{
	GHashTableIter iter;
	gpointer value;

	g_hash_table_iter_init(&iter, h->w->frames);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_assert_false(gtk_widget_get_visible(((GsurfWmuxFrame *)value)->drop));
}

/* A background tab of the window opened from @opener (or none). */
static GsurfView *
background_tab(H *h, GsurfView *opener, const gchar *name)
{
	g_autofree gchar *uri = page(name);
	GsurfView *view = gsurf_view_new_full(opener, NULL, uri, FALSE);

	gsurf_window_add_view(h->window, view);
	g_object_unref(view);
	gsurf_view_load_uri(view, uri);
	return view;
}

/* Feature: a tab dragged from the sidebar onto a pane. */
static void
test_drag_into_pane(void)
{
	static const gchar *const modules[] = { "tabs", NULL };
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	H h;
	GsurfView *a, *b, *c, *work;
	g_autofree gchar *work_uri = page("work");
	gpointer weak_c;
	guint pane;
	guint n;

	if (!gsurf_webkit_test_init())
		return;
	g_assert_nonnull(gsurf_container_registry_define(registry, "work", "Work", "#f38ba8", NULL));
	if (!h_init(&h, NULL, "  tabs:\n    enabled: true\n    sidebar: true\n", modules)) {
		gsurf_container_registry_undefine(registry, "work", NULL);
		return;
	}
	a = h.first;
	cmd(&h, GDK_KEY_percent, SHIFT);
	b = active_view(&h);
	c = background_tab(&h, NULL, "c");
	weak_c = c;
	g_object_add_weak_pointer(G_OBJECT(c), &weak_c);
	settle();
	assert_no_drop_targets(&h);

	/* 1. The drag shows a drop target over each pane on screen only. */
	row_drag(&h, c, TRUE);
	WAIT_UNTIL(gtk_widget_get_visible(frame_of(&h, a)->drop));
	g_assert_true(gtk_widget_get_visible(frame_of(&h, b)->drop));
	g_assert_false(gtk_widget_get_visible(frame_of(&h, c)->drop));
	/* Hovering the dragged tab over its own row is refused; the sidebar
	 * is no target for itself. */
	g_assert_false(drag_over(find_row(h.toplevel, gsurf_view_get_id(c)), 1, 1, FALSE));
	row_drag(&h, c, FALSE);
	assert_no_drop_targets(&h);

	/* 2. A background tab on a pane's centre: shown there, active; the
	 *    tab the pane showed becomes a background tab. */
	g_assert_true(drop_on_pane(&h, c, a, AT_CENTER));
	g_assert_true(active_view(&h) == c);
	g_assert_true(gsurf_window_get_view_visible(h.window, c));
	g_assert_false(gsurf_window_get_view_visible(h.window, a));
	g_assert_cmpuint(n_panes(&h), ==, 2);
	assert_no_drop_targets(&h);
	check_invariants(&h);

	/* 3. A tab shown in another pane on a pane's centre: they swap. */
	pane = gsurf_wmux_layout_find_payload(cur(&h)->layout, gsurf_view_get_id(c));
	g_assert_true(drop_on_pane(&h, c, b, AT_CENTER));
	g_assert_cmpuint(gsurf_wmux_layout_get_payload(cur(&h)->layout, pane), ==,
		gsurf_view_get_id(b));
	g_assert_true(gsurf_window_get_view_visible(h.window, b));
	g_assert_true(active_view(&h) == c);
	g_assert_cmpuint(n_panes(&h), ==, 2);
	check_invariants(&h);

	/* 4. A background tab on an edge: that pane splits on that side, the
	 *    new pane shows it. */
	g_assert_true(drop_on_pane(&h, a, c, AT_RIGHT));
	g_assert_cmpuint(n_panes(&h), ==, 3);
	g_assert_true(active_view(&h) == a);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(cur(&h)->layout,
		gsurf_wmux_layout_find_payload(cur(&h)->layout, gsurf_view_get_id(c)),
		GSURF_WMUX_DIR_RIGHT, FALSE), ==,
		gsurf_wmux_layout_find_payload(cur(&h)->layout, gsurf_view_get_id(a)));
	check_invariants(&h);

	/* 5. A shown tab on another pane's edge: it moves (join-pane), its
	 *    old pane closes, it lands above the target. */
	g_assert_true(drop_on_pane(&h, a, b, AT_TOP));
	g_assert_cmpuint(n_panes(&h), ==, 3);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(cur(&h)->layout,
		gsurf_wmux_layout_find_payload(cur(&h)->layout, gsurf_view_get_id(a)),
		GSURF_WMUX_DIR_DOWN, FALSE), ==,
		gsurf_wmux_layout_find_payload(cur(&h)->layout, gsurf_view_get_id(b)));
	check_invariants(&h);
	g_assert_true(drop_on_pane(&h, c, a, AT_LEFT));
	g_assert_true(drop_on_pane(&h, b, c, AT_BOTTOM));
	check_invariants(&h);

	/* 6. A pane's own tab: the centre selects its pane, an edge does
	 *    nothing. */
	n = n_panes(&h);
	gsurf_window_set_active_view(h.window, a);
	g_assert_true(drop_on_pane(&h, b, b, AT_CENTER));
	g_assert_true(active_view(&h) == b);
	/* (GTK3's drag-drop always claims the drop; the layout's answer is
	 *  only visible on GTK4, the arrangement on both.) */
#ifdef GSURF_BACKEND_GTK4
	g_assert_false(drop_on_pane(&h, b, b, AT_LEFT));
#else
	drop_on_pane(&h, b, b, AT_LEFT);
#endif
	g_assert_cmpuint(n_panes(&h), ==, n);
	g_assert_false(gsurf_window_layout_show_view_at(GSURF_WINDOW_LAYOUT(h.module), h.window,
		b, b, GSURF_LAYOUT_DROP_RIGHT));

	/* 7. Through the core seam directly: a target off screen refuses. */
	cmd(&h, GDK_KEY_c, 0);
	g_assert_false(gsurf_window_layout_show_view_at(GSURF_WINDOW_LAYOUT(h.module), h.window,
		active_view(&h), a, GSURF_LAYOUT_DROP_CENTER));
	cmd(&h, GDK_KEY_p, 0);

	/* 8. The highlight wears the dragged tab's container colour. */
	work = gsurf_view_new_full(NULL, "work", work_uri, FALSE);
	gsurf_window_add_view(h.window, work);
	g_object_unref(work);
	row_drag(&h, work, TRUE);
	WAIT_UNTIL(gtk_widget_get_visible(frame_of(&h, a)->drop));
	g_assert_true(has_class(frame_of(&h, a)->zone, "wmux-ctr-work"));
	{
		g_autofree gchar *css = gtk_css_provider_to_string(h.w->css);

		g_assert_nonnull(strstr(css, "box.wmux-ctr-work.wmux-drop-zone"));
	}
	row_drag(&h, work, FALSE);
	g_assert_false(has_class(frame_of(&h, a)->zone, "wmux-ctr-work") &&
		gtk_widget_get_visible(frame_of(&h, a)->drop));
	assert_no_drop_targets(&h);
	check_invariants(&h);

	h_free(&h);
	gsurf_container_registry_undefine(registry, "work", NULL);
	WAIT_UNTIL(weak_c == NULL);
}

/* Feature: the sidebar grouped by wmux window. */
static void
test_grouped_tree(void)
{
	static const gchar *const modules[] = { "tabs", NULL };
	H h;
	GsurfView *a, *b, *child, *grand, *d, *m;
	GsurfModule *tabs;

	/* select_background: active_pane, so a selected background tab takes
	 * the active pane (test_solo covers the default). */
	if (!gsurf_webkit_test_init() ||
	    !h_init(&h, "    select_background: active_pane\n",
			"  tabs:\n    enabled: true\n    sidebar: true\n"
			"    background_label: Hidden\n", modules))
		return;
	tabs = gsurf_module_manager_get_module(h.mgr, "tabs");
	a = h.first;
	ASSERT_SIDEBAR(&h, "[0:first*] first@1");

	/* 1. A split: the window's panes in pane order; the window name
	 *    follows its active pane. */
	cmd(&h, GDK_KEY_percent, SHIFT);
	b = active_view(&h);
	ASSERT_SIDEBAR(&h, "[0:new*] first@1 new@1");

	/* 2. Background tabs opened from a pane tab nest under it; one that
	 *    descends from no pane tab goes to the background group. */
	child = background_tab(&h, a, "child");
	grand = background_tab(&h, child, "grand");
	d = background_tab(&h, NULL, "d");
	ASSERT_SIDEBAR(&h, "[0:new*] first@1 child@2 grand@3 new@1 [Hidden] d@1");

	/* 3. A new window: a second heading, current. Renaming it. */
	cmd(&h, GDK_KEY_c, 0);
	m = active_view(&h);
	ASSERT_SIDEBAR(&h, "[0:new] first@1 child@2 grand@3 new@1 [1:new*] new@1 [Hidden] d@1");
	cmd(&h, GDK_KEY_comma, 0);
	prompt_set_text(&h, "mail");
	prompt_key(&h, GDK_KEY_Return);
	ASSERT_SIDEBAR(&h, "[0:new] first@1 child@2 grand@3 new@1 [1:mail*] new@1 [Hidden] d@1");

	/* 4. A click on a heading selects that window. */
	click_row_widget(group_row(&h, 0), 1);
	WAIT_UNTIL(h.w->current == 0);
	ASSERT_SIDEBAR(&h, "[0:new*] first@1 child@2 grand@3 new@1 [1:mail] new@1 [Hidden] d@1");

	/* 5. A pane tab's child shown in a pane (it takes the active pane;
	 *    the tab there goes to the background) is listed under its own
	 *    pane, not its parent's; collapsing a heading hides its group. */
	gsurf_window_set_active_view(h.window, child);
	ASSERT_SIDEBAR(&h, "[0:child*] first@1 child@1 grand@2 [1:mail] new@1 [Hidden] new@1 d@1");
	gsurf_window_set_active_view(h.window, b);
	gsurf_window_set_active_view(h.window, a);
	g_assert_true(gsurf_wmux_layout_get_payload(cur(&h)->layout,
		gsurf_wmux_layout_get_active(cur(&h)->layout)) == gsurf_view_get_id(a));
	ASSERT_SIDEBAR(&h, "[0:first*] first@1 child@2 grand@3 new@1 [1:mail] new@1 [Hidden] d@1");
	gsurf_window_set_active_view(h.window, b);
	{
		GtkWidget *twisty = find_named(group_row(&h, 1), "gsurf-tab-twisty");

		g_signal_emit_by_name(twisty, "clicked");
		ASSERT_SIDEBAR(&h, "[0:new*] first@1 child@2 grand@3 new@1 [1:mail] [Hidden] d@1");
		g_signal_emit_by_name(twisty, "clicked");
	}

	/* 6. Killing a window drops its heading. */
	cmd(&h, GDK_KEY_1, 0);
	g_assert_true(active_view(&h) == m);
	cmd(&h, GDK_KEY_ampersand, SHIFT);
	prompt_key(&h, GDK_KEY_y);
	gsurf_window_set_active_view(h.window, b);
	ASSERT_SIDEBAR(&h, "[0:new*] first@1 child@2 grand@3 new@1 [Hidden] d@1");

	/* 7. wmux off: the plain opener tree, exactly as without wmux. */
	gsurf_module_deactivate(h.module);
	g_assert_null(gsurf_window_get_layout(h.window));
	ASSERT_SIDEBAR(&h, "first@0 child@1 grand@2 new@0 d@0");
	g_assert_true(gsurf_module_activate(h.module));
	h.w = (GsurfWmuxModule *)h.module;
	ASSERT_SIDEBAR(&h, "[0:new*] new@1 [Hidden] first@1 child@2 grand@3 d@1");

	/* 8. wmux_groups: false keeps the opener tree under wmux too. */
	{
		g_autoptr(GsurfConfig) flat = gsurf_config_new();

		g_assert_true(gsurf_config_load_from_data(flat,
			"modules:\n  tabs:\n    enabled: true\n    sidebar: true\n"
			"    wmux_groups: false\n", -1, NULL));
		gsurf_module_configure(tabs, flat);
		gsurf_module_deactivate(tabs);
		g_assert_true(gsurf_module_activate(tabs));
		ASSERT_SIDEBAR(&h, "first@0 child@1 grand@2 new@0 d@0");
		gsurf_module_configure(tabs, h.config);
	}
	(void)grand;
	(void)d;
	h_free(&h);
}

/* The tab the solo view shows, or NULL when the panes are on screen. */
static GsurfView *
solo_view(H *h)
{
	if (!h->w->solo_active)
		return NULL;
	return gsurf_view_lookup(gsurf_wmux_layout_get_payload(h->w->solo->layout,
		gsurf_wmux_layout_get_active(h->w->solo->layout)));
}

/* Feature: selecting a tab no pane shows detaches to a solo view; the
 * panes stay as they were and come back on the next pane or window
 * selection.  select_background: active_pane keeps the old behaviour. */
static void
test_solo(void)
{
	H h;
	GsurfView *a, *b, *bg, *bg2, *fresh;
	g_autofree gchar *status = NULL;
	g_autofree gchar *fresh_uri = page("fresh");
	g_autofree gchar *layout_before = NULL;
	g_autofree gchar *layout_now = NULL;

	if (!gsurf_webkit_test_init() || !h_init(&h, "    confirm_kill: false\n", NULL, NULL))
		return;
	a = h.first;
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(a), "first") == 0);
	cmd(&h, GDK_KEY_percent, SHIFT);
	b = active_view(&h);
	bg = background_tab(&h, NULL, "bg");
	bg2 = background_tab(&h, NULL, "bg2");
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(bg), "bg") == 0);
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(bg2), "bg2") == 0);
	WAIT_UNTIL(g_strcmp0(gsurf_view_get_title(b), "new") == 0);
	layout_before = gsurf_wmux_layout_serialize(cur(&h)->layout, TRUE);
	check_invariants(&h);

	/* 1. Selecting a background tab shows it solo; the panes keep their
	 *    tabs; the status line marks the window "+" and shows the tab. */
	gsurf_window_set_active_view(h.window, bg);
	g_assert_true(solo_view(&h) == bg);
	g_assert_true(active_view(&h) == bg);
	g_assert_true(gsurf_window_get_view_visible(h.window, bg));
	g_assert_false(gsurf_window_get_view_visible(h.window, a));
	g_assert_false(gsurf_window_get_view_visible(h.window, b));
	g_assert_true(active_pane_view(&h) == b);
	layout_now = gsurf_wmux_layout_serialize(cur(&h)->layout, TRUE);
	g_assert_cmpstr(layout_now, ==, layout_before);
	g_clear_pointer(&layout_now, g_free);
	settle();
	check_invariants(&h);
	status = status_text(&h);
	g_assert_cmpstr(status, ==, "[gsurf] 0:new+ [bg]  solo");
	g_clear_pointer(&status, g_free);

	/* 2. Another background tab replaces it: one solo view, reused. */
	gsurf_window_set_active_view(h.window, bg2);
	g_assert_true(solo_view(&h) == bg2);
	g_assert_false(gsurf_window_get_view_visible(h.window, bg));
	settle();
	check_invariants(&h);

	/* 3. Selecting a tab a pane shows brings the layout back, that pane
	 *    active. */
	gsurf_window_set_active_view(h.window, a);
	g_assert_null(solo_view(&h));
	g_assert_true(active_pane_view(&h) == a);
	g_assert_true(gsurf_window_get_view_visible(h.window, b));
	g_assert_false(gsurf_window_get_view_visible(h.window, bg2));
	settle();
	check_invariants(&h);
	status = status_text(&h);
	g_assert_cmpstr(status, ==, "[gsurf] 0:first*  1/2");
	g_clear_pointer(&status, g_free);

	/* 4. A pane command (select-pane) leaves solo too. */
	gsurf_window_set_active_view(h.window, bg);
	g_assert_true(solo_view(&h) == bg);
	cmd(&h, GDK_KEY_o, 0);
	g_assert_null(solo_view(&h));
	g_assert_true(active_view(&h) == b);
	check_invariants(&h);

	/* 5. Selecting the current window leaves solo. */
	gsurf_window_set_active_view(h.window, bg);
	cmd(&h, GDK_KEY_0, 0);
	g_assert_null(solo_view(&h));
	g_assert_true(active_view(&h) == b);
	check_invariants(&h);

	/* 6. break-pane from solo: the solo tab gets a window of its own. */
	gsurf_window_set_active_view(h.window, bg);
	cmd(&h, GDK_KEY_exclam, SHIFT);
	g_assert_null(solo_view(&h));
	g_assert_cmpuint(h.w->wins->len, ==, 2);
	g_assert_cmpuint(h.w->current, ==, 1);
	g_assert_true(active_pane_view(&h) == bg);
	g_assert_cmpuint(n_panes(&h), ==, 1);
	settle();
	check_invariants(&h);
	cmd(&h, GDK_KEY_0, 0);

	/* 7. Closing the solo tab returns to the layout. */
	gsurf_window_set_active_view(h.window, bg2);
	g_assert_true(solo_view(&h) == bg2);
	gsurf_window_remove_view(h.window, bg2);
	g_assert_null(solo_view(&h));
	g_assert_true(active_view(&h) == b);
	settle();
	check_invariants(&h);

	/* 8. A new tab opened and selected at once (Ctrl+t) still goes into
	 *    the active pane; b becomes a background tab. */
	fresh = gsurf_view_new_full(NULL, NULL, fresh_uri, FALSE);
	gsurf_window_add_view(h.window, fresh);
	gsurf_window_set_active_view(h.window, fresh);
	g_object_unref(fresh);
	g_assert_null(solo_view(&h));
	g_assert_true(active_pane_view(&h) == fresh);
	settle();
	check_invariants(&h);

	/* 9. select_background: active_pane: the old behaviour. */
	h.w->select_active_pane = TRUE;
	gsurf_window_set_active_view(h.window, b);
	g_assert_null(solo_view(&h));
	g_assert_true(active_pane_view(&h) == b);
	g_assert_false(gsurf_window_get_view_visible(h.window, fresh));
	check_invariants(&h);
	h_free(&h);
}

#ifndef GSURF_BACKEND_GTK4
/* (GTK3 only: the wheel case below skips on GTK4.) */
/* What the window's button-press saw for a wheel notch. */
typedef struct {
	guint      button;
	guint      state;
	GsurfView *pointer;
	guint      n;
} WheelSeen;

static gboolean
on_wheel_press(GsurfWindow *window, guint button, guint state, gpointer data)
{
	WheelSeen *seen = data;

	seen->button = button;
	seen->state = state;
	seen->pointer = gsurf_window_get_pointer_view(window);
	seen->n++;
	/* Like the default mousebinds: only Ctrl+wheel is bound. */
	return (state & GSURF_MOD_CTRL) != 0;
}

/* Runs after gsurf's scroll handler only when it let the event through to
 * the page (a consumed scroll stops the emission). */
static gboolean
on_page_scroll(GtkWidget *widget, GdkEvent *event, gpointer data)
{
	(void)widget;
	(void)event;
	(*(guint *)data)++;
	return FALSE;
}
#endif

/* Feature: the wheel over a page reaches the window as wheel buttons
 * (Ctrl+wheel zooms by default), for the view under the pointer: a wmux
 * pane that is not the active one. GTK3 (GTK4's scroll controller needs a
 * real event for the modifier state). */
static void
test_wheel_pointer_view(void)
{
#ifdef GSURF_BACKEND_GTK4
	g_test_skip("GTK3 only: synthesizes GdkEventScroll");
#else
	H h;
	GsurfView *a, *b;
	WheelSeen seen = { 0, 0, NULL, 0 };
	GdkEvent *event;
	gboolean handled = FALSE;
	guint page_a = 0;
	guint page_b = 0;
	gulong id;

	if (!gsurf_webkit_test_init() || !h_init(&h, NULL, NULL, NULL))
		return;
	a = h.first;
	cmd(&h, GDK_KEY_percent, SHIFT);
	b = active_view(&h);
	g_assert_true(a != b);
	id = g_signal_connect(h.window, "button-press", G_CALLBACK(on_wheel_press), &seen);
	g_signal_connect(gsurf_view_get_native_widget(a), "scroll-event",
		G_CALLBACK(on_page_scroll), &page_a);
	g_signal_connect(gsurf_view_get_native_widget(b), "scroll-event",
		G_CALLBACK(on_page_scroll), &page_b);

	/* 1. Ctrl+wheel up over the inactive pane: button 4, Ctrl, that view;
	 *    consumed (the page does not scroll); the active pane stays. */
	event = gdk_event_new(GDK_SCROLL);
	event->scroll.direction = GDK_SCROLL_UP;
	event->scroll.state = GDK_CONTROL_MASK;
	g_signal_emit_by_name(gsurf_view_get_native_widget(a), "scroll-event", event, &handled);
	g_assert_true(handled);
	g_assert_cmpuint(page_a, ==, 0);
	g_assert_cmpuint(seen.n, ==, 1);
	g_assert_cmpuint(seen.button, ==, GSURF_BUTTON_WHEEL_UP);
	g_assert_cmpuint(seen.state, ==, GSURF_MOD_CTRL);
	g_assert_true(seen.pointer == a);
	g_assert_true(active_view(&h) == b);
	g_assert_null(gsurf_window_get_pointer_view(h.window));

	/* 2. A plain smooth scroll down: button 5, unbound, so the page gets
	 *    it (not consumed; WebKit itself then scrolls). */
	event->scroll.direction = GDK_SCROLL_SMOOTH;
	event->scroll.delta_x = 0.0;
	event->scroll.delta_y = 1.0;
	event->scroll.state = 0;
	g_signal_emit_by_name(gsurf_view_get_native_widget(b), "scroll-event", event, &handled);
	g_assert_cmpuint(page_b, ==, 1);
	g_assert_cmpuint(seen.n, ==, 2);
	g_assert_cmpuint(seen.button, ==, GSURF_BUTTON_WHEEL_DOWN);
	g_assert_true(seen.pointer == b);
	gdk_event_free(event);

	g_signal_handler_disconnect(h.window, id);
	g_signal_handlers_disconnect_by_func(gsurf_view_get_native_widget(a), on_page_scroll, &page_a);
	g_signal_handlers_disconnect_by_func(gsurf_view_get_native_widget(b), on_page_scroll, &page_b);
	check_invariants(&h);
	h_free(&h);
#endif
}

/* --- Opened-from-tab children (main.c's host handlers, copied) --- */

/* main.c's make_tab_full(): a new view of @opener, active or not. */
static GsurfView *
host_make_tab(GsurfWindow *window, GsurfView *opener, const gchar *uri, gboolean related,
              gboolean activate)
{
	GsurfView *view = gsurf_view_new_full(opener, NULL, uri, related);

	gsurf_window_add_view(window, view);
	if (activate)
		gsurf_window_set_active_view(window, view);
	if (uri != NULL)
		gsurf_view_load_uri(view, uri);
	g_object_unref(view);
	return view;
}

/* main.c's on_new_view_requested(). */
static gboolean
host_new_view_requested(GsurfView *source, const gchar *uri, gboolean background,
                        gpointer user_data)
{
	H *h = user_data;

	return host_make_tab(h->window, source, uri, FALSE, !background) != NULL;
}

/* main.c's on_create_view() (a popup: related, active, WebKit loads it). */
static GsurfView *
host_create_view(GsurfView *source, const gchar *uri, gpointer user_data)
{
	H *h = user_data;

	(void)uri;
	return host_make_tab(h->window, source, NULL, TRUE, TRUE);
}

/* The page context menu of @view on a link to @uri, as WebKit builds it
 * (with its "Open Link in New Window"), run through the backend's
 * context-menu handler; returns the menu (transfer full). */
static WebKitContextMenu *
link_context_menu(GsurfView *view, const gchar *uri)
{
	WebKitWebView *wv = WEBKIT_WEB_VIEW(gsurf_webkit_test_native(view));
	WebKitContextMenu *menu = webkit_context_menu_new();
	g_autoptr(WebKitHitTestResult) hit = g_object_new(WEBKIT_TYPE_HIT_TEST_RESULT,
		"context", WEBKIT_HIT_TEST_RESULT_CONTEXT_LINK | WEBKIT_HIT_TEST_RESULT_CONTEXT_DOCUMENT,
		"link-uri", uri, NULL);
	gboolean handled = FALSE;

	webkit_context_menu_append(menu,
		webkit_context_menu_item_new_from_stock_action(WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK));
	webkit_context_menu_append(menu, webkit_context_menu_item_new_from_stock_action(
		WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WINDOW));
	webkit_context_menu_append(menu, webkit_context_menu_item_new_from_stock_action(
		WEBKIT_CONTEXT_MENU_ACTION_COPY_LINK_TO_CLIPBOARD));
#ifdef GSURF_BACKEND_GTK4
	g_signal_emit_by_name(wv, "context-menu", menu, hit, &handled);
#else
	g_signal_emit_by_name(wv, "context-menu", menu, NULL, hit, &handled);
#endif
	g_assert_false(handled);
	return menu;
}

/* The item of @menu whose action is @name, or NULL; its position. */
static WebKitContextMenuItem *
menu_item_for(WebKitContextMenu *menu, const gchar *name, guint *position)
{
	GList *l;
	guint i = 0;

	for (l = webkit_context_menu_get_items(menu); l != NULL; l = l->next, i++) {
		GAction *action = webkit_context_menu_item_get_gaction(l->data);

		if (action != NULL && g_strcmp0(g_action_get_name(action), name) == 0) {
			if (position != NULL)
				*position = i;
			return l->data;
		}
	}
	return NULL;
}

/* The views of the window not in @before (a new one). */
static GsurfView *
new_view_since(H *h, GPtrArray *before)
{
	GPtrArray *views = gsurf_window_get_views(h->window);
	guint i;

	for (i = 0; i < views->len; i++) {
		if (!g_ptr_array_find(before, g_ptr_array_index(views, i), NULL))
			return g_ptr_array_index(views, i);
	}
	return NULL;
}

static GPtrArray *
snapshot_views(H *h)
{
	GPtrArray *views = gsurf_window_get_views(h->window);
	GPtrArray *copy = g_ptr_array_new();
	guint i;

	for (i = 0; i < views->len; i++)
		g_ptr_array_add(copy, g_ptr_array_index(views, i));
	return copy;
}

/* Feature: every way of opening a tab from a tab makes it a child (with
 * wmux on: a background child stays a background tab, nested under its
 * parent's pane tab in the grouped tree). */
static void
test_opened_children(void)
{
	static const gchar *const modules[] = { "tabs", "modal", NULL };
	H h;
	GsurfView *a, *bg, *fg, *popup, *hinted, *fresh, *kept;
	g_autofree gchar *link = page("linked");
	g_autofree gchar *links = g_strdup_printf("http://127.0.0.1:%u/links", port);
	g_autoptr(GPtrArray) before = NULL;
	guint64 opener_id;

	if (!gsurf_webkit_test_init() ||
	    !h_init(&h, NULL, "  tabs:\n    enabled: true\n    sidebar: true\n"
			"  modal:\n    enabled: true\n", modules))
		return;
	a = h.first;
	gsurf_webkit_test_wait_finished(gsurf_webkit_test_native(a));
	g_signal_connect(a, "new-view-requested", G_CALLBACK(host_new_view_requested), &h);
	g_signal_connect(a, "create-view", G_CALLBACK(host_create_view), &h);

	/* 1. The context menu: "Open Link in New Window" became "Open Link
	 *    in New Tab", in its place; it opens a background child. */
	{
		g_autoptr(WebKitContextMenu) menu = link_context_menu(a, link);
		WebKitContextMenuItem *item;
		GList *l;
		guint position = 99;

		for (l = webkit_context_menu_get_items(menu); l != NULL; l = l->next)
			g_assert_cmpint(webkit_context_menu_item_get_stock_action(l->data), !=,
				WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WINDOW);
		item = menu_item_for(menu, "gsurf-open-link-new-tab", &position);
		g_assert_nonnull(item);
		g_assert_cmpuint(position, ==, 1);
		before = snapshot_views(&h);
		g_action_activate(webkit_context_menu_item_get_gaction(item), NULL);
	}
	bg = new_view_since(&h, before);
	g_assert_nonnull(bg);
	g_assert_true(gsurf_view_get_opener(bg) == a);
	g_assert_false(gsurf_view_get_related(bg));
	g_assert_true(active_view(&h) == a);
	g_assert_false(gsurf_window_get_view_visible(h.window, bg));
	ASSERT_SIDEBAR(&h, "[0:first*] first@1 linked@2");
	check_invariants(&h);

	/* 2. Without a host handler the stock item stays. */
	{
		GsurfView *other = background_tab(&h, NULL, "other");
		g_autoptr(WebKitContextMenu) menu = NULL;
		guint i = 0;
		GList *l;

		gsurf_webkit_test_wait_finished(gsurf_webkit_test_native(other));
		menu = link_context_menu(other, link);
		g_assert_null(menu_item_for(menu, "gsurf-open-link-new-tab", NULL));
		for (l = webkit_context_menu_get_items(menu); l != NULL; l = l->next)
			i += webkit_context_menu_item_get_stock_action(l->data) ==
				WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WINDOW;
		g_assert_cmpuint(i, ==, 1);
		gsurf_window_remove_view(h.window, other);
	}

	/* 3. A foreground link tab (open-new-view): a child that takes the
	 *    active pane; its parent becomes a background tab. */
	fg = host_make_tab(h.window, a, link, FALSE, TRUE);
	g_assert_true(active_view(&h) == fg);
	g_assert_cmpuint(gsurf_view_get_opener_id(fg), ==, gsurf_view_get_id(a));
	ASSERT_SIDEBAR(&h, "[0:linked*] linked@1 [Background] first@1 linked@2");
	gsurf_window_remove_view(h.window, fg);
	WAIT_UNTIL(active_view(&h) == a);

	/* 4. A popup (target=_blank, window.open): a related child,
	 *    activated. */
	g_ptr_array_unref(before);
	before = snapshot_views(&h);
	g_assert_nonnull(gsurf_view_emit_create_view(a, link));
	popup = new_view_since(&h, before);
	g_assert_nonnull(popup);
	g_assert_true(gsurf_view_get_opener(popup) == a);
	g_assert_true(gsurf_view_get_related(popup));
	g_assert_true(active_view(&h) == popup);
	gsurf_window_remove_view(h.window, popup);
	WAIT_UNTIL(active_view(&h) == a);

	/* 5. The modal F hints: a child of the hinted page. */
	gsurf_view_load_uri(a, links);
	gsurf_webkit_test_wait_finished(gsurf_webkit_test_native(a));
	g_ptr_array_unref(before);
	before = snapshot_views(&h);
	g_assert_true(gsurf_module_manager_dispatch_action(h.mgr, a,
		GSURF_ACTION_FOLLOW_HINTS_NEW_VIEW));
	g_assert_true(gsurf_module_manager_dispatch_key_event(h.mgr, a, GDK_KEY_a, 0, 0,
		GSURF_MODE_NORMAL));
	hinted = new_view_since(&h, before);
	g_assert_nonnull(hinted);
	g_assert_true(gsurf_view_get_opener(hinted) == a);
	gsurf_window_remove_view(h.window, hinted);
	WAIT_UNTIL(active_view(&h) == a);

	/* 6. A background child replaced in another container (the
	 *    containers module's reassignment) stays a background child in
	 *    its place, even after its opener closed. */
	opener_id = gsurf_view_get_id(a);
	kept = background_tab(&h, bg, "kept");
	fresh = gsurf_window_replace_view(h.window, bg, GSURF_CONTAINER_TEMPORARY, link);
	g_assert_nonnull(fresh);
	g_assert_true(gsurf_view_get_opener(fresh) == a);
	g_assert_true(active_view(&h) == a);
	g_assert_false(gsurf_window_get_view_visible(h.window, fresh));
	g_assert_true(gsurf_view_lookup(gsurf_view_get_id(kept)) == kept);
	ASSERT_SIDEBAR(&h, "[0:links*] links@1 linked@2 kept@3");
	gsurf_window_remove_view(h.window, a);
	g_assert_null(gsurf_view_get_opener(fresh));
	g_assert_cmpuint(gsurf_view_get_opener_id(fresh), ==, opener_id);
	{
		GsurfView *again = gsurf_window_replace_view(h.window, fresh, NULL, NULL);

		g_assert_nonnull(again);
		g_assert_cmpuint(gsurf_view_get_opener_id(again), ==, opener_id);
	}
	check_invariants(&h);
	h_free(&h);
}

/* Feature: the vim mode indicator in the status line (and the chromebar),
 * driven end to end by the modal module's keys. */
static void
test_mode_indicator(void)
{
	static const gchar *const modules[] = { "modal", "chromebar", NULL };
	H h;
	GtkWidget *bar_mode;
	g_autofree gchar *links = g_strdup_printf("http://127.0.0.1:%u/links", port);

	if (!gsurf_webkit_test_init() ||
	    !h_init(&h, "    mode_insert_color: \"#ff0000\"\n",
			"  modal:\n    enabled: true\n  chromebar:\n    enabled: true\n", modules))
		return;
	gsurf_module_manager_set_mode(h.mgr, GSURF_MODE_NORMAL);
	g_assert_nonnull(h.w->status_mode);
	bar_mode = find_named(h.toplevel, "gsurf-chromebar-mode");
	g_assert_nonnull(bar_mode);
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(h.w->status_mode)), ==, "-- NORMAL --");
	g_assert_true(gtk_widget_get_visible(h.w->status_mode));
	{
		g_autofree gchar *css = gtk_css_provider_to_string(h.w->css);
		g_autofree gchar *red = NULL;
		GdkRGBA rgba;

		g_assert_true(gdk_rgba_parse(&rgba, "#ff0000"));
		red = gdk_rgba_to_string(&rgba);
		g_assert_nonnull(strstr(css, "wmux-mode-insert"));
		g_assert_nonnull(strstr(css, red));
	}

	/* 1. i: INSERT, in both indicators; Escape: NORMAL. */
	gsurf_webkit_test_wait_finished(gsurf_webkit_test_native(h.first));
	g_assert_true(press(&h, GDK_KEY_i, 0));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(h.w->status_mode)), ==, "-- INSERT --");
	g_assert_true(has_class(h.w->status_mode, "wmux-mode-insert"));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(bar_mode)), ==, "-- INSERT --");
	g_assert_true(press(&h, GDK_KEY_Escape, 0));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(h.w->status_mode)), ==, "-- NORMAL --");
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(bar_mode)), ==, "-- NORMAL --");

	/* 1b. A page field the user clicks into: its keys are text, so both
	 *     indicators read INSERT (the modal module itself stays NORMAL);
	 *     Escape leaves the field and they read NORMAL again. */
	g_free(gsurf_webkit_test_eval(gsurf_webkit_test_native(h.first),
		"var i=document.createElement('input');i.id='typed';"
		"document.body.appendChild(i);i.focus();'f'"));
	WAIT_UNTIL(gsurf_view_get_editing(h.first));
	g_assert_false(gsurf_module_manager_get_input_passthrough(h.mgr));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(h.w->status_mode)), ==, "-- INSERT --");
	g_assert_true(has_class(h.w->status_mode, "wmux-mode-insert"));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(bar_mode)), ==, "-- INSERT --");
	g_assert_true(press(&h, GDK_KEY_Escape, 0));
	g_assert_false(gsurf_view_get_editing(h.first));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(h.w->status_mode)), ==, "-- NORMAL --");
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(bar_mode)), ==, "-- NORMAL --");

	/* 1c. The address bar (open-prompt): typing a URL reads INSERT;
	 *     enter-normal-mode gives the page the keys back. */
	g_assert_true(gsurf_module_manager_dispatch_action(h.mgr, h.first,
		GSURF_ACTION_OPEN_PROMPT));
	WAIT_UNTIL(gsurf_window_get_entry_focus(h.window));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(h.w->status_mode)), ==, "-- INSERT --");
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(bar_mode)), ==, "-- INSERT --");
	g_assert_true(gsurf_module_manager_dispatch_action(h.mgr, h.first,
		GSURF_ACTION_ENTER_NORMAL_MODE));
	WAIT_UNTIL(!gsurf_window_get_entry_focus(h.window));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(h.w->status_mode)), ==, "-- NORMAL --");
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(bar_mode)), ==, "-- NORMAL --");

	/* 2. Hints: FOLLOW while they are up. */
	gsurf_view_load_uri(h.first, links);
	gsurf_webkit_test_wait_finished(gsurf_webkit_test_native(h.first));
	g_assert_true(gsurf_module_manager_dispatch_action(h.mgr, h.first,
		GSURF_ACTION_FOLLOW_HINTS));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(h.w->status_mode)), ==, "-- FOLLOW --");
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(bar_mode)), ==, "-- FOLLOW --");
	g_assert_true(press(&h, GDK_KEY_Escape, 0));
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(h.w->status_mode)), ==, "-- NORMAL --");
	h_free(&h);

	/* 3. mode_hide_normal hides it in NORMAL; mode_indicator: false
	 *    leaves it out. */
	if (!h_init(&h, "    mode_hide_normal: true\n", NULL, NULL))
		return;
	gsurf_module_manager_set_mode(h.mgr, GSURF_MODE_NORMAL);
	g_assert_false(gtk_widget_get_visible(h.w->status_mode));
	gsurf_module_manager_set_mode(h.mgr, GSURF_MODE_INSERT);
	g_assert_true(gtk_widget_get_visible(h.w->status_mode));
	gsurf_module_manager_set_mode(h.mgr, GSURF_MODE_NORMAL);
	h_free(&h);
	if (!h_init(&h, "    mode_indicator: false\n", NULL, NULL))
		return;
	g_assert_null(h.w->status_mode);
	g_assert_null(find_named(h.toplevel, "gsurf-wmux-status-mode"));
	h_free(&h);
}

/* Press @button on a sidebar row widget (GTK4: its click gesture for the
 * button, @n_press presses; GTK3: a press, a double press for 2). */
static void
press_row_widget(GtkWidget *row, guint button, gint n_press)
{
#ifdef GSURF_BACKEND_GTK4
	g_autofree gchar *name = n_press == 2 ? g_strdup("gsurf-chrome-dblclick-1")
		: g_strdup_printf("gsurf-chrome-click-%u", button);
	GtkWidget *title = find_named(row, "gsurf-tab-title");
	graphene_rect_t bounds;

	WAIT_UNTIL(gtk_widget_get_width(title) > 0);
	g_assert_true(gtk_widget_compute_bounds(title, row, &bounds));
	g_signal_emit_by_name(controller_named(row, name), "pressed", n_press,
		(gdouble)(bounds.origin.x + bounds.size.width / 2),
		(gdouble)(bounds.origin.y + bounds.size.height / 2));
#else
	GdkEvent *event = gdk_event_new(n_press == 2 ? GDK_2BUTTON_PRESS : GDK_BUTTON_PRESS);
	gboolean handled = FALSE;

	event->button.button = button;
	g_signal_emit_by_name(row, "button-press-event", event, &handled);
	gdk_event_free(event);
	g_assert_true(handled);
#endif
}

/* A key press delivered to @widget's chrome key handler. */
static gboolean
key_to(H *h, GtkWidget *widget, guint keyval)
{
	gboolean handled = FALSE;
#ifdef GSURF_BACKEND_GTK4
	(void)h;
	g_signal_emit_by_name(controller_named(widget, "gsurf-chrome-key"), "key-pressed",
		keyval, 0u, (GdkModifierType)0, &handled);
#else
	GdkEvent *event = gdk_event_new(GDK_KEY_PRESS);

	event->key.window = g_object_ref(gtk_widget_get_window(h->toplevel));
	event->key.keyval = keyval;
	event->key.string = g_strdup("");
	g_signal_emit_by_name(widget, "key-press-event", event, &handled);
	gdk_event_free(event);
#endif
	return handled;
}

/* v0.4.3: renaming a wmux window from its sidebar heading (the menu, a
 * double click, F2), consistent with the status line, the C-b , prompt
 * and the saved state. */
static void
test_group_rename(void)
{
	static const gchar *const modules[] = { "tabs", NULL };
	H h;
	GtkWidget *entry;
	GtkWidget *title;
	GActionGroup *actions;
	GsurfModule *tabs;
	g_autofree gchar *state = NULL;

	if (!gsurf_webkit_test_init() ||
	    !h_init(&h, NULL, "  tabs:\n    enabled: true\n    sidebar: true\n", modules))
		return;
	tabs = gsurf_module_manager_get_module(h.mgr, "tabs");
	cmd(&h, GDK_KEY_c, 0);
	background_tab(&h, NULL, "d");
	ASSERT_SIDEBAR(&h, "[0:first] first@1 [1:new*] new@1 [Background] d@1");

	/* 1. The heading's menu: Rename Window… opens the entry with the
	 *    name; Enter applies it to the right window. */
	press_row_widget(group_row(&h, 0), 3, 1);
	actions = g_object_get_data(G_OBJECT(tabs), "gsurf-tabs-menu-actions");
	g_assert_nonnull(actions);
	{
		GMenuModel *model = g_object_get_data(G_OBJECT(tabs), "gsurf-tabs-menu-model");
		g_autofree gchar *action = NULL;

		g_assert_nonnull(model);
		g_assert_cmpint(g_menu_model_get_n_items(model), ==, 1);
		g_assert_true(g_menu_model_get_item_attribute(model, 0, G_MENU_ATTRIBUTE_ACTION,
			"s", &action));
		g_assert_cmpstr(action, ==, "tab.rename-group");
	}
	g_action_group_activate_action(actions, "rename-group", NULL);
	entry = find_named(group_row(&h, 0), "gsurf-tab-group-rename");
	title = find_named(group_row(&h, 0), "gsurf-tab-title");
	g_assert_true(gtk_widget_get_visible(entry));
	g_assert_false(gtk_widget_get_visible(title));
	g_assert_cmpstr(gsurf_chrome_editable_get_text(entry), ==, "first");
	gsurf_chrome_editable_set_text(entry, "  docs ");
	g_assert_true(key_to(&h, entry, GDK_KEY_Return));
	g_assert_false(gtk_widget_get_visible(entry));
	g_assert_true(gtk_widget_get_visible(title));
	ASSERT_SIDEBAR(&h, "[0:docs] first@1 [1:new*] new@1 [Background] d@1");
	g_assert_true(((GsurfWmuxWin *)g_ptr_array_index(h.w->wins, 0))->renamed);
	g_assert_cmpuint(h.w->current, ==, 1);
	{
		g_autofree gchar *status = status_text(&h);

		g_assert_nonnull(strstr(status, "0:docs-"));
	}

	/* 2. A double click; Escape cancels and keeps the name. */
	press_row_widget(group_row(&h, 1), 1, 2);
	entry = find_named(group_row(&h, 1), "gsurf-tab-group-rename");
	g_assert_true(gtk_widget_get_visible(entry));
	gsurf_chrome_editable_set_text(entry, "nope");
	g_assert_true(key_to(&h, entry, GDK_KEY_Escape));
	g_assert_false(gtk_widget_get_visible(entry));
	ASSERT_SIDEBAR(&h, "[0:docs] first@1 [1:new*] new@1 [Background] d@1");
	/* Other keys are typing (the handler leaves them to the entry; on
	 * GTK3 the entry's own handler then takes the emitted key). */
	press_row_widget(group_row(&h, 1), 1, 2);
#ifdef GSURF_BACKEND_GTK4
	g_assert_false(key_to(&h, entry, GDK_KEY_a));
#else
	key_to(&h, entry, GDK_KEY_a);
#endif
	gsurf_chrome_editable_set_text(entry, "mail");
	g_assert_true(key_to(&h, entry, GDK_KEY_KP_Enter));
	ASSERT_SIDEBAR(&h, "[0:docs] first@1 [1:mail*] new@1 [Background] d@1");

	/* 3. The C-b , prompt starts from the same name; the saved state
	 *    carries it. */
	cmd(&h, GDK_KEY_comma, 0);
	g_assert_cmpstr(gsurf_chrome_editable_get_text(h.w->prompt_entry), ==, "mail");
	prompt_key(&h, GDK_KEY_Escape);
	state = gsurf_window_layout_serialize(GSURF_WINDOW_LAYOUT(h.module), h.window,
		map_index, h.window);
	g_assert_nonnull(strstr(state, "name=\"docs\" renamed=1"));
	g_assert_nonnull(strstr(state, "name=\"mail\" renamed=1"));

	/* 4. F2 on a heading; an empty name goes back to automatic naming. */
	g_assert_true(key_to(&h, group_row(&h, 0), GDK_KEY_F2));
	entry = find_named(group_row(&h, 0), "gsurf-tab-group-rename");
	g_assert_true(gtk_widget_get_visible(entry));
	gsurf_chrome_editable_set_text(entry, "   ");
	g_assert_true(key_to(&h, entry, GDK_KEY_Return));
	ASSERT_SIDEBAR(&h, "[0:first] first@1 [1:mail*] new@1 [Background] d@1");
	g_assert_false(((GsurfWmuxWin *)g_ptr_array_index(h.w->wins, 0))->renamed);
	/* Other keys on a heading are not F2. */
#ifdef GSURF_BACKEND_GTK4
	g_assert_false(key_to(&h, group_row(&h, 0), GDK_KEY_F3));
#endif

	/* 5. The background heading cannot be renamed (F2 is taken, nothing
	 *    opens). */
	g_assert_true(key_to(&h, group_row(&h, G_MAXUINT), GDK_KEY_F2));
	entry = find_named(group_row(&h, G_MAXUINT), "gsurf-tab-group-rename");
	g_assert_true(entry == NULL || !gtk_widget_get_visible(entry));
	h_free(&h);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *tmp = NULL;
	int ret;

	g_test_init(&argc, &argv, NULL);
	tmp = g_dir_make_tmp("gsurf-wmux-webkit-XXXXXX", NULL);
	g_assert_nonnull(tmp);
	g_setenv("XDG_DATA_HOME", tmp, TRUE);
	g_setenv("XDG_CACHE_HOME", tmp, TRUE);
	g_setenv("XDG_CONFIG_HOME", tmp, TRUE);

	server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, serve, NULL, NULL);
	port = gsurf_webkit_test_listen(server);

	g_test_add_func("/gsurf/wmux/webkit/split-navigate", test_split_navigate);
	g_test_add_func("/gsurf/wmux/webkit/pane-ops", test_pane_ops);
	g_test_add_func("/gsurf/wmux/webkit/windows", test_windows);
	g_test_add_func("/gsurf/wmux/webkit/integration", test_integration);
	g_test_add_func("/gsurf/wmux/webkit/lifetime", test_lifetime);
	g_test_add_func("/gsurf/wmux/webkit/session-round-trip", test_session_round_trip);
	g_test_add_func("/gsurf/wmux/webkit/stress", test_stress);
	g_test_add_func("/gsurf/wmux/webkit/drag-into-pane", test_drag_into_pane);
	g_test_add_func("/gsurf/wmux/webkit/grouped-tree", test_grouped_tree);
	g_test_add_func("/gsurf/wmux/webkit/opened-children", test_opened_children);
	g_test_add_func("/gsurf/wmux/webkit/mode-indicator", test_mode_indicator);
	g_test_add_func("/gsurf/wmux/webkit/group-rename", test_group_rename);
	g_test_add_func("/gsurf/wmux/webkit/solo", test_solo);
	g_test_add_func("/gsurf/wmux/webkit/wheel-pointer-view", test_wheel_pointer_view);

	ret = g_test_run();
	g_clear_object(&server);
	gsurf_test_rmtree(tmp);
	return ret;
}
