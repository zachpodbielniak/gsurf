/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gsurf.h>

/* Borrowed getter results and slices remain valid setter arguments. This
 * catches freeing the previous value before duplicating an aliased input. */
#define CHECK_STRING(object, type, field) G_STMT_START { \
	gsurf_##type##_set_##field(object, "prefix:value"); \
	gsurf_##type##_set_##field(object, gsurf_##type##_get_##field(object)); \
	g_assert_cmpstr(gsurf_##type##_get_##field(object), ==, "prefix:value"); \
	gsurf_##type##_set_##field(object, gsurf_##type##_get_##field(object) + 7); \
	g_assert_cmpstr(gsurf_##type##_get_##field(object), ==, "value"); \
	gsurf_##type##_set_##field(object, NULL); \
	g_assert_null(gsurf_##type##_get_##field(object)); \
} G_STMT_END

static void
test_boxed_strings(void)
{
	g_autoptr(GsurfHitTest) hit = gsurf_hit_test_new();
	g_autoptr(GsurfCertificate) cert = gsurf_certificate_new();
	g_autoptr(GsurfNavigationAction) nav = gsurf_navigation_action_new();
	g_autoptr(GsurfMenuItem) menu = gsurf_menu_item_new();
	g_autoptr(GsurfUriParameters) params = gsurf_uri_parameters_new(NULL);
	g_autoptr(GsurfKeybind) key = gsurf_keybind_new(0, 0, GSURF_MODE_NORMAL, GSURF_ACTION_NONE, NULL);
	g_autoptr(GsurfMousebind) mouse = gsurf_mousebind_new(0, 0, GSURF_MODE_NORMAL, GSURF_ACTION_NONE, NULL);

	CHECK_STRING(hit, hit_test, link_uri);
	CHECK_STRING(hit, hit_test, image_uri);
	CHECK_STRING(hit, hit_test, media_uri);
	CHECK_STRING(hit, hit_test, link_label);
	CHECK_STRING(cert, certificate, subject);
	CHECK_STRING(cert, certificate, issuer);
	CHECK_STRING(cert, certificate, not_before);
	CHECK_STRING(cert, certificate, not_after);
	CHECK_STRING(cert, certificate, fingerprint);
	CHECK_STRING(cert, certificate, pem);
	CHECK_STRING(nav, navigation_action, uri);
	CHECK_STRING(menu, menu_item, label);
	CHECK_STRING(menu, menu_item, action);
	CHECK_STRING(menu, menu_item, arg);
	CHECK_STRING(params, uri_parameters, pattern);
	CHECK_STRING(key, keybind, arg);
	CHECK_STRING(mouse, mousebind, arg);
}

static void
test_download_strings(void)
{
	g_autoptr(GsurfDownload) download = gsurf_download_new(NULL);

	CHECK_STRING(download, download, uri);
	CHECK_STRING(download, download, destination);
	CHECK_STRING(download, download, suggested_filename);
}

static void
test_settings_strings(void)
{
	g_autoptr(GsurfSettings) settings = gsurf_settings_new();

	gsurf_settings_set_user_agent(settings, "Agent/1.0");
	gsurf_settings_set_user_agent(settings, settings->user_agent);
	g_assert_cmpstr(settings->user_agent, ==, "Agent/1.0");
	gsurf_settings_set_user_agent(settings, settings->user_agent + 6);
	g_assert_cmpstr(settings->user_agent, ==, "1.0");
	gsurf_settings_set_user_agent(settings, settings->user_agent + 3);
	g_assert_null(settings->user_agent);
	gsurf_settings_set_default_charset(settings, settings->default_charset);
	g_assert_cmpstr(settings->default_charset, ==, "UTF-8");
	gsurf_settings_set_default_charset(settings, "prefix:UTF-8");
	gsurf_settings_set_default_charset(settings, settings->default_charset + 7);
	g_assert_cmpstr(settings->default_charset, ==, "UTF-8");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/ownership/boxed-strings", test_boxed_strings);
	g_test_add_func("/gsurf/ownership/download-strings", test_download_strings);
	g_test_add_func("/gsurf/ownership/settings-strings", test_settings_strings);
	return g_test_run();
}
