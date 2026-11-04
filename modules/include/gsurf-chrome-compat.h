/*
 * gsurf-chrome-compat.h - GTK3/GTK4 helpers for the window-chrome modules
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Header-only, module-private compatibility layer shared by the chrome
 * modules (chromebar, find_bar, status_bar). Every helper is a static
 * inline wrapper that compiles to the GTK3 call when GSURF_BACKEND_GTK3 is
 * defined and to the non-deprecated GTK4 equivalent when
 * GSURF_BACKEND_GTK4 is defined, so the modules themselves carry no
 * backend #ifdefs.
 *
 * This header deliberately lives outside src/: it is not part of
 * libgsurf's public (installed, GIR-scanned) API. Modules include it as
 * "modules/include/gsurf-chrome-compat.h" (the repository root is on the
 * module include path).
 */

#ifndef GSURF_CHROME_COMPAT_H
#define GSURF_CHROME_COMPAT_H

#include <gtk/gtk.h>

#if !defined(GSURF_BACKEND_GTK3) && !defined(GSURF_BACKEND_GTK4)
#error "gsurf-chrome-compat.h needs GSURF_BACKEND_GTK3 or GSURF_BACKEND_GTK4"
#endif

G_BEGIN_DECLS

/*
 * GSURF_CHROME_KEY_CONTROLLER_NAME:
 *
 * Name given (on GTK4) to the GtkEventControllerKey that
 * gsurf_chrome_connect_key_press() attaches, so tests and the disconnect
 * helper can find it among a widget's controllers.
 */
#define GSURF_CHROME_KEY_CONTROLLER_NAME "gsurf-chrome-key"

/* Object-data keys used by the key-press helpers. */
#define GSURF_CHROME_KEY_FUNC_DATA       "gsurf-chrome-key-func"
#define GSURF_CHROME_KEY_CONTROLLER_DATA "gsurf-chrome-key-controller"

/**
 * GsurfChromeKeyFunc:
 * @widget: the chrome widget the key was pressed in
 * @keyval: the GDK keyval
 * @state: the modifier state at the time of the press
 * @user_data: the data passed to gsurf_chrome_connect_key_press()
 *
 * Backend-neutral key-press callback. It runs before the widget's own key
 * handling (GTK3 "key-press-event" handlers connected with
 * g_signal_connect() run before the class handler; on GTK4 the controller
 * uses the capture phase so it precedes the entry's inner #GtkText).
 *
 * Returns: %TRUE to consume the key, %FALSE to let the widget handle it
 */
typedef gboolean (*GsurfChromeKeyFunc)(GtkWidget       *widget,
                                       guint            keyval,
                                       GdkModifierType  state,
                                       gpointer         user_data);

/**
 * gsurf_chrome_editable_get_text:
 * @editable: a #GtkEntry (GTK3) or any #GtkEditable (GTK4, including
 *   #GtkSearchEntry, which is not a #GtkEntry there)
 *
 * Returns: (transfer none): the current text, never %NULL
 */
static inline const gchar *
gsurf_chrome_editable_get_text(GtkWidget *editable)
{
	const gchar *text;

#ifdef GSURF_BACKEND_GTK4
	text = gtk_editable_get_text(GTK_EDITABLE(editable));
#else
	text = gtk_entry_get_text(GTK_ENTRY(editable));
#endif
	return text != NULL ? text : "";
}

/**
 * gsurf_chrome_editable_set_text:
 * @editable: a #GtkEntry (GTK3) or any #GtkEditable (GTK4)
 * @text: (nullable): the new text; %NULL clears the entry
 *
 * Replaces the text of an entry.
 */
static inline void
gsurf_chrome_editable_set_text(GtkWidget *editable, const gchar *text)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_editable_set_text(GTK_EDITABLE(editable), text != NULL ? text : "");
#else
	gtk_entry_set_text(GTK_ENTRY(editable), text != NULL ? text : "");
#endif
}

/**
 * gsurf_chrome_widget_has_focus:
 * @widget: a chrome widget
 *
 * GTK3 entries take the focus themselves; a GTK4 #GtkEntry or
 * #GtkSearchEntry delegates it to an inner #GtkText, so "has focus" there
 * means the root's focus widget is @widget or one of its descendants.
 *
 * Returns: %TRUE if the keyboard focus is in @widget
 */
static inline gboolean
gsurf_chrome_widget_has_focus(GtkWidget *widget)
{
#ifdef GSURF_BACKEND_GTK4
	GtkRoot *root;
	GtkWidget *focus;

	root = gtk_widget_get_root(widget);
	if (root == NULL)
		return FALSE;
	focus = gtk_root_get_focus(root);
	return focus != NULL && (focus == widget || gtk_widget_is_ancestor(focus, widget));
#else
	return gtk_widget_has_focus(widget);
#endif
}

/**
 * gsurf_chrome_box_pack:
 * @box: a horizontal or vertical #GtkBox
 * @child: the widget to append
 * @expand: whether @child takes the extra space along the box's axis
 * @padding: pixels of space on both sides of @child along the box's axis
 *
 * Appends @child after the box's existing children, the GTK3
 * gtk_box_pack_start(@box, @child, @expand, @expand, @padding) layout. GTK4
 * has no per-child packing properties, so the expansion and padding become
 * the child's own expand flag and margins.
 */
static inline void
gsurf_chrome_box_pack(GtkWidget *box, GtkWidget *child, gboolean expand, guint padding)
{
#ifdef GSURF_BACKEND_GTK4
	/* Expansion and padding apply along the box's orientation only. */
	if (gtk_orientable_get_orientation(GTK_ORIENTABLE(box)) == GTK_ORIENTATION_HORIZONTAL) {
		gtk_widget_set_hexpand(child, expand);
		gtk_widget_set_margin_start(child, (gint)padding);
		gtk_widget_set_margin_end(child, (gint)padding);
	} else {
		gtk_widget_set_vexpand(child, expand);
		gtk_widget_set_margin_top(child, (gint)padding);
		gtk_widget_set_margin_bottom(child, (gint)padding);
	}
	gtk_box_append(GTK_BOX(box), child);
#else
	gtk_box_pack_start(GTK_BOX(box), child, expand, expand, padding);
#endif
}

/**
 * gsurf_chrome_widget_start_hidden:
 * @widget: a chrome widget already packed into the window chrome
 *
 * Hides a chrome widget (and keeps it hidden) until the module shows it
 * with gtk_widget_set_visible(). On GTK3 the window's add_chrome_widget
 * and its startup gtk_widget_show_all() would reveal it again, so its
 * children are shown once and no-show-all is set on the container. GTK4
 * widgets are visible by default and nothing re-shows them, so hiding the
 * container is enough.
 */
static inline void
gsurf_chrome_widget_start_hidden(GtkWidget *widget)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_widget_set_visible(widget, FALSE);
#else
	gtk_widget_show_all(widget);
	gtk_widget_set_visible(widget, FALSE);
	gtk_widget_set_no_show_all(widget, TRUE);
#endif
}

/**
 * gsurf_chrome_widget_detach:
 * @widget: a chrome widget the module retains a reference to
 *
 * Removes @widget from the window chrome without releasing the module's
 * own reference (drop that with g_clear_object() afterwards). On GTK3 this
 * is gtk_widget_destroy(), which also removes it from its container. On
 * GTK4 a widget is removed from its parent (gtk_box_remove() for a
 * #GtkBox, gtk_widget_unparent() otherwise); a widget whose host
 * container already died has no parent and is left alone. Safe to call
 * more than once.
 */
static inline void
gsurf_chrome_widget_detach(GtkWidget *widget)
{
#ifdef GSURF_BACKEND_GTK4
	GtkWidget *parent;

	parent = gtk_widget_get_parent(widget);
	if (parent == NULL)
		return;
	if (GTK_IS_BOX(parent))
		gtk_box_remove(GTK_BOX(parent), widget);
	else
		gtk_widget_unparent(widget);
#else
	gtk_widget_destroy(widget);
#endif
}

#ifdef GSURF_BACKEND_GTK4
/* GTK4 trampoline: "key-pressed" on the capture-phase key controller. */
static inline gboolean
gsurf_chrome_on_key_pressed(GtkEventControllerKey *controller,
                            guint                  keyval,
                            guint                  keycode,
                            GdkModifierType        state,
                            gpointer               user_data)
{
	GsurfChromeKeyFunc func;

	(void)keycode;
	func = (GsurfChromeKeyFunc)g_object_get_data(G_OBJECT(controller),
		GSURF_CHROME_KEY_FUNC_DATA);
	if (func == NULL)
		return FALSE;
	return func(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)),
		keyval, state, user_data);
}
#else
/* GTK3 trampoline: the widget's "key-press-event" signal. */
static inline gboolean
gsurf_chrome_on_key_press_event(GtkWidget   *widget,
                                GdkEventKey *event,
                                gpointer     user_data)
{
	GsurfChromeKeyFunc func;

	func = (GsurfChromeKeyFunc)g_object_get_data(G_OBJECT(widget),
		GSURF_CHROME_KEY_FUNC_DATA);
	if (func == NULL)
		return FALSE;
	return func(widget, event->keyval, (GdkModifierType)event->state, user_data);
}
#endif

/**
 * gsurf_chrome_connect_key_press:
 * @widget: the chrome widget (typically an entry)
 * @func: (scope notified): the handler
 * @user_data: (closure): data for @func; also the signal data, so a
 *   g_signal_handlers_disconnect_by_data() or
 *   gsurf_chrome_disconnect_key_press() with it removes the handler
 *
 * Calls @func for each key press in @widget before the widget's own key
 * handling. GTK3 connects "key-press-event"; GTK4 adds a capture-phase
 * #GtkEventControllerKey named %GSURF_CHROME_KEY_CONTROLLER_NAME. Only one
 * handler per widget is supported.
 */
static inline void
gsurf_chrome_connect_key_press(GtkWidget *widget, GsurfChromeKeyFunc func, gpointer user_data)
{
#ifdef GSURF_BACKEND_GTK4
	GtkEventController *controller;

	controller = gtk_event_controller_key_new();
	gtk_event_controller_set_name(controller, GSURF_CHROME_KEY_CONTROLLER_NAME);
	gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
	/* Function pointers cannot be stored as gpointer portably in ISO C,
	 * but GLib's own GCallback convention relies on this on every
	 * platform gsurf supports. */
	g_object_set_data(G_OBJECT(controller), GSURF_CHROME_KEY_FUNC_DATA, (gpointer)func);
	g_signal_connect(controller, "key-pressed",
		G_CALLBACK(gsurf_chrome_on_key_pressed), user_data);
	/* The widget takes ownership of the controller; remember it (borrowed)
	 * so gsurf_chrome_disconnect_key_press() can remove it again. */
	gtk_widget_add_controller(widget, controller);
	g_object_set_data(G_OBJECT(widget), GSURF_CHROME_KEY_CONTROLLER_DATA, controller);
#else
	g_object_set_data(G_OBJECT(widget), GSURF_CHROME_KEY_FUNC_DATA, (gpointer)func);
	g_signal_connect(widget, "key-press-event",
		G_CALLBACK(gsurf_chrome_on_key_press_event), user_data);
#endif
}

/**
 * gsurf_chrome_disconnect_key_press:
 * @widget: a widget passed to gsurf_chrome_connect_key_press()
 * @user_data: the data it was connected with
 *
 * Undoes gsurf_chrome_connect_key_press(): no key callback carrying
 * @user_data survives on @widget or (GTK4) its key controller, which is
 * also removed from @widget. Safe to call more than once.
 */
static inline void
gsurf_chrome_disconnect_key_press(GtkWidget *widget, gpointer user_data)
{
#ifdef GSURF_BACKEND_GTK4
	GtkEventController *controller;

	controller = g_object_get_data(G_OBJECT(widget), GSURF_CHROME_KEY_CONTROLLER_DATA);
	if (controller != NULL) {
		g_signal_handlers_disconnect_by_data(controller, user_data);
		g_object_set_data(G_OBJECT(widget), GSURF_CHROME_KEY_CONTROLLER_DATA, NULL);
		gtk_widget_remove_controller(widget, controller);
	}
#else
	g_signal_handlers_disconnect_by_data(widget, user_data);
	g_object_set_data(G_OBJECT(widget), GSURF_CHROME_KEY_FUNC_DATA, NULL);
#endif
}

G_END_DECLS

#endif /* GSURF_CHROME_COMPAT_H */
