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
#ifdef GSURF_HAVE_LRG_3D
#include "backend/lrg/gsurf-lrg-scene.h"
#include <glib/gstdio.h>
#endif

/*
 * tolerate_system_gtk:
 * The raylib window goes through GLFW, which on Wayland loads the system's
 * libdecor GTK plugin for decorations; it calls gtk_disable_setlocale() after
 * GTK (initialised by the WebKitGTK engine) has started, which logs a
 * Gtk-WARNING that g_test_init() would make fatal.  Drop warnings of the
 * "Gtk" domain only; everything else keeps the default (fatal) handling.
 * The same writer libregnum's tests/test-term.c uses.
 */
static GLogWriterOutput
tolerate_system_gtk(GLogLevelFlags log_level, const GLogField *fields,
                    gsize n_fields, gpointer user_data)
{
	gsize i;

	if ((log_level & G_LOG_LEVEL_WARNING) != 0) {
		for (i = 0; i < n_fields; i++) {
			if (g_str_equal(fields[i].key, "GLIB_DOMAIN") &&
			    fields[i].length < 0 &&
			    g_str_equal((const gchar *)fields[i].value, "Gtk")) {
				g_test_message("ignored a system GTK warning (libdecor plugin)");
				return G_LOG_WRITER_HANDLED;
			}
		}
	}
	return g_log_writer_default(log_level, fields, n_fields, user_data);
}

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
	/* 2D always; 3D with the libregnum scene (LRG_3D=1); VR reserved. */
	g_assert_true(gsurf_lrg_render_mode_is_implemented(GSURF_LRG_RENDER_MODE_2D));
#ifdef GSURF_HAVE_LRG_3D
	g_assert_true(gsurf_lrg_render_mode_is_implemented(GSURF_LRG_RENDER_MODE_3D));
#else
	g_assert_false(gsurf_lrg_render_mode_is_implemented(GSURF_LRG_RENDER_MODE_3D));
#endif
	g_assert_false(gsurf_lrg_render_mode_is_implemented(GSURF_LRG_RENDER_MODE_3DVR));
}

/* --- the full MODE:ARRANGEMENT:ENVIRONMENT spec --- */

static void
test_spec_parse(void)
{
	GsurfLrgRenderMode m = GSURF_LRG_RENDER_MODE_2D;
	gchar *arr = NULL;
	gchar *env = NULL;

	/* Both tokens, in either order; ids are lowercased. */
	g_assert_true(gsurf_lrg_spec_parse("3d:grid:sky", &m, &arr, &env));
	g_assert_cmpint(m, ==, GSURF_LRG_RENDER_MODE_3D);
	g_assert_cmpstr(arr, ==, "grid");
	g_assert_cmpstr(env, ==, "sky");
	g_clear_pointer(&arr, g_free);
	g_clear_pointer(&env, g_free);

	g_assert_true(gsurf_lrg_spec_parse("3D:Studio:Carousel", &m, &arr, &env));
	g_assert_cmpstr(arr, ==, "carousel");
	g_assert_cmpstr(env, ==, "studio");
	g_clear_pointer(&arr, g_free);
	g_clear_pointer(&env, g_free);

	/* An environment alone; empty tokens and commas are fine. */
	g_assert_true(gsurf_lrg_spec_parse("3d::workshop", &m, &arr, &env));
	g_assert_null(arr);
	g_assert_cmpstr(env, ==, "workshop");
	g_clear_pointer(&env, g_free);
	g_assert_true(gsurf_lrg_spec_parse("3d,stack", &m, &arr, &env));
	g_assert_cmpstr(arr, ==, "stack");
	g_assert_null(env);
	g_clear_pointer(&arr, g_free);

	/* Plain modes name nothing; NULL outputs are allowed. */
	g_assert_true(gsurf_lrg_spec_parse("2d", &m, &arr, &env));
	g_assert_cmpint(m, ==, GSURF_LRG_RENDER_MODE_2D);
	g_assert_null(arr);
	g_assert_null(env);
	g_assert_true(gsurf_lrg_spec_parse(NULL, &m, NULL, NULL));
	g_assert_true(gsurf_lrg_spec_parse("3d:grid:sky", &m, NULL, NULL));

	/* An unknown mode fails, but still reports the tokens. */
	g_assert_false(gsurf_lrg_spec_parse("4d:grid", &m, &arr, &env));
	g_assert_cmpint(m, ==, GSURF_LRG_RENDER_MODE_2D);
	g_assert_cmpstr(arr, ==, "grid");
	g_clear_pointer(&arr, g_free);
}

/* --- the 3D actions are real actions with names and descriptions --- */

static void
test_lrg_actions(void)
{
	static const gchar *const names[] = {
		"lrg-cycle-arrangement", "lrg-cycle-environment", "lrg-reading",
		"lrg-history", "lrg-focus", "lrg-pin", "lrg-unpin-all",
		"lrg-camera-reset", "lrg-overview", "lrg-orbit-left", "lrg-orbit-right",
		"lrg-orbit-up", "lrg-orbit-down", "lrg-zoom-in", "lrg-zoom-out", NULL
	};
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		GsurfAction a = gsurf_action_from_string(names[i]);

		g_assert_cmpint(a, !=, GSURF_ACTION_NONE);
		g_assert_cmpstr(gsurf_action_to_string(a), ==, names[i]);
		g_assert_nonnull(gsurf_action_get_description(a));
	}
	g_assert_cmpint(gsurf_action_from_string("lrg-zoom-out"), ==,
	                GSURF_ACTION_LAST - 1);
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

#ifdef GSURF_HAVE_LRG_3D
/* --- scene policy helpers (pure) --- */

static void
test_scene_refresh_interval(void)
{
	/* The active tab: every frame, whatever its coverage. */
	g_assert_cmpint(gsurf_lrg_scene_refresh_interval(TRUE, 0.0f, 4.0), ==, 0);
	g_assert_cmpint(gsurf_lrg_scene_refresh_interval(TRUE, 900.0f, 4.0), ==, 0);

	/* Visible background tabs at the configured rate, half that when tiny. */
	g_assert_cmpint(gsurf_lrg_scene_refresh_interval(FALSE, 400.0f, 4.0), ==,
	                G_USEC_PER_SEC / 4);
	g_assert_cmpint(gsurf_lrg_scene_refresh_interval(FALSE, 50.0f, 4.0), ==,
	                G_USEC_PER_SEC / 2);

	/* Hidden tabs: slowly, so they are not stale when the camera turns. */
	g_assert_cmpint(gsurf_lrg_scene_refresh_interval(FALSE, 0.0f, 4.0), ==,
	                3 * G_USEC_PER_SEC);

	/* The rate is clamped to [0.2, 60]. */
	g_assert_cmpint(gsurf_lrg_scene_refresh_interval(FALSE, 400.0f, 1000.0), ==,
	                G_USEC_PER_SEC / 60);
	g_assert_cmpint(gsurf_lrg_scene_refresh_interval(FALSE, 400.0f, 0.0), ==,
	                5 * G_USEC_PER_SEC);
}

static void
test_scene_history_keys(void)
{
	gint offsets[] = { -32, -1, 1, 7, 32 };
	gint off = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(offsets); i++) {
		guint64 key = gsurf_lrg_scene_history_key(offsets[i]);

		g_assert_true(gsurf_lrg_scene_history_offset(key, &off));
		g_assert_cmpint(off, ==, offsets[i]);
	}
	/* Distinct per offset, and never a (user-space pointer) tab key. */
	g_assert_cmpuint(gsurf_lrg_scene_history_key(-1), !=,
	                 gsurf_lrg_scene_history_key(1));
	g_assert_false(gsurf_lrg_scene_history_offset(
		(guint64)GPOINTER_TO_SIZE(&off), NULL));
	g_assert_false(gsurf_lrg_scene_history_offset(0, NULL));
}

/* --- a live scene (display + WebKit + GL) --- */

static gboolean shot_ok;
static gboolean shot_done;

static void
on_scene_shot(GsurfLrgScene *scene, const gchar *path, gboolean ok, gpointer data)
{
	(void)scene;
	(void)path;
	(void)data;
	shot_ok = ok;
	shot_done = TRUE;
}

/* Run frames (and the main loop, for WebKit's snapshots) until @done says
 * so or ~10 s pass. */
static gboolean
run_frames(GsurfLrgScene *scene, GPtrArray *views, GsurfView *active,
           gboolean (*done)(GsurfLrgScene *scene))
{
	gint i;

	for (i = 0; i < 600; i++) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		gsurf_lrg_scene_frame(scene, views, active, 640, 400, 1.0f / 60.0f);
		if (done(scene))
			return TRUE;
		g_usleep(16 * 1000);
	}
	return FALSE;
}

static gboolean
two_panels(GsurfLrgScene *scene)
{
	return gsurf_lrg_scene_get_panel_count(scene) == 2
	    && !gsurf_lrg_scene_is_animating(scene);
}

static gboolean
flat_ready(GsurfLrgScene *scene)
{
	return gsurf_lrg_scene_wants_flat(scene);
}

static gboolean
settled(GsurfLrgScene *scene)
{
	return !gsurf_lrg_scene_is_animating(scene) || shot_done;
}

static void
test_scene_live(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfLrgScene) scene = NULL;
	g_autoptr(GPtrArray) views = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *png = NULL;
	GsurfView *a, *b;
	GsurfLrgPick pick;

	if (g_getenv("DISPLAY") == NULL && g_getenv("WAYLAND_DISPLAY") == NULL) {
		g_test_skip("no display (headless)");
		return;
	}
	if (!gsurf_backend_init_backend(GSURF_BACKEND_LRG, NULL, NULL, &error)) {
		g_test_skip("LRG backend init failed (no usable display)");
		return;
	}
	/* An unknown environment is reported and falls back to the default. */
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*unknown 3D environment 'nope'*");
	scene = gsurf_lrg_scene_new(640, 428, "test-lrg-scene", "carousel", "nope");
	g_test_assert_expected_messages();
	if (gsurf_lrg_scene_get_window(scene) == NULL) {
		g_test_skip("no raylib window");
		return;
	}
	/* An unknown environment fell back to the default. */
	g_assert_cmpstr(gsurf_lrg_scene_get_environment(scene), ==,
	                GSURF_LRG_SCENE_DEFAULT_ENVIRONMENT);
	g_assert_cmpstr(gsurf_lrg_scene_get_arrangement(scene), ==, "carousel");

	views = g_ptr_array_new_with_free_func(g_object_unref);
	a = gsurf_view_new_for_backend(GSURF_BACKEND_LRG);
	b = gsurf_view_new_for_backend(GSURF_BACKEND_LRG);
	gsurf_view_load_uri(a, "data:text/html,<body style='background:%23c33'>a</body>");
	gsurf_view_load_uri(b, "data:text/html,<body style='background:%233c3'>b</body>");
	g_ptr_array_add(views, a);
	g_ptr_array_add(views, b);

	/* Both tabs get a panel once their first frame arrives. */
	g_assert_true(run_frames(scene, views, a, two_panels));

	/* The default camera looks at the front panel: the active tab. */
	g_assert_true(gsurf_lrg_scene_pick(scene, 320.0f, 214.0f, &pick));
	g_assert_cmpint(pick.kind, ==, GSURF_LRG_PICK_TAB);
	g_assert_true(pick.view == a);
	g_assert_cmpint(pick.x, >=, 0);
	g_assert_cmpint(pick.x, <, 640);

	/* Arrangements and environments by id, unknown ones refused, cycling. */
	g_assert_true(gsurf_lrg_scene_set_arrangement(scene, "grid"));
	g_assert_false(gsurf_lrg_scene_set_arrangement(scene, "nope"));
	g_assert_cmpstr(gsurf_lrg_scene_get_arrangement(scene), ==, "grid");
	g_assert_cmpstr(gsurf_lrg_scene_cycle_arrangement(scene), ==, "free");
	g_assert_true(gsurf_lrg_scene_set_environment(scene, "sky"));
	g_assert_cmpstr(gsurf_lrg_scene_cycle_environment(scene), ==, "workshop");

	/* Pins and placements round-trip. */
	g_assert_true(run_frames(scene, views, a, two_panels));
	g_assert_true(gsurf_lrg_scene_pin_view(scene, b));
	g_assert_true(gsurf_lrg_scene_is_view_pinned(scene, b));
	png = gsurf_lrg_scene_get_view_placement(scene, b);
	g_assert_nonnull(png);
	g_assert_true(gsurf_lrg_scene_set_view_placement(scene, b, png));
	g_assert_false(gsurf_lrg_scene_set_view_placement(scene, b, "1 2 3"));
	g_assert_false(gsurf_lrg_scene_set_view_placement(scene, b, "0 0 0 0 -1 1"));
	gsurf_lrg_scene_unpin_view(scene, NULL);
	g_assert_false(gsurf_lrg_scene_is_view_pinned(scene, b));
	g_clear_pointer(&png, g_free);

	/* Reading mode flies in, then asks for the flat path. */
	gsurf_lrg_scene_set_reading(scene, TRUE);
	g_assert_true(gsurf_lrg_scene_get_reading(scene));
	g_assert_true(run_frames(scene, views, a, flat_ready));

	/* History depth leaves reading and swaps to the stack; leaving it
	 * restores the tab arrangement. */
	gsurf_lrg_scene_set_history(scene, TRUE);
	g_assert_false(gsurf_lrg_scene_get_reading(scene));
	g_assert_true(gsurf_lrg_scene_get_history(scene));
	gsurf_lrg_scene_set_history(scene, FALSE);
	g_assert_cmpstr(gsurf_lrg_scene_get_arrangement(scene), ==, "free");

	/* Camera bookmarks. */
	gsurf_lrg_scene_save_view(scene, "home");
	g_assert_true(gsurf_lrg_scene_restore_view(scene, "home"));
	g_assert_false(gsurf_lrg_scene_restore_view(scene, "missing"));

	/* A screenshot of the next frame. */
	dir = g_dir_make_tmp("gsurf-lrg-XXXXXX", NULL);
	png = g_build_filename(dir, "frame.png", NULL);
	shot_ok = shot_done = FALSE;
	gsurf_lrg_scene_request_screenshot(scene, png, on_scene_shot, NULL);
	run_frames(scene, views, a, settled);
	g_assert_true(shot_done);
	g_assert_true(shot_ok);
	g_assert_true(g_file_test(png, G_FILE_TEST_IS_REGULAR));
	g_unlink(png);
	g_rmdir(dir);
}
#endif /* GSURF_HAVE_LRG_3D */

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
	/* Before any message: GLib takes the writer only once. */
	g_log_set_writer_func(tolerate_system_gtk, NULL, NULL);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/lrg/render-mode/from-string", test_render_mode_from_string);
	g_test_add_func("/gsurf/lrg/render-mode/to-string", test_render_mode_to_string);
	g_test_add_func("/gsurf/lrg/render-mode/implemented", test_render_mode_implemented);
	g_test_add_func("/gsurf/lrg/spec/parse", test_spec_parse);
	g_test_add_func("/gsurf/lrg/actions", test_lrg_actions);
	g_test_add_func("/gsurf/lrg/backend/enum", test_backend_enum_registered);
	g_test_add_func("/gsurf/lrg/backend/availability", test_backend_availability);
	g_test_add_func("/gsurf/lrg/backend/default-roundtrip", test_backend_default_roundtrip);
#ifdef GSURF_HAVE_LRG_BACKEND
	g_test_add_func("/gsurf/lrg/view/create", test_lrg_view_create);
#endif
#ifdef GSURF_HAVE_LRG_3D
	g_test_add_func("/gsurf/lrg/scene/refresh-interval", test_scene_refresh_interval);
	g_test_add_func("/gsurf/lrg/scene/history-keys", test_scene_history_keys);
	g_test_add_func("/gsurf/lrg/scene/live", test_scene_live);
#endif
	return g_test_run();
}
