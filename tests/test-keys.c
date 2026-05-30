/*
 * test-keys.c - Keybinding string helpers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <glib.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>

static void
test_to_string(void)
{
	g_autofree char *bare = gsurf_keys_to_string(GDK_KEY_r, GSURF_MOD_NONE);
	g_autofree char *ctrl = gsurf_keys_to_string(GDK_KEY_r, GSURF_MOD_CTRL);
	g_autofree char *cs = gsurf_keys_to_string(GDK_KEY_r, GSURF_MOD_CTRL | GSURF_MOD_SHIFT);
	g_autofree char *all = gsurf_keys_to_string(GDK_KEY_x,
		GSURF_MOD_SHIFT | GSURF_MOD_SUPER | GSURF_MOD_ALT | GSURF_MOD_CTRL);
	g_autofree char *special = gsurf_keys_to_string(GDK_KEY_slash, GSURF_MOD_NONE);

	g_assert_cmpstr(bare, ==, "r");
	g_assert_cmpstr(ctrl, ==, "Ctrl+r");
	g_assert_cmpstr(cs, ==, "Ctrl+Shift+r");
	/* Canonical modifier order: Ctrl, Alt, Shift, Super. */
	g_assert_cmpstr(all, ==, "Ctrl+Alt+Shift+Super+x");
	g_assert_cmpstr(special, ==, "slash");
}

static void
test_normalize(void)
{
	g_autofree char *a = gsurf_keys_normalize("shift+ctrl+r");   /* reorder */
	g_autofree char *b = gsurf_keys_normalize("CONTROL+R");      /* case + alias */
	g_autofree char *c = gsurf_keys_normalize("r");
	g_autofree char *d = gsurf_keys_normalize("mod1+Tab");       /* mod1 == alt */

	g_assert_cmpstr(a, ==, "Ctrl+Shift+r");
	g_assert_cmpstr(b, ==, "Ctrl+R");
	g_assert_cmpstr(c, ==, "r");
	g_assert_cmpstr(d, ==, "Alt+Tab");

	g_assert_null(gsurf_keys_normalize(NULL));
	g_assert_null(gsurf_keys_normalize(""));
}

static void
test_normalize_matches_to_string(void)
{
	/* What config stores (normalized) must equal what dispatch produces. */
	g_autofree char *evt = gsurf_keys_to_string(GDK_KEY_g, GSURF_MOD_CTRL | GSURF_MOD_SHIFT);
	g_autofree char *cfg = gsurf_keys_normalize("shift+ctrl+g");
	g_assert_cmpstr(evt, ==, cfg);
}

static void
test_match(void)
{
	g_assert_true(gsurf_keys_match(GDK_KEY_r, GSURF_MOD_CTRL, "ctrl+r"));
	g_assert_true(gsurf_keys_match(GDK_KEY_r, GSURF_MOD_CTRL, "Ctrl+r"));
	g_assert_true(gsurf_keys_match(GDK_KEY_r, GSURF_MOD_NONE, "r"));
	g_assert_true(gsurf_keys_match(GDK_KEY_g, GSURF_MOD_CTRL | GSURF_MOD_SHIFT, "shift+ctrl+g"));

	g_assert_false(gsurf_keys_match(GDK_KEY_r, GSURF_MOD_CTRL, "r"));        /* missing mod */
	g_assert_false(gsurf_keys_match(GDK_KEY_r, GSURF_MOD_NONE, "Ctrl+r"));   /* extra mod */
	g_assert_false(gsurf_keys_match(GDK_KEY_r, GSURF_MOD_NONE, "k"));        /* wrong key */
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/keys/to-string", test_to_string);
	g_test_add_func("/gsurf/keys/normalize", test_normalize);
	g_test_add_func("/gsurf/keys/normalize-matches-to-string", test_normalize_matches_to_string);
	g_test_add_func("/gsurf/keys/match", test_match);
	return g_test_run();
}
