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
#include "boxed/gsurf-keybind-help.h"
#include "window/gsurf-window-layout.h"

#include <gtk/gtk.h>

struct _GsurfGtk4Window
{
	GsurfWindow parent_instance;

	GtkWidget *window;  /* GtkWindow toplevel */
	GtkWidget *vbox;    /* content box */
	GtkWidget *hbox;    /* side panels + the view stack */
	GtkWidget *stack;   /* GtkStack of view widgets */
	GtkWidget *content; /* a layout's root in place of the stack (borrowed) */
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

/* Shortcut chords (and function keys) still reach the keymap while a
 * chrome text entry has focus; bare keys and Escape stay with the entry. */
static gboolean
is_shortcut_key(guint keyval, guint mods)
{
	if ((mods & (GSURF_MOD_CTRL | GSURF_MOD_ALT | GSURF_MOD_SUPER)) != 0)
		return TRUE;
	return keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F35;
}

/* Capture phase: runs before the focused web view, like the GTK3
 * window's key-press-event handler, so TRUE consumes a command key. */
static gboolean
on_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
               GdkModifierType state, gpointer user_data)
{
	GsurfGtk4Window *self = user_data;
	GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self->window));
	guint mods = translate_modifiers(state);

	/* Typing in the address bar / find box: not a keymap command, unless
	 * a key grab (gsurf_window_set_key_grab(), e.g. after a wmux prefix)
	 * wants every key; a key it declines then still reaches the entry. */
	if (focus != NULL && GTK_IS_EDITABLE(focus) && !is_shortcut_key(keyval, mods) &&
	    !gsurf_window_get_key_grab(GSURF_WINDOW(self)))
		return FALSE;

	return gsurf_window_emit_key_press(GSURF_WINDOW(self), keyval, keycode, mods);
}

/* The toplevel's focus widget moved: a chrome text entry (address bar,
 * find box, prompt; GTK4 focuses the GtkText inside a GtkEntry) takes
 * bare keys as text, so the window is typing (the indicator reads
 * INSERT). */
static void
on_focus_widget(GObject *toplevel, GParamSpec *pspec, gpointer user_data)
{
	GsurfGtk4Window *self = user_data;
	GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(toplevel));

	(void)pspec;
	gsurf_window_set_entry_focus(GSURF_WINDOW(self), focus != NULL &&
		(GTK_IS_EDITABLE(focus) || GTK_IS_TEXT_VIEW(focus)));
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

	/* vbox: [top chrome] hbox [bottom chrome]; hbox: [left] stack [right]. */
	self->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_vexpand(self->stack, TRUE);
	gtk_widget_set_hexpand(self->stack, TRUE);
	gtk_widget_set_vexpand(self->hbox, TRUE);
	gtk_box_append(GTK_BOX(self->hbox), self->stack);
	gtk_box_append(GTK_BOX(self->vbox), self->hbox);
	gtk_window_set_child(GTK_WINDOW(self->window), self->vbox);
	gtk_window_set_default_size(GTK_WINDOW(self->window),
		GSURF_DEFAULT_WIDTH, GSURF_DEFAULT_HEIGHT);

	key = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
	g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), self);
	gtk_widget_add_controller(self->window, key);

	g_signal_connect(self->window, "close-request",
		G_CALLBACK(on_close_request), self);
	g_signal_connect(self->window, "notify::focus-widget",
		G_CALLBACK(on_focus_widget), self);
}

/* A mouse button pressed on a view widget (capture phase, so it runs
 * before WebKit's own gestures). The press goes through the window like
 * GTK3's button-press-event: a handler that consumes it (a mousebind, an
 * input module, a side-button history step) claims the sequence so the
 * page never sees it; otherwise the gesture steps aside. */
static void
on_view_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                gpointer user_data)
{
	GsurfGtk4Window *self = user_data;
	GtkEventController *ctrl = GTK_EVENT_CONTROLLER(gesture);
	guint button;
	guint mods;

	(void)n_press;
	(void)x;
	(void)y;
	button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
	mods = translate_modifiers(gtk_event_controller_get_current_event_state(ctrl));
	if (gsurf_window_emit_button_press(GSURF_WINDOW(self), button, mods))
		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	else
		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
}

/* The view owning @widget in @self, or NULL. */
static GsurfView *
view_for_widget(GsurfGtk4Window *self, GtkWidget *widget)
{
	GPtrArray *views = gsurf_window_get_views(GSURF_WINDOW(self));
	guint i;

	for (i = 0; i < views->len; i++) {
		GsurfView *view = g_ptr_array_index(views, i);

		if (gsurf_view_get_native_widget(view) == widget)
			return view;
	}
	return NULL;
}

/* Surface-unit scroll deltas (a touchpad, in pixels) per wheel notch. */
#define GSURF_GTK4_SCROLL_PX_PER_NOTCH (50.0)

/* A wheel or touchpad scroll on a view widget (capture phase, before
 * WebKit scrolls the page): it goes through the window as wheel buttons
 * (4-7), so a mousebind (Ctrl+wheel zooms by default) can consume it and
 * act on this view. Wheel deltas are already in notches; touchpad ones are
 * pixels, scaled to notches. */
static gboolean
on_view_scroll(GtkEventControllerScroll *scroll, gdouble dx, gdouble dy, gpointer user_data)
{
	GsurfGtk4Window *self = user_data;
	GtkEventController *ctrl = GTK_EVENT_CONTROLLER(scroll);
	GtkWidget *widget = gtk_event_controller_get_widget(ctrl);
	guint mods = translate_modifiers(gtk_event_controller_get_current_event_state(ctrl));

	if (gtk_event_controller_scroll_get_unit(scroll) == GDK_SCROLL_UNIT_SURFACE) {
		dx /= GSURF_GTK4_SCROLL_PX_PER_NOTCH;
		dy /= GSURF_GTK4_SCROLL_PX_PER_NOTCH;
	}
	return gsurf_window_emit_wheel(GSURF_WINDOW(self), view_for_widget(self, widget),
		dx, dy, mods);
}

/* Focus follows click: with a layout showing several views, the view
 * that takes keyboard focus (a click into it, Tab) becomes the active
 * view. Without a layout only the active view is visible; a hidden
 * background view never steals the active role. The setter returns
 * early for the active view, so there is no focus loop. */
static void
on_view_focus_enter(GtkEventControllerFocus *ctrl, gpointer user_data)
{
	GsurfGtk4Window *self = user_data;
	GsurfWindow *window = GSURF_WINDOW(self);
	GtkWidget *widget;
	GsurfView *view;

	if (gsurf_window_get_layout(window) == NULL)
		return;
	widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
	view = view_for_widget(self, widget);
	if (view != NULL && gsurf_window_get_view_visible(window, view))
		gsurf_window_set_active_view(window, view);
}

static void
free_weak_ref(gpointer data)
{
	g_weak_ref_clear(data);
	g_free(data);
}

/* Whether @widget already carries this window's controllers. The mark is
 * a weak reference, so a view that outlives its window is wired again by
 * the next window even if that one reuses the old address. */
static gboolean
is_wired(GsurfGtk4Window *self, GtkWidget *widget)
{
	GWeakRef *ref = g_object_get_data(G_OBJECT(widget), "gsurf-gtk4-wired");
	g_autoptr(GObject) owner = NULL;

	if (ref == NULL)
		return FALSE;
	owner = g_weak_ref_get(ref);
	return owner == G_OBJECT(self);
}

/* Remove the click and focus controllers a window added to @widget. */
static void
remove_view_controllers(GtkWidget *widget)
{
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(widget);
	g_autoptr(GPtrArray) ours = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;

	/* Collect first: removing while walking the live list model would
	 * shift it under us. */
	for (i = 0; i < g_list_model_get_n_items(controllers); i++) {
		GtkEventController *c = g_list_model_get_item(controllers, i);
		const gchar *name = gtk_event_controller_get_name(c);

		if (g_strcmp0(name, "gsurf-view-click") == 0 ||
		    g_strcmp0(name, "gsurf-view-scroll") == 0 ||
		    g_strcmp0(name, "gsurf-view-focus") == 0)
			g_ptr_array_add(ours, c);
		else
			g_object_unref(c);
	}
	for (i = 0; i < ours->len; i++)
		gtk_widget_remove_controller(widget, g_ptr_array_index(ours, i));
}

/* Per-view event routing, done once per (widget, window): a view widget
 * keeps its controllers when it moves between the stack and a layout
 * (gsurf_window_set_layout()), so wiring it again would emit every press
 * twice. The handlers are tied to the window's lifetime. */
static void
gsurf_gtk4_window_wire_view(GsurfGtk4Window *self, GtkWidget *widget)
{
	GtkGesture *click;
	GtkEventController *focus;
	GtkEventController *scroll;
	GWeakRef *ref;

	if (is_wired(self, widget))
		return;
	/* Another window wired it before (the view moved, or that window is
	 * gone): its controllers go, the widget now reports to this one. */
	remove_view_controllers(widget);
	ref = g_new0(GWeakRef, 1);
	g_weak_ref_init(ref, self);
	g_object_set_data_full(G_OBJECT(widget), "gsurf-gtk4-wired", ref, free_weak_ref);

	/* Any button (0), so side buttons (8/9) reach the mousebinds too. */
	click = gtk_gesture_click_new();
	gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(click), "gsurf-view-click");
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
		GTK_PHASE_CAPTURE);
	g_signal_connect_object(click, "pressed", G_CALLBACK(on_view_pressed), self, 0);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));

	/* The wheel too (Ctrl+wheel zooms by default), before WebKit's own. */
	scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
	gtk_event_controller_set_name(scroll, "gsurf-view-scroll");
	gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
	g_signal_connect_object(scroll, "scroll", G_CALLBACK(on_view_scroll), self, 0);
	gtk_widget_add_controller(widget, scroll);

	/* Focus follows click (only acts while a layout is set). */
	focus = gtk_event_controller_focus_new();
	gtk_event_controller_set_name(focus, "gsurf-view-focus");
	g_signal_connect_object(focus, "enter", G_CALLBACK(on_view_focus_enter), self, 0);
	gtk_widget_add_controller(widget, focus);
}

static void
gsurf_gtk4_window_insert_view(GsurfWindow *window, GsurfView *view)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkWidget *widget = gsurf_view_get_native_widget(view);

	if (widget == NULL)
		return;
	gsurf_gtk4_window_wire_view(self, widget);
	/* Safe to repeat (a layout being removed re-inserts every view). A
	 * widget still parented elsewhere is a layout that did not let go
	 * of it in detach/remove_view: say so instead of a GTK critical. */
	if (gtk_widget_get_parent(widget) == self->stack)
		return;
	if (gtk_widget_get_parent(widget) != NULL) {
		g_warning("gsurf-gtk4-window: view widget still has a parent; not stacked");
		return;
	}
	gtk_stack_add_child(GTK_STACK(self->stack), widget);
}

static void
gsurf_gtk4_window_connect_view(GsurfWindow *window, GsurfView *view)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkWidget *widget = gsurf_view_get_native_widget(view);

	if (widget != NULL)
		gsurf_gtk4_window_wire_view(self, widget);
}

/* Show @widget (a layout's root) where the stack is, or the stack again
 * for NULL. The hbox holds a reference while the root is packed; the
 * layout keeps its own. The root expands like the stack. */
static void
gsurf_gtk4_window_set_content(GsurfWindow *window, gpointer widget)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkWidget *w = widget;

	if (self->hbox == NULL || w == self->content)
		return;

	/* 1. Whatever was there before leaves (a root replacing a root). */
	if (self->content != NULL) {
		if (gtk_widget_get_parent(self->content) == self->hbox)
			gtk_box_remove(GTK_BOX(self->hbox), self->content);
		self->content = NULL;
	}

	/* 2. NULL: the stack comes back. */
	if (w == NULL) {
		gtk_widget_set_visible(self->stack, TRUE);
		return;
	}

	/* 3. The root goes right after the hidden stack. */
	gtk_widget_set_visible(self->stack, FALSE);
	gtk_widget_set_hexpand(w, TRUE);
	gtk_widget_set_vexpand(w, TRUE);
	gtk_box_insert_child_after(GTK_BOX(self->hbox), w, self->stack);
	gtk_widget_set_visible(w, TRUE);
	self->content = w;
}

static void
gsurf_gtk4_window_remove_view(GsurfWindow *window, GsurfView *view)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkWidget *widget = gsurf_view_get_native_widget(view);

	/* Only the stack's own child: a widget elsewhere (or nowhere) is not
	 * ours to remove, and gtk_stack_remove() would warn. */
	if (widget != NULL && gtk_widget_get_parent(widget) == self->stack)
		gtk_stack_remove(GTK_STACK(self->stack), widget);
}

static void
gsurf_gtk4_window_set_active_view(GsurfWindow *window, GsurfView *view)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkWidget *widget = gsurf_view_get_native_widget(view);

	if (widget == NULL)
		return;
	gtk_stack_set_visible_child(GTK_STACK(self->stack), widget);
	gsurf_view_focus(view);
}

static void
gsurf_gtk4_window_present(GsurfWindow *window)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GsurfView *view = gsurf_window_get_active_view(window);

	gtk_window_present(GTK_WINDOW(self->window));
	/* Start on the page rather than the first focusable chrome widget. */
	if (view != NULL)
		gsurf_view_focus(view);
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
gsurf_gtk4_window_add_side_widget(GsurfWindow *window, gpointer widget, GsurfChromeEdge edge)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkWidget *w = widget;

	if (w == NULL || self->hbox == NULL)
		return;
	/* A side panel keeps its own width, like GTK3's pack with expand
	 * FALSE. GTK4 computes a widget's expand flag from its children
	 * unless it is set explicitly, so a label that expands deep inside
	 * the panel would make the hbox split its space between the panel
	 * and the page. Setting it explicitly stops that propagation. */
	gtk_widget_set_hexpand(w, FALSE);
	if (edge == GSURF_CHROME_EDGE_LEFT)
		gtk_box_prepend(GTK_BOX(self->hbox), w);
	else
		gtk_box_append(GTK_BOX(self->hbox), w);
}

static void
on_help4_destroy(GtkWidget *dialog, gpointer user_data)
{
	GsurfGtk4Window *self = user_data;

	(void)dialog;
	if (self->window != NULL)
		g_object_set_data(G_OBJECT(self->window), "gsurf-keybind-help", NULL);
}

static void
on_help4_close(GtkButton *button, gpointer user_data)
{
	(void)button;
	gtk_window_destroy(GTK_WINDOW(user_data));
}

static void
help4_list_move(GtkListBox *list, gint delta)
{
	GtkListBoxRow *row;
	gint idx = 0;
	gint n = 0;

	while (gtk_list_box_get_row_at_index(list, n) != NULL)
		n++;
	if (n == 0)
		return;

	row = gtk_list_box_get_selected_row(list);
	if (row != NULL)
		idx = gtk_list_box_row_get_index(row) + delta;
	else if (delta < 0)
		idx = n - 1;

	if (idx < 0)
		idx = 0;
	if (idx >= n)
		idx = n - 1;
	row = gtk_list_box_get_row_at_index(list, idx);
	gtk_list_box_select_row(list, row);
	if (row != NULL)
		gtk_widget_grab_focus(GTK_WIDGET(row));
}

static void
help4_scroll_x(GtkScrolledWindow *scrolled, gint delta)
{
	GtkAdjustment *adj;

	adj = gtk_scrolled_window_get_hadjustment(scrolled);
	if (adj == NULL)
		return;
	gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + (gdouble)delta);
}

static gboolean
on_help4_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                     GdkModifierType state, gpointer user_data)
{
	GtkWindow *dialog = GTK_WINDOW(user_data);
	GtkListBox *list;
	GtkScrolledWindow *scrolled;
	GsurfKeybindHelpKey action;

	(void)ctrl;
	(void)keycode;
	action = gsurf_keybind_help_key_action(keyval, translate_modifiers(state));
	if (action == GSURF_KEYBIND_HELP_KEY_NONE)
		return FALSE;
	if (action == GSURF_KEYBIND_HELP_KEY_CLOSE) {
		gtk_window_destroy(dialog);
		return TRUE;
	}

	list = GTK_LIST_BOX(g_object_get_data(G_OBJECT(dialog), "gsurf-help-list"));
	scrolled = GTK_SCROLLED_WINDOW(g_object_get_data(G_OBJECT(dialog),
		"gsurf-help-scrolled"));
	if (action == GSURF_KEYBIND_HELP_KEY_UP && list != NULL)
		help4_list_move(list, -1);
	else if (action == GSURF_KEYBIND_HELP_KEY_DOWN && list != NULL)
		help4_list_move(list, 1);
	else if (scrolled != NULL)
		help4_scroll_x(scrolled,
			action == GSURF_KEYBIND_HELP_KEY_LEFT ? -80 : 80);
	return TRUE;
}

static void
gsurf_gtk4_window_show_keybind_help(GsurfWindow *window, GPtrArray *entries)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(window);
	GtkWidget *dialog, *vbox, *scrolled, *list, *old, *close_btn;
	guint i;

	if (self->window == NULL)
		return;

	old = g_object_get_data(G_OBJECT(self->window), "gsurf-keybind-help");
	if (old != NULL) {
		gtk_window_destroy(GTK_WINDOW(old));
		return;
	}

	dialog = gtk_window_new();
	gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(self->window));
	gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
	gtk_window_set_title(GTK_WINDOW(dialog), "Keybindings (hjkl / q)");
	gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 520);
	g_object_set_data(G_OBJECT(self->window), "gsurf-keybind-help", dialog);
	g_signal_connect(dialog, "destroy", G_CALLBACK(on_help4_destroy), self);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_window_set_child(GTK_WINDOW(dialog), vbox);

	list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_BROWSE);
	for (i = 0; entries != NULL && i < entries->len; i++) {
		const GsurfKeybindHelp *h = g_ptr_array_index(entries, i);
		g_autofree gchar *pretty = gsurf_keybind_help_pretty_key(h->key);
		GtkWidget *row, *key_l, *desc_l, *src_l;

		row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
		key_l = gtk_label_new(pretty != NULL ? pretty : "");
		gtk_widget_add_css_class(key_l, "monospace");
		gtk_label_set_xalign(GTK_LABEL(key_l), 0.0);
		gtk_widget_set_size_request(key_l, 160, -1);
		desc_l = gtk_label_new(h->description != NULL ? h->description : "");
		gtk_label_set_xalign(GTK_LABEL(desc_l), 0.0);
		gtk_widget_set_hexpand(desc_l, TRUE);
		src_l = gtk_label_new(h->source != NULL ? h->source : "");
		gtk_label_set_xalign(GTK_LABEL(src_l), 0.0);
		gtk_widget_set_size_request(src_l, 120, -1);
		gtk_box_append(GTK_BOX(row), key_l);
		gtk_box_append(GTK_BOX(row), desc_l);
		gtk_box_append(GTK_BOX(row), src_l);
		gtk_list_box_append(GTK_LIST_BOX(list), row);
	}

	scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
	gtk_widget_set_vexpand(scrolled, TRUE);
	gtk_box_append(GTK_BOX(vbox), scrolled);

	close_btn = gtk_button_new_with_mnemonic("_Close");
	g_signal_connect(close_btn, "clicked", G_CALLBACK(on_help4_close), dialog);
	gtk_box_append(GTK_BOX(vbox), close_btn);

	g_object_set_data(G_OBJECT(dialog), "gsurf-help-list", list);
	g_object_set_data(G_OBJECT(dialog), "gsurf-help-scrolled", scrolled);
	{
		GtkEventController *keys = gtk_event_controller_key_new();

		gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
		g_signal_connect(keys, "key-pressed", G_CALLBACK(on_help4_key_pressed), dialog);
		gtk_widget_add_controller(dialog, keys);
	}
	if (entries != NULL && entries->len > 0) {
		GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0);

		if (row != NULL)
			gtk_list_box_select_row(GTK_LIST_BOX(list), row);
	}

	gtk_window_present(GTK_WINDOW(dialog));
}

static void
gsurf_gtk4_window_dispose(GObject *object)
{
	GsurfGtk4Window *self = GSURF_GTK4_WINDOW(object);

	/* A layout leaves first, while its root and the stack still exist. */
	gsurf_window_set_layout(GSURF_WINDOW(self), NULL);
	if (self->window != NULL) {
		/* Teardown moves the focus: no typing reports from a dying window. */
		g_signal_handlers_disconnect_by_func(self->window, on_focus_widget, self);
		gtk_window_destroy(GTK_WINDOW(self->window));
		self->window = NULL;
		self->content = NULL;
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
	window_class->show_keybind_help = gsurf_gtk4_window_show_keybind_help;
	window_class->add_side_widget = gsurf_gtk4_window_add_side_widget;
	window_class->set_content = gsurf_gtk4_window_set_content;
	window_class->connect_view = gsurf_gtk4_window_connect_view;
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
