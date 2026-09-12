/*
 * gsurf-gtk3-window.c - GTK3 browser window backend
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "backend/gtk3/gsurf-gtk3-window.h"
#include "boxed/gsurf-keybind-help.h"

#include <gtk/gtk.h>
#include <gdk/gdk.h>

struct _GsurfGtk3Window
{
	GsurfWindow parent_instance;

	GtkWidget *window;  /* GtkWindow toplevel */
	GtkWidget *vbox;    /* content box (chrome from modules goes here) */
	GtkWidget *stack;   /* GtkStack of view widgets */
};

G_DEFINE_FINAL_TYPE(GsurfGtk3Window, gsurf_gtk3_window, GSURF_TYPE_WINDOW)

/* Translate a GDK modifier mask into GsurfKeyMod flags. */
static guint
translate_modifiers(GdkModifierType state)
{
	guint mods = GSURF_MOD_NONE;

	if (state & GDK_SHIFT_MASK)
		mods |= GSURF_MOD_SHIFT;
	if (state & GDK_CONTROL_MASK)
		mods |= GSURF_MOD_CTRL;
	if (state & GDK_MOD1_MASK)
		mods |= GSURF_MOD_ALT;
	if (state & GDK_SUPER_MASK)
		mods |= GSURF_MOD_SUPER;

	return mods;
}

static gboolean
on_key_press_event(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	GsurfGtk3Window *self = user_data;
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(self->window));
	guint mods;

	/* If a chrome text entry (address bar, find box) has focus, let it
	 * handle its own keys — don't route them through the keymap/modal
	 * layer. The web view is not a GtkEditable, so it still routes. */
	if (focus != NULL && GTK_IS_EDITABLE(focus))
		return FALSE;

	mods = translate_modifiers(event->state);

	/* Runs before the focused webview, so returning TRUE consumes the
	 * key (normal-mode commands); FALSE lets the page handle it. */
	return gsurf_window_emit_key_press(GSURF_WINDOW(self),
		event->keyval, event->hardware_keycode, mods);
}

static gboolean
on_delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	GsurfGtk3Window *self = user_data;

	/* If a handler consumes the close request, veto the default. */
	return gsurf_window_emit_close_request(GSURF_WINDOW(self));
}

static gboolean
on_view_button_press_event(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	GsurfGtk3Window *self = user_data;
	guint mods;

	if (event->type != GDK_BUTTON_PRESS)
		return FALSE;

	mods = translate_modifiers(event->state);

	/* Runs before the webview's default handler; returning TRUE consumes
	 * the button (e.g. side-button history nav) and stops it reaching the
	 * page, while FALSE lets WebKit handle it normally. */
	return gsurf_window_emit_button_press(GSURF_WINDOW(self), event->button, mods);
}

/* --- Vfunc implementations --- */

static void
gsurf_gtk3_window_realize(GsurfWindow *window)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(window);

	self->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	self->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	self->stack = gtk_stack_new();

	gtk_stack_set_transition_type(GTK_STACK(self->stack),
		GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_box_pack_start(GTK_BOX(self->vbox), self->stack, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(self->window), self->vbox);

	gtk_window_set_default_size(GTK_WINDOW(self->window),
		GSURF_DEFAULT_WIDTH, GSURF_DEFAULT_HEIGHT);

	g_signal_connect(self->window, "key-press-event",
		G_CALLBACK(on_key_press_event), self);
	g_signal_connect(self->window, "delete-event",
		G_CALLBACK(on_delete_event), self);
}

static void
gsurf_gtk3_window_insert_view(GsurfWindow *window, GsurfView *view)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(window);
	GtkWidget *widget = gsurf_view_get_native_widget(view);

	if (widget == NULL)
		return;

	gtk_container_add(GTK_CONTAINER(self->stack), widget);
	gtk_widget_show(widget);

	/* Route mouse buttons (incl. side history buttons) through the window
	 * so mousebind config + input modules can act on them. Connect once. */
	if (g_object_get_data(G_OBJECT(widget), "gsurf-btn-wired") == NULL) {
		g_object_set_data(G_OBJECT(widget), "gsurf-btn-wired", GINT_TO_POINTER(1));
		gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK);
		g_signal_connect(widget, "button-press-event",
			G_CALLBACK(on_view_button_press_event), self);
	}
}

static void
gsurf_gtk3_window_remove_view(GsurfWindow *window, GsurfView *view)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(window);
	GtkWidget *widget = gsurf_view_get_native_widget(view);

	if (widget != NULL && gtk_widget_get_parent(widget) == self->stack)
		gtk_container_remove(GTK_CONTAINER(self->stack), widget);
}

static void
gsurf_gtk3_window_set_active_view(GsurfWindow *window, GsurfView *view)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(window);
	GtkWidget *widget = gsurf_view_get_native_widget(view);

	if (widget != NULL)
		gtk_stack_set_visible_child(GTK_STACK(self->stack), widget);
}

static void
gsurf_gtk3_window_present(GsurfWindow *window)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(window);

	gtk_widget_show_all(self->window);
	gtk_window_present(GTK_WINDOW(self->window));
}

static void
gsurf_gtk3_window_set_title(GsurfWindow *window, const gchar *title)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(window);
	gtk_window_set_title(GTK_WINDOW(self->window), title ? title : "gsurf");
}

static void
gsurf_gtk3_window_set_default_size(GsurfWindow *window, gint width, gint height)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(window);
	gtk_window_set_default_size(GTK_WINDOW(self->window), width, height);
}

static void
gsurf_gtk3_window_set_fullscreen(GsurfWindow *window, gboolean fullscreen)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(window);
	if (fullscreen)
		gtk_window_fullscreen(GTK_WINDOW(self->window));
	else
		gtk_window_unfullscreen(GTK_WINDOW(self->window));
}

static gpointer
gsurf_gtk3_window_get_native_widget(GsurfWindow *window)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(window);
	return self->window;
}

static void
on_help_response(GtkDialog *dialog, gint response, gpointer user_data)
{
	(void)response;
	(void)user_data;
	gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
on_help_destroy(GtkWidget *dialog, gpointer user_data)
{
	GsurfGtk3Window *self = user_data;

	(void)dialog;
	if (self->window != NULL)
		g_object_set_data(G_OBJECT(self->window), "gsurf-keybind-help", NULL);
}

/* Move the tree cursor by @delta rows and keep the cell in view. */
static void
help_tree_move(GtkTreeView *tree, gint delta)
{
	GtkTreeModel *model;
	GtkTreePath *path = NULL;
	gint n;
	gint idx = 0;
	gint *indices;

	model = gtk_tree_view_get_model(tree);
	n = gtk_tree_model_iter_n_children(model, NULL);
	if (n <= 0)
		return;

	gtk_tree_view_get_cursor(tree, &path, NULL);
	if (path != NULL) {
		indices = gtk_tree_path_get_indices(path);
		idx = indices[0] + delta;
		gtk_tree_path_free(path);
	} else if (delta < 0) {
		idx = n - 1;
	}

	if (idx < 0)
		idx = 0;
	if (idx >= n)
		idx = n - 1;

	path = gtk_tree_path_new_from_indices(idx, -1);
	gtk_tree_view_set_cursor(tree, path, NULL, FALSE);
	gtk_tree_view_scroll_to_cell(tree, path, NULL, FALSE, 0, 0);
	gtk_tree_path_free(path);
}

static void
help_scroll_x(GtkScrolledWindow *scrolled, gint delta)
{
	GtkAdjustment *adj;

	adj = gtk_scrolled_window_get_hadjustment(scrolled);
	if (adj == NULL)
		return;
	gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + (gdouble)delta);
}

/* q closes; hjkl move. Typeahead search is off so letters are ours. */
static gboolean
on_help_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	GtkTreeView *tree = GTK_TREE_VIEW(user_data);
	GtkWidget *scrolled;
	GtkWidget *toplevel;
	GsurfKeybindHelpKey action;

	action = gsurf_keybind_help_key_action(event->keyval,
		translate_modifiers(event->state));
	if (action == GSURF_KEYBIND_HELP_KEY_NONE)
		return FALSE;
	if (action == GSURF_KEYBIND_HELP_KEY_CLOSE) {
		toplevel = gtk_widget_get_toplevel(widget);
		if (GTK_IS_WINDOW(toplevel))
			gtk_widget_destroy(toplevel);
		return TRUE;
	}

	scrolled = gtk_widget_get_parent(GTK_WIDGET(tree));
	if (action == GSURF_KEYBIND_HELP_KEY_UP)
		help_tree_move(tree, -1);
	else if (action == GSURF_KEYBIND_HELP_KEY_DOWN)
		help_tree_move(tree, 1);
	else if (GTK_IS_SCROLLED_WINDOW(scrolled))
		help_scroll_x(GTK_SCROLLED_WINDOW(scrolled),
			action == GSURF_KEYBIND_HELP_KEY_LEFT ? -80 : 80);
	return TRUE;
}

static void
gsurf_gtk3_window_show_keybind_help(GsurfWindow *window, GPtrArray *entries)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(window);
	GtkWidget *dialog, *content, *scrolled, *tree, *old;
	GtkListStore *store;
	GtkCellRenderer *renderer;
	guint i;

	if (self->window == NULL)
		return;

	old = g_object_get_data(G_OBJECT(self->window), "gsurf-keybind-help");
	if (old != NULL) {
		gtk_widget_destroy(old);
		return;
	}

	dialog = gtk_dialog_new_with_buttons("Keybindings (hjkl / q)",
		GTK_WINDOW(self->window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		"_Close", GTK_RESPONSE_CLOSE,
		NULL);
	gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 520);
	g_object_set_data(G_OBJECT(self->window), "gsurf-keybind-help", dialog);
	g_signal_connect(dialog, "response", G_CALLBACK(on_help_response), NULL);
	g_signal_connect(dialog, "destroy", G_CALLBACK(on_help_destroy), self);

	store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	for (i = 0; entries != NULL && i < entries->len; i++) {
		const GsurfKeybindHelp *h = g_ptr_array_index(entries, i);
		g_autofree gchar *pretty = gsurf_keybind_help_pretty_key(h->key);
		GtkTreeIter iter;

		gtk_list_store_append(store, &iter);
		gtk_list_store_set(store, &iter,
			0, pretty != NULL ? pretty : "",
			1, h->description != NULL ? h->description : "",
			2, h->source != NULL ? h->source : "",
			3, h->action != NULL ? h->action : "",
			-1);
	}

	tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(store);
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tree), FALSE);
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)),
		GTK_SELECTION_BROWSE);
	if (entries != NULL && entries->len > 0) {
		GtkTreePath *path = gtk_tree_path_new_first();

		gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree), path, NULL, FALSE);
		gtk_tree_path_free(path);
	}

	renderer = gtk_cell_renderer_text_new();
	g_object_set(renderer, "family", "monospace", NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tree),
		gtk_tree_view_column_new_with_attributes("Key", renderer, "text", 0, NULL));
	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_append_column(GTK_TREE_VIEW(tree),
		gtk_tree_view_column_new_with_attributes("Action", renderer, "text", 1, NULL));
	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_append_column(GTK_TREE_VIEW(tree),
		gtk_tree_view_column_new_with_attributes("Source", renderer, "text", 2, NULL));

	scrolled = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scrolled), tree);

	content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_box_pack_start(GTK_BOX(content), scrolled, TRUE, TRUE, 0);
	g_signal_connect(tree, "key-press-event", G_CALLBACK(on_help_key_press), tree);
	g_signal_connect(dialog, "key-press-event", G_CALLBACK(on_help_key_press), tree);
	gtk_widget_show_all(dialog);
	gtk_widget_grab_focus(tree);
}

static void
gsurf_gtk3_window_add_chrome_widget(GsurfWindow *window, gpointer widget, gboolean top)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(window);
	GtkWidget *w = widget;

	if (w == NULL)
		return;

	if (top) {
		gtk_box_pack_start(GTK_BOX(self->vbox), w, FALSE, FALSE, 0);
		gtk_box_reorder_child(GTK_BOX(self->vbox), w, 0);
	} else {
		gtk_box_pack_end(GTK_BOX(self->vbox), w, FALSE, FALSE, 0);
	}
	gtk_widget_show_all(w);
}

/* --- GObject lifecycle --- */

static void
gsurf_gtk3_window_dispose(GObject *object)
{
	GsurfGtk3Window *self = GSURF_GTK3_WINDOW(object);

	if (self->window != NULL) {
		gtk_widget_destroy(self->window);
		self->window = NULL;
	}

	G_OBJECT_CLASS(gsurf_gtk3_window_parent_class)->dispose(object);
}

static void
gsurf_gtk3_window_class_init(GsurfGtk3WindowClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfWindowClass *window_class = GSURF_WINDOW_CLASS(klass);

	object_class->dispose = gsurf_gtk3_window_dispose;

	window_class->realize = gsurf_gtk3_window_realize;
	window_class->insert_view = gsurf_gtk3_window_insert_view;
	window_class->remove_view = gsurf_gtk3_window_remove_view;
	window_class->set_active_view = gsurf_gtk3_window_set_active_view;
	window_class->present = gsurf_gtk3_window_present;
	window_class->set_title = gsurf_gtk3_window_set_title;
	window_class->set_default_size = gsurf_gtk3_window_set_default_size;
	window_class->set_fullscreen = gsurf_gtk3_window_set_fullscreen;
	window_class->get_native_widget = gsurf_gtk3_window_get_native_widget;
	window_class->add_chrome_widget = gsurf_gtk3_window_add_chrome_widget;
	window_class->show_keybind_help = gsurf_gtk3_window_show_keybind_help;
}

static void
gsurf_gtk3_window_init(GsurfGtk3Window *self)
{
}

GsurfWindow *
gsurf_gtk3_window_new(void)
{
	return g_object_new(GSURF_TYPE_GTK3_WINDOW, NULL);
}
