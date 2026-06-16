/*
 * gsurf-lrg-engine.h - Web-engine seam for the libregnum (LRG) backend
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The LRG backend renders a web page into a CPU RGBA buffer that the host
 * (the standalone raylib window, or cmacs's lrgterm) uploads to a GL texture
 * and composites. *Which* web engine produces those pixels is selected at
 * build time, because the WebKit family all export the same webkit_* symbols
 * and so only one may be linked per process:
 *
 *   - GSURF_LRG_ENGINE_WEBKITGTK: render an offscreen WebKitGTK WebView (the
 *     same webkit2gtk-4.1 / webkitgtk-6.0 already linked by the GTK backend)
 *     and read back its pixels. This is the engine used inside cmacs and on
 *     platforms without WPE.
 *   - GSURF_LRG_ENGINE_WPE: WPE WebKit + wpebackend-fdo (a GTK-free build).
 *     Reserved; selected only when no GTK backend is linked.
 *
 * This header is the engine-agnostic contract #GsurfLrgView talks to. The
 * concrete engine lives in gsurf-lrg-engine-webkitgtk.c / -wpe.c (exactly one
 * is compiled). It is internal to the LRG backend; never installed.
 */

#ifndef GSURF_LRG_ENGINE_H
#define GSURF_LRG_ENGINE_H

#include <glib-object.h>

#include "core/gsurf-view.h"

G_BEGIN_DECLS

/**
 * GsurfLrgEngine:
 *
 * Opaque per-view web-engine instance. Owns the native web view and the
 * offscreen rendering surface; produces a CPU RGBA frame and accepts input.
 */
typedef struct _GsurfLrgEngine GsurfLrgEngine;

/**
 * gsurf_lrg_engine_backend_init:
 * @argc: (inout) (optional): address of argc from main()
 * @argv: (inout) (array length=argc) (optional): address of argv
 * @error: (out) (optional): location for an error
 *
 * One-time, process-wide initialization for the selected engine (e.g.
 * gtk_init_check() for the WebKitGTK engine, or the WPE/EGL bootstrap for the
 * WPE engine). Idempotent.
 *
 * Returns: %TRUE on success.
 */
gboolean gsurf_lrg_engine_backend_init(int *argc, char ***argv, GError **error);

/**
 * gsurf_lrg_engine_get_engine_name:
 *
 * Returns: (transfer none): the compiled-in engine name ("lrg-webkitgtk" or
 *   "lrg-wpe").
 */
const gchar *gsurf_lrg_engine_get_engine_name(void);

/**
 * gsurf_lrg_engine_get_ui_font_path:
 *
 * A TTF/OTF file for the LRG chrome text: the @env{GSURF_LRG_FONT} override,
 * else the GTK theme's font family (so it matches the GTK client), else
 * "sans-serif" — resolved to a file via fontconfig.  Cached.
 *
 * Returns: (transfer none) (nullable): the font path, or %NULL if none found.
 */
const char *gsurf_lrg_engine_get_ui_font_path(void);

/**
 * gsurf_lrg_engine_new:
 * @owner: the #GsurfView that owns this engine (for signal emission)
 * @error: (out) (optional): location for an error
 *
 * Creates a per-view engine: the native web view + its offscreen surface. The
 * caller wires the native web view's signals (obtained via
 * gsurf_lrg_engine_get_webview()) to translate them into #GsurfView signals.
 *
 * Returns: (transfer full) (nullable): a new engine, or %NULL on error.
 */
GsurfLrgEngine *gsurf_lrg_engine_new(GsurfView *owner, GError **error);

/**
 * gsurf_lrg_engine_free:
 * @self: (transfer full): an engine
 *
 * Releases the engine and its native web view + surface.
 */
void gsurf_lrg_engine_free(GsurfLrgEngine *self);

/**
 * gsurf_lrg_engine_get_webview:
 * @self: an engine
 *
 * The native web view object. For the WebKitGTK engine this is a
 * @code{WebKitWebView*}; the shared vfunc code in gsurf-lrg-view.c casts it.
 *
 * Returns: (transfer none): the native web view.
 */
gpointer gsurf_lrg_engine_get_webview(GsurfLrgEngine *self);

/**
 * gsurf_lrg_engine_set_size:
 * @self: an engine
 * @width: new width in device pixels
 * @height: new height in device pixels
 * @scale: device scale factor (1.0 = 100%)
 *
 * Resizes the offscreen surface / viewport. A no-op if unchanged.
 */
void gsurf_lrg_engine_set_size(GsurfLrgEngine *self, int width, int height,
                               double scale);

/**
 * gsurf_lrg_engine_capture:
 * @self: an engine
 *
 * Grabs the current rendered page into the engine's CPU RGBA buffer (no GPU
 * work). Cheap and safe to call from any thread that owns the GLib main
 * context; call it once per host frame before gsurf_lrg_engine_get_pixels().
 *
 * Returns: %TRUE if a new frame was produced since the last capture.
 */
gboolean gsurf_lrg_engine_capture(GsurfLrgEngine *self);

/**
 * gsurf_lrg_engine_get_pixels:
 * @self: an engine
 * @out_width: (out) (optional): the buffer width
 * @out_height: (out) (optional): the buffer height
 *
 * The most recently captured frame as tightly-packed RGBA (R8G8B8A8) bytes,
 * @out_width * @out_height * 4 in size. Valid until the next
 * gsurf_lrg_engine_capture() / set_size() / free().
 *
 * Returns: (transfer none) (nullable): the RGBA buffer, or %NULL if none yet.
 */
const guint8 *gsurf_lrg_engine_get_pixels(GsurfLrgEngine *self,
                                          int *out_width, int *out_height);

/* --- Input injection (coordinates are page-relative device pixels) --- */

/**
 * gsurf_lrg_engine_send_key:
 * @self: an engine
 * @keysym: an X/GDK keysym (e.g. GDK_KEY_a)
 * @keycode: a hardware keycode, or 0 if unknown
 * @mods: a #GsurfKeyMod modifier mask
 * @pressed: %TRUE for press, %FALSE for release
 */
void gsurf_lrg_engine_send_key(GsurfLrgEngine *self, guint keysym, guint keycode,
                               guint mods, gboolean pressed);

/**
 * gsurf_lrg_engine_send_motion:
 * @self: an engine
 * @x: page-relative x
 * @y: page-relative y
 * @mods: a #GsurfKeyMod modifier mask
 */
void gsurf_lrg_engine_send_motion(GsurfLrgEngine *self, int x, int y, guint mods);

/**
 * gsurf_lrg_engine_send_button:
 * @self: an engine
 * @x: page-relative x
 * @y: page-relative y
 * @button: a #GsurfMousebutton
 * @pressed: %TRUE for press, %FALSE for release
 * @mods: a #GsurfKeyMod modifier mask
 */
void gsurf_lrg_engine_send_button(GsurfLrgEngine *self, int x, int y,
                                  guint button, gboolean pressed, guint mods);

/**
 * gsurf_lrg_engine_send_axis:
 * @self: an engine
 * @x: page-relative x
 * @y: page-relative y
 * @dx: horizontal scroll delta (wheel ticks; positive = right)
 * @dy: vertical scroll delta (wheel ticks; positive = down)
 * @mods: a #GsurfKeyMod modifier mask
 */
void gsurf_lrg_engine_send_axis(GsurfLrgEngine *self, int x, int y,
                                double dx, double dy, guint mods);

/**
 * gsurf_lrg_engine_set_focus:
 * @self: an engine
 * @focused: whether the page should be treated as focused (caret/IME active)
 */
void gsurf_lrg_engine_set_focus(GsurfLrgEngine *self, gboolean focused);

G_END_DECLS

#endif /* GSURF_LRG_ENGINE_H */
