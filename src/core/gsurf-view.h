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

/**
 * GsurfViewInput:
 * @GSURF_VIEW_INPUT_KEY: press and release one key (keyval, modifiers)
 * @GSURF_VIEW_INPUT_CLICK: press and release a pointer button at a point
 *   in the view (x, y, button, modifiers)
 * @GSURF_VIEW_INPUT_TEXT: type a string, one character at a time
 *
 * What #GsurfViewClass.send_input is asked to do (since 0.5.10).
 */
typedef enum {
	GSURF_VIEW_INPUT_KEY,
	GSURF_VIEW_INPUT_CLICK,
	GSURF_VIEW_INPUT_TEXT
} GsurfViewInput;

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
 * @focus: give keyboard focus to the page (see gsurf_view_focus())
 * @send_input: deliver input as the native toolkit's own events, so the
 *   page sees it as trusted (isTrusted); %NULL makes gsurf_view_send_key(),
 *   gsurf_view_send_click() and gsurf_view_send_text() dispatch DOM events
 *   from JavaScript instead (since 0.5.10)
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

	/* Keyboard focus: move toolkit focus onto the page. */
	void         (*focus)                       (GsurfView *self);

	/* Isolated script worlds and page -> host messages (form autofill,
	 * per-site dark mode). @world %NULL is the page's main world. */
	void         (*register_script_message)     (GsurfView *self, const gchar *name,
	                                             const gchar *world);
	void         (*add_user_script_in_world)    (GsurfView *self, const gchar *source,
	                                             gboolean at_end, const gchar *const *allow,
	                                             const gchar *world);
	void         (*run_javascript_in_world_async) (GsurfView *self, const gchar *script,
	                                             const gchar *world, GCancellable *cancellable,
	                                             GAsyncReadyCallback callback, gpointer user_data);
	gchar       *(*run_javascript_in_world_finish) (GsurfView *self, GAsyncResult *result,
	                                             GError **error);

	/* The page favicon as the native image type. */
	gpointer     (*get_favicon)                 (GsurfView *self);

	gboolean     (*send_input)                  (GsurfView *self, GsurfViewInput kind,
	                                             gdouble x, gdouble y, guint code,
	                                             guint modifiers, const gchar *text,
	                                             GError **error);

	gpointer padding[1];
};

/* --- Navigation --- */

/**
 * gsurf_view_normalize_uri:
 * @input: a URI or bare address as typed by the user
 *
 * Normalizes @input into a loadable URI.
 *
 * A bare host such as "duckduckgo.com", "localhost:8080" or "192.168.0.1"
 * gains an "https://" prefix. Filesystem locations become file URIs: absolute
 * paths, "~/…" and "~user/…" paths, "./" and "../" paths, relative paths that
 * contain a slash and do not look like a host, and any relative name that
 * names an existing file or directory. "file:" URIs are canonicalized to the
 * local "file:///…" form when they have no userinfo, port, or foreign host.
 * Their query and fragment are kept. Percent-encoding, dot segments, and
 * scheme case are normalized. A non-local file host is not remapped onto the
 * local disk.
 *
 * Every other scheme ("https://…", "about:blank", "data:…", "mailto:…",
 * "gopher://…", "gemini://…", "view-source:…", and so on) is returned with
 * its original spelling. Search phrases stay unchanged. Leading and trailing
 * whitespace is trimmed. %NULL stays %NULL. Applied automatically by
 * gsurf_view_load_uri(), which is how the command line, the address bar, and
 * embedders load a path.
 *
 * Returns: (transfer full) (nullable): a newly-allocated normalized URI.
 */
gchar       *gsurf_view_normalize_uri(const gchar *input);
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

/**
 * gsurf_view_get_container_user_agent:
 * @self: a #GsurfView
 *
 * The user agent of the view's container (see
 * gsurf_container_registry_set_network()).  gsurf_view_apply_settings()
 * sends it in place of the global user agent; a per-site user agent (the
 * useragent module's rules) still wins.  Setting providers that would set
 * a general default should leave the container's alone.
 *
 * Returns: (transfer none) (nullable): the container's user agent, or
 *   %NULL (since 0.5.5)
 */
const gchar *gsurf_view_get_container_user_agent(GsurfView *self);

/* --- Scripting / capture --- */
void         gsurf_view_run_javascript_async(GsurfView *self, const gchar *script,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback, gpointer user_data);
gchar       *gsurf_view_run_javascript_finish(GsurfView *self, GAsyncResult *result, GError **error);
void         gsurf_view_get_snapshot_async(GsurfView *self, GCancellable *cancellable,
                                           GAsyncReadyCallback callback, gpointer user_data);
GBytes      *gsurf_view_get_snapshot_finish(GsurfView *self, GAsyncResult *result, GError **error);

/* --- Input (since 0.5.10) --- */

/**
 * gsurf_view_send_key:
 * @self: a #GsurfView
 * @keyval: an X/GDK keysym (gsurf_view_keyval_from_name() resolves names)
 * @modifiers: #GsurfKeyMod flags held while it is pressed
 * @error: (out) (optional): return location for a #GError
 *
 * Presses and releases one key in the page, as the keyboard would.  On
 * a backend with native input (GTK3) the page sees a trusted event; on
 * the others it is dispatched from JavaScript and is not trusted.
 *
 * Returns: %TRUE if the input was delivered
 */
gboolean     gsurf_view_send_key(GsurfView *self, guint keyval, guint modifiers,
                                 GError **error);

/**
 * gsurf_view_send_click:
 * @self: a #GsurfView
 * @x: horizontal position in the view, in CSS pixels from its left edge
 * @y: vertical position in the view, in CSS pixels from its top edge
 * @button: 1 left, 2 middle, 3 right
 * @modifiers: #GsurfKeyMod flags held during the click
 * @error: (out) (optional): return location for a #GError
 *
 * Moves the pointer to (@x, @y) and presses and releases @button there.
 * Trusted where the backend has native input, as for gsurf_view_send_key().
 *
 * Returns: %TRUE if the input was delivered
 */
gboolean     gsurf_view_send_click(GsurfView *self, gdouble x, gdouble y, guint button,
                                   guint modifiers, GError **error);

/**
 * gsurf_view_send_text:
 * @self: a #GsurfView
 * @text: UTF-8 text; a newline is the Return key
 * @error: (out) (optional): return location for a #GError
 *
 * Types @text into whatever has the page's focus, one key at a time.
 * Trusted where the backend has native input, as for gsurf_view_send_key().
 *
 * Returns: %TRUE if the input was delivered
 */
gboolean     gsurf_view_send_text(GsurfView *self, const gchar *text, GError **error);

/**
 * gsurf_view_keyval_from_name:
 * @name: a key name -- "Enter", "Tab", "Escape", "Backspace", "Delete",
 *   "ArrowLeft"/"Left", "Home", "PageDown", "space", ... -- or one character
 *
 * The keysym gsurf_view_send_key() takes for @name.  The DOM's names
 * (KeyboardEvent.key) and the X ones are both accepted, case-insensitively
 * for the named keys.
 *
 * Returns: the keysym, or 0 when @name is not a key
 */
guint        gsurf_view_keyval_from_name(const gchar *name);

/**
 * gsurf_view_locate_script:
 * @selector: a CSS selector
 *
 * JavaScript that finds the first element matching @selector, scrolls it
 * into view, focuses it, and evaluates to a JSON array [x, y] -- the
 * centre of its box in view coordinates, ready for
 * gsurf_view_send_click() -- or to null when nothing matches.  The one
 * spelling for every caller that clicks or types "at a selector", so they
 * agree on what "the element" is.
 *
 * Returns: (transfer full): the script
 */
gchar       *gsurf_view_locate_script(const gchar *selector);

/* --- Editing state (DOM focus awareness) --- */

/**
 * gsurf_view_get_editing:
 * @self: a #GsurfView
 *
 * Whether an editable element (text input, textarea, contenteditable,
 * select) is currently focused in the page. The backend keeps this
 * updated from page focus events (and clears it when a load commits or
 * the web process dies); modal input handlers use it to pass keystrokes
 * through to the field instead of treating them as commands, and the
 * published input mode reads INSERT while the active view is editing.
 * Changes are notified as #GsurfView:editing.
 *
 * Returns: %TRUE if the page has an editable element focused
 */
gboolean gsurf_view_get_editing(GsurfView *self);

/**
 * gsurf_view_set_editing:
 * @self: a #GsurfView
 * @editing: whether an editable element is focused
 *
 * Sets the editing state and notifies #GsurfView:editing when it
 * changes. Called by the backend, and by code that focuses a field
 * itself and knows the result before the page's asynchronous report
 * (the modal module's `gi`); not normally used otherwise.
 */
void gsurf_view_set_editing(GsurfView *self, gboolean editing);

/**
 * gsurf_view_blur_editable:
 * @self: a #GsurfView
 *
 * Leaves the focused editable element in the page: blurs
 * `document.activeElement` and clears the editing state at once, so the
 * next key is treated as a command rather than typing (the page's own
 * focusout report arrives asynchronously and would otherwise lag one key
 * behind). Bound to Escape while a page field has focus.
 */
void gsurf_view_blur_editable(GsurfView *self);

/* --- Keyboard focus --- */

/**
 * gsurf_view_focus:
 * @self: a #GsurfView
 *
 * Moves keyboard focus onto the page itself, away from window chrome such
 * as the address bar or the find bar. Backends without toolkit focus (the
 * libregnum backend) treat this as a no-op.
 */
void gsurf_view_focus(GsurfView *self);

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

/* --- Identity, opener and container --- */

/**
 * gsurf_view_get_id:
 * @self: a #GsurfView
 *
 * Returns: a process-unique, never reused id (starting at 1), assigned
 *   at construction. Stable across tab moves, so the tab tree, sessions
 *   and the MCP interface can refer to views by it.
 */
guint64 gsurf_view_get_id(GsurfView *self);

/**
 * gsurf_view_lookup:
 * @id: a view id
 *
 * Returns: (transfer none) (nullable): the live view with @id, or %NULL
 */
GsurfView *gsurf_view_lookup(guint64 id);

/**
 * gsurf_view_get_opener:
 * @self: a #GsurfView
 *
 * The view this one was opened from (link in new tab, popup, hint), set
 * at construction. Held weakly: %NULL once the opener is finalized.
 *
 * Returns: (transfer none) (nullable): the opener
 */
GsurfView *gsurf_view_get_opener(GsurfView *self);

/**
 * gsurf_view_get_opener_id:
 * @self: a #GsurfView
 *
 * Returns: the opener's id, kept after the opener closes, or 0 for a
 *   view opened by the user (new tab, startup)
 */
guint64 gsurf_view_get_opener_id(GsurfView *self);

/**
 * gsurf_view_get_container_id:
 * @self: a #GsurfView
 *
 * Returns: (transfer none) (nullable): the id of the container (see
 *   #GsurfContainerRegistry) this view browses in, or %NULL for the
 *   default container. Fixed for the life of the view.
 */
const gchar *gsurf_view_get_container_id(GsurfView *self);

/**
 * gsurf_view_get_related:
 * @self: a #GsurfView
 *
 * Returns: whether this view is a popup created as a related view of its
 *   opener (sharing its web process and session)
 */
gboolean gsurf_view_get_related(GsurfView *self);

/**
 * gsurf_view_get_session:
 * @self: a #GsurfView
 *
 * For backends: the native session of the view's container, acquired
 * during construction (a WebKitWebContext on WebKitGTK 4.1, a
 * WebKitNetworkSession on 6.0).
 *
 * Returns: (transfer none) (nullable): the native session, or %NULL for
 *   the engine default
 */
gpointer gsurf_view_get_session(GsurfView *self);

/**
 * gsurf_view_get_initial_uri:
 * @self: a #GsurfView
 *
 * Returns: (transfer none) (nullable): the URI given at construction as a
 *   hint for container placement (it is not loaded automatically)
 */
const gchar *gsurf_view_get_initial_uri(GsurfView *self);

/**
 * gsurf_view_get_favicon:
 * @self: a #GsurfView
 *
 * Returns the page favicon in the backend's native image type: a
 * cairo_surface_t on GTK3/WebKitGTK 4.1, a GdkTexture on GTK4/6.0.
 * Toolkit-aware callers (the tabs module) cast it.
 *
 * Returns: (transfer none) (nullable): the favicon, or %NULL
 */
gpointer gsurf_view_get_favicon(GsurfView *self);

/* --- Script worlds and messages --- */

/**
 * gsurf_view_register_script_message:
 * @self: a #GsurfView
 * @name: the handler name scripts post to
 * @world: (nullable): script world name, %NULL for the page's main world
 *
 * Lets scripts in @world call
 * `window.webkit.messageHandlers.<name>.postMessage(value)`. Each message
 * is delivered as #GsurfView::script-message with detail @name and the
 * value serialized as JSON. Registering the same name twice is a no-op.
 * Backends without script messaging ignore it.
 */
void gsurf_view_register_script_message(GsurfView *self, const gchar *name,
                                        const gchar *world);

/**
 * gsurf_view_add_user_script_in_world:
 * @self: a #GsurfView
 * @source: the JavaScript source
 * @at_end: inject at document end (%TRUE) or start (%FALSE)
 * @allow: (nullable) (array zero-terminated=1): URL allow-patterns
 * @world: (nullable): script world name, %NULL for the main world
 *
 * Injects a user script into every frame, in an isolated world when
 * @world is set: such scripts share the DOM with the page but not its
 * JavaScript globals, so the page cannot read or tamper with them.
 */
void gsurf_view_add_user_script_in_world(GsurfView *self, const gchar *source,
                                         gboolean at_end, const gchar *const *allow,
                                         const gchar *world);

/**
 * gsurf_view_run_javascript_in_world_async:
 * @self: a #GsurfView
 * @script: JavaScript to evaluate in the main frame
 * @world: (nullable): script world name, %NULL for the main world
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (nullable) (closure user_data): completion callback
 * @user_data: data for @callback
 *
 * Like gsurf_view_run_javascript_async() but in script world @world, and
 * the #GAsyncResult's source object is @self (not the native web view).
 * Finish with gsurf_view_run_javascript_in_world_finish(). A backend
 * without script worlds completes with %G_IO_ERROR_NOT_SUPPORTED.
 */
void gsurf_view_run_javascript_in_world_async(GsurfView *self, const gchar *script,
                                              const gchar *world, GCancellable *cancellable,
                                              GAsyncReadyCallback callback, gpointer user_data);

/**
 * gsurf_view_run_javascript_in_world_finish:
 * @self: a #GsurfView
 * @result: the #GAsyncResult passed to the callback
 * @error: return location for a #GError
 *
 * Finishes gsurf_view_run_javascript_in_world_async().
 *
 * Returns: (transfer full) (nullable): the value as a string (strings
 *   as-is, anything else as JSON), or %NULL on error
 */
gchar *gsurf_view_run_javascript_in_world_finish(GsurfView *self, GAsyncResult *result,
                                                 GError **error);

/**
 * gsurf_view_emit_script_message:
 * @self: a #GsurfView
 * @name: the handler name
 * @json: (nullable): the posted value as JSON
 *
 * Backend helper: emits #GsurfView::script-message.
 */
void gsurf_view_emit_script_message(GsurfView *self, const gchar *name, const gchar *json);

/* --- Signal emission helpers (for backend subclasses) --- */
void gsurf_view_emit_load_changed(GsurfView *self, GsurfLoadEvent event);
void gsurf_view_emit_uri_changed(GsurfView *self, const gchar *uri);
void gsurf_view_emit_title_changed(GsurfView *self, const gchar *title);
void gsurf_view_emit_progress_changed(GsurfView *self, gdouble progress);
void gsurf_view_emit_favicon_changed(GsurfView *self);
/**
 * gsurf_view_emit_create_view:
 * @self: a #GsurfView
 * @uri: (nullable): requested popup URI
 *
 * Returns: (transfer none) (nullable): the host-owned popup view, or %NULL to block
 */
GsurfView *gsurf_view_emit_create_view(GsurfView *self, const gchar *uri);

/**
 * gsurf_view_request_new_view:
 * @self: a #GsurfView
 * @uri: the address to open
 * @background: keep @self active (%TRUE) or activate the new view
 *
 * Asks the host to open @uri in a new tab whose opener is @self (emits
 * #GsurfView::new-view-requested). Kiosk mode refuses.
 *
 * Returns: %TRUE when the host opened it
 *
 * Since: 0.4.2
 */
gboolean   gsurf_view_request_new_view(GsurfView *self, const gchar *uri, gboolean background);

/**
 * gsurf_view_has_new_view_handler:
 * @self: a #GsurfView
 *
 * Returns: whether a host handles #GsurfView::new-view-requested for
 *   @self (the context menu offers "Open Link in New Tab" only then)
 *
 * Since: 0.4.2
 */
gboolean   gsurf_view_has_new_view_handler(GsurfView *self);
void gsurf_view_emit_web_process_terminated(GsurfView *self);

G_END_DECLS

#endif /* GSURF_VIEW_H */
