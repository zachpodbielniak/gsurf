/*
 * gsurf-backend.c - Backend factory and initialization
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/gsurf-backend.h"
#include "config/gsurf-config.h"
#include "core/gsurf-container-registry.h"

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

/* The concrete view GType of a backend, or G_TYPE_INVALID. */
static GType
view_type_for_backend(GsurfBackendType type)
{
	switch (type) {
#ifdef GSURF_BACKEND_GTK4
	case GSURF_BACKEND_GTK4_WEBKIT6:
		return GSURF_TYPE_WEBKIT6_VIEW;
#endif
#ifdef GSURF_BACKEND_GTK3
	case GSURF_BACKEND_GTK3_WEBKIT2:
		return GSURF_TYPE_WEBKIT2_VIEW;
#endif
#ifdef GSURF_HAVE_LRG_BACKEND
	case GSURF_BACKEND_LRG:
		return GSURF_TYPE_LRG_VIEW;
#endif
	default:
		return G_TYPE_INVALID;
	}
}

/* Construct a backend view with the base-class construct properties; the
 * base constructed() resolves the container before the backend builds
 * its native web view. */
static GsurfView *
view_new_for_backend_full(GsurfBackendType type, GsurfView *opener, guint64 opener_id,
                          const gchar *container_id, const gchar *initial_uri,
                          gboolean related)
{
	GType gtype = view_type_for_backend(type);

	if (gtype == G_TYPE_INVALID) {
		g_warning("gsurf: cannot create view for unavailable backend '%s'",
		          gsurf_backend_type_to_name(type));
		return NULL;
	}
	return g_object_new(gtype,
		"opener", opener,
		"opener-id", opener_id,
		"container-id", container_id,
		"initial-uri", initial_uri,
		"related", related && opener != NULL,
		NULL);
}

GsurfView *
gsurf_view_new_for_backend(GsurfBackendType type)
{
	return view_new_for_backend_full(type, NULL, 0, NULL, NULL, FALSE);
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

GsurfView *
gsurf_view_new_full(GsurfView *opener, const gchar *container_id,
                    const gchar *initial_uri, gboolean related)
{
	g_return_val_if_fail(opener == NULL || GSURF_IS_VIEW(opener), NULL);

	return view_new_for_backend_full(gsurf_backend_get_default_type(), opener, 0,
		container_id, initial_uri, related);
}

GsurfWindow *
gsurf_window_new(void)
{
	return gsurf_window_new_for_backend(gsurf_backend_get_default_type());
}

/*
 * reopen_view:
 * @uri: (nullable): what to load; %NULL picks @view's address
 * @activate: always activate the new view (reopen), or only when @view
 *   was active (replace)
 *
 * The shared body of gsurf_window_reopen_view() and
 * gsurf_window_replace_view().
 */
static GsurfView *
reopen_view(GsurfWindow *window, GsurfView *view, const gchar *container_id,
            const gchar *uri, gboolean activate)
{
	GsurfConfig *config = gsurf_config_get_default();
	g_autoptr(GsurfView) old = NULL;
	g_autoptr(GsurfView) fresh = NULL;
	gboolean was_active;
	gint index;

	index = gsurf_window_get_view_index(window, view);
	if (index < 0)
		return NULL;
	was_active = (gsurf_window_get_active_view(window) == view);

	/* 1. What to load: the given address, else the current URI, else
	 *    the first URI the view was created for (a tab still loading
	 *    it), else a new-tab page. */
	if (uri == NULL || *uri == '\0')
		uri = gsurf_view_get_uri(view);
	if (uri == NULL || *uri == '\0')
		uri = gsurf_view_get_initial_uri(view);
	if (uri == NULL || *uri == '\0')
		uri = (config != NULL && config->new_view_uri != NULL)
			? config->new_view_uri : "about:blank";

	/* 2. Hold the old view: its opener and URI must outlive the new
	 *    view's construction, and removal drops the window's reference.
	 *    The opener id survives an opener that already closed. */
	old = g_object_ref(view);
	fresh = view_new_for_backend_full(gsurf_backend_get_default_type(),
		gsurf_view_get_opener(old), gsurf_view_get_opener_id(old),
		(container_id != NULL && *container_id != '\0') ? container_id
		                                                : GSURF_CONTAINER_DEFAULT,
		uri, FALSE);
	if (fresh == NULL)
		return NULL;
	if (config != NULL && config->settings != NULL)
		gsurf_view_apply_settings(fresh, config->settings);

	/* 3. Same place; chrome hands over the old view's place (its tab
	 *    tree node, a layout its pane) before activation, so activating
	 *    the new view finds it where the old one was instead of moving
	 *    it; then active (when asked), loading, and the old view goes. */
	gsurf_window_insert_view(window, fresh, (guint)index);
	g_signal_emit_by_name(window, "view-replaced", old, fresh);
	if (activate || was_active)
		gsurf_window_set_active_view(window, fresh);
	gsurf_view_load_uri(fresh, uri);
	gsurf_window_remove_view(window, old);
	return fresh;
}

GsurfView *
gsurf_window_reopen_view(GsurfWindow *window, GsurfView *view, const gchar *container_id)
{
	g_return_val_if_fail(GSURF_IS_WINDOW(window), NULL);
	g_return_val_if_fail(GSURF_IS_VIEW(view), NULL);

	return reopen_view(window, view, container_id, NULL, TRUE);
}

GsurfView *
gsurf_window_replace_view(GsurfWindow *window, GsurfView *view, const gchar *container_id,
                          const gchar *uri)
{
	g_return_val_if_fail(GSURF_IS_WINDOW(window), NULL);
	g_return_val_if_fail(GSURF_IS_VIEW(view), NULL);

	return reopen_view(window, view, container_id, uri, FALSE);
}
