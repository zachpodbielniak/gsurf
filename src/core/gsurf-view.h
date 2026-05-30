/*
 * gsurf-view.h - Abstract web view
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GsurfView is the central, backend-agnostic abstraction over a single
 * web view. Concrete subclasses (GsurfWebkit2View for GTK3/WebKit2GTK
 * 4.1, GsurfWebkit6View for GTK4/WebKitGTK 6.0) wrap the native
 * WebKitWebView and translate its signals into the GsurfView signals
 * below. The rest of the library depends only on this interface, never
 * on GTK/WebKit directly. Mirrors the abstract-derivable pattern of
 * gst's GstRenderer.
 */

#ifndef GSURF_VIEW_H
#define GSURF_VIEW_H

#include <glib-object.h>
#include <gio/gio.h>

#include "gsurf-types.h"
#include "gsurf-enums.h"
#include "config/gsurf-settings.h"

G_BEGIN_DECLS

#define GSURF_TYPE_VIEW (gsurf_view_get_type())

G_DECLARE_DERIVABLE_TYPE(GsurfView, gsurf_view, GSURF, VIEW, GObject)

/**
 * GsurfViewClass:
 * @parent_class: the parent class
 * @load_uri: load a URI
 * @load_html: load an HTML string with a base URI
 * @reload: reload (optionally bypassing the cache)
 * @stop_loading: stop the current load
 * @go_back: navigate back in history
 * @go_forward: navigate forward in history
 * @can_go_back: whether back navigation is possible
 * @can_go_forward: whether forward navigation is possible
 * @get_uri: the current URI
 * @get_title: the current title
 * @get_estimated_load_progress: load progress 0.0-1.0
 * @set_zoom_level: set the zoom level (1.0 = 100%)
 * @get_zoom_level: get the zoom level
 * @apply_settings: translate #GsurfSettings into the native engine
 * @run_javascript_async: evaluate JavaScript asynchronously
 * @run_javascript_finish: finish gsurf_view_run_javascript_async()
 * @get_snapshot_async: capture a snapshot asynchronously
 * @get_snapshot_finish: finish a snapshot (returns PNG #GBytes)
 * @get_native_widget: the underlying native widget (as gpointer)
 *
 * Virtual table for #GsurfView. Backend subclasses fill this in.
 */
struct _GsurfViewClass
{
	GObjectClass parent_class;

	void         (*load_uri)                    (GsurfView *self, const gchar *uri);
	void         (*load_html)                   (GsurfView *self, const gchar *html, const gchar *base_uri);
	void         (*reload)                      (GsurfView *self, gboolean bypass_cache);
	void         (*stop_loading)                (GsurfView *self);
	void         (*go_back)                     (GsurfView *self);
	void         (*go_forward)                  (GsurfView *self);
	gboolean     (*can_go_back)                 (GsurfView *self);
	gboolean     (*can_go_forward)              (GsurfView *self);
	const gchar *(*get_uri)                     (GsurfView *self);
	const gchar *(*get_title)                   (GsurfView *self);
	gdouble      (*get_estimated_load_progress) (GsurfView *self);
	void         (*set_zoom_level)              (GsurfView *self, gdouble level);
	gdouble      (*get_zoom_level)              (GsurfView *self);
	void         (*apply_settings)              (GsurfView *self, GsurfSettings *settings);
	void         (*run_javascript_async)        (GsurfView *self, const gchar *script,
	                                             GCancellable *cancellable,
	                                             GAsyncReadyCallback callback, gpointer user_data);
	gchar       *(*run_javascript_finish)       (GsurfView *self, GAsyncResult *result, GError **error);
	void         (*get_snapshot_async)          (GsurfView *self, GCancellable *cancellable,
	                                             GAsyncReadyCallback callback, gpointer user_data);
	GBytes      *(*get_snapshot_finish)         (GsurfView *self, GAsyncResult *result, GError **error);
	gpointer     (*get_native_widget)           (GsurfView *self);

	/* Extended API (used by modules: inspector, userscripts, site-styles,
	 * dark-mode, find-bar, cookie-policy). */
	void         (*show_inspector)              (GsurfView *self);
	void         (*add_user_script)             (GsurfView *self, const gchar *source, gboolean at_end);
	void         (*add_user_style)              (GsurfView *self, const gchar *css);
	void         (*clear_user_content)          (GsurfView *self);
	void         (*find)                        (GsurfView *self, const gchar *text,
	                                             gboolean case_sensitive, gboolean forward);
	void         (*find_next)                   (GsurfView *self);
	void         (*find_previous)               (GsurfView *self);
	void         (*set_cookie_accept_policy)    (GsurfView *self, gint policy);
	void         (*set_proxy)                   (GsurfView *self, const gchar *uri);
	void         (*copy_uri)                    (GsurfView *self);
	gchar       *(*read_clipboard_text)         (GsurfView *self);
	void         (*add_user_script_full)        (GsurfView *self, const gchar *source,
	                                             gboolean at_end, const gchar *const *allow);
	void         (*add_user_style_full)         (GsurfView *self, const gchar *css,
	                                             const gchar *const *allow);
	void         (*add_content_filter)          (GsurfView *self, const gchar *identifier,
	                                             const gchar *json_rules);

	gpointer padding[8];
};

/* --- Navigation --- */
void         gsurf_view_load_uri(GsurfView *self, const gchar *uri);
void         gsurf_view_load_html(GsurfView *self, const gchar *html, const gchar *base_uri);
void         gsurf_view_reload(GsurfView *self, gboolean bypass_cache);
void         gsurf_view_stop_loading(GsurfView *self);
void         gsurf_view_go_back(GsurfView *self);
void         gsurf_view_go_forward(GsurfView *self);
gboolean     gsurf_view_can_go_back(GsurfView *self);
gboolean     gsurf_view_can_go_forward(GsurfView *self);

/* --- State --- */
const gchar *gsurf_view_get_uri(GsurfView *self);
const gchar *gsurf_view_get_title(GsurfView *self);
gdouble      gsurf_view_get_estimated_load_progress(GsurfView *self);

/* --- Zoom --- */
void         gsurf_view_set_zoom_level(GsurfView *self, gdouble level);
gdouble      gsurf_view_get_zoom_level(GsurfView *self);

/* --- Settings --- */
void         gsurf_view_apply_settings(GsurfView *self, GsurfSettings *settings);

/* --- Scripting / capture --- */
void         gsurf_view_run_javascript_async(GsurfView *self, const gchar *script,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback, gpointer user_data);
gchar       *gsurf_view_run_javascript_finish(GsurfView *self, GAsyncResult *result, GError **error);
void         gsurf_view_get_snapshot_async(GsurfView *self, GCancellable *cancellable,
                                           GAsyncReadyCallback callback, gpointer user_data);
GBytes      *gsurf_view_get_snapshot_finish(GsurfView *self, GAsyncResult *result, GError **error);

/* --- Editing state (DOM focus awareness) --- */

/**
 * gsurf_view_get_editing:
 * @self: a #GsurfView
 *
 * Whether an editable element (text input, textarea, contenteditable,
 * select) is currently focused in the page. The backend keeps this
 * updated from page focus events; modal input handlers use it to pass
 * keystrokes through to the field instead of treating them as commands.
 *
 * Returns: %TRUE if the page has an editable element focused
 */
gboolean gsurf_view_get_editing(GsurfView *self);

/**
 * gsurf_view_set_editing:
 * @self: a #GsurfView
 * @editing: whether an editable element is focused
 *
 * Sets the editing state. Called by the backend; not normally used
 * directly.
 */
void gsurf_view_set_editing(GsurfView *self, gboolean editing);

/* --- Embedding --- */
/**
 * gsurf_view_get_native_widget:
 * @self: a #GsurfView
 *
 * Returns the underlying native widget (a #GtkWidget) as an opaque
 * pointer so the abstract API does not leak the GTK type. Embedders
 * cast it in their own GTK-aware code.
 *
 * Returns: (transfer none): the native widget, or %NULL
 */
gpointer     gsurf_view_get_native_widget(GsurfView *self);

/* --- Extended API --- */

/**
 * gsurf_view_show_inspector:
 * @self: a #GsurfView
 *
 * Opens the web inspector / developer tools, if supported.
 */
void gsurf_view_show_inspector(GsurfView *self);

/**
 * gsurf_view_add_user_script:
 * @self: a #GsurfView
 * @source: the JavaScript source
 * @at_end: inject at document end (%TRUE) or start (%FALSE)
 *
 * Injects a user script into all frames of the view.
 */
void gsurf_view_add_user_script(GsurfView *self, const gchar *source, gboolean at_end);

/**
 * gsurf_view_add_user_style:
 * @self: a #GsurfView
 * @css: the CSS source
 *
 * Applies a user stylesheet to the view.
 */
void gsurf_view_add_user_style(GsurfView *self, const gchar *css);

/**
 * gsurf_view_clear_user_content:
 * @self: a #GsurfView
 *
 * Removes all injected user scripts and stylesheets.
 */
void gsurf_view_clear_user_content(GsurfView *self);

/**
 * gsurf_view_find:
 * @self: a #GsurfView
 * @text: the text to search for
 * @case_sensitive: whether the search is case-sensitive
 * @forward: search forward (%TRUE) or backward (%FALSE)
 *
 * Starts an in-page text search.
 */
void gsurf_view_find(GsurfView *self, const gchar *text, gboolean case_sensitive, gboolean forward);

/**
 * gsurf_view_find_next:
 * @self: a #GsurfView
 *
 * Moves to the next search match.
 */
void gsurf_view_find_next(GsurfView *self);

/**
 * gsurf_view_find_previous:
 * @self: a #GsurfView
 *
 * Moves to the previous search match.
 */
void gsurf_view_find_previous(GsurfView *self);

/**
 * gsurf_view_set_cookie_accept_policy:
 * @self: a #GsurfView
 * @policy: 0 = always, 1 = never, 2 = no third-party
 *
 * Sets the cookie accept policy on the view's network session.
 */
void gsurf_view_set_cookie_accept_policy(GsurfView *self, gint policy);

/**
 * gsurf_view_set_proxy:
 * @self: a #GsurfView
 * @uri: (nullable): a proxy URI (e.g. "socks5://127.0.0.1:9050"), "" or
 *   %NULL to use the system default
 *
 * Sets the network proxy for the view's web context.
 */
void gsurf_view_set_proxy(GsurfView *self, const gchar *uri);

/**
 * gsurf_view_copy_uri:
 * @self: a #GsurfView
 *
 * Copies the current URI to the clipboard.
 */
void gsurf_view_copy_uri(GsurfView *self);

/**
 * gsurf_view_read_clipboard_text:
 * @self: a #GsurfView
 *
 * Returns: (transfer full) (nullable): the clipboard text
 */
gchar *gsurf_view_read_clipboard_text(GsurfView *self);

/**
 * gsurf_view_add_user_script_for:
 * @self: a #GsurfView
 * @source: the JavaScript source
 * @at_end: inject at document end (%TRUE) or start (%FALSE)
 * @allow: (nullable) (array zero-terminated=1): URL allow-patterns, %NULL = all
 *
 * Injects a user script, optionally scoped to URLs matching @allow.
 */
void gsurf_view_add_user_script_for(GsurfView *self, const gchar *source,
                                    gboolean at_end, const gchar *const *allow);

/**
 * gsurf_view_add_user_style_for:
 * @self: a #GsurfView
 * @css: the CSS source
 * @allow: (nullable) (array zero-terminated=1): URL allow-patterns, %NULL = all
 *
 * Applies a user stylesheet, optionally scoped to URLs matching @allow.
 */
void gsurf_view_add_user_style_for(GsurfView *self, const gchar *css, const gchar *const *allow);

/**
 * gsurf_view_add_content_filter:
 * @self: a #GsurfView
 * @identifier: a stable id for the compiled filter
 * @json_rules: a WebKit content-blocker JSON ruleset
 *
 * Compiles and applies a content-blocker ruleset (sub-resource blocking).
 */
void gsurf_view_add_content_filter(GsurfView *self, const gchar *identifier, const gchar *json_rules);

/**
 * gsurf_view_get_hovered_uri:
 * @self: a #GsurfView
 *
 * Returns: (transfer none) (nullable): the link URI under the pointer
 */
const gchar *gsurf_view_get_hovered_uri(GsurfView *self);

/**
 * gsurf_view_set_hovered_uri:
 * @self: a #GsurfView
 * @uri: (nullable): the hovered link URI
 *
 * Backend setter; emits #GsurfView::hovered-uri-changed.
 */
void gsurf_view_set_hovered_uri(GsurfView *self, const gchar *uri);

/* --- Signal emission helpers (for backend subclasses) --- */
void gsurf_view_emit_load_changed(GsurfView *self, GsurfLoadEvent event);
void gsurf_view_emit_uri_changed(GsurfView *self, const gchar *uri);
void gsurf_view_emit_title_changed(GsurfView *self, const gchar *title);
void gsurf_view_emit_progress_changed(GsurfView *self, gdouble progress);
void gsurf_view_emit_favicon_changed(GsurfView *self);
/* Returns a new view to host a popup/new-window, or %NULL to block it. */
GsurfView *gsurf_view_emit_create_view(GsurfView *self, const gchar *uri);
void gsurf_view_emit_web_process_terminated(GsurfView *self);

G_END_DECLS

#endif /* GSURF_VIEW_H */
