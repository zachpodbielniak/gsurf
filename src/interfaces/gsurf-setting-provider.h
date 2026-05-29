/*
 * gsurf-setting-provider.h - Module hook: per-URI web engine settings
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfSettingProvider override #GsurfSettings for
 * the URI about to load (uri-params, useragent). The backend builds a
 * settings object per navigation, lets providers mutate it, then applies
 * it to the view.
 */

#ifndef GSURF_SETTING_PROVIDER_H
#define GSURF_SETTING_PROVIDER_H

#include <glib-object.h>

#include "core/gsurf-view.h"
#include "config/gsurf-settings.h"

G_BEGIN_DECLS

#define GSURF_TYPE_SETTING_PROVIDER (gsurf_setting_provider_get_type())

G_DECLARE_INTERFACE(GsurfSettingProvider, gsurf_setting_provider, GSURF, SETTING_PROVIDER, GObject)

struct _GsurfSettingProviderInterface
{
	GTypeInterface parent_iface;

	void (*apply_settings)(GsurfSettingProvider *self, GsurfView *view,
	                       GsurfSettings *settings, const gchar *uri);
};

void gsurf_setting_provider_apply_settings(GsurfSettingProvider *self, GsurfView *view,
                                           GsurfSettings *settings, const gchar *uri);

G_END_DECLS

#endif /* GSURF_SETTING_PROVIDER_H */
