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
	g_autofree char *b = gsurf_keys_normalize("CONTROL+R");      /* alias; R is Shift+r */
	g_autofree char *c = gsurf_keys_normalize("r");
	g_autofree char *d = gsurf_keys_normalize("mod1+Tab");       /* mod1 == alt */

	g_assert_cmpstr(a, ==, "Ctrl+Shift+r");
	g_assert_cmpstr(b, ==, "Ctrl+Shift+r");
	g_assert_cmpstr(c, ==, "r");
	g_assert_cmpstr(d, ==, "Alt+Tab");

	/* "?" is the user-facing spelling of GDK's "question" keyval. */
	{
		g_autofree char *q = gsurf_keys_normalize("?");
		g_autofree char *sq = gsurf_keys_normalize("Shift+?");

		g_assert_cmpstr(q, ==, "question");
		g_assert_cmpstr(sq, ==, "Shift+question");
	}

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

/* GDK reports Shift+letter as the uppercase keyval *and* the Shift mask;
 * every config spelling of that key must still match it. */
static void
test_shift_letters(void)
{
	g_autofree char *evt = gsurf_keys_to_string(GDK_KEY_N, GSURF_MOD_SHIFT);
	g_autofree char *ctrl = gsurf_keys_to_string(GDK_KEY_S, GSURF_MOD_CTRL | GSURF_MOD_SHIFT);
	g_autofree char *caps = gsurf_keys_to_string(GDK_KEY_H, GSURF_MOD_NONE);
	g_autofree char *n1 = gsurf_keys_normalize("N");
	g_autofree char *n2 = gsurf_keys_normalize("Shift+N");
	g_autofree char *n3 = gsurf_keys_normalize("shift+n");

	g_assert_cmpstr(evt, ==, "Shift+n");
	g_assert_cmpstr(ctrl, ==, "Ctrl+Shift+s");
	/* An uppercase keyval without the mask (Caps Lock) is still shifted. */
	g_assert_cmpstr(caps, ==, "Shift+h");
	g_assert_cmpstr(n1, ==, "Shift+n");
	g_assert_cmpstr(n2, ==, "Shift+n");
	g_assert_cmpstr(n3, ==, "Shift+n");

	g_assert_true(gsurf_keys_match(GDK_KEY_N, GSURF_MOD_SHIFT, "N"));
	g_assert_true(gsurf_keys_match(GDK_KEY_N, GSURF_MOD_SHIFT, "Shift+n"));
	g_assert_true(gsurf_keys_match(GDK_KEY_S, GSURF_MOD_CTRL | GSURF_MOD_SHIFT, "Ctrl+Shift+s"));
	g_assert_true(gsurf_keys_match(GDK_KEY_O, GSURF_MOD_CTRL | GSURF_MOD_SHIFT, "Ctrl+Shift+o"));
	g_assert_false(gsurf_keys_match(GDK_KEY_N, GSURF_MOD_SHIFT, "n"));
	g_assert_false(gsurf_keys_match(GDK_KEY_n, GSURF_MOD_NONE, "N"));
}

/* GDK names Shift+Tab "ISO_Left_Tab"; it must meet "Shift+Tab" configs. */
static void
test_iso_left_tab(void)
{
	g_autofree char *evt = gsurf_keys_to_string(GDK_KEY_ISO_Left_Tab,
		GSURF_MOD_CTRL | GSURF_MOD_SHIFT);
	g_autofree char *cfg = gsurf_keys_normalize("ISO_Left_Tab");

	g_assert_cmpstr(evt, ==, "Ctrl+Shift+Tab");
	g_assert_cmpstr(cfg, ==, "Shift+Tab");
	g_assert_true(gsurf_keys_match(GDK_KEY_ISO_Left_Tab,
		GSURF_MOD_CTRL | GSURF_MOD_SHIFT, "Ctrl+Shift+Tab"));
}

/* The bare name keeps case so vi keymaps can tell g from G. */
static void
test_to_name(void)
{
	g_autofree char *g = gsurf_keys_to_name(GDK_KEY_g, GSURF_MOD_NONE);
	g_autofree char *sg = gsurf_keys_to_name(GDK_KEY_G, GSURF_MOD_SHIFT);
	g_autofree char *mg = gsurf_keys_to_name(GDK_KEY_g, GSURF_MOD_SHIFT);
	g_autofree char *esc = gsurf_keys_to_name(GDK_KEY_Escape, GSURF_MOD_CTRL);
	g_autofree char *tab = gsurf_keys_to_name(GDK_KEY_ISO_Left_Tab, GSURF_MOD_SHIFT);

	g_assert_cmpstr(g, ==, "g");
	g_assert_cmpstr(sg, ==, "G");
	g_assert_cmpstr(mg, ==, "G");
	g_assert_cmpstr(esc, ==, "Escape");
	g_assert_cmpstr(tab, ==, "Tab");
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
	g_assert_true(gsurf_keys_match(GDK_KEY_question, GSURF_MOD_NONE, "?"));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/keys/to-string", test_to_string);
	g_test_add_func("/gsurf/keys/normalize", test_normalize);
	g_test_add_func("/gsurf/keys/normalize-matches-to-string", test_normalize_matches_to_string);
	g_test_add_func("/gsurf/keys/match", test_match);
	g_test_add_func("/gsurf/keys/shift-letters", test_shift_letters);
	g_test_add_func("/gsurf/keys/iso-left-tab", test_iso_left_tab);
	g_test_add_func("/gsurf/keys/to-name", test_to_name);
	return g_test_run();
}
