/*
 * gsurf-gtk4-window.c - GTK4 browser window backend
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GTK4 port of the GTK3 window: GtkWindow + GtkBox + GtkStack of views,
 * with a GtkEventControllerKey forwarding key presses. Built only when
 * GTK_BACKEND=gtk4.
 */

#include "backend/gtk4/gsurf-gtk4-window.h"

#include <gtk/gtk.h>

struct _GsurfGtk4Window
{
	GsurfWindow parent_instance;

	GtkWidget *window;  /* GtkWindow toplevel */
	GtkWidget *vbox;    /* content box */
	GtkWidget *stack;   /* GtkStack of view widgets */
};

G_DEFINE_FINAL_TYPE(GsurfGtk4Window, gsurf_gtk4_window, GSURF_TYPE_WINDOW)

static guint
translate_modifiers(GdkModifierType state)
{
	guint mods = GSURF_MOD_NONE;

	if (state & GDK_SHIFT_MASK)   mods |= GSURF_MOD_SHIFT;
	if (state & GDK_CONTROL_MASK) mods |= GSURF_MOD_CTRL;
	if (state & GDK_ALT_MASK)     mods |= GSURF_MOD_ALT;
	if (state & GDK_SUPER_MASK)   mods |= GSURF_MOD_SUPER;

	return mods;
}

static gboolean
on_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
               GdkModifierType state, gpointer user_data)
{
	GsurfGtk4Window *self = user_data;
	return gsurf_window_emit_key_press(GSURF_WINDOW(self), keyval, keycode,
		translate_modifiers(state));
}

static gboolean
on_close_request(GtkWindow *window, gpointer user_data)
{
	GsurfGtk4Window *self = user_data;
	return gsurf_window_emit_close_request(GSURF_WINDOW(self));
}

static void
gsurf_gtk4_window_realize(GsurfWindow *window)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkEventController *key;

	self->window = gtk_window_new();
	self->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	self->stack = gtk_stack_new();

	gtk_widget_set_vexpand(self->stack, TRUE);
	gtk_box_append(GTK_BOX(self->vbox), self->stack);
	gtk_window_set_child(GTK_WINDOW(self->window), self->vbox);
	gtk_window_set_default_size(GTK_WINDOW(self->window),
		GSURF_DEFAULT_WIDTH, GSURF_DEFAULT_HEIGHT);

	key = gtk_event_controller_key_new();
	g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), self);
	gtk_widget_add_controller(self->window, key);

	g_signal_connect(self->window, "close-request",
		G_CALLBACK(on_close_request), self);
}

static void
gsurf_gtk4_window_insert_view(GsurfWindow *window, GsurfView *view)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkWidget *widget = gsurf_view_get_native_widget(view);

	if (widget != NULL)
		gtk_stack_add_child(GTK_STACK(self->stack), widget);
}

static void
gsurf_gtk4_window_remove_view(GsurfWindow *window, GsurfView *view)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkWidget *widget = gsurf_view_get_native_widget(view);

	if (widget != NULL)
		gtk_stack_remove(GTK_STACK(self->stack), widget);
}

static void
gsurf_gtk4_window_set_active_view(GsurfWindow *window, GsurfView *view)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkWidget *widget = gsurf_view_get_native_widget(view);

	if (widget != NULL)
		gtk_stack_set_visible_child(GTK_STACK(self->stack), widget);
}

static void
gsurf_gtk4_window_present(GsurfWindow *window)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	gtk_window_present(GTK_WINDOW(self->window));
}

static void
gsurf_gtk4_window_set_title(GsurfWindow *window, const gchar *title)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	gtk_window_set_title(GTK_WINDOW(self->window), title ? title : "gsurf");
}

static void
gsurf_gtk4_window_set_default_size(GsurfWindow *window, gint width, gint height)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	gtk_window_set_default_size(GTK_WINDOW(self->window), width, height);
}

static void
gsurf_gtk4_window_set_fullscreen(GsurfWindow *window, gboolean fullscreen)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	if (fullscreen)
		gtk_window_fullscreen(GTK_WINDOW(self->window));
	else
		gtk_window_unfullscreen(GTK_WINDOW(self->window));
}

static gpointer
gsurf_gtk4_window_get_native_widget(GsurfWindow *window)
{
	return GSURF_GTK4_WINDOW(window)->window;
}

static void
gsurf_gtk4_window_add_chrome_widget(GsurfWindow *window, gpointer widget, gboolean top)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkWidget *w = widget;

	if (w == NULL)
		return;
	if (top) {
		gtk_box_prepend(GTK_BOX(self->vbox), w);
	} else {
		gtk_box_append(GTK_BOX(self->vbox), w);
	}
}

static void
gsurf_gtk4_window_dispose(GObject *object)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(object);

	if (self->window != NULL) {
		gtk_window_destroy(GTK_WINDOW(self->window));
		self->window = NULL;
	}
	G_OBJECT_CLASS(gsurf_gtk4_window_parent_class)->dispose(object);
}

static void
gsurf_gtk4_window_class_init(GsurfGtk4WindowClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfWindowClass *window_class = GSURF_WINDOW_CLASS(klass);

	object_class->dispose = gsurf_gtk4_window_dispose;

	window_class->realize = gsurf_gtk4_window_realize;
	window_class->insert_view = gsurf_gtk4_window_insert_view;
	window_class->remove_view = gsurf_gtk4_window_remove_view;
	window_class->set_active_view = gsurf_gtk4_window_set_active_view;
	window_class->present = gsurf_gtk4_window_present;
	window_class->set_title = gsurf_gtk4_window_set_title;
	window_class->set_default_size = gsurf_gtk4_window_set_default_size;
	window_class->set_fullscreen = gsurf_gtk4_window_set_fullscreen;
	window_class->get_native_widget = gsurf_gtk4_window_get_native_widget;
	window_class->add_chrome_widget = gsurf_gtk4_window_add_chrome_widget;
}

static void
gsurf_gtk4_window_init(GsurfGtk4Window *self)
{
}

GsurfWindow *
gsurf_gtk4_window_new(void)
{
	return g_object_new(GSURF_TYPE_GTK4_WINDOW, NULL);
}
