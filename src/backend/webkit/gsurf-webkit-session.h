/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * gsurf-webkit-session.h - Per-container WebKit sessions
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * The WebKit implementation of #GsurfSessionBackend. Each container gets
 * its own session object: a WebKitWebContext (with its own
 * WebKitWebsiteDataManager) on WebKitGTK 4.1, a WebKitNetworkSession on
 * 6.0. Persistent containers store data under their directories;
 * temporary ones are ephemeral. Every session — including the engine's
 * default one — is configured the same way from the active config: TLS
 * policy, cache model, cookie storage and proxy.
 */
#ifndef GSURF_WEBKIT_SESSION_H
#define GSURF_WEBKIT_SESSION_H

#ifdef GSURF_BACKEND_GTK4
#include <webkit/webkit.h>
#else
#include <webkit2/webkit2.h>
#endif

G_BEGIN_DECLS

/* Install the WebKit session backend into the default container
 * registry. Idempotent; WebKit views call it before construction. */
void gsurf_webkit_session_install(void);

/* (Re)apply the active config to the engine's default session (cookie
 * jar from `cookie_jar:` unless `ephemeral`). Called for every new view
 * so config changes made at runtime reach later views. */
void gsurf_webkit_session_prepare_default(void);

/* Tell @ctx which languages pages see (navigator.language,
 * Accept-Language): `browser.languages`, else the locale's, en-US under
 * the C locale (left alone, WebKit reports "C", which Intl rejects). Every
 * context a view uses gets it. */
void gsurf_webkit_session_apply_languages(WebKitWebContext *ctx);

/* The web context a view in @native's container must use (4.1: @native
 * itself, or the default context; 6.0: always the default context, as
 * isolation lives in the network session). Web-process extensions are
 * attached to it. */
WebKitWebContext *gsurf_webkit_session_get_context(gpointer native);

/* Route downloads started in @webview's session (its web context on
 * 4.1, its network session on 6.0) to download-handler modules. Hooks
 * each session once, however many views share it. */
void gsurf_webkit_session_track_downloads(WebKitWebView *webview);

G_END_DECLS

#endif
