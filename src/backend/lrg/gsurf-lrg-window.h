/*
 * gsurf-lrg-window.h - libregnum (LRG) standalone window
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * #GsurfLrgWindow is the #GsurfWindow subclass used by the standalone
 * @command{gsurf --lrg} binary: it owns a raylib (graylib #GrlWindow), runs a
 * per-frame render loop on the default GLib main context, draws the active
 * view's page texture, and forwards raylib input to the focused view. cmacs
 * never instantiates it (cmacs composites #GsurfLrgView textures through its own
 * lrgterm runtime).
 *
 * It holds a #GsurfLrgRenderMode.  2D draws the active tab as a flat textured
 * quad.  3D (builds with LRG_3D=1) hands the frame to a #GsurfLrgScene: every
 * tab becomes a panel in libregnum's 3D scene, the address bar stays a 2D HUD
 * on top, and the pointer is ray-cast onto the panels.  3D-VR is reserved.
 */

#ifndef GSURF_LRG_WINDOW_H
#define GSURF_LRG_WINDOW_H

#include <glib-object.h>

#include "gsurf-enums.h"
#include "window/gsurf-window.h"

G_BEGIN_DECLS

#define GSURF_TYPE_LRG_WINDOW (gsurf_lrg_window_get_type())

G_DECLARE_FINAL_TYPE(GsurfLrgWindow, gsurf_lrg_window, GSURF, LRG_WINDOW, GsurfWindow)

/**
 * gsurf_lrg_window_new:
 *
 * Returns: (transfer full): a new #GsurfLrgWindow as a #GsurfWindow.
 */
GsurfWindow *gsurf_lrg_window_new(void);

/**
 * gsurf_lrg_window_set_render_mode:
 * @self: a #GsurfLrgWindow
 * @mode: a #GsurfLrgRenderMode
 *
 * Selects the render mode.  Call it before the window is presented.  A mode
 * this build does not implement (see gsurf_lrg_render_mode_is_implemented())
 * emits a warning and falls back to 2D.
 */
void gsurf_lrg_window_set_render_mode(GsurfLrgWindow *self,
                                      GsurfLrgRenderMode mode);

/**
 * gsurf_lrg_window_get_render_mode:
 * @self: a #GsurfLrgWindow
 *
 * Returns: the current #GsurfLrgRenderMode.
 */
GsurfLrgRenderMode gsurf_lrg_window_get_render_mode(GsurfLrgWindow *self);

/**
 * gsurf_lrg_window_begin_url_edit:
 * @self: a #GsurfLrgWindow
 *
 * Enter the LRG-native address-bar edit mode (the GTK chromebar/omnibar
 * modules cannot render under @samp{--lrg}).  Pre-fills with the current URI;
 * the render loop draws the editable field and captures typing until Enter
 * (load) or Escape (cancel).  Bound to the @code{open-prompt} action.
 */
void gsurf_lrg_window_begin_url_edit(GsurfLrgWindow *self);

/**
 * GsurfLrgSceneOptions:
 * @arrangement: (nullable): 3D arrangement id, %NULL for the default
 * @environment: (nullable): 3D environment id, %NULL for the default
 * @background_fps: refresh rate of visible background tabs (<= 0: default)
 * @history_limit: entries per direction in history depth (0: default)
 * @reading_single_tab: start in reading mode when the window opens with one tab
 *
 * Start-up settings for the 3D scene (the `lrg:` config block and the
 * `--lrg=3d:ARRANGEMENT:ENVIRONMENT` spec).
 */
typedef struct {
	const gchar *arrangement;
	const gchar *environment;
	gdouble      background_fps;
	guint        history_limit;
	gboolean     reading_single_tab;
} GsurfLrgSceneOptions;

/**
 * gsurf_lrg_window_set_scene_options:
 * @self: a #GsurfLrgWindow
 * @options: the settings (copied)
 *
 * Sets how the 3D scene starts.  Call before the window is presented; a
 * no-op in 2D mode.
 */
void gsurf_lrg_window_set_scene_options(GsurfLrgWindow *self,
                                        const GsurfLrgSceneOptions *options);

/**
 * gsurf_lrg_window_get_scene:
 * @self: a #GsurfLrgWindow
 *
 * Returns: (transfer none) (nullable): the window's #GsurfLrgScene, or %NULL
 *   in 2D mode, in a build without 3D, or before the window is presented
 */
gpointer gsurf_lrg_window_get_scene(GsurfLrgWindow *self);

/**
 * gsurf_lrg_window_run_scene_action:
 * @self: a #GsurfLrgWindow
 * @action: a 3D #GsurfAction (the `lrg-*` actions)
 *
 * Performs a 3D-scene action on this window and announces the change through
 * #GsurfLrgWindow::scene-changed.
 *
 * Returns: %TRUE if @action is a 3D action and the window has a scene
 */
gboolean gsurf_lrg_window_run_scene_action(GsurfLrgWindow *self,
                                           GsurfAction action);

/**
 * gsurf_lrg_window_notify_scene:
 * @self: a #GsurfLrgWindow
 * @what: what changed ("arrangement", "environment", "reading", "history",
 *   "focus", "pin", "camera")
 *
 * Emits #GsurfLrgWindow::scene-changed.  For callers (the control socket)
 * that change the scene directly.
 */
void gsurf_lrg_window_notify_scene(GsurfLrgWindow *self, const gchar *what);

G_END_DECLS

#endif /* GSURF_LRG_WINDOW_H */
