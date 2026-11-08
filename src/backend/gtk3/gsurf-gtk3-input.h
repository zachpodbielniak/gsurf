/*
 * gsurf-gtk3-input.h - Native input for a GTK3 widget, as GDK events
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Synthesized GDK key, motion, button and scroll events delivered to a
 * widget through gtk_main_do_event(), so a WebKitWebView handles them
 * exactly as it handles the real ones -- including telling the page they
 * are trusted.  Shared by the GTK3 view (agent input over the control
 * socket and MCP) and the LRG engine (the 3D scene's input), which once
 * had the only copy.  Compiled only when GTK_BACKEND=gtk3.
 */

#ifndef GSURF_GTK3_INPUT_H
#define GSURF_GTK3_INPUT_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * gsurf_gtk3_input_state:
 * @mods: #GsurfKeyMod flags
 *
 * Returns: the matching #GdkModifierType mask
 */
guint gsurf_gtk3_input_state(guint mods);

/*
 * gsurf_gtk3_input_device:
 * @widget: the target widget
 * @keyboard: the keyboard rather than the pointer
 *
 * Returns: (transfer none) (nullable): the default seat's device
 */
GdkDevice *gsurf_gtk3_input_device(GtkWidget *widget, gboolean keyboard);

/*
 * gsurf_gtk3_input_key:
 * @widget: the target widget
 * @keyval: a keysym
 * @keycode: a hardware keycode, or 0 to look it up on the keymap
 * @mods: #GsurfKeyMod flags
 * @pressed: press (TRUE) or release
 *
 * Returns: %FALSE when the widget has no window to deliver to
 */
gboolean gsurf_gtk3_input_key(GtkWidget *widget, guint keyval, guint keycode,
                              guint mods, gboolean pressed);

/*
 * gsurf_gtk3_input_motion:
 * @widget: the target widget
 * @x: horizontal position, widget coordinates
 * @y: vertical position, widget coordinates
 * @mods: #GsurfKeyMod flags
 *
 * Returns: %FALSE when the widget has no window to deliver to
 */
gboolean gsurf_gtk3_input_motion(GtkWidget *widget, gdouble x, gdouble y, guint mods);

/*
 * gsurf_gtk3_input_button:
 * @widget: the target widget
 * @x: horizontal position, widget coordinates
 * @y: vertical position, widget coordinates
 * @button: 1 left, 2 middle, 3 right
 * @pressed: press (TRUE) or release
 * @mods: #GsurfKeyMod flags
 *
 * Returns: %FALSE when the widget has no window to deliver to
 */
gboolean gsurf_gtk3_input_button(GtkWidget *widget, gdouble x, gdouble y, guint button,
                                 gboolean pressed, guint mods);

/*
 * gsurf_gtk3_input_focus:
 * @widget: the target widget
 *
 * Gives @widget the keyboard focus and, when its toplevel is not the
 * active window -- always the case on a headless display nobody clicked
 * -- tells it so with a synthesized focus-in, which WebKit needs before
 * an editable element takes typed keys.
 */
void gsurf_gtk3_input_focus(GtkWidget *widget);

/*
 * gsurf_gtk3_input_window:
 * @widget: the target widget
 * @x: (inout): a widget x coordinate, made relative to the returned window
 * @y: (inout): a widget y coordinate, made relative to the returned window
 *
 * The GdkWindow events for @widget are delivered on, with @x and @y
 * translated into it: a widget without a window of its own draws in its
 * parent's, offset by its allocation.
 *
 * Returns: (transfer none) (nullable): the window, or %NULL before realize
 */
GdkWindow *gsurf_gtk3_input_window(GtkWidget *widget, gdouble *x, gdouble *y);

G_END_DECLS

#endif /* GSURF_GTK3_INPUT_H */
