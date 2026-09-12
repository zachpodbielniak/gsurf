/*
 * test-backend-settings.c - Verify settings reach the native web engine
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <gtk/gtk.h>
#ifdef GSURF_BACKEND_GTK3
#include <webkit2/webkit2.h>
#else
#include <webkit/webkit.h>
#endif

/* These tests create an unshown native view, without loading a page. */
static gboolean
settings_display_available(void)
{
	if (g_getenv("GSURF_TEST_GUI") == NULL) {
		g_test_skip("set GSURF_TEST_GUI=1 with a display to test native settings");
		return FALSE;
	}
#ifdef GSURF_BACKEND_GTK3
	g_assert_true(gtk_init_check(NULL, NULL));
#else
	g_assert_true(gtk_init_check());
#endif
	return TRUE;
}

/* Leaving an overridden site must restore WebKit's default user agent. */
static void
test_user_agent_reset(void)
{
	g_autoptr(GsurfView) view = NULL;
	g_autoptr(GsurfSettings) settings = NULL;
	g_autoptr(WebKitSettings) defaults = NULL;
	WebKitSettings *native;
	guint i;

	if (!settings_display_available())
		return;
	view = gsurf_view_new();
	settings = gsurf_settings_new();
	defaults = webkit_settings_new();
	native = webkit_web_view_get_settings(
		WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view)));

	/* Test both representations: C configs can assign an empty string
	 * directly, while the public setter normalizes it to NULL. */
	for (i = 0; i < 2; i++) {
		gsurf_settings_set_user_agent(settings, "GsurfRegression/1.0");
		gsurf_view_apply_settings(view, settings);
		g_assert_cmpstr(webkit_settings_get_user_agent(native), ==,
			"GsurfRegression/1.0");
		gsurf_settings_set_user_agent(settings, NULL);
		if (i == 1)
			settings->user_agent = g_strdup("");
		gsurf_view_apply_settings(view, settings);
		g_assert_cmpstr(webkit_settings_get_user_agent(native), ==,
			webkit_settings_get_user_agent(defaults));
	}
}

/* Exercise both directions and reapplication: WebRTC implies media streams,
 * but media streams alone must not enable peer connections. */
static void
test_webrtc_settings(void)
{
	g_autoptr(GsurfView) view = NULL;
	g_autoptr(GsurfSettings) settings = NULL;
	WebKitSettings *native;
	guint i, repeat;
	const gboolean values[][2] = {
		{ TRUE, FALSE }, { TRUE, TRUE },
		{ FALSE, TRUE }, { FALSE, FALSE }
	};

	if (!settings_display_available())
		return;
	view = gsurf_view_new();
	settings = gsurf_settings_new();
	native = webkit_web_view_get_settings(
		WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view)));

	for (i = 0; i < G_N_ELEMENTS(values); i++) {
		settings->webrtc = values[i][0];
		settings->media_stream = values[i][1];
		for (repeat = 0; repeat < 2; repeat++) {
			gsurf_view_apply_settings(view, settings);
			g_assert_cmpint(webkit_settings_get_enable_webrtc(native), ==,
				settings->webrtc);
			g_assert_cmpint(webkit_settings_get_enable_media_stream(native), ==,
				settings->webrtc || settings->media_stream);
		}
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/backend/settings/user-agent-reset", test_user_agent_reset);
	g_test_add_func("/gsurf/backend/settings/webrtc", test_webrtc_settings);
	return g_test_run();
}
