/*
 * gsurf-chrome-compat.h - GTK3/GTK4 helpers for the window-chrome modules
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Header-only, module-private compatibility layer shared by the chrome
 * modules (chromebar, find_bar, status_bar, tabs). Every helper is a static
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
#include <string.h>

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

/* ------------------------------------------------------------------ */
/* Widgets, styling and clicks (used by the tabs strip and sidebar)    */
/* ------------------------------------------------------------------ */

/*
 * GSURF_CHROME_CLICK_CONTROLLER_PREFIX:
 *
 * Prefix of the name given (on GTK4) to each GtkGestureClick that
 * gsurf_chrome_connect_click() attaches; the button number follows, so
 * the primary-button gesture is "gsurf-chrome-click-1".
 */
#define GSURF_CHROME_CLICK_CONTROLLER_PREFIX "gsurf-chrome-click-"

/* Object-data keys used by the click helpers. */
#define GSURF_CHROME_CLICK_FUNC_DATA      "gsurf-chrome-click-func"
#define GSURF_CHROME_CLICK_CONNECTED_DATA "gsurf-chrome-click-connected"

/**
 * GsurfChromeClickFunc:
 * @widget: the widget passed to gsurf_chrome_connect_click()
 * @user_data: the data passed to gsurf_chrome_connect_click()
 *
 * Backend-neutral single-click callback.
 */
typedef void (*GsurfChromeClickFunc)(GtkWidget *widget, gpointer user_data);

/**
 * gsurf_chrome_widget_show_all:
 * @widget: a chrome widget
 *
 * Makes @widget and its descendants visible. GTK3 widgets start hidden
 * and need gtk_widget_show_all(); GTK4 widgets are visible by default,
 * so only @widget itself is (re)shown.
 */
static inline void
gsurf_chrome_widget_show_all(GtkWidget *widget)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_widget_set_visible(widget, TRUE);
#else
	gtk_widget_show_all(widget);
#endif
}

/**
 * gsurf_chrome_widget_manage_visibility:
 * @widget: a chrome widget whose visibility the module controls
 *
 * Shows @widget and its descendants once and then leaves its visibility
 * to gtk_widget_set_visible() calls by the module. On GTK3 this also sets
 * no-show-all, so a later gtk_widget_show_all() of an ancestor (the
 * window's startup show-all, or a side panel being packed) does not
 * reveal a widget the module hid. GTK4 never re-shows widgets.
 */
static inline void
gsurf_chrome_widget_manage_visibility(GtkWidget *widget)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_widget_set_visible(widget, TRUE);
#else
	gtk_widget_show_all(widget);
	gtk_widget_set_no_show_all(widget, TRUE);
#endif
}

/**
 * gsurf_chrome_widget_add_class:
 * @widget: a widget
 * @css_class: a CSS class name
 *
 * Adds a CSS style class to @widget.
 */
static inline void
gsurf_chrome_widget_add_class(GtkWidget *widget, const gchar *css_class)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_widget_add_css_class(widget, css_class);
#else
	gtk_style_context_add_class(gtk_widget_get_style_context(widget), css_class);
#endif
}

/**
 * gsurf_chrome_widget_remove_class:
 * @widget: a widget
 * @css_class: a CSS class name
 *
 * Removes a CSS style class from @widget (no-op when it is not set).
 */
static inline void
gsurf_chrome_widget_remove_class(GtkWidget *widget, const gchar *css_class)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_widget_remove_css_class(widget, css_class);
#else
	gtk_style_context_remove_class(gtk_widget_get_style_context(widget), css_class);
#endif
}

/**
 * gsurf_chrome_widget_has_class:
 * @widget: a widget
 * @css_class: a CSS class name
 *
 * Returns: whether @widget carries the CSS class
 */
static inline gboolean
gsurf_chrome_widget_has_class(GtkWidget *widget, const gchar *css_class)
{
#ifdef GSURF_BACKEND_GTK4
	return gtk_widget_has_css_class(widget, css_class);
#else
	return gtk_style_context_has_class(gtk_widget_get_style_context(widget), css_class);
#endif
}

/**
 * gsurf_chrome_widget_set_class:
 * @widget: a widget
 * @css_class: a CSS class name
 * @set: whether the class should be present
 *
 * Adds or removes a CSS class.
 */
static inline void
gsurf_chrome_widget_set_class(GtkWidget *widget, const gchar *css_class, gboolean set)
{
	if (set)
		gsurf_chrome_widget_add_class(widget, css_class);
	else
		gsurf_chrome_widget_remove_class(widget, css_class);
}

/**
 * gsurf_chrome_button_new_flat:
 * @label: (nullable): the button label
 *
 * Creates a frameless button that does not take keyboard focus when
 * clicked, so clicking chrome never parks the focus away from the page.
 *
 * Returns: (transfer floating): the button
 */
static inline GtkWidget *
gsurf_chrome_button_new_flat(const gchar *label)
{
	GtkWidget *button;

	button = gtk_button_new_with_label(label != NULL ? label : "");
#ifdef GSURF_BACKEND_GTK4
	gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
#else
	gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
#endif
	gtk_widget_set_focus_on_click(button, FALSE);
	return button;
}

/**
 * gsurf_chrome_scrolled_new:
 * @child: the widget to scroll
 *
 * Wraps @child in a vertically scrolling #GtkScrolledWindow (never
 * scrolling horizontally). A non-scrollable child gets a viewport on
 * both backends.
 *
 * Returns: (transfer floating): the scrolled window
 */
static inline GtkWidget *
gsurf_chrome_scrolled_new(GtkWidget *child)
{
	GtkWidget *scrolled;

#ifdef GSURF_BACKEND_GTK4
	scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), child);
#else
	scrolled = gtk_scrolled_window_new(NULL, NULL);
	gtk_container_add(GTK_CONTAINER(scrolled), child);
#endif
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scrolled, TRUE);
	return scrolled;
}

/**
 * gsurf_chrome_box_clear:
 * @box: a #GtkBox
 *
 * Removes (GTK3: destroys) every child of @box.
 */
static inline void
gsurf_chrome_box_clear(GtkWidget *box)
{
#ifdef GSURF_BACKEND_GTK4
	GtkWidget *child;

	while ((child = gtk_widget_get_first_child(box)) != NULL)
		gtk_box_remove(GTK_BOX(box), child);
#else
	GList *children;
	GList *l;

	children = gtk_container_get_children(GTK_CONTAINER(box));
	for (l = children; l != NULL; l = l->next)
		gtk_widget_destroy(GTK_WIDGET(l->data));
	g_list_free(children);
#endif
}

/**
 * gsurf_chrome_box_move_child:
 * @box: a #GtkBox
 * @child: a child of @box
 * @index: its new position among the box's children
 *
 * Moves @child to position @index (positions past the end move it last).
 */
static inline void
gsurf_chrome_box_move_child(GtkWidget *box, GtkWidget *child, guint index)
{
#ifdef GSURF_BACKEND_GTK4
	GtkWidget *sibling;
	GtkWidget *after = NULL;
	guint pos = 0;

	/* GTK4 positions a child after a sibling: find the sibling that will
	 * precede @child, counting every child except @child itself. */
	for (sibling = gtk_widget_get_first_child(box); sibling != NULL && pos < index;
	     sibling = gtk_widget_get_next_sibling(sibling)) {
		if (sibling == child)
			continue;
		after = sibling;
		pos++;
	}
	gtk_box_reorder_child_after(GTK_BOX(box), child, after);
#else
	gtk_box_reorder_child(GTK_BOX(box), child, (gint)MIN(index, (guint)G_MAXINT));
#endif
}

/**
 * gsurf_chrome_image_set_native:
 * @image: a #GtkImage
 * @native: (nullable): the backend's native image: a cairo_surface_t on
 *   GTK3, a #GdkPaintable (such as a #GdkTexture) on GTK4, as returned by
 *   gsurf_view_get_favicon()
 * @size: the edge length in pixels to show it at
 *
 * Shows a favicon-like native image at @size x @size, or clears @image
 * when @native is %NULL. A larger GTK3 image surface is scaled down.
 */
static inline void
gsurf_chrome_image_set_native(GtkWidget *image, gpointer native, gint size)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_image_set_pixel_size(GTK_IMAGE(image), size);
	if (native == NULL || !GDK_IS_PAINTABLE(native)) {
		gtk_image_clear(GTK_IMAGE(image));
		return;
	}
	gtk_image_set_from_paintable(GTK_IMAGE(image), GDK_PAINTABLE(native));
#else
	cairo_surface_t *src = native;
	cairo_surface_t *scaled;
	cairo_t *cr;
	gint w;
	gint h;

	if (src == NULL || cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
		gtk_image_clear(GTK_IMAGE(image));
		return;
	}
	if (cairo_surface_get_type(src) != CAIRO_SURFACE_TYPE_IMAGE) {
		gtk_image_set_from_surface(GTK_IMAGE(image), src);
		return;
	}
	w = cairo_image_surface_get_width(src);
	h = cairo_image_surface_get_height(src);
	if (w <= 0 || h <= 0 || (w == size && h == size)) {
		gtk_image_set_from_surface(GTK_IMAGE(image), src);
		return;
	}

	/* Paint the favicon scaled into a size x size surface. */
	scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cr = cairo_create(scaled);
	cairo_scale(cr, (gdouble)size / (gdouble)w, (gdouble)size / (gdouble)h);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	gtk_image_set_from_surface(GTK_IMAGE(image), scaled);
	cairo_surface_destroy(scaled);
#endif
}

/**
 * gsurf_chrome_css_provider_new:
 * @css: the stylesheet
 *
 * Creates a CSS provider holding @css and attaches it to the default
 * screen (GTK3) or display (GTK4) at application priority. Remove it with
 * gsurf_chrome_css_provider_remove().
 *
 * Returns: (transfer full): the provider
 */
static inline GtkCssProvider *
gsurf_chrome_css_provider_new(const gchar *css)
{
	GtkCssProvider *provider;

	provider = gtk_css_provider_new();
#ifdef GSURF_BACKEND_GTK4
	gtk_css_provider_load_from_string(provider, css);
	if (gdk_display_get_default() != NULL)
		gtk_style_context_add_provider_for_display(gdk_display_get_default(),
			GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
#else
	gtk_css_provider_load_from_data(provider, css, -1, NULL);
	if (gdk_screen_get_default() != NULL)
		gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
			GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
#endif
	return provider;
}

/**
 * gsurf_chrome_css_provider_reload:
 * @provider: a provider from gsurf_chrome_css_provider_new()
 * @css: the replacement stylesheet
 *
 * Replaces the provider's stylesheet; styled widgets update.
 */
static inline void
gsurf_chrome_css_provider_reload(GtkCssProvider *provider, const gchar *css)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_css_provider_load_from_string(provider, css);
#else
	gtk_css_provider_load_from_data(provider, css, -1, NULL);
#endif
}

/**
 * gsurf_chrome_css_provider_remove:
 * @provider: a provider from gsurf_chrome_css_provider_new()
 *
 * Detaches the provider from the default screen or display. The caller
 * still owns its reference.
 */
static inline void
gsurf_chrome_css_provider_remove(GtkCssProvider *provider)
{
#ifdef GSURF_BACKEND_GTK4
	if (gdk_display_get_default() != NULL)
		gtk_style_context_remove_provider_for_display(gdk_display_get_default(),
			GTK_STYLE_PROVIDER(provider));
#else
	if (gdk_screen_get_default() != NULL)
		gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(),
			GTK_STYLE_PROVIDER(provider));
#endif
}

/* TRUE when @hit (a widget under the pointer inside @owner) is, or sits
 * inside, a #GtkButton nested in @owner: such presses belong to that
 * button (a close or twisty button inside a clickable row). */
static inline gboolean
gsurf_chrome_hit_is_nested_button(GtkWidget *owner, GtkWidget *hit)
{
	GtkWidget *w;

	for (w = hit; w != NULL && w != owner; w = gtk_widget_get_parent(w)) {
		if (GTK_IS_BUTTON(w))
			return TRUE;
	}
	return FALSE;
}

/* The per-button callback key ("gsurf-chrome-click-func-<button>"). */
static inline gchar *
gsurf_chrome_click_key(guint button)
{
	return g_strdup_printf(GSURF_CHROME_CLICK_FUNC_DATA "-%u", button);
}

#ifdef GSURF_BACKEND_GTK4
/* GTK4 trampoline: "pressed" on a per-button GtkGestureClick. */
static inline void
gsurf_chrome_on_click_pressed(GtkGestureClick *gesture,
                              gint             n_press,
                              gdouble          x,
                              gdouble          y,
                              gpointer         user_data)
{
	GtkWidget *widget;
	GtkWidget *hit;
	GsurfChromeClickFunc func;

	if (n_press != 1)
		return;
	widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	hit = gtk_widget_pick(widget, x, y, GTK_PICK_DEFAULT);
	if (gsurf_chrome_hit_is_nested_button(widget, hit))
		return;
	func = (GsurfChromeClickFunc)g_object_get_data(G_OBJECT(gesture),
		GSURF_CHROME_CLICK_FUNC_DATA);
	if (func != NULL)
		func(widget, user_data);
}
#else
/* GTK3 trampoline: the widget's "button-press-event" signal. */
static inline gboolean
gsurf_chrome_on_button_press_event(GtkWidget      *widget,
                                   GdkEventButton *event,
                                   gpointer        user_data)
{
	g_autofree gchar *key = NULL;
	GsurfChromeClickFunc func;
	gpointer hit = NULL;

	if (event->type != GDK_BUTTON_PRESS)
		return FALSE;
	/* A press on a nested button's own input window is that button's. */
	if (event->window != NULL)
		gdk_window_get_user_data(event->window, &hit);
	if (hit != NULL && GTK_IS_WIDGET(hit) &&
	    gsurf_chrome_hit_is_nested_button(widget, GTK_WIDGET(hit)))
		return FALSE;
	key = gsurf_chrome_click_key(event->button);
	func = (GsurfChromeClickFunc)g_object_get_data(G_OBJECT(widget), key);
	if (func == NULL)
		return FALSE;
	func(widget, user_data);
	return TRUE;
}
#endif

/**
 * gsurf_chrome_clickable_new:
 * @child: the content to make clickable
 *
 * Returns the widget to pass to gsurf_chrome_connect_click() for clicks
 * on @child: on GTK3 a #GtkEventBox holding @child (plain boxes and
 * labels have no input window there); on GTK4 @child itself, since any
 * widget accepts a gesture.
 *
 * Returns: (transfer floating): the clickable widget
 */
static inline GtkWidget *
gsurf_chrome_clickable_new(GtkWidget *child)
{
#ifdef GSURF_BACKEND_GTK4
	return child;
#else
	GtkWidget *event_box;

	event_box = gtk_event_box_new();
	gtk_container_add(GTK_CONTAINER(event_box), child);
	return event_box;
#endif
}

/**
 * gsurf_chrome_connect_click:
 * @widget: a widget from gsurf_chrome_clickable_new()
 * @button: the mouse button (1 = primary, 2 = middle, 3 = secondary)
 * @func: (scope notified): the handler
 * @user_data: (closure): data for @func; also the signal data, so
 *   gsurf_chrome_disconnect_click() with it removes the handler
 *
 * Calls @func for each single press of @button on @widget, except presses
 * on a #GtkButton nested inside @widget, which stay with that button.
 * GTK3 connects "button-press-event"; GTK4 adds a #GtkGestureClick named
 * %GSURF_CHROME_CLICK_CONTROLLER_PREFIX followed by the button number.
 * One handler per button and widget.
 */
static inline void
gsurf_chrome_connect_click(GtkWidget            *widget,
                           guint                 button,
                           GsurfChromeClickFunc  func,
                           gpointer              user_data)
{
#ifdef GSURF_BACKEND_GTK4
	GtkGesture *gesture;
	g_autofree gchar *name = NULL;

	name = g_strdup_printf(GSURF_CHROME_CLICK_CONTROLLER_PREFIX "%u", button);
	gesture = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), button);
	gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(gesture), name);
	g_object_set_data(G_OBJECT(gesture), GSURF_CHROME_CLICK_FUNC_DATA, (gpointer)func);
	g_signal_connect(gesture, "pressed", G_CALLBACK(gsurf_chrome_on_click_pressed), user_data);
	/* The widget owns the gesture. */
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(gesture));
#else
	g_autofree gchar *key = NULL;

	key = gsurf_chrome_click_key(button);
	g_object_set_data(G_OBJECT(widget), key, (gpointer)func);
	/* One signal handler serves every button of this widget and data. */
	if (g_object_get_data(G_OBJECT(widget), GSURF_CHROME_CLICK_CONNECTED_DATA) != user_data) {
		gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK);
		g_signal_connect(widget, "button-press-event",
			G_CALLBACK(gsurf_chrome_on_button_press_event), user_data);
		g_object_set_data(G_OBJECT(widget), GSURF_CHROME_CLICK_CONNECTED_DATA, user_data);
	}
#endif
}

/**
 * gsurf_chrome_disconnect_click:
 * @widget: a widget passed to gsurf_chrome_connect_click()
 * @user_data: the data it was connected with
 *
 * Undoes every gsurf_chrome_connect_click() and
 * gsurf_chrome_connect_release() on @widget made with @user_data; on
 * GTK4 the gestures are removed from @widget too. Safe to call more than
 * once.
 */
static inline void
gsurf_chrome_disconnect_click(GtkWidget *widget, gpointer user_data)
{
#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GListModel) controllers = NULL;
	g_autoptr(GPtrArray) doomed = NULL;
	guint i;
	guint n;

	/* Collect first: removing a controller changes the observed list. */
	doomed = g_ptr_array_new_with_free_func(g_object_unref);
	controllers = gtk_widget_observe_controllers(widget);
	n = g_list_model_get_n_items(controllers);
	for (i = 0; i < n; i++) {
		GtkEventController *c = g_list_model_get_item(controllers, i);
		const gchar *name = gtk_event_controller_get_name(c);

		/* Release gestures too ("gsurf-chrome-release-N"). */
		if (name != NULL &&
		    (g_str_has_prefix(name, GSURF_CHROME_CLICK_CONTROLLER_PREFIX) ||
		     g_str_has_prefix(name, "gsurf-chrome-release-")) &&
		    g_signal_handlers_disconnect_by_data(c, user_data) > 0)
			g_ptr_array_add(doomed, c);
		else
			g_object_unref(c);
	}
	for (i = 0; i < doomed->len; i++)
		gtk_widget_remove_controller(widget, g_ptr_array_index(doomed, i));
#else
	g_signal_handlers_disconnect_by_data(widget, user_data);
	if (g_object_get_data(G_OBJECT(widget), GSURF_CHROME_CLICK_CONNECTED_DATA) == user_data)
		g_object_set_data(G_OBJECT(widget), GSURF_CHROME_CLICK_CONNECTED_DATA, NULL);
	if (g_object_get_data(G_OBJECT(widget), "gsurf-chrome-release-connected") == user_data)
		g_object_set_data(G_OBJECT(widget), "gsurf-chrome-release-connected", NULL);
#endif
}

/**
 * gsurf_chrome_widget_get_height:
 * @widget: a widget
 *
 * Returns: the allocated height of @widget in pixels (0 before the
 *   first allocation)
 */
static inline gint
gsurf_chrome_widget_get_height(GtkWidget *widget)
{
#ifdef GSURF_BACKEND_GTK4
	return gtk_widget_get_height(widget);
#else
	return gtk_widget_get_allocated_height(widget);
#endif
}

/**
 * gsurf_chrome_widget_get_width:
 * @widget: a widget
 *
 * Returns: the allocated width of @widget in pixels (0 before the first
 *   allocation)
 */
static inline gint
gsurf_chrome_widget_get_width(GtkWidget *widget)
{
#ifdef GSURF_BACKEND_GTK4
	return gtk_widget_get_width(widget);
#else
	return gtk_widget_get_allocated_width(widget);
#endif
}

/* ------------------------------------------------------------------ */
/* Releases (a click that may also start a drag)                       */
/* ------------------------------------------------------------------ */

/*
 * GSURF_CHROME_RELEASE_CONTROLLER_PREFIX:
 *
 * Prefix of the name given (on GTK4) to each GtkGestureClick that
 * gsurf_chrome_connect_release() attaches; the button number follows.
 */
#define GSURF_CHROME_RELEASE_CONTROLLER_PREFIX "gsurf-chrome-release-"

/* Object-data key of the GTK3 release callbacks ("...-<button>"). */
#define GSURF_CHROME_RELEASE_FUNC_DATA "gsurf-chrome-release-func"
#define GSURF_CHROME_RELEASE_CONNECTED_DATA "gsurf-chrome-release-connected"

#ifdef GSURF_BACKEND_GTK4
/* GTK4 trampoline: "released" on a per-button GtkGestureClick. A drag
 * source on the same widget claims the sequence once a drag starts, so
 * the gesture is cancelled and never reports that release. */
static inline void
gsurf_chrome_on_click_released(GtkGestureClick *gesture,
                               gint             n_press,
                               gdouble          x,
                               gdouble          y,
                               gpointer         user_data)
{
	GtkWidget *widget;
	GtkWidget *hit;
	GsurfChromeClickFunc func;

	if (n_press != 1)
		return;
	widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	hit = gtk_widget_pick(widget, x, y, GTK_PICK_DEFAULT);
	if (gsurf_chrome_hit_is_nested_button(widget, hit))
		return;
	func = (GsurfChromeClickFunc)g_object_get_data(G_OBJECT(gesture),
		GSURF_CHROME_CLICK_FUNC_DATA);
	if (func != NULL)
		func(widget, user_data);
}
#else
/* GTK3 trampoline: the widget's "button-release-event" signal. Once a
 * drag starts, the drag owns the pointer and the release never comes. */
static inline gboolean
gsurf_chrome_on_button_release_event(GtkWidget      *widget,
                                     GdkEventButton *event,
                                     gpointer        user_data)
{
	g_autofree gchar *key = NULL;
	GsurfChromeClickFunc func;
	gpointer hit = NULL;

	if (event->type != GDK_BUTTON_RELEASE)
		return FALSE;
	if (event->window != NULL)
		gdk_window_get_user_data(event->window, &hit);
	if (hit != NULL && GTK_IS_WIDGET(hit) &&
	    gsurf_chrome_hit_is_nested_button(widget, GTK_WIDGET(hit)))
		return FALSE;
	key = g_strdup_printf(GSURF_CHROME_RELEASE_FUNC_DATA "-%u", event->button);
	func = (GsurfChromeClickFunc)g_object_get_data(G_OBJECT(widget), key);
	if (func == NULL)
		return FALSE;
	func(widget, user_data);
	return TRUE;
}
#endif

/**
 * gsurf_chrome_connect_release:
 * @widget: a widget from gsurf_chrome_clickable_new()
 * @button: the mouse button
 * @func: (scope notified): the handler
 * @user_data: (closure): data for @func; also the signal data, so
 *   gsurf_chrome_disconnect_click() with it removes the handler
 *
 * Like gsurf_chrome_connect_click(), but @func runs when the button is
 * released over @widget without a drag having started in between: a
 * widget that is also a drag source (gsurf_chrome_drag_source_add())
 * acts on a plain click only. GTK4 adds a #GtkGestureClick named
 * %GSURF_CHROME_RELEASE_CONTROLLER_PREFIX plus the button number.
 */
static inline void
gsurf_chrome_connect_release(GtkWidget            *widget,
                             guint                 button,
                             GsurfChromeClickFunc  func,
                             gpointer              user_data)
{
#ifdef GSURF_BACKEND_GTK4
	GtkGesture *gesture;
	g_autofree gchar *name = NULL;

	name = g_strdup_printf(GSURF_CHROME_RELEASE_CONTROLLER_PREFIX "%u", button);
	gesture = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), button);
	gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(gesture), name);
	g_object_set_data(G_OBJECT(gesture), GSURF_CHROME_CLICK_FUNC_DATA, (gpointer)func);
	g_signal_connect(gesture, "released", G_CALLBACK(gsurf_chrome_on_click_released), user_data);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(gesture));
#else
	g_autofree gchar *key = NULL;

	key = g_strdup_printf(GSURF_CHROME_RELEASE_FUNC_DATA "-%u", button);
	g_object_set_data(G_OBJECT(widget), key, (gpointer)func);
	if (g_object_get_data(G_OBJECT(widget), GSURF_CHROME_RELEASE_CONNECTED_DATA) != user_data) {
		gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
		g_signal_connect(widget, "button-release-event",
			G_CALLBACK(gsurf_chrome_on_button_release_event), user_data);
		g_object_set_data(G_OBJECT(widget), GSURF_CHROME_RELEASE_CONNECTED_DATA, user_data);
	}
#endif
}

/* ------------------------------------------------------------------ */
/* Drag and drop of a private, same-application target                 */
/* ------------------------------------------------------------------ */

/* Controller names (GTK4) and object-data keys of the DnD helpers. */
#define GSURF_CHROME_DRAG_SOURCE_NAME  "gsurf-chrome-drag-source"
#define GSURF_CHROME_DROP_TARGET_NAME  "gsurf-chrome-drop-target"
#define GSURF_CHROME_DRAG_FUNCS_DATA   "gsurf-chrome-drag-funcs"
#define GSURF_CHROME_DROP_FUNCS_DATA   "gsurf-chrome-drop-funcs"
#define GSURF_CHROME_DND_MIME_DATA     "gsurf-chrome-dnd-mime"

/**
 * GsurfChromeDragFuncs:
 * @payload: the data the drag carries (transfer full), or %NULL to
 *   refuse the drag
 * @begin: (nullable): the drag started
 * @end: (nullable): the drag ended, dropped or not
 *
 * What a drag source asks of its owner. Every callback gets the widget
 * and the data given to gsurf_chrome_drag_source_add(). The table must
 * outlive the widget (a static const).
 */
typedef struct {
	gchar *(*payload) (GtkWidget *widget, gpointer user_data);
	void   (*begin)   (GtkWidget *widget, gpointer user_data);
	void   (*end)     (GtkWidget *widget, gpointer user_data);
} GsurfChromeDragFuncs;

/**
 * GsurfChromeDropFuncs:
 * @motion: the drag is over (@x, @y) in the widget's coordinates: show
 *   where a drop would go; %TRUE when a drop there is accepted
 * @leave: (nullable): the drag left the widget (or dropped): hide it
 * @drop: a drop at (@x, @y): act on it; %TRUE when it was taken
 *
 * What a drop target asks of its owner (see #GsurfChromeDragFuncs).
 */
typedef struct {
	gboolean (*motion) (GtkWidget *widget, gint x, gint y, gpointer user_data);
	void     (*leave)  (GtkWidget *widget, gpointer user_data);
	gboolean (*drop)   (GtkWidget *widget, gint x, gint y, gpointer user_data);
} GsurfChromeDropFuncs;

#ifdef GSURF_BACKEND_GTK4
static inline GdkContentProvider *
gsurf_chrome_on_drag_prepare(GtkDragSource *source, gdouble x, gdouble y, gpointer user_data)
{
	GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
	const GsurfChromeDragFuncs *funcs = g_object_get_data(G_OBJECT(source),
		GSURF_CHROME_DRAG_FUNCS_DATA);
	const gchar *mime = g_object_get_data(G_OBJECT(source), GSURF_CHROME_DND_MIME_DATA);
	gchar *payload;
	gsize len;

	(void)x;
	(void)y;
	if (funcs == NULL || mime == NULL || (payload = funcs->payload(widget, user_data)) == NULL)
		return NULL;
	len = strlen(payload);
	return gdk_content_provider_new_for_bytes(mime, g_bytes_new_take(payload, len));
}

static inline void
gsurf_chrome_on_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data)
{
	const GsurfChromeDragFuncs *funcs = g_object_get_data(G_OBJECT(source),
		GSURF_CHROME_DRAG_FUNCS_DATA);

	(void)drag;
	if (funcs != NULL && funcs->begin != NULL)
		funcs->begin(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source)), user_data);
}

static inline void
gsurf_chrome_on_drag_end(GtkDragSource *source, GdkDrag *drag, gboolean delete_data,
                         gpointer user_data)
{
	const GsurfChromeDragFuncs *funcs = g_object_get_data(G_OBJECT(source),
		GSURF_CHROME_DRAG_FUNCS_DATA);

	(void)drag;
	(void)delete_data;
	if (funcs != NULL && funcs->end != NULL)
		funcs->end(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source)), user_data);
}

static inline GdkDragAction
gsurf_chrome_on_drop_motion(GtkDropTargetAsync *target, GdkDrop *drop, gdouble x, gdouble y,
                            gpointer user_data)
{
	const GsurfChromeDropFuncs *funcs = g_object_get_data(G_OBJECT(target),
		GSURF_CHROME_DROP_FUNCS_DATA);

	(void)drop;
	if (funcs == NULL ||
	    !funcs->motion(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)),
	                   (gint)x, (gint)y, user_data))
		return 0;
	return GDK_ACTION_MOVE;
}

static inline void
gsurf_chrome_on_drop_leave(GtkDropTargetAsync *target, GdkDrop *drop, gpointer user_data)
{
	const GsurfChromeDropFuncs *funcs = g_object_get_data(G_OBJECT(target),
		GSURF_CHROME_DROP_FUNCS_DATA);

	(void)drop;
	if (funcs != NULL && funcs->leave != NULL)
		funcs->leave(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)), user_data);
}

static inline gboolean
gsurf_chrome_on_drop(GtkDropTargetAsync *target, GdkDrop *drop, gdouble x, gdouble y,
                     gpointer user_data)
{
	const GsurfChromeDropFuncs *funcs = g_object_get_data(G_OBJECT(target),
		GSURF_CHROME_DROP_FUNCS_DATA);
	GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
	gboolean taken;

	taken = funcs != NULL && funcs->drop(widget, (gint)x, (gint)y, user_data);
	if (funcs != NULL && funcs->leave != NULL)
		funcs->leave(widget, user_data);
	/* A test drives the handler without a real drop. */
	if (drop != NULL)
		gdk_drop_finish(drop, taken ? GDK_ACTION_MOVE : 0);
	return taken;
}
#else
static inline void
gsurf_chrome_on_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
	const GsurfChromeDragFuncs *funcs = g_object_get_data(G_OBJECT(widget),
		GSURF_CHROME_DRAG_FUNCS_DATA);

	(void)context;
	if (funcs != NULL && funcs->begin != NULL)
		funcs->begin(widget, user_data);
}

static inline void
gsurf_chrome_on_drag_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
	const GsurfChromeDragFuncs *funcs = g_object_get_data(G_OBJECT(widget),
		GSURF_CHROME_DRAG_FUNCS_DATA);

	(void)context;
	if (funcs != NULL && funcs->end != NULL)
		funcs->end(widget, user_data);
}

static inline void
gsurf_chrome_on_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                              GtkSelectionData *data, guint info, guint time_,
                              gpointer user_data)
{
	const GsurfChromeDragFuncs *funcs = g_object_get_data(G_OBJECT(widget),
		GSURF_CHROME_DRAG_FUNCS_DATA);
	g_autofree gchar *payload = NULL;

	(void)context;
	(void)info;
	(void)time_;
	if (funcs == NULL || (payload = funcs->payload(widget, user_data)) == NULL)
		return;
	gtk_selection_data_set(data, gtk_selection_data_get_target(data), 8,
		(const guchar *)payload, (gint)strlen(payload));
}

static inline gboolean
gsurf_chrome_on_drag_motion(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                            guint time_, gpointer user_data)
{
	const GsurfChromeDropFuncs *funcs = g_object_get_data(G_OBJECT(widget),
		GSURF_CHROME_DROP_FUNCS_DATA);
	gboolean ok;

	ok = funcs != NULL && funcs->motion(widget, x, y, user_data);
	/* A test drives the handler without a real drag context. */
	if (context != NULL)
		gdk_drag_status(context, ok ? GDK_ACTION_MOVE : 0, time_);
	return ok;
}

static inline void
gsurf_chrome_on_drag_leave(GtkWidget *widget, GdkDragContext *context, guint time_,
                           gpointer user_data)
{
	const GsurfChromeDropFuncs *funcs = g_object_get_data(G_OBJECT(widget),
		GSURF_CHROME_DROP_FUNCS_DATA);

	(void)context;
	(void)time_;
	if (funcs != NULL && funcs->leave != NULL)
		funcs->leave(widget, user_data);
}

static inline gboolean
gsurf_chrome_on_drag_drop(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                          guint time_, gpointer user_data)
{
	const GsurfChromeDropFuncs *funcs = g_object_get_data(G_OBJECT(widget),
		GSURF_CHROME_DROP_FUNCS_DATA);
	gboolean taken;

	taken = funcs != NULL && funcs->drop(widget, x, y, user_data);
	if (funcs != NULL && funcs->leave != NULL)
		funcs->leave(widget, user_data);
	if (context != NULL)
		gtk_drag_finish(context, taken, FALSE, time_);
	return TRUE;
}
#endif

/**
 * gsurf_chrome_drag_source_add:
 * @widget: the widget to drag (GTK3: one with its own window, such as a
 *   widget from gsurf_chrome_clickable_new())
 * @mime: the private target, e.g. %GSURF_LAYOUT_VIEW_MIME_TYPE (static)
 * @funcs: (not nullable): the callbacks, outliving @widget
 * @user_data: (closure): their data; also the signal data, so
 *   gsurf_chrome_disconnect_dnd() with it removes the source
 *
 * Makes @widget a primary-button drag source for @mime within the
 * application. GTK3: gtk_drag_source_set() with a same-app target; call
 * it before gsurf_chrome_connect_click() so the drag sees the press
 * first. GTK4: a #GtkDragSource named %GSURF_CHROME_DRAG_SOURCE_NAME.
 */
static inline void
gsurf_chrome_drag_source_add(GtkWidget                  *widget,
                             const gchar                *mime,
                             const GsurfChromeDragFuncs *funcs,
                             gpointer                    user_data)
{
#ifdef GSURF_BACKEND_GTK4
	GtkDragSource *source = gtk_drag_source_new();

	gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
	gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(source), GSURF_CHROME_DRAG_SOURCE_NAME);
	g_object_set_data(G_OBJECT(source), GSURF_CHROME_DRAG_FUNCS_DATA, (gpointer)funcs);
	g_object_set_data(G_OBJECT(source), GSURF_CHROME_DND_MIME_DATA, (gpointer)mime);
	g_signal_connect(source, "prepare", G_CALLBACK(gsurf_chrome_on_drag_prepare), user_data);
	g_signal_connect(source, "drag-begin", G_CALLBACK(gsurf_chrome_on_drag_begin), user_data);
	g_signal_connect(source, "drag-end", G_CALLBACK(gsurf_chrome_on_drag_end), user_data);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(source));
#else
	GtkTargetEntry entry = { (gchar *)mime, GTK_TARGET_SAME_APP, 0 };

	gtk_drag_source_set(widget, GDK_BUTTON1_MASK, &entry, 1, GDK_ACTION_MOVE);
	g_object_set_data(G_OBJECT(widget), GSURF_CHROME_DRAG_FUNCS_DATA, (gpointer)funcs);
	g_signal_connect(widget, "drag-begin", G_CALLBACK(gsurf_chrome_on_drag_begin), user_data);
	g_signal_connect(widget, "drag-end", G_CALLBACK(gsurf_chrome_on_drag_end), user_data);
	g_signal_connect(widget, "drag-data-get", G_CALLBACK(gsurf_chrome_on_drag_data_get),
		user_data);
#endif
}

/**
 * gsurf_chrome_drop_target_add:
 * @widget: the widget to drop on (GTK3: one with its own window)
 * @mime: the private target it accepts (static)
 * @funcs: (not nullable): the callbacks, outliving @widget
 * @user_data: (closure): their data, as for gsurf_chrome_drag_source_add()
 *
 * Makes @widget accept drops of @mime (move only). GTK3:
 * gtk_drag_dest_set() with no default behaviour, the drag-motion /
 * drag-leave / drag-drop signals; GTK4: a #GtkDropTargetAsync named
 * %GSURF_CHROME_DROP_TARGET_NAME. The owner knows what is dragged (it
 * started the drag), so the drop never reads the data back.
 */
static inline void
gsurf_chrome_drop_target_add(GtkWidget                  *widget,
                             const gchar                *mime,
                             const GsurfChromeDropFuncs *funcs,
                             gpointer                    user_data)
{
#ifdef GSURF_BACKEND_GTK4
	const gchar *mimes[] = { mime, NULL };
	GdkContentFormats *formats = gdk_content_formats_new(mimes, 1);
	GtkDropTargetAsync *target = gtk_drop_target_async_new(formats, GDK_ACTION_MOVE);

	gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(target), GSURF_CHROME_DROP_TARGET_NAME);
	g_object_set_data(G_OBJECT(target), GSURF_CHROME_DROP_FUNCS_DATA, (gpointer)funcs);
	g_signal_connect(target, "drag-enter", G_CALLBACK(gsurf_chrome_on_drop_motion), user_data);
	g_signal_connect(target, "drag-motion", G_CALLBACK(gsurf_chrome_on_drop_motion), user_data);
	g_signal_connect(target, "drag-leave", G_CALLBACK(gsurf_chrome_on_drop_leave), user_data);
	g_signal_connect(target, "drop", G_CALLBACK(gsurf_chrome_on_drop), user_data);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(target));
#else
	GtkTargetEntry entry = { (gchar *)mime, GTK_TARGET_SAME_APP, 0 };

	gtk_drag_dest_set(widget, 0, &entry, 1, GDK_ACTION_MOVE);
	g_object_set_data(G_OBJECT(widget), GSURF_CHROME_DROP_FUNCS_DATA, (gpointer)funcs);
	g_signal_connect(widget, "drag-motion", G_CALLBACK(gsurf_chrome_on_drag_motion), user_data);
	g_signal_connect(widget, "drag-leave", G_CALLBACK(gsurf_chrome_on_drag_leave), user_data);
	g_signal_connect(widget, "drag-drop", G_CALLBACK(gsurf_chrome_on_drag_drop), user_data);
#endif
}

/**
 * gsurf_chrome_disconnect_dnd:
 * @widget: a widget passed to gsurf_chrome_drag_source_add() or
 *   gsurf_chrome_drop_target_add()
 * @user_data: the data it was added with
 *
 * Removes the drag source and drop target added with @user_data (GTK4:
 * their controllers; GTK3: the handlers, and the widget stops being a
 * source and a destination). Safe to call more than once.
 */
static inline void
gsurf_chrome_disconnect_dnd(GtkWidget *widget, gpointer user_data)
{
#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GListModel) controllers = NULL;
	g_autoptr(GPtrArray) doomed = NULL;
	guint i;
	guint n;

	doomed = g_ptr_array_new_with_free_func(g_object_unref);
	controllers = gtk_widget_observe_controllers(widget);
	n = g_list_model_get_n_items(controllers);
	for (i = 0; i < n; i++) {
		GtkEventController *c = g_list_model_get_item(controllers, i);
		const gchar *name = gtk_event_controller_get_name(c);

		if ((g_strcmp0(name, GSURF_CHROME_DRAG_SOURCE_NAME) == 0 ||
		     g_strcmp0(name, GSURF_CHROME_DROP_TARGET_NAME) == 0) &&
		    g_signal_handlers_disconnect_by_data(c, user_data) > 0)
			g_ptr_array_add(doomed, c);
		else
			g_object_unref(c);
	}
	for (i = 0; i < doomed->len; i++)
		gtk_widget_remove_controller(widget, g_ptr_array_index(doomed, i));
#else
	if (g_object_get_data(G_OBJECT(widget), GSURF_CHROME_DRAG_FUNCS_DATA) != NULL) {
		gtk_drag_source_unset(widget);
		g_object_set_data(G_OBJECT(widget), GSURF_CHROME_DRAG_FUNCS_DATA, NULL);
	}
	if (g_object_get_data(G_OBJECT(widget), GSURF_CHROME_DROP_FUNCS_DATA) != NULL) {
		gtk_drag_dest_unset(widget);
		g_object_set_data(G_OBJECT(widget), GSURF_CHROME_DROP_FUNCS_DATA, NULL);
	}
	g_signal_handlers_disconnect_by_data(widget, user_data);
#endif
}

G_END_DECLS

#endif /* GSURF_CHROME_COMPAT_H */
