/*
 * gsurf-application.h - Top-level browser session
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GsurfApplication is the embeddable top-level object: it owns the
 * configuration, the list of windows, and (when run standalone) the
 * GLib main loop. It is deliberately NOT a GtkApplication so the
 * library stays free of GTK in its public API and can be embedded into
 * a host that drives its own main loop (e.g. cmacs).
 */

#ifndef GSURF_APPLICATION_H
#define GSURF_APPLICATION_H

#include <glib-object.h>

#include "config/gsurf-config.h"
#include "window/gsurf-window.h"

G_BEGIN_DECLS

#define GSURF_TYPE_APPLICATION (gsurf_application_get_type())

G_DECLARE_FINAL_TYPE(GsurfApplication, gsurf_application, GSURF, APPLICATION, GObject)

/**
 * gsurf_application_new:
 * @config: (transfer none) (nullable): the configuration, or %NULL for defaults
 *
 * Creates a new application session.
 *
 * Returns: (transfer full): a new #GsurfApplication
 */
GsurfApplication *gsurf_application_new(GsurfConfig *config);

/**
 * gsurf_application_get_config:
 * @self: a #GsurfApplication
 *
 * Returns: (transfer none): the application configuration
 */
GsurfConfig *gsurf_application_get_config(GsurfApplication *self);

/**
 * gsurf_application_add_window:
 * @self: a #GsurfApplication
 * @window: (transfer none): a window to track
 *
 * Adds @window to the application. When the last window is closed the
 * main loop (if running) quits.
 */
void gsurf_application_add_window(GsurfApplication *self, GsurfWindow *window);

/**
 * gsurf_application_remove_window:
 * @self: a #GsurfApplication
 * @window: (transfer none): a tracked window
 */
void gsurf_application_remove_window(GsurfApplication *self, GsurfWindow *window);

/**
 * gsurf_application_get_windows:
 * @self: a #GsurfApplication
 *
 * Returns: (transfer none) (element-type GsurfWindow): the window list
 */
GPtrArray *gsurf_application_get_windows(GsurfApplication *self);

/**
 * gsurf_application_get_active_window:
 * @self: a #GsurfApplication
 *
 * Returns: (transfer none) (nullable): the most recently added window
 */
GsurfWindow *gsurf_application_get_active_window(GsurfApplication *self);

/**
 * gsurf_application_run:
 * @self: a #GsurfApplication
 *
 * Runs the GLib main loop until gsurf_application_quit() is called or
 * the last window closes. Only used when running standalone.
 */
void gsurf_application_run(GsurfApplication *self);

/**
 * gsurf_application_quit:
 * @self: a #GsurfApplication
 *
 * Quits the main loop started by gsurf_application_run().
 */
void gsurf_application_quit(GsurfApplication *self);

G_END_DECLS

#endif /* GSURF_APPLICATION_H */
