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
 * It holds a #GsurfLrgRenderMode; only 2D (a flat textured quad) is implemented.
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
 * Selects the render mode. Only %GSURF_LRG_RENDER_MODE_2D is implemented;
 * passing 3D / 3D-VR emits a warning and falls back to 2D.
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

G_END_DECLS

#endif /* GSURF_LRG_WINDOW_H */
