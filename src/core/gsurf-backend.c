/*
 * gsurf-backend.c - Backend factory and initialization
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/gsurf-backend.h"

/*
 * Exactly one GTK toolkit may be linked (GTK3 and GTK4 export the same gtk_*
 * symbols), so GSURF_BACKEND_GTK3 / GSURF_BACKEND_GTK4 stay mutually exclusive.
 * GSURF_BACKEND_LRG (libregnum/raylib) is additive: it links no GTK windowing
 * of its own, so it can coexist with a GTK backend in one libgsurf.
 */
#if defined(GSURF_BACKEND_GTK4)
#include <gtk/gtk.h>
#include "backend/gtk4/gsurf-gtk4-window.h"
#include "backend/gtk4/gsurf-webkit6-view.h"
#define GSURF_HAVE_GTK_BACKEND 1
#define GSURF_GTK_BACKEND_TYPE GSURF_BACKEND_GTK4_WEBKIT6
#elif defined(GSURF_BACKEND_GTK3)
#include <gtk/gtk.h>
#include "backend/gtk3/gsurf-gtk3-window.h"
#include "backend/gtk3/gsurf-webkit2-view.h"
#define GSURF_HAVE_GTK_BACKEND 1
#define GSURF_GTK_BACKEND_TYPE GSURF_BACKEND_GTK3_WEBKIT2
#endif

#ifdef GSURF_HAVE_LRG_BACKEND
#include "backend/lrg/gsurf-lrg-backend.h"
#include "backend/lrg/gsurf-lrg-view.h"
#include "backend/lrg/gsurf-lrg-window.h"
#endif

/*
 * The default backend used by the parameterless factory wrappers. Initialized
 * lazily to the GTK backend when present, else to LRG.
 */
static GsurfBackendType g_default_backend;
static gboolean g_default_backend_set = FALSE;

static GsurfBackendType
backend_compute_default(void)
{
#ifdef GSURF_HAVE_GTK_BACKEND
	return GSURF_GTK_BACKEND_TYPE;
#elif defined(GSURF_HAVE_LRG_BACKEND)
	return GSURF_BACKEND_LRG;
#else
	/* No backend compiled in; should not happen in a real build. */
	return GSURF_BACKEND_GTK3_WEBKIT2;
#endif
}

gboolean
gsurf_backend_is_available(GsurfBackendType type)
{
	switch (type) {
#ifdef GSURF_BACKEND_GTK3
	case GSURF_BACKEND_GTK3_WEBKIT2:
		return TRUE;
#endif
#ifdef GSURF_BACKEND_GTK4
	case GSURF_BACKEND_GTK4_WEBKIT6:
		return TRUE;
#endif
#ifdef GSURF_HAVE_LRG_BACKEND
	case GSURF_BACKEND_LRG:
		return TRUE;
#endif
	default:
		return FALSE;
	}
}

GsurfBackendType
gsurf_backend_get_default_type(void)
{
	if (!g_default_backend_set) {
		g_default_backend = backend_compute_default();
		g_default_backend_set = TRUE;
	}
	return g_default_backend;
}

gboolean
gsurf_backend_set_default_type(GsurfBackendType type)
{
	if (!gsurf_backend_is_available(type)) {
		g_warning("gsurf: backend '%s' is not available in this build",
		          gsurf_backend_type_to_name(type));
		return FALSE;
	}
	g_default_backend = type;
	g_default_backend_set = TRUE;
	return TRUE;
}

const gchar *
gsurf_backend_type_to_name(GsurfBackendType type)
{
	switch (type) {
	case GSURF_BACKEND_GTK3_WEBKIT2:
		return "gtk3-webkit2";
	case GSURF_BACKEND_GTK4_WEBKIT6:
		return "gtk4-webkit6";
	case GSURF_BACKEND_LRG:
#ifdef GSURF_HAVE_LRG_BACKEND
		return gsurf_lrg_backend_engine_name();
#else
		return "lrg";
#endif
	default:
		return "unknown";
	}
}

gboolean
gsurf_backend_init_backend(GsurfBackendType type, int *argc, char ***argv,
                           GError **error)
{
	switch (type) {
#ifdef GSURF_HAVE_GTK_BACKEND
	case GSURF_GTK_BACKEND_TYPE:
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
#endif /* GSURF_HAVE_GTK_BACKEND */
#ifdef GSURF_HAVE_LRG_BACKEND
	case GSURF_BACKEND_LRG:
		return gsurf_lrg_backend_init(argc, argv, error);
#endif
	default:
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		            "gsurf backend '%s' is not available in this build",
		            gsurf_backend_type_to_name(type));
		return FALSE;
	}
}

GsurfView *
gsurf_view_new_for_backend(GsurfBackendType type)
{
	switch (type) {
#ifdef GSURF_BACKEND_GTK4
	case GSURF_BACKEND_GTK4_WEBKIT6:
		return gsurf_webkit6_view_new();
#endif
#ifdef GSURF_BACKEND_GTK3
	case GSURF_BACKEND_GTK3_WEBKIT2:
		return gsurf_webkit2_view_new();
#endif
#ifdef GSURF_HAVE_LRG_BACKEND
	case GSURF_BACKEND_LRG:
		return gsurf_lrg_view_new();
#endif
	default:
		g_warning("gsurf: cannot create view for unavailable backend '%s'",
		          gsurf_backend_type_to_name(type));
		return NULL;
	}
}

GsurfWindow *
gsurf_window_new_for_backend(GsurfBackendType type)
{
	switch (type) {
#ifdef GSURF_BACKEND_GTK4
	case GSURF_BACKEND_GTK4_WEBKIT6:
		return gsurf_gtk4_window_new();
#endif
#ifdef GSURF_BACKEND_GTK3
	case GSURF_BACKEND_GTK3_WEBKIT2:
		return gsurf_gtk3_window_new();
#endif
#ifdef GSURF_HAVE_LRG_BACKEND
	case GSURF_BACKEND_LRG:
		return gsurf_lrg_window_new();
#endif
	default:
		g_warning("gsurf: cannot create window for unavailable backend '%s'",
		          gsurf_backend_type_to_name(type));
		return NULL;
	}
}

/* --- Convenience wrappers over the current default backend --- */

gboolean
gsurf_backend_init(int *argc, char ***argv, GError **error)
{
	return gsurf_backend_init_backend(gsurf_backend_get_default_type(),
	                                  argc, argv, error);
}

GsurfBackendType
gsurf_backend_get_backend_type(void)
{
	return gsurf_backend_get_default_type();
}

const gchar *
gsurf_backend_get_name(void)
{
	return gsurf_backend_type_to_name(gsurf_backend_get_default_type());
}

GsurfView *
gsurf_view_new(void)
{
	return gsurf_view_new_for_backend(gsurf_backend_get_default_type());
}

GsurfWindow *
gsurf_window_new(void)
{
	return gsurf_window_new_for_backend(gsurf_backend_get_default_type());
}
