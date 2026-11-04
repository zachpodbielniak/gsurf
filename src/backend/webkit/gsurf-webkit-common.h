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

/* Append context-menu entries contributed by context-menu-provider
 * modules for @hit to @menu. */
void gsurf_webkit_common_populate_menu(WebKitContextMenu *menu, WebKitHitTestResult *hit);

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

/* Track the hovered link for a #GsurfView. */
void gsurf_webkit_common_mouse_target(GsurfView *view, WebKitHitTestResult *hit);

/* WebKit's "create" request: let the host create a related view for the
 * popup and return its native widget (or %NULL to block it). */
GtkWidget *gsurf_webkit_common_create(GsurfView *view, WebKitNavigationAction *action);

G_END_DECLS

#endif
