/*
 * gsurf-status-provider.h - Module hook: status-bar text contribution
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfStatusProvider contribute text to the
 * status line for a view (load progress, security indicators, hovered
 * link, mode display).
 */

#ifndef GSURF_STATUS_PROVIDER_H
#define GSURF_STATUS_PROVIDER_H

#include <glib-object.h>

#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_STATUS_PROVIDER (gsurf_status_provider_get_type())

G_DECLARE_INTERFACE(GsurfStatusProvider, gsurf_status_provider, GSURF, STATUS_PROVIDER, GObject)

/**
 * GsurfStatusProviderInterface:
 * @parent_iface: the parent interface
 * @status_text: produce the status text for a view
 */
struct _GsurfStatusProviderInterface
{
	GTypeInterface parent_iface;

	gchar *(*status_text)(GsurfStatusProvider *self, GsurfView *view);
};

gchar *gsurf_status_provider_get_status_text(GsurfStatusProvider *self, GsurfView *view);

G_END_DECLS

#endif /* GSURF_STATUS_PROVIDER_H */
