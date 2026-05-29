/*
 * gsurf-setting-provider.c - Module hook: per-URI web engine settings
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-setting-provider.h"

G_DEFINE_INTERFACE(GsurfSettingProvider, gsurf_setting_provider, G_TYPE_OBJECT)

static void
gsurf_setting_provider_default_init(GsurfSettingProviderInterface *iface)
{
}

void
gsurf_setting_provider_apply_settings(GsurfSettingProvider *self, GsurfView *view,
                                      GsurfSettings *settings, const gchar *uri)
{
	GsurfSettingProviderInterface *iface;

	g_return_if_fail(GSURF_IS_SETTING_PROVIDER(self));

	iface = GSURF_SETTING_PROVIDER_GET_IFACE(self);
	if (iface->apply_settings != NULL)
		iface->apply_settings(self, view, settings, uri);
}
