/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef GSURF_WEBKIT_PROTOCOL_H
#define GSURF_WEBKIT_PROTOCOL_H
#ifdef GSURF_BACKEND_GTK4
#include <webkit/webkit.h>
#else
#include <webkit2/webkit2.h>
#endif

/* Install once per context, and attach per-view navigation/cancellation. */
void gsurf_webkit_protocol_attach(WebKitWebView *view);
/* Owners call before releasing a native view retained by an embedder. */
void gsurf_webkit_protocol_cancel(WebKitWebView *view);
/* Mirror the native backend's shared proxy policy for GIO transports. */
void gsurf_webkit_protocol_set_proxy(WebKitWebView *view, const gchar *uri);
#endif
