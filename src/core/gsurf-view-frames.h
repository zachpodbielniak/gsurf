/*
 * gsurf-view-frames.h - A view's frames: list them, run a script in one
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GSURF_VIEW_FRAMES_H
#define GSURF_VIEW_FRAMES_H

#include "gsurf-view.h"

G_BEGIN_DECLS

/**
 * gsurf_view_list_frames_async:
 * @self: a #GsurfView
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (nullable) (closure user_data): completion callback
 * @user_data: data for @callback
 *
 * Lists the frames of @self's page, cross-origin ones included, in
 * document order with the main frame first.  Needs the frames module's
 * web-process extension; without it the call completes with
 * %G_IO_ERROR_NOT_SUPPORTED.  Finish with gsurf_view_list_frames_finish().
 *
 * Since: 0.6.0
 */
void gsurf_view_list_frames_async(GsurfView *self, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data);

/**
 * gsurf_view_list_frames_finish:
 * @self: a #GsurfView
 * @result: the #GAsyncResult passed to the callback
 * @error: return location for a #GError
 *
 * Finishes gsurf_view_list_frames_async().
 *
 * Returns: (transfer full) (nullable): a JSON array of objects {id, uri,
 *   main, parent (an id, or null for the main frame), x, y, width, height
 *   (the frame's content box in the view's CSS pixels), visible}, or
 *   %NULL with @error set
 *
 * Since: 0.6.0
 */
gchar *gsurf_view_list_frames_finish(GsurfView *self, GAsyncResult *result, GError **error);

/**
 * gsurf_view_run_javascript_in_frame_async:
 * @self: a #GsurfView
 * @frame: (nullable): the frame: its id (decimal, from
 *   gsurf_view_list_frames_async()) or its site ("host", "host:port" or
 *   "*.host", matched against the host of the frame's URI, never text in
 *   it); %NULL is the main frame.  Several frames on the site: the one
 *   visible frame among them, else %G_IO_ERROR_NOT_FOUND naming their ids
 * @require: (nullable): a CSS selector: only frames on the site with an
 *   element matching it are considered
 * @script: JavaScript to evaluate in the frame's page world
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (nullable) (closure user_data): completion callback
 * @user_data: data for @callback
 *
 * Like gsurf_view_run_javascript_async(), in one frame of the page.  No
 * matching frame completes with %G_IO_ERROR_NOT_FOUND, a script that
 * throws with %G_IO_ERROR_FAILED, and a page without the frames module's
 * extension with %G_IO_ERROR_NOT_SUPPORTED.  Finish with
 * gsurf_view_run_javascript_in_frame_finish().
 *
 * Since: 0.6.0
 */
void gsurf_view_run_javascript_in_frame_async(GsurfView *self, const gchar *frame,
                                              const gchar *require, const gchar *script,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);

/**
 * gsurf_view_run_javascript_in_frame_finish:
 * @self: a #GsurfView
 * @result: the #GAsyncResult passed to the callback
 * @x: (out) (optional): the frame's content-box left in the view, after
 *   the script ran (0 for the main frame)
 * @y: (out) (optional): its top
 * @frame_id: (out) (optional): the id of the frame the script ran in (0
 *   for the main frame): the same frame again, even where several URIs
 *   match
 * @error: return location for a #GError
 *
 * Finishes gsurf_view_run_javascript_in_frame_async().  A point the
 * script measured in the frame (getBoundingClientRect()) is a point in
 * the view once @x and @y are added, which is what gsurf_view_send_click()
 * takes.
 *
 * Returns: (transfer full) (nullable): the value as a string (strings
 *   as-is, anything else as JSON), or %NULL with @error set
 *
 * Since: 0.6.0
 */
gchar *gsurf_view_run_javascript_in_frame_finish(GsurfView *self, GAsyncResult *result,
                                                 gdouble *x, gdouble *y, guint64 *frame_id,
                                                 GError **error);

/**
 * gsurf_view_send_click_in_frame:
 * @self: a #GsurfView
 * @frame: (nullable): the frame, as gsurf_view_run_javascript_in_frame_async()
 *   takes it; %NULL is the main frame
 * @x: the point, in the view's CSS pixels
 * @y: the point, in the view's CSS pixels
 * @frame_x: the frame's content-box left in the view
 *   (gsurf_view_run_javascript_in_frame_finish())
 * @frame_y: its top
 * @button: 1 (left), 2 (middle) or 3 (right)
 * @modifiers: #GsurfModifier flags held during the click
 * @error: return location for a #GError
 *
 * Like gsurf_view_send_click().  A backend with native input clicks the
 * view at @x, @y, which lands in the frame shown there; one without
 * dispatches the DOM events inside @frame itself, at the point less the
 * frame's origin, since a script in the parent cannot reach into a
 * cross-origin frame.
 *
 * Returns: %TRUE when the click was handed over
 *
 * Since: 0.6.0
 */
gboolean gsurf_view_send_click_in_frame(GsurfView *self, const gchar *frame, gdouble x,
                                        gdouble y, gdouble frame_x, gdouble frame_y,
                                        guint button, guint modifiers, GError **error);

/**
 * gsurf_view_type_in_frame_async:
 * @self: a #GsurfView
 * @frame: the frame, as gsurf_view_run_javascript_in_frame_async() takes it
 * @selector: the field in @frame the text is for
 * @x: the field's point in the view's CSS pixels (where
 *   gsurf_view_locate_script() put it, plus the frame's origin)
 * @y: its top coordinate
 * @frame_x: the frame's content-box left in the view
 * @frame_y: its top
 * @text: UTF-8 text; a newline is Enter
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (nullable) (closure user_data): completion callback
 * @user_data: data for @callback
 *
 * Types @text into a field inside @frame.  A script cannot move the focus
 * into a cross-origin frame (WebKit wants a user gesture), so where the
 * backend has native input the field is clicked at @x, @y first, as a
 * person would, and the keys follow once the field has the focus (WebKit
 * queues clicks and keys apart, so a key sent at once could land first);
 * the caret is where the click put it.  Without native input the DOM
 * events are dispatched in @frame, at the field.  Completes once the keys
 * are handed over; finish with gsurf_view_type_in_frame_finish().
 *
 * Since: 0.6.0
 */
void gsurf_view_type_in_frame_async(GsurfView *self, const gchar *frame, const gchar *selector,
                                    gdouble x, gdouble y, gdouble frame_x, gdouble frame_y,
                                    const gchar *text, GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer user_data);

/**
 * gsurf_view_type_in_frame_finish:
 * @self: a #GsurfView
 * @result: the #GAsyncResult passed to the callback
 * @error: return location for a #GError
 *
 * Finishes gsurf_view_type_in_frame_async().
 *
 * Returns: %TRUE when the text was handed over
 *
 * Since: 0.6.0
 */
gboolean gsurf_view_type_in_frame_finish(GsurfView *self, GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* GSURF_VIEW_FRAMES_H */
