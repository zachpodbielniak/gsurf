/*
 * gsurf-settings.c - Backend-agnostic web engine settings
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "config/gsurf-settings.h"

G_DEFINE_TYPE(GsurfSettings, gsurf_settings, G_TYPE_OBJECT)

static void
gsurf_settings_finalize(GObject *object)
{
	GsurfSettings *self = GSURF_SETTINGS(object);

	g_clear_pointer(&self->default_charset, g_free);
	g_clear_pointer(&self->user_agent, g_free);

	G_OBJECT_CLASS(gsurf_settings_parent_class)->finalize(object);
}

static void
gsurf_settings_class_init(GsurfSettingsClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gsurf_settings_finalize;
}

static void
gsurf_settings_init(GsurfSettings *self)
{
	/* Browser baseline (mirrors surf defconfig). */
	self->javascript = TRUE;
	self->images = TRUE;
	self->webgl = FALSE;
	self->webaudio = TRUE;
	self->media = TRUE;
	self->media_stream = FALSE;
	self->webrtc = FALSE;
	self->plugins = FALSE;
	self->smooth_scrolling = TRUE;
	self->caret_browsing = FALSE;
	self->dns_prefetch = FALSE;
	self->hyperlink_auditing = FALSE;
	self->site_quirks = TRUE;
	self->developer_extras = FALSE;
	self->js_can_open_windows = FALSE;
	self->js_can_access_clipboard = FALSE;

	self->default_font_size = 16;
	self->default_monospace_font_size = 13;

	self->default_charset = g_strdup("UTF-8");
	self->user_agent = NULL;
}

GsurfSettings *
gsurf_settings_new(void)
{
	return g_object_new(GSURF_TYPE_SETTINGS, NULL);
}

GsurfSettings *
gsurf_settings_copy(GsurfSettings *self)
{
	GsurfSettings *copy;

	g_return_val_if_fail(GSURF_IS_SETTINGS(self), NULL);

	copy = gsurf_settings_new();

	copy->javascript = self->javascript;
	copy->images = self->images;
	copy->webgl = self->webgl;
	copy->webaudio = self->webaudio;
	copy->media = self->media;
	copy->media_stream = self->media_stream;
	copy->webrtc = self->webrtc;
	copy->plugins = self->plugins;
	copy->smooth_scrolling = self->smooth_scrolling;
	copy->caret_browsing = self->caret_browsing;
	copy->dns_prefetch = self->dns_prefetch;
	copy->hyperlink_auditing = self->hyperlink_auditing;
	copy->site_quirks = self->site_quirks;
	copy->developer_extras = self->developer_extras;
	copy->js_can_open_windows = self->js_can_open_windows;
	copy->js_can_access_clipboard = self->js_can_access_clipboard;
	copy->default_font_size = self->default_font_size;
	copy->default_monospace_font_size = self->default_monospace_font_size;

	gsurf_settings_set_default_charset(copy, self->default_charset);
	gsurf_settings_set_user_agent(copy, self->user_agent);

	return copy;
}

void
gsurf_settings_set_user_agent(GsurfSettings *self, const gchar *user_agent)
{
	g_return_if_fail(GSURF_IS_SETTINGS(self));

	g_free(self->user_agent);
	self->user_agent = (user_agent && *user_agent) ? g_strdup(user_agent) : NULL;
}

void
gsurf_settings_set_default_charset(GsurfSettings *self, const gchar *charset)
{
	g_return_if_fail(GSURF_IS_SETTINGS(self));

	g_free(self->default_charset);
	self->default_charset = g_strdup(charset ? charset : "UTF-8");
}
