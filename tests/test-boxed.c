/*
 * test-boxed.c - GSURF boxed types (new/copy/free, getters, setters)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <glib.h>

static void
test_hit_test(void)
{
	g_autoptr(GsurfHitTest) h = gsurf_hit_test_new();

	g_assert_nonnull(h);
	g_assert_cmpuint(gsurf_hit_test_get_context(h), ==, 0);
	g_assert_false(gsurf_hit_test_context_is_link(h));

	gsurf_hit_test_set_context(h, GSURF_HIT_TEST_LINK | GSURF_HIT_TEST_IMAGE);
	gsurf_hit_test_set_link_uri(h, "https://example.com/a");
	gsurf_hit_test_set_image_uri(h, "https://example.com/i.png");
	gsurf_hit_test_set_link_label(h, "A link");

	g_assert_true(gsurf_hit_test_context_is_link(h));
	g_assert_true(gsurf_hit_test_context_is_image(h));
	g_assert_false(gsurf_hit_test_context_is_media(h));
	g_assert_cmpstr(gsurf_hit_test_get_link_uri(h), ==, "https://example.com/a");
	g_assert_cmpstr(gsurf_hit_test_get_image_uri(h), ==, "https://example.com/i.png");
	g_assert_cmpstr(gsurf_hit_test_get_link_label(h), ==, "A link");

	/* Deep copy is independent. */
	{
		g_autoptr(GsurfHitTest) c = gsurf_hit_test_copy(h);
		gsurf_hit_test_set_link_uri(h, "https://changed/");
		g_assert_cmpstr(gsurf_hit_test_get_link_uri(c), ==, "https://example.com/a");
		g_assert_cmpuint(gsurf_hit_test_get_context(c), ==,
			GSURF_HIT_TEST_LINK | GSURF_HIT_TEST_IMAGE);
	}
}

static void
test_certificate(void)
{
	g_autoptr(GsurfCertificate) c = gsurf_certificate_new();

	gsurf_certificate_set_subject(c, "CN=example.com");
	gsurf_certificate_set_issuer(c, "CN=Test CA");
	gsurf_certificate_set_fingerprint(c, "AA:BB:CC");
	gsurf_certificate_set_trusted(c, TRUE);

	g_assert_cmpstr(gsurf_certificate_get_subject(c), ==, "CN=example.com");
	g_assert_cmpstr(gsurf_certificate_get_issuer(c), ==, "CN=Test CA");
	g_assert_cmpstr(gsurf_certificate_get_fingerprint(c), ==, "AA:BB:CC");
	g_assert_true(gsurf_certificate_get_trusted(c));

	{
		g_autoptr(GsurfCertificate) d = gsurf_certificate_copy(c);
		g_assert_cmpstr(gsurf_certificate_get_subject(d), ==, "CN=example.com");
		g_assert_true(gsurf_certificate_get_trusted(d));
	}
}

static void
test_navigation_action(void)
{
	g_autoptr(GsurfNavigationAction) a = gsurf_navigation_action_new();

	gsurf_navigation_action_set_uri(a, "https://example.com/");
	gsurf_navigation_action_set_mouse_button(a, 2);
	gsurf_navigation_action_set_modifiers(a, GSURF_MOD_CTRL);
	gsurf_navigation_action_set_is_user_gesture(a, TRUE);

	g_assert_cmpstr(gsurf_navigation_action_get_uri(a), ==, "https://example.com/");
	g_assert_cmpuint(gsurf_navigation_action_get_mouse_button(a), ==, 2);
	g_assert_cmpuint(gsurf_navigation_action_get_modifiers(a), ==, GSURF_MOD_CTRL);
	g_assert_true(gsurf_navigation_action_get_is_user_gesture(a));
}

static void
test_menu_item(void)
{
	g_autoptr(GsurfMenuItem) m = gsurf_menu_item_new();
	g_autoptr(GsurfMenuItem) sep = gsurf_menu_item_new_separator();

	gsurf_menu_item_set_label(m, "Play externally");
	gsurf_menu_item_set_action(m, "spawn");
	gsurf_menu_item_set_arg(m, "mpv 'https://example.com/v'");

	g_assert_cmpstr(gsurf_menu_item_get_label(m), ==, "Play externally");
	g_assert_cmpstr(gsurf_menu_item_get_action(m), ==, "spawn");
	g_assert_cmpstr(gsurf_menu_item_get_arg(m), ==, "mpv 'https://example.com/v'");
	g_assert_false(gsurf_menu_item_get_is_separator(m));
	g_assert_true(gsurf_menu_item_get_is_separator(sep));
}

static void
test_keybind(void)
{
	g_autoptr(GsurfKeybind) k = gsurf_keybind_new(0x66 /* f */, GSURF_MOD_CTRL,
		GSURF_MODE_NORMAL, GSURF_ACTION_RELOAD, "arg");

	g_assert_cmpuint(gsurf_keybind_get_keyval(k), ==, 0x66);
	g_assert_cmpuint(gsurf_keybind_get_modifiers(k), ==, GSURF_MOD_CTRL);
	g_assert_cmpint(gsurf_keybind_get_mode(k), ==, GSURF_MODE_NORMAL);
	g_assert_cmpint(gsurf_keybind_get_action(k), ==, GSURF_ACTION_RELOAD);
	g_assert_cmpstr(gsurf_keybind_get_arg(k), ==, "arg");

	{
		g_autoptr(GsurfKeybind) c = gsurf_keybind_copy(k);
		g_assert_cmpstr(gsurf_keybind_get_arg(c), ==, "arg");
		g_assert_cmpint(gsurf_keybind_get_action(c), ==, GSURF_ACTION_RELOAD);
	}
}

static void
test_mousebind(void)
{
	g_autoptr(GsurfMousebind) b = gsurf_mousebind_new(8, GSURF_MOD_NONE,
		GSURF_MODE_NORMAL, GSURF_ACTION_BACK, NULL);

	g_assert_cmpuint(gsurf_mousebind_get_button(b), ==, 8);
	g_assert_cmpint(gsurf_mousebind_get_action(b), ==, GSURF_ACTION_BACK);
	g_assert_null(gsurf_mousebind_get_arg(b));
}

static void
test_uri_parameters(void)
{
	g_autoptr(GsurfUriParameters) p = gsurf_uri_parameters_new(".*\\.example\\.com");

	g_assert_cmpstr(gsurf_uri_parameters_get_pattern(p), ==, ".*\\.example\\.com");

	gsurf_uri_parameters_set_override(p, "javascript", "false");
	gsurf_uri_parameters_set_override(p, "images", "true");
	g_assert_cmpstr(gsurf_uri_parameters_get_override(p, "javascript"), ==, "false");
	g_assert_cmpstr(gsurf_uri_parameters_get_override(p, "images"), ==, "true");
	g_assert_null(gsurf_uri_parameters_get_override(p, "missing"));

	{
		g_autoptr(GsurfUriParameters) c = gsurf_uri_parameters_copy(p);
		/* Mutating the original must not affect the copy. */
		gsurf_uri_parameters_set_override(p, "javascript", "true");
		g_assert_cmpstr(gsurf_uri_parameters_get_override(c, "javascript"), ==, "false");
		g_assert_cmpstr(gsurf_uri_parameters_get_pattern(c), ==, ".*\\.example\\.com");
	}
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/gsurf/boxed/hit-test", test_hit_test);
	g_test_add_func("/gsurf/boxed/certificate", test_certificate);
	g_test_add_func("/gsurf/boxed/navigation-action", test_navigation_action);
	g_test_add_func("/gsurf/boxed/menu-item", test_menu_item);
	g_test_add_func("/gsurf/boxed/keybind", test_keybind);
	g_test_add_func("/gsurf/boxed/mousebind", test_mousebind);
	g_test_add_func("/gsurf/boxed/uri-parameters", test_uri_parameters);

	return g_test_run();
}
