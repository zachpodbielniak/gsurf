/*
 * gsurf-gtk3-input.c - Native input for a GTK3 widget, as GDK events
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gsurf-enums.h"
#include "backend/gtk3/gsurf-gtk3-input.h"

guint
gsurf_gtk3_input_state(guint mods)
{
	guint state = 0;

	if (mods & GSURF_MOD_SHIFT)
		state |= GDK_SHIFT_MASK;
	if (mods & GSURF_MOD_CTRL)
		state |= GDK_CONTROL_MASK;
	if (mods & GSURF_MOD_ALT)
		state |= GDK_MOD1_MASK;
	if (mods & GSURF_MOD_SUPER)
		state |= GDK_SUPER_MASK;
	return state;
}

GdkDevice *
gsurf_gtk3_input_device(GtkWidget *widget, gboolean keyboard)
{
	GdkDisplay *display = gtk_widget_get_display(widget);
	GdkSeat *seat = display != NULL ? gdk_display_get_default_seat(display) : NULL;

	if (seat == NULL)
		return NULL;
	return keyboard ? gdk_seat_get_keyboard(seat) : gdk_seat_get_pointer(seat);
}

GdkWindow *
gsurf_gtk3_input_window(GtkWidget *widget, gdouble *x, gdouble *y)
{
	GdkWindow *win = gtk_widget_get_window(widget);

	if (win == NULL)
		return NULL;

	/*
	 * A widget with no window of its own is drawn in its parent's,
	 * offset by its allocation; the event coordinates are that window's.
	 */
	if (!gtk_widget_get_has_window(widget)) {
		GtkAllocation alloc;

		gtk_widget_get_allocation(widget, &alloc);
		if (x != NULL)
			*x += alloc.x;
		if (y != NULL)
			*y += alloc.y;
	}
	return win;
}

/*
 * A hardware keycode (and group) for a keysym from the display's keymap.
 * Real key events carry one, and WebKit's handling of editing keys
 * (Backspace, Delete, the arrows) is unreliable with keycode 0 even though
 * printable characters resolve from the keyval alone.  0 when the keysym is
 * not on the keymap.
 */
static guint16
keycode_for_keyval(GtkWidget *widget, guint keyval, guint8 *out_group)
{
	GdkKeymap *keymap = gdk_keymap_get_for_display(gtk_widget_get_display(widget));
	GdkKeymapKey *keys = NULL;
	gint n = 0;
	guint16 code = 0;

	if (out_group != NULL)
		*out_group = 0;
	if (keymap != NULL && gdk_keymap_get_entries_for_keyval(keymap, keyval, &keys, &n) &&
	    n > 0) {
		code = (guint16)keys[0].keycode;
		if (out_group != NULL)
			*out_group = (guint8)keys[0].group;
	}
	g_free(keys);
	return code;
}

void
gsurf_gtk3_input_focus(GtkWidget *widget)
{
	GdkWindow *win;
	GdkEvent *ev;

	gtk_widget_grab_focus(widget);
	if (gtk_widget_has_focus(widget))
		return;

	win = gsurf_gtk3_input_window(widget, NULL, NULL);
	if (win == NULL)
		return;

	ev = gdk_event_new(GDK_FOCUS_CHANGE);
	ev->focus_change.window = g_object_ref(win);
	ev->focus_change.send_event = TRUE;
	ev->focus_change.in = TRUE;
	/* without a device GDK warns and may drop it */
	gdk_event_set_device(ev, gsurf_gtk3_input_device(widget, TRUE));
	gtk_main_do_event(ev);
	gdk_event_free(ev);
}

gboolean
gsurf_gtk3_input_key(GtkWidget *widget, guint keyval, guint keycode, guint mods,
                     gboolean pressed)
{
	GdkWindow *win = gsurf_gtk3_input_window(widget, NULL, NULL);
	GdkEvent *ev;
	guint8 group = 0;

	if (win == NULL)
		return FALSE;
	if (keycode == 0)
		keycode = keycode_for_keyval(widget, keyval, &group);

	ev = gdk_event_new(pressed ? GDK_KEY_PRESS : GDK_KEY_RELEASE);
	ev->key.window = g_object_ref(win);
	ev->key.send_event = TRUE;
	ev->key.time = GDK_CURRENT_TIME;
	ev->key.state = gsurf_gtk3_input_state(mods);
	ev->key.keyval = keyval;
	ev->key.hardware_keycode = (guint16)keycode;
	ev->key.group = group;
	gdk_event_set_device(ev, gsurf_gtk3_input_device(widget, TRUE));

	gtk_main_do_event(ev);
	gdk_event_free(ev);
	return TRUE;
}

gboolean
gsurf_gtk3_input_motion(GtkWidget *widget, gdouble x, gdouble y, guint mods)
{
	GdkWindow *win = gsurf_gtk3_input_window(widget, &x, &y);
	GdkEvent *ev;

	if (win == NULL)
		return FALSE;

	ev = gdk_event_new(GDK_MOTION_NOTIFY);
	ev->motion.window = g_object_ref(win);
	ev->motion.send_event = TRUE;
	ev->motion.time = GDK_CURRENT_TIME;
	ev->motion.x = x;
	ev->motion.y = y;
	ev->motion.state = gsurf_gtk3_input_state(mods);
	gdk_event_set_device(ev, gsurf_gtk3_input_device(widget, FALSE));

	gtk_main_do_event(ev);
	gdk_event_free(ev);
	return TRUE;
}

gboolean
gsurf_gtk3_input_button(GtkWidget *widget, gdouble x, gdouble y, guint button,
                        gboolean pressed, guint mods)
{
	GdkWindow *win = gsurf_gtk3_input_window(widget, &x, &y);
	GdkEvent *ev;

	if (win == NULL)
		return FALSE;

	ev = gdk_event_new(pressed ? GDK_BUTTON_PRESS : GDK_BUTTON_RELEASE);
	ev->button.window = g_object_ref(win);
	ev->button.send_event = TRUE;
	ev->button.time = GDK_CURRENT_TIME;
	ev->button.x = x;
	ev->button.y = y;
	ev->button.button = button;
	ev->button.state = gsurf_gtk3_input_state(mods);
	gdk_event_set_device(ev, gsurf_gtk3_input_device(widget, FALSE));

	gtk_main_do_event(ev);
	gdk_event_free(ev);
	return TRUE;
}
