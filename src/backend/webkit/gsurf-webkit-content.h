/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * gsurf-webkit-content.h - Shared WebKit user-content helpers
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * Used by every WebKit-based backend (GTK3, GTK4, offscreen LRG):
 *   - content-blocker rulesets are compiled once per process (and reloaded
 *     from WebKit's on-disk store when unchanged) instead of once per view;
 *   - registered web-process extensions (core/gsurf-web-process.h) are
 *     handed to every web process the context launches.
 */
#ifndef GSURF_WEBKIT_CONTENT_H
#define GSURF_WEBKIT_CONTENT_H
#ifdef GSURF_BACKEND_GTK4
#include <webkit/webkit.h>
#else
#include <webkit2/webkit2.h>
#endif

/* Apply a content-blocker ruleset to @view's user content manager. The
 * compiled form is shared by every view that applies the same identifier
 * and JSON; an identifier whose JSON changes is recompiled. */
void gsurf_webkit_content_add_filter(WebKitWebView *view, const gchar *identifier,
                                     const gchar *json_rules);

/* Connect the web-process extension registry to @context. Call before the
 * first view of the context is created; repeated calls are no-ops. */
void gsurf_webkit_content_attach_context(WebKitWebContext *context);

#endif
