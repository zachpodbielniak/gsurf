/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * gsurf-webkit-common.h - WebKit signal handling shared by GTK3 and GTK4
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * The parts of a WebKit view backend that are the same on WebKitGTK 4.1
 * and 6.0: module context-menu entries, download destinations, TLS
 * exceptions, permission requests, hovered links and popup creation.
 */
#ifndef GSURF_WEBKIT_COMMON_H
#define GSURF_WEBKIT_COMMON_H

#ifdef GSURF_BACKEND_GTK4
#include <webkit/webkit.h>
#else
#include <webkit2/webkit2.h>
#endif

#include "core/gsurf-view.h"

G_BEGIN_DECLS

/* GSURF_WEBKIT_MENU_NEW_TAB_ACTION: the name of the GAction behind the
 * "Open Link in New Tab" item (tests find the item by it). */
#define GSURF_WEBKIT_MENU_NEW_TAB_ACTION "gsurf-open-link-new-tab"

/* Rework @menu for @view: on a link, when the host handles
 * #GsurfView::new-view-requested, WebKit's "Open Link in New Window"
 * (a popup, in gsurf a related tab) becomes "Open Link in New Tab" (a
 * background tab, @view's child); then the entries contributed by
 * context-menu-provider modules for @hit are appended. */
void gsurf_webkit_common_populate_menu(GsurfView *view, WebKitContextMenu *menu,
                                       WebKitHitTestResult *hit);

/* Let download-handler modules choose the destination of @download. */
void gsurf_webkit_common_track_download(WebKitDownload *download);

/* Ask cert-handler modules whether to proceed past @errors for the host
 * of @failing_uri; on success the certificate is allowed and the page
 * reloaded. Returns %TRUE when handled. */
gboolean gsurf_webkit_common_handle_tls_error(WebKitWebView *webview,
                                              const gchar *failing_uri,
                                              GTlsCertificate *certificate,
                                              GTlsCertificateFlags errors);

/* Dispatch a permission request to permission-handler modules. Returns
 * %TRUE when a module allowed or denied it. */
gboolean gsurf_webkit_common_handle_permission(GsurfView *view,
                                               WebKitPermissionRequest *request);

/* A RESPONSE policy decision: when it is the main frame's main resource
 * and displayable, ask navigation-hook modules
 * (gsurf_module_manager_dispatch_main_response()). Returns %TRUE when a
 * module vetoed it and the decision was ignored; %FALSE leaves the
 * decision to the caller (and WebKit's default handler). */
gboolean gsurf_webkit_common_decide_response(GsurfView *view,
                                             WebKitPolicyDecision *decision);

/* Track the hovered link for a #GsurfView. */
void gsurf_webkit_common_mouse_target(GsurfView *view, WebKitHitTestResult *hit);

/* WebKit's "create" request: let the host create a related view for the
 * popup and return its native widget (or %NULL to block it). */
GtkWidget *gsurf_webkit_common_create(GsurfView *view, WebKitNavigationAction *action);

/* Add @cookie to @manager, @view's session cookie store. The task's source
 * object is @view; finish with gsurf_webkit_common_add_cookie_finish(). */
void     gsurf_webkit_common_add_cookie_async(GsurfView *view, WebKitCookieManager *manager,
                                              const GsurfCookie *cookie,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);
gboolean gsurf_webkit_common_add_cookie_finish(GAsyncResult *result, GError **error);

/* The focus tracker content script (document start, every frame). Each
 * frame posts {f: frame token, t: is top frame, e: editable focused} to
 * the "gsurfFocus" message handler whenever focus moves inside it; see
 * gsurf_webkit_common_focus_message(). */
extern const gchar gsurf_webkit_focus_tracker_js[];

/* Fold one "gsurfFocus" message from any frame of @view into its
 * editing state (gsurf_view_set_editing()). Only one element is focused
 * at a time, so an editable report from any frame wins; a "not editable"
 * report clears the state only when it comes from the top frame (focus
 * is not in a subframe, else the top frame stays silent) or from the
 * frame that reported the focused field. A late report from a frame
 * focus has already left can therefore not override the current one. */
void gsurf_webkit_common_focus_message(GsurfView *view, JSCValue *value);

G_END_DECLS

#endif
