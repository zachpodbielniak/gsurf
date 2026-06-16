/*
 * test-backend-lrg.c - libregnum (LRG) backend: enum, render mode, factory
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Headless-safe: the enum / render-mode-parser / factory-availability checks
 * need no display. The actual view creation (which spawns an offscreen WebKit)
 * is display-gated and skips in a pure-headless run.
 */

#include <gsurf.h>
#include <glib.h>

#ifdef GSURF_HAVE_LRG_BACKEND
#include "backend/lrg/gsurf-lrg-view.h"
#include "backend/lrg/gsurf-lrg-window.h"
#endif

/* --- GsurfLrgRenderMode parser (always present; lives in gsurf-enums) --- */

static void
test_render_mode_from_string(void)
{
	GsurfLrgRenderMode m = GSURF_LRG_RENDER_MODE_3DVR;

	g_assert_true(gsurf_lrg_render_mode_from_string("2d", &m));
	g_assert_cmpint(m, ==, GSURF_LRG_RENDER_MODE_2D);
	g_assert_true(gsurf_lrg_render_mode_from_string("3d", &m));
	g_assert_cmpint(m, ==, GSURF_LRG_RENDER_MODE_3D);
	g_assert_true(gsurf_lrg_render_mode_from_string("3dvr", &m));
	g_assert_cmpint(m, ==, GSURF_LRG_RENDER_MODE_3DVR);

	/* Case-insensitive, and only the leading MODE token of cmacs's
	 * MODE:ARRANGEMENT:ENVIRONMENT form is interpreted. */
	g_assert_true(gsurf_lrg_render_mode_from_string("3D:per-window:workshop", &m));
	g_assert_cmpint(m, ==, GSURF_LRG_RENDER_MODE_3D);

	/* Bare --lrg (NULL / "") selects 2D and succeeds. */
	m = GSURF_LRG_RENDER_MODE_3DVR;
	g_assert_true(gsurf_lrg_render_mode_from_string(NULL, &m));
	g_assert_cmpint(m, ==, GSURF_LRG_RENDER_MODE_2D);
	m = GSURF_LRG_RENDER_MODE_3DVR;
	g_assert_true(gsurf_lrg_render_mode_from_string("", &m));
	g_assert_cmpint(m, ==, GSURF_LRG_RENDER_MODE_2D);

	/* Unknown mode -> FALSE, output reset to 2D. */
	m = GSURF_LRG_RENDER_MODE_3DVR;
	g_assert_false(gsurf_lrg_render_mode_from_string("garbage", &m));
	g_assert_cmpint(m, ==, GSURF_LRG_RENDER_MODE_2D);
}

static void
test_render_mode_to_string(void)
{
	g_assert_cmpstr(gsurf_lrg_render_mode_to_string(GSURF_LRG_RENDER_MODE_2D), ==, "2d");
	g_assert_cmpstr(gsurf_lrg_render_mode_to_string(GSURF_LRG_RENDER_MODE_3D), ==, "3d");
	g_assert_cmpstr(gsurf_lrg_render_mode_to_string(GSURF_LRG_RENDER_MODE_3DVR), ==, "3dvr");
}

static void
test_render_mode_implemented(void)
{
	/* Only 2D ships today; 3D / 3D-VR are reserved. */
	g_assert_true(gsurf_lrg_render_mode_is_implemented(GSURF_LRG_RENDER_MODE_2D));
	g_assert_false(gsurf_lrg_render_mode_is_implemented(GSURF_LRG_RENDER_MODE_3D));
	g_assert_false(gsurf_lrg_render_mode_is_implemented(GSURF_LRG_RENDER_MODE_3DVR));
}

/* --- backend enum + factory --- */

static void
test_backend_enum_registered(void)
{
	GEnumClass *klass = g_type_class_ref(GSURF_TYPE_BACKEND_TYPE);
	GEnumValue *v = g_enum_get_value(klass, GSURF_BACKEND_LRG);

	g_assert_nonnull(v);
	g_assert_cmpstr(v->value_nick, ==, "lrg");
	g_type_class_unref(klass);
}

static void
test_backend_availability(void)
{
	/* type_to_name is total. */
	g_assert_nonnull(gsurf_backend_type_to_name(GSURF_BACKEND_GTK3_WEBKIT2));
	g_assert_nonnull(gsurf_backend_type_to_name(GSURF_BACKEND_LRG));

#ifdef GSURF_HAVE_LRG_BACKEND
	g_assert_true(gsurf_backend_is_available(GSURF_BACKEND_LRG));
	g_assert_cmpstr(gsurf_backend_type_to_name(GSURF_BACKEND_LRG), ==, "lrg-webkitgtk");
#else
	g_assert_false(gsurf_backend_is_available(GSURF_BACKEND_LRG));
#endif
}

static void
test_backend_default_roundtrip(void)
{
#ifdef GSURF_HAVE_LRG_BACKEND
	GsurfBackendType prev = gsurf_backend_get_default_type();

	g_assert_true(gsurf_backend_set_default_type(GSURF_BACKEND_LRG));
	g_assert_cmpint(gsurf_backend_get_default_type(), ==, GSURF_BACKEND_LRG);
	g_assert_cmpstr(gsurf_backend_get_name(), ==, "lrg-webkitgtk");

	gsurf_backend_set_default_type(prev);
	g_assert_cmpint(gsurf_backend_get_default_type(), ==, prev);
#else
	g_test_skip("LRG backend not built");
#endif
}

#ifdef GSURF_HAVE_LRG_BACKEND
static void
test_lrg_view_create(void)
{
	GsurfView *view;
	g_autoptr(GError) error = NULL;

	if (g_getenv("DISPLAY") == NULL && g_getenv("WAYLAND_DISPLAY") == NULL) {
		g_test_skip("no display (headless)");
		return;
	}
	if (!gsurf_backend_init_backend(GSURF_BACKEND_LRG, NULL, NULL, &error)) {
		g_test_skip("LRG backend init failed (no usable display)");
		return;
	}

	view = gsurf_view_new_for_backend(GSURF_BACKEND_LRG);
	g_assert_nonnull(view);
	g_assert_true(GSURF_IS_LRG_VIEW(view));

	/* No frame captured yet -> no texture. */
	g_assert_null(gsurf_lrg_view_get_texture(GSURF_LRG_VIEW(view), NULL, NULL));
	g_assert_false(gsurf_lrg_view_get_focus(GSURF_LRG_VIEW(view)));

	g_object_unref(view);
}
#endif /* GSURF_HAVE_LRG_BACKEND */

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/lrg/render-mode/from-string", test_render_mode_from_string);
	g_test_add_func("/gsurf/lrg/render-mode/to-string", test_render_mode_to_string);
	g_test_add_func("/gsurf/lrg/render-mode/implemented", test_render_mode_implemented);
	g_test_add_func("/gsurf/lrg/backend/enum", test_backend_enum_registered);
	g_test_add_func("/gsurf/lrg/backend/availability", test_backend_availability);
	g_test_add_func("/gsurf/lrg/backend/default-roundtrip", test_backend_default_roundtrip);
#ifdef GSURF_HAVE_LRG_BACKEND
	g_test_add_func("/gsurf/lrg/view/create", test_lrg_view_create);
#endif
	return g_test_run();
}
