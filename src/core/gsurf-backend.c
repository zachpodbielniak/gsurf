/*
 * gsurf-backend.c - Backend factory and initialization
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/gsurf-backend.h"

#include <gtk/gtk.h>

#ifdef GSURF_BACKEND_GTK4
#include "backend/gtk4/gsurf-gtk4-window.h"
#include "backend/gtk4/gsurf-webkit6-view.h"
#else
#include "backend/gtk3/gsurf-gtk3-window.h"
#include "backend/gtk3/gsurf-webkit2-view.h"
#endif

gboolean
gsurf_backend_init(int *argc, char ***argv, GError **error)
{
#ifdef GSURF_BACKEND_GTK4
	(void)argc;
	(void)argv;
	if (!gtk_init_check()) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"Failed to initialize GTK4");
		return FALSE;
	}
#else
	if (!gtk_init_check(argc, argv)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"Failed to initialize GTK3 (no display?)");
		return FALSE;
	}
#endif
	return TRUE;
}

GsurfBackendType
gsurf_backend_get_backend_type(void)
{
#ifdef GSURF_BACKEND_GTK4
	return GSURF_BACKEND_GTK4_WEBKIT6;
#else
	return GSURF_BACKEND_GTK3_WEBKIT2;
#endif
}

const gchar *
gsurf_backend_get_name(void)
{
#ifdef GSURF_BACKEND_GTK4
	return "gtk4-webkit6";
#else
	return "gtk3-webkit2";
#endif
}

GsurfView *
gsurf_view_new(void)
{
#ifdef GSURF_BACKEND_GTK4
	return gsurf_webkit6_view_new();
#else
	return gsurf_webkit2_view_new();
#endif
}

GsurfWindow *
gsurf_window_new(void)
{
#ifdef GSURF_BACKEND_GTK4
	return gsurf_gtk4_window_new();
#else
	return gsurf_gtk3_window_new();
#endif
}
