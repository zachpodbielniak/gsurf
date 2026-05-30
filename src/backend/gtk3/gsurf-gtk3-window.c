/*
 * gsurf-gtk3-window.c - GTK3 browser window backend
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "backend/gtk3/gsurf-gtk3-window.h"

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
