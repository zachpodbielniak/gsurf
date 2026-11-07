/*
 * gsurf-lrg-view.h - libregnum (LRG) web view
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * #GsurfLrgView is the #GsurfView subclass for the libregnum backend. It owns a
 * #GsurfLrgEngine (the build-selected web engine), translates its signals into
 * the #GsurfView signals, and implements the full #GsurfView vfunc table. On
 * top of the abstract API it exposes the LRG-only surface used by the host
 * compositor (the standalone raylib window, or cmacs's lrgterm): obtaining the
 * rendered page as a #GrlTexture and injecting input.
 *
 * The page is always rendered as a flat 2D texture; the render *mode*
 * (2D / 3D / 3D-VR, see #GsurfLrgRenderMode) is a property of the host window,
 * not the view — only 2D is implemented today.
 */

#ifndef GSURF_LRG_VIEW_H
#define GSURF_LRG_VIEW_H

#include <glib-object.h>

#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_LRG_VIEW (gsurf_lrg_view_get_type())

G_DECLARE_FINAL_TYPE(GsurfLrgView, gsurf_lrg_view, GSURF, LRG_VIEW, GsurfView)

/**
 * gsurf_lrg_view_new:
 *
 * Returns: (transfer full): a new #GsurfLrgView as a #GsurfView.
 */
GsurfView *gsurf_lrg_view_new(void);

/**
 * gsurf_lrg_view_capture:
 * @self: a #GsurfLrgView
 *
 * Grabs the latest rendered page into the view's CPU buffer (no GPU work). Call
 * once per host frame before gsurf_lrg_view_get_texture().
 *
 * Returns: %TRUE if a new frame was produced since the last capture.
 */
gboolean gsurf_lrg_view_capture(GsurfLrgView *self);

/**
 * gsurf_lrg_view_get_texture:
 * @self: a #GsurfLrgView
 * @out_width: (out) (optional): the texture width in pixels
 * @out_height: (out) (optional): the texture height in pixels
 *
 * Returns the page as a libregnum texture, (re)creating and uploading it from
 * the most recent captured frame as needed. MUST be called on the thread that
 * owns the GL context that will draw it (the host's render thread), because it
 * performs GPU uploads.
 *
 * The return is a @code{GrlTexture*} as an opaque #gpointer so this header does
 * not pull in graylib; the caller (which already links libregnum) casts it.
 *
 * Returns: (transfer none) (nullable): the #GrlTexture, or %NULL if no frame /
 *   no GL context yet.
 */
gpointer gsurf_lrg_view_get_texture(GsurfLrgView *self, int *out_width,
                                    int *out_height);

/**
 * gsurf_lrg_view_resize:
 * @self: a #GsurfLrgView
 * @width: new width in device pixels
 * @height: new height in device pixels
 * @scale: device scale factor (1.0 = 100%)
 *
 * Resizes the page viewport / offscreen surface.
 */
void gsurf_lrg_view_resize(GsurfLrgView *self, int width, int height,
                           double scale);

/* --- Input injection --- */

/**
 * gsurf_lrg_view_send_key:
 * @self: a #GsurfLrgView
 * @keysym: an X/GDK keysym
 * @keycode: a hardware keycode, or 0 if unknown
 * @mods: a #GsurfKeyMod modifier mask
 * @pressed: %TRUE for press, %FALSE for release
 */
void gsurf_lrg_view_send_key(GsurfLrgView *self, guint keysym, guint keycode,
                             guint mods, gboolean pressed);

/**
 * gsurf_lrg_view_send_motion:
 * @self: a #GsurfLrgView
 * @x: page-relative x in device pixels
 * @y: page-relative y in device pixels
 * @mods: a #GsurfKeyMod modifier mask
 */
void gsurf_lrg_view_send_motion(GsurfLrgView *self, int x, int y, guint mods);

/**
 * gsurf_lrg_view_send_button:
 * @self: a #GsurfLrgView
 * @x: page-relative x in device pixels
 * @y: page-relative y in device pixels
 * @button: a #GsurfMousebutton
 * @pressed: %TRUE for press, %FALSE for release
 * @mods: a #GsurfKeyMod modifier mask
 */
void gsurf_lrg_view_send_button(GsurfLrgView *self, int x, int y, guint button,
                                gboolean pressed, guint mods);

/**
 * gsurf_lrg_view_send_axis:
 * @self: a #GsurfLrgView
 * @x: page-relative x in device pixels
 * @y: page-relative y in device pixels
 * @dx: horizontal scroll delta (wheel ticks; positive = right)
 * @dy: vertical scroll delta (wheel ticks; positive = down)
 * @mods: a #GsurfKeyMod modifier mask
 */
void gsurf_lrg_view_send_axis(GsurfLrgView *self, int x, int y, double dx,
                              double dy, guint mods);

/**
 * gsurf_lrg_view_set_focus:
 * @self: a #GsurfLrgView
 * @focused: whether the page is focused (caret/IME active, keys routed to page)
 */
void gsurf_lrg_view_set_focus(GsurfLrgView *self, gboolean focused);

/**
 * gsurf_lrg_view_get_focus:
 * @self: a #GsurfLrgView
 *
 * Returns: whether the page currently holds input focus.
 */
gboolean gsurf_lrg_view_get_focus(GsurfLrgView *self);

/**
 * gsurf_lrg_view_get_texture_serial:
 * @self: a #GsurfLrgView
 *
 * A counter bumped every time gsurf_lrg_view_get_texture() uploads new
 * pixels.  A consumer that derives data from the texture (the 3D scene's
 * mipmaps) compares it to know whether to refresh, whoever did the upload.
 *
 * Returns: the upload count
 */
guint64 gsurf_lrg_view_get_texture_serial(GsurfLrgView *self);

/**
 * gsurf_lrg_view_peek_pixels:
 * @self: a #GsurfLrgView
 * @out_width: (out) (optional): frame width in pixels
 * @out_height: (out) (optional): frame height in pixels
 *
 * Borrows the most recent captured frame as tightly packed RGBA8 pixels (the
 * same data gsurf_lrg_view_get_texture() uploads), for callers that keep a
 * copy -- the 3D history view's page thumbnails.  The pointer stays valid
 * only until the next capture, resize or the view's destruction.
 *
 * Returns: (transfer none) (nullable): the pixels, or %NULL before the first
 *   frame
 */
const guint8 *gsurf_lrg_view_peek_pixels(GsurfLrgView *self, int *out_width,
                                         int *out_height);

/**
 * GsurfLrgHistoryEntry:
 * @uri: the entry's URI
 * @title: (nullable): the entry's page title, if WebKit recorded one
 *
 * One back/forward-list entry, as returned by gsurf_lrg_view_get_history().
 * Free with gsurf_lrg_history_entry_free().
 */
typedef struct {
	gchar *uri;
	gchar *title;
} GsurfLrgHistoryEntry;

/**
 * gsurf_lrg_history_entry_free:
 * @entry: (transfer full) (nullable): an entry
 */
void gsurf_lrg_history_entry_free(GsurfLrgHistoryEntry *entry);

/**
 * gsurf_lrg_view_get_history:
 * @self: a #GsurfLrgView
 * @limit: the maximum number of entries to return in each direction
 * @out_back: (out) (transfer full) (element-type GsurfLrgHistoryEntry):
 *   the back list, nearest first (element 0 is one step back)
 * @out_forward: (out) (transfer full) (element-type GsurfLrgHistoryEntry):
 *   the forward list, nearest first
 *
 * Reads the page's back/forward list.  The 3D history-depth view shows these
 * entries as panels receding behind the current page.  The arrays own their
 * elements (freed with gsurf_lrg_history_entry_free()).
 */
void gsurf_lrg_view_get_history(GsurfLrgView *self, guint limit,
                                GPtrArray **out_back, GPtrArray **out_forward);

/**
 * gsurf_lrg_view_go_history:
 * @self: a #GsurfLrgView
 * @offset: steps to move: negative goes back, positive goes forward
 *
 * Jumps straight to a back/forward-list entry (several steps at once).
 *
 * Returns: %TRUE if an entry exists at @offset and navigation started
 */
gboolean gsurf_lrg_view_go_history(GsurfLrgView *self, gint offset);

G_END_DECLS

#endif /* GSURF_LRG_VIEW_H */
