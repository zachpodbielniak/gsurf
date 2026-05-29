/*
 * gsurf-settings.h - Backend-agnostic web engine settings
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GsurfSettings mirrors the configurable WebKit engine settings (surf's
 * `defconfig` ParamName table) as a plain GObject. The active backend
 * translates these into native WebKitSettings via
 * gsurf_web_view_apply_settings(). Fields are public (like GsurfConfig)
 * so the backend can read them directly.
 */

#ifndef GSURF_SETTINGS_H
#define GSURF_SETTINGS_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GSURF_TYPE_SETTINGS (gsurf_settings_get_type())

G_DECLARE_FINAL_TYPE(GsurfSettings, gsurf_settings, GSURF, SETTINGS, GObject)

/**
 * GsurfSettings:
 * @javascript: enable JavaScript
 * @images: auto-load images
 * @webgl: enable WebGL
 * @webaudio: enable Web Audio
 * @media: enable HTML5 media
 * @media_stream: enable getUserMedia (camera/mic)
 * @webrtc: enable WebRTC
 * @plugins: enable plugins
 * @smooth_scrolling: enable smooth scrolling
 * @caret_browsing: enable caret browsing
 * @dns_prefetch: enable DNS prefetching
 * @hyperlink_auditing: enable hyperlink auditing
 * @site_quirks: enable site-specific quirks
 * @developer_extras: enable the web inspector
 * @js_can_open_windows: allow JS to open windows automatically
 * @js_can_access_clipboard: allow JS clipboard access
 * @default_font_size: default font size in px
 * @default_monospace_font_size: default monospace font size in px
 * @default_charset: fallback character set
 * @user_agent: user agent override ("" / NULL = engine default)
 *
 * Public settings struct. Boolean fields default to a sensible browser
 * baseline (see gsurf_settings_new()).
 */
struct _GsurfSettings
{
	GObject parent_instance;

	gboolean javascript;
	gboolean images;
	gboolean webgl;
	gboolean webaudio;
	gboolean media;
	gboolean media_stream;
	gboolean webrtc;
	gboolean plugins;
	gboolean smooth_scrolling;
	gboolean caret_browsing;
	gboolean dns_prefetch;
	gboolean hyperlink_auditing;
	gboolean site_quirks;
	gboolean developer_extras;
	gboolean js_can_open_windows;
	gboolean js_can_access_clipboard;

	guint default_font_size;
	guint default_monospace_font_size;

	gchar *default_charset;
	gchar *user_agent;
};

/**
 * gsurf_settings_new:
 *
 * Creates a settings object initialized to the default browser baseline.
 *
 * Returns: (transfer full): a new #GsurfSettings
 */
GsurfSettings *gsurf_settings_new(void);

/**
 * gsurf_settings_copy:
 * @self: a #GsurfSettings
 *
 * Returns: (transfer full): a deep copy of @self
 */
GsurfSettings *gsurf_settings_copy(GsurfSettings *self);

/**
 * gsurf_settings_set_user_agent:
 * @self: a #GsurfSettings
 * @user_agent: (nullable): the UA string, or %NULL for engine default
 */
void gsurf_settings_set_user_agent(GsurfSettings *self, const gchar *user_agent);

/**
 * gsurf_settings_set_default_charset:
 * @self: a #GsurfSettings
 * @charset: (nullable): the charset name
 */
void gsurf_settings_set_default_charset(GsurfSettings *self, const gchar *charset);

G_END_DECLS

#endif /* GSURF_SETTINGS_H */
