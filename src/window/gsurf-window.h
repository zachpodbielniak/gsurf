/*
 * gsurf-window.h - Abstract browser window (container of views)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GsurfWindow is a backend-agnostic top-level window that holds one or
 * more #GsurfView instances (tabs/splits). The abstract base owns the
 * view list and active-view bookkeeping and emits view/key/button
 * signals; concrete subclasses (GsurfGtk3Window / GsurfGtk4Window)
 * implement the native widget operations. Mirrors gst's GstWindow.
 */

#ifndef GSURF_WINDOW_H
#define GSURF_WINDOW_H

#include <glib-object.h>

#include "gsurf-enums.h"
#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_WINDOW (gsurf_window_get_type())

G_DECLARE_DERIVABLE_TYPE(GsurfWindow, gsurf_window, GSURF, WINDOW, GObject)

/**
 * GsurfWindowClass:
 * @parent_class: the parent class
 * @realize: create the native toplevel + container
 * @insert_view: insert a view's native widget into the container
 * @remove_view: remove a view's native widget from the container
 * @set_active_view: make a view the visible/focused one
 * @present: show and raise the window
 * @set_title: set the window title
 * @set_default_size: set the default window size
 * @set_fullscreen: enter/leave fullscreen
 * @get_native_widget: the native toplevel widget (as gpointer)
 *
 * Virtual table for #GsurfWindow.
 */
struct _GsurfWindowClass
{
	GObjectClass parent_class;

	void     (*realize)           (GsurfWindow *self);
	void     (*insert_view)       (GsurfWindow *self, GsurfView *view);
	void     (*remove_view)       (GsurfWindow *self, GsurfView *view);
	void     (*set_active_view)   (GsurfWindow *self, GsurfView *view);
	void     (*present)           (GsurfWindow *self);
	void     (*set_title)         (GsurfWindow *self, const gchar *title);
	void     (*set_default_size)  (GsurfWindow *self, gint width, gint height);
	void     (*set_fullscreen)    (GsurfWindow *self, gboolean fullscreen);
	gpointer (*get_native_widget) (GsurfWindow *self);

	/* Chrome: pack a native widget above (@top) or below the view area
	 * (used by chromebar / status-bar modules). */
	void     (*add_chrome_widget) (GsurfWindow *self, gpointer widget, gboolean top);

	gpointer padding[8];
};

/* --- View management (handled by the abstract base) --- */
void        gsurf_window_add_view(GsurfWindow *self, GsurfView *view);
void        gsurf_window_remove_view(GsurfWindow *self, GsurfView *view);
void        gsurf_window_set_active_view(GsurfWindow *self, GsurfView *view);
GsurfView  *gsurf_window_get_active_view(GsurfWindow *self);
GPtrArray  *gsurf_window_get_views(GsurfWindow *self);
guint       gsurf_window_get_n_views(GsurfWindow *self);
GsurfView  *gsurf_window_get_nth_view(GsurfWindow *self, guint index);

/* --- Native operations --- */
void        gsurf_window_present(GsurfWindow *self);
void        gsurf_window_set_title(GsurfWindow *self, const gchar *title);
void        gsurf_window_set_default_size(GsurfWindow *self, gint width, gint height);
void        gsurf_window_set_fullscreen(GsurfWindow *self, gboolean fullscreen);
gboolean    gsurf_window_get_fullscreen(GsurfWindow *self);
gpointer    gsurf_window_get_native_widget(GsurfWindow *self);

/**
 * gsurf_window_add_top_widget:
 * @self: a #GsurfWindow
 * @widget: a native widget (a #GtkWidget as gpointer)
 *
 * Packs a chrome widget above the view area (e.g. an address bar).
 */
void        gsurf_window_add_top_widget(GsurfWindow *self, gpointer widget);

/**
 * gsurf_window_add_bottom_widget:
 * @self: a #GsurfWindow
 * @widget: a native widget (a #GtkWidget as gpointer)
 *
 * Packs a chrome widget below the view area (e.g. a status bar).
 */
void        gsurf_window_add_bottom_widget(GsurfWindow *self, gpointer widget);

/* --- Event emission helpers (for backend subclasses) --- */
gboolean    gsurf_window_emit_key_press(GsurfWindow *self, guint keyval, guint keycode, guint state);
gboolean    gsurf_window_emit_button_press(GsurfWindow *self, guint button, guint state);
gboolean    gsurf_window_emit_close_request(GsurfWindow *self);

G_END_DECLS

#endif /* GSURF_WINDOW_H */
