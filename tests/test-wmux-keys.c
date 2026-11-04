/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-wmux-keys.c - wmux key notation
 *
 * Compiles modules/wmux/gsurf-wmux-keys.c straight into the test and
 * checks docs/wmux.org, "Key notation": tmux and gsurf notation normalize
 * to one canonical name, key events (keyval + modifiers) map to the same
 * name (shifted printables name their character, arrows keep S-, bare
 * modifiers and Super have no name), the GDK modifier conversion, every
 * error, and a seeded fuzz run (normalize is idempotent and never
 * crashes; every event name normalizes to itself).
 *
 * The seed defaults to a constant; set GSURF_WMUX_SEED to replay
 * another. Failures print it.
 */
#include <gsurf.h>
#include <glib.h>
#include <string.h>

#include "modules/wmux/gsurf-wmux-keys.c"

static guint32
test_seed(void)
{
	const gchar *env = g_getenv("GSURF_WMUX_SEED");
	guint32 seed = env != NULL ? (guint32)g_ascii_strtoull(env, NULL, 10) : 20261104u;

	g_test_message("GSURF_WMUX_SEED=%u", seed);
	return seed;
}

/* normalize(@in) must give @out. */
static void
assert_norm(const gchar *in, const gchar *out)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *got = gsurf_wmux_keys_normalize(in, &error);

	if (got == NULL)
		g_error("normalize(\"%s\") failed: %s", in, error->message);
	if (strcmp(got, out) != 0)
		g_error("normalize(\"%s\") = \"%s\", expected \"%s\"", in, got, out);
	g_assert_no_error(error);
}

static void
assert_norm_error(const gchar *in, GsurfWmuxKeysError code)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *got = gsurf_wmux_keys_normalize(in, &error);

	g_assert_null(got);
	g_assert_error(error, GSURF_WMUX_KEYS_ERROR, (gint)code);
}

static void
assert_event(guint keyval, guint mods, const gchar *out)
{
	g_autofree gchar *got = gsurf_wmux_keys_from_event(keyval, mods);

	if (g_strcmp0(got, out) != 0)
		g_error("from_event(0x%x, 0x%x) = \"%s\", expected \"%s\"", keyval, mods,
			got != NULL ? got : "(null)", out != NULL ? out : "(null)");
}

/* tmux notation is canonical already and is accepted in either case for
 * the modifier letters. */
static void
test_tmux_notation(void)
{
	assert_norm("C-b", "C-b");
	assert_norm("c-b", "C-b");
	assert_norm("^b", "C-b");
	assert_norm("M-1", "M-1");
	assert_norm("m-1", "M-1");
	assert_norm("S-Up", "S-Up");
	assert_norm("s-Up", "S-Up");
	assert_norm("C-M-x", "C-M-x");
	assert_norm("M-C-x", "C-M-x");
	assert_norm("C-M-S-Up", "C-M-S-Up");
	assert_norm("S-M-C-Up", "C-M-S-Up");
	assert_norm("C-C-b", "C-b");
	assert_norm("%", "%");
	assert_norm("\"", "\"");
	assert_norm("{", "{");
	assert_norm("}", "}");
	assert_norm(";", ";");
	assert_norm(":", ":");
	assert_norm("?", "?");
	assert_norm("^", "^");
	assert_norm("-", "-");
	assert_norm("+", "+");
	assert_norm("C--", "C--");
	assert_norm("C-+", "C-+");
	assert_norm("C-%", "C-%");
	assert_norm("C-\"", "C-\"");
	assert_norm("Space", "Space");
	assert_norm("space", "Space");
	assert_norm(" ", "Space");
	assert_norm("  C-b  ", "C-b");
	assert_norm("Up", "Up");
	assert_norm("up", "Up");
	assert_norm("Down", "Down");
	assert_norm("Left", "Left");
	assert_norm("Right", "Right");
	assert_norm("Enter", "Enter");
	assert_norm("Escape", "Escape");
	assert_norm("Esc", "Escape");
	assert_norm("Tab", "Tab");
	assert_norm("BTab", "BTab");
	assert_norm("S-Tab", "BTab");
	assert_norm("S-BTab", "BTab");
	assert_norm("C-S-Tab", "C-BTab");
	assert_norm("BSpace", "BSpace");
	assert_norm("Home", "Home");
	assert_norm("End", "End");
	assert_norm("PPage", "PPage");
	assert_norm("NPage", "NPage");
	assert_norm("PgUp", "PPage");
	assert_norm("PgDn", "NPage");
	assert_norm("IC", "IC");
	assert_norm("DC", "DC");
	assert_norm("F1", "F1");
	assert_norm("f12", "F12");
	assert_norm("C-F5", "C-F5");
	assert_norm("KP5", "KP5");
	assert_norm("KPEnter", "KPEnter");
	assert_norm("0", "0");
	assert_norm("9", "9");
	assert_norm("a", "a");
	assert_norm("A", "A");
	/* Shift folds into printables. */
	assert_norm("S-a", "A");
	assert_norm("S-A", "A");
	assert_norm("S-5", "%");
	assert_norm("S-'", "\"");
	assert_norm("S-[", "{");
	assert_norm("S-]", "}");
	assert_norm("S-%", "%");
	assert_norm("C-S-5", "C-%");
	assert_norm("C-S-a", "C-A");
	assert_norm("C-A", "C-A");
	assert_norm("S-Space", "S-Space");
	assert_norm("é", "é");
	assert_norm("S-é", "É");
	assert_norm("M-ф", "M-ф");
}

/* gsurf notation reaches the same canonical names. */
static void
test_gsurf_notation(void)
{
	assert_norm("Ctrl+b", "C-b");
	assert_norm("ctrl+b", "C-b");
	assert_norm("Control+b", "C-b");
	assert_norm("Ctrl+B", "C-B");
	assert_norm("Ctrl+Shift+b", "C-B");
	assert_norm("Alt+1", "M-1");
	assert_norm("Mod1+1", "M-1");
	assert_norm("Meta+1", "M-1");
	assert_norm("Shift+Up", "S-Up");
	assert_norm("Alt+Shift+Up", "M-S-Up");
	assert_norm("Ctrl+Shift+Up", "C-S-Up");
	assert_norm("Shift+Ctrl+Alt+x", "C-M-X");
	assert_norm("Ctrl+Alt+x", "C-M-x");
	assert_norm("Shift+5", "%");
	assert_norm("percent", "%");
	assert_norm("Shift+percent", "%");
	assert_norm("Ctrl+Shift+percent", "C-%");
	assert_norm("Ctrl+percent", "C-%");
	assert_norm("Ctrl+Shift+5", "C-%");
	assert_norm("Shift+apostrophe", "\"");
	assert_norm("quotedbl", "\"");
	assert_norm("Ctrl+quotedbl", "C-\"");
	assert_norm("Ctrl+Shift+apostrophe", "C-\"");
	assert_norm("braceleft", "{");
	assert_norm("Shift+bracketleft", "{");
	assert_norm("Shift+bracketright", "}");
	assert_norm("Ctrl+Shift+period", "C->");
	assert_norm("Shift+slash", "?");
	assert_norm("question", "?");
	assert_norm("colon", ":");
	assert_norm("Shift+semicolon", ":");
	assert_norm("semicolon", ";");
	assert_norm("comma", ",");
	assert_norm("exclam", "!");
	assert_norm("ampersand", "&");
	assert_norm("Ctrl++", "C-+");
	assert_norm("Ctrl+plus", "C-+");
	assert_norm("Ctrl+minus", "C--");
	assert_norm("Return", "Enter");
	assert_norm("Ctrl+Return", "C-Enter");
	assert_norm("BackSpace", "BSpace");
	assert_norm("Page_Up", "PPage");
	assert_norm("Page_Down", "NPage");
	assert_norm("Prior", "PPage");
	assert_norm("Insert", "IC");
	assert_norm("Delete", "DC");
	assert_norm("ISO_Left_Tab", "BTab");
	assert_norm("Shift+Tab", "BTab");
	assert_norm("KP_5", "KP5");
	assert_norm("KP_Enter", "KPEnter");
	assert_norm("Ctrl+space", "C-Space");
	assert_norm("Shift+space", "S-Space");
	assert_norm("Alt+F4", "M-F4");

	/* Equivalence helper. */
	g_assert_true(gsurf_wmux_keys_equal("Ctrl+b", "C-b"));
	g_assert_true(gsurf_wmux_keys_equal("Ctrl+Shift+percent", "C-%"));
	g_assert_true(gsurf_wmux_keys_equal("Shift+apostrophe", "\""));
	g_assert_false(gsurf_wmux_keys_equal("C-b", "C-B"));
	g_assert_false(gsurf_wmux_keys_equal("C-b", "Super+b"));
	g_assert_false(gsurf_wmux_keys_equal("Nope", "Nope"));
	g_assert_false(gsurf_wmux_keys_equal(NULL, "C-b"));
}

static void
test_errors(void)
{
	assert_norm_error(NULL, GSURF_WMUX_KEYS_ERROR_EMPTY);
	assert_norm_error("", GSURF_WMUX_KEYS_ERROR_EMPTY);
	assert_norm_error("   ", GSURF_WMUX_KEYS_ERROR_EMPTY);
	assert_norm_error("C-", GSURF_WMUX_KEYS_ERROR_NO_KEY);
	assert_norm_error("C-M-", GSURF_WMUX_KEYS_ERROR_NO_KEY);
	assert_norm_error("Ctrl+", GSURF_WMUX_KEYS_ERROR_NO_KEY);
	assert_norm_error("Ctrl+Shift+", GSURF_WMUX_KEYS_ERROR_NO_KEY);
	assert_norm_error("Super+x", GSURF_WMUX_KEYS_ERROR_SUPER);
	assert_norm_error("Logo+a", GSURF_WMUX_KEYS_ERROR_SUPER);
	assert_norm_error("Mod4+a", GSURF_WMUX_KEYS_ERROR_SUPER);
	assert_norm_error("Hyper+a", GSURF_WMUX_KEYS_ERROR_SUPER);
	assert_norm_error("Ctrl+Super+a", GSURF_WMUX_KEYS_ERROR_SUPER);
	assert_norm_error("Foo+b", GSURF_WMUX_KEYS_ERROR_UNKNOWN_MODIFIER);
	assert_norm_error("Ctrl+Foo+b", GSURF_WMUX_KEYS_ERROR_UNKNOWN_MODIFIER);
	assert_norm_error("Nope", GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY);
	assert_norm_error("C-Nope", GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY);
	assert_norm_error("F13", GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY);
	assert_norm_error("Ctrl+M-x", GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY);
	assert_norm_error("\xff", GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY);
	assert_norm_error("C-\xc3", GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY);
	assert_norm_error("\x01", GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY);
	assert_norm_error("C-\x7f", GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY);
	assert_norm_error("^Up", GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY);
	/* A NULL error location is fine. */
	g_assert_null(gsurf_wmux_keys_normalize("Nope", NULL));
}

/* Key events as GTK delivers them. */
static void
test_events(void)
{
	assert_event(GDK_KEY_b, GSURF_WMUX_MOD_CTRL, "C-b");
	assert_event(GDK_KEY_B, GSURF_WMUX_MOD_CTRL | GSURF_WMUX_MOD_SHIFT, "C-B");
	assert_event(GDK_KEY_b, GSURF_WMUX_MOD_CTRL | GSURF_WMUX_MOD_SHIFT, "C-B");
	/* Shifted printables name their character, without S-. */
	assert_event(GDK_KEY_percent, GSURF_WMUX_MOD_SHIFT, "%");
	assert_event(GDK_KEY_percent, GSURF_WMUX_MOD_SHIFT | GSURF_WMUX_MOD_CTRL, "C-%");
	assert_event(GDK_KEY_quotedbl, GSURF_WMUX_MOD_SHIFT, "\"");
	assert_event(GDK_KEY_quotedbl, GSURF_WMUX_MOD_SHIFT | GSURF_WMUX_MOD_CTRL, "C-\"");
	assert_event(GDK_KEY_braceleft, GSURF_WMUX_MOD_SHIFT, "{");
	assert_event(GDK_KEY_braceright, GSURF_WMUX_MOD_SHIFT, "}");
	assert_event(GDK_KEY_exclam, GSURF_WMUX_MOD_SHIFT, "!");
	assert_event(GDK_KEY_question, GSURF_WMUX_MOD_SHIFT, "?");
	assert_event(GDK_KEY_colon, GSURF_WMUX_MOD_SHIFT, ":");
	assert_event(GDK_KEY_ampersand, GSURF_WMUX_MOD_SHIFT, "&");
	/* Synthesized events: Shift with the unshifted keyval. */
	assert_event(GDK_KEY_5, GSURF_WMUX_MOD_SHIFT, "%");
	assert_event(GDK_KEY_apostrophe, GSURF_WMUX_MOD_SHIFT, "\"");
	assert_event(GDK_KEY_bracketleft, GSURF_WMUX_MOD_SHIFT, "{");
	assert_event(GDK_KEY_5, GSURF_WMUX_MOD_SHIFT | GSURF_WMUX_MOD_CTRL, "C-%");
	/* Unshifted punctuation. */
	assert_event(GDK_KEY_comma, 0, ",");
	assert_event(GDK_KEY_semicolon, 0, ";");
	assert_event(GDK_KEY_minus, GSURF_WMUX_MOD_CTRL, "C--");
	/* Letters: case follows Shift (Caps Lock is not a modifier here). */
	assert_event(GDK_KEY_a, GSURF_WMUX_MOD_SHIFT, "A");
	assert_event(GDK_KEY_A, GSURF_WMUX_MOD_SHIFT, "A");
	assert_event(GDK_KEY_E, GSURF_WMUX_MOD_SHIFT, "E");
	assert_event(GDK_KEY_A, 0, "a");
	assert_event(GDK_KEY_x, GSURF_WMUX_MOD_CTRL | GSURF_WMUX_MOD_ALT, "C-M-x");
	assert_event(GDK_KEY_1, GSURF_WMUX_MOD_ALT, "M-1");
	assert_event(GDK_KEY_0, 0, "0");
	/* Named keys keep S-. */
	assert_event(GDK_KEY_Up, 0, "Up");
	assert_event(GDK_KEY_Up, GSURF_WMUX_MOD_SHIFT, "S-Up");
	assert_event(GDK_KEY_Up, GSURF_WMUX_MOD_CTRL, "C-Up");
	assert_event(GDK_KEY_Left, GSURF_WMUX_MOD_ALT, "M-Left");
	assert_event(GDK_KEY_Down, GSURF_WMUX_MOD_ALT | GSURF_WMUX_MOD_SHIFT, "M-S-Down");
	assert_event(GDK_KEY_Right, GSURF_WMUX_MOD_CTRL | GSURF_WMUX_MOD_ALT | GSURF_WMUX_MOD_SHIFT,
		"C-M-S-Right");
	assert_event(GDK_KEY_space, 0, "Space");
	assert_event(GDK_KEY_space, GSURF_WMUX_MOD_SHIFT, "S-Space");
	assert_event(GDK_KEY_space, GSURF_WMUX_MOD_CTRL, "C-Space");
	assert_event(GDK_KEY_Return, 0, "Enter");
	assert_event(GDK_KEY_KP_Enter, 0, "KPEnter");
	assert_event(GDK_KEY_Escape, 0, "Escape");
	assert_event(GDK_KEY_Tab, 0, "Tab");
	assert_event(GDK_KEY_Tab, GSURF_WMUX_MOD_SHIFT, "BTab");
	assert_event(GDK_KEY_ISO_Left_Tab, GSURF_WMUX_MOD_SHIFT, "BTab");
	assert_event(GDK_KEY_ISO_Left_Tab, 0, "BTab");
	assert_event(GDK_KEY_BackSpace, 0, "BSpace");
	assert_event(GDK_KEY_Page_Up, 0, "PPage");
	assert_event(GDK_KEY_Page_Down, GSURF_WMUX_MOD_CTRL, "C-NPage");
	assert_event(GDK_KEY_Insert, 0, "IC");
	assert_event(GDK_KEY_Delete, 0, "DC");
	assert_event(GDK_KEY_Home, 0, "Home");
	assert_event(GDK_KEY_End, 0, "End");
	assert_event(GDK_KEY_F1, 0, "F1");
	assert_event(GDK_KEY_F12, GSURF_WMUX_MOD_SHIFT, "S-F12");
	assert_event(GDK_KEY_KP_5, 0, "KP5");
	assert_event(GDK_KEY_KP_Up, 0, "Up");
	assert_event(GDK_KEY_KP_Page_Down, 0, "NPage");
	assert_event(GDK_KEY_eacute, 0, "é");
	assert_event(GDK_KEY_Eacute, GSURF_WMUX_MOD_SHIFT, "É");
	assert_event(0x01000444, GSURF_WMUX_MOD_ALT, "M-ф");
	/* Unknown bits are ignored. */
	assert_event(GDK_KEY_b, GSURF_WMUX_MOD_CTRL | (1u << 20), "C-b");

	/* No name: bare modifiers, Super, dead keys, unknown keyvals. */
	assert_event(GDK_KEY_Shift_L, GSURF_WMUX_MOD_SHIFT, NULL);
	assert_event(GDK_KEY_Shift_R, 0, NULL);
	assert_event(GDK_KEY_Control_L, GSURF_WMUX_MOD_CTRL, NULL);
	assert_event(GDK_KEY_Control_R, 0, NULL);
	assert_event(GDK_KEY_Alt_L, GSURF_WMUX_MOD_ALT, NULL);
	assert_event(GDK_KEY_Meta_L, 0, NULL);
	assert_event(GDK_KEY_Super_L, GSURF_WMUX_MOD_SUPER, NULL);
	assert_event(GDK_KEY_Hyper_L, 0, NULL);
	assert_event(GDK_KEY_Caps_Lock, 0, NULL);
	assert_event(GDK_KEY_Num_Lock, 0, NULL);
	assert_event(GDK_KEY_ISO_Level3_Shift, 0, NULL);
	assert_event(GDK_KEY_b, GSURF_WMUX_MOD_SUPER, NULL);
	assert_event(GDK_KEY_Up, GSURF_WMUX_MOD_SUPER | GSURF_WMUX_MOD_CTRL, NULL);
	assert_event(GDK_KEY_dead_acute, 0, NULL);
	assert_event(GDK_KEY_VoidSymbol, 0, NULL);
	assert_event(0, 0, NULL);
	assert_event(0x1f, 0, NULL);
	assert_event(0xa0, 0, NULL);
	assert_event(0x01000007, 0, NULL);
}

/* The GDK modifier conversion and the GsurfKeyMod contract. */
static void
test_mods(void)
{
	/* The bits equal the core's GsurfKeyMod, which input hooks get. */
	g_assert_cmpuint(GSURF_WMUX_MOD_SHIFT, ==, GSURF_MOD_SHIFT);
	g_assert_cmpuint(GSURF_WMUX_MOD_CTRL, ==, GSURF_MOD_CTRL);
	g_assert_cmpuint(GSURF_WMUX_MOD_ALT, ==, GSURF_MOD_ALT);
	g_assert_cmpuint(GSURF_WMUX_MOD_SUPER, ==, GSURF_MOD_SUPER);

	g_assert_cmpuint(gsurf_wmux_keys_mods_from_gdk(0), ==, 0);
	g_assert_cmpuint(gsurf_wmux_keys_mods_from_gdk(1u << 0), ==, GSURF_WMUX_MOD_SHIFT);
	g_assert_cmpuint(gsurf_wmux_keys_mods_from_gdk(1u << 2), ==, GSURF_WMUX_MOD_CTRL);
	g_assert_cmpuint(gsurf_wmux_keys_mods_from_gdk(1u << 3), ==, GSURF_WMUX_MOD_ALT);
	g_assert_cmpuint(gsurf_wmux_keys_mods_from_gdk(1u << 28), ==, GSURF_WMUX_MOD_ALT);
	g_assert_cmpuint(gsurf_wmux_keys_mods_from_gdk(1u << 26), ==, GSURF_WMUX_MOD_SUPER);
	g_assert_cmpuint(gsurf_wmux_keys_mods_from_gdk(1u << 27), ==, GSURF_WMUX_MOD_SUPER);
	/* Lock, NumLock (Mod2), Mod3..5 and the buttons are dropped. */
	g_assert_cmpuint(gsurf_wmux_keys_mods_from_gdk((1u << 1) | (1u << 4) | (1u << 5) |
		(1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) |
		(1u << 12)), ==, 0);
	g_assert_cmpuint(gsurf_wmux_keys_mods_from_gdk((1u << 0) | (1u << 1) | (1u << 2) |
		(1u << 4) | (1u << 8)), ==, GSURF_WMUX_MOD_SHIFT | GSURF_WMUX_MOD_CTRL);
}

/* Random strings never crash normalize(); what parses is canonical and
 * normalizes to itself. Random events: every name normalizes to itself. */
static void
test_fuzz(void)
{
	static const gchar *const atoms[] = {
		"C-", "M-", "S-", "c-", "s-", "^", "Ctrl+", "Alt+", "Shift+", "Super+",
		"+", "-", "a", "B", "5", "%", "\"", "'", "{", "Up", "Space", "Tab", "F1",
		"F99", "percent", "Return", "é", "\xff", "\x01", " ", "Foo+", "KP_",
		"Enter", "BTab", "PgUp", "quotedbl", "",
	};
	guint32 seed = test_seed();
	g_autoptr(GRand) rand = g_rand_new_with_seed(seed);
	guint i;

	for (i = 0; i < 20000; i++) {
		g_autoptr(GString) s = g_string_new(NULL);
		g_autoptr(GError) error = NULL;
		g_autofree gchar *norm = NULL;
		guint n = (guint)g_rand_int_range(rand, 0, 5);
		guint j;

		for (j = 0; j < n; j++)
			g_string_append(s, atoms[g_rand_int_range(rand, 0, G_N_ELEMENTS(atoms))]);
		norm = gsurf_wmux_keys_normalize(s->str, &error);
		if (norm == NULL) {
			if (error == NULL || error->domain != GSURF_WMUX_KEYS_ERROR)
				g_error("seed %u, \"%s\": failed without a keys error", seed, s->str);
		} else {
			g_autofree gchar *again = gsurf_wmux_keys_normalize(norm, NULL);

			if (g_strcmp0(again, norm) != 0)
				g_error("seed %u, \"%s\": \"%s\" renormalizes to \"%s\"", seed, s->str,
					norm, again != NULL ? again : "(null)");
		}
	}

	for (i = 0; i < 20000; i++) {
		guint keyval;
		guint mods = (guint)g_rand_int_range(rand, 0, 16);
		g_autofree gchar *name = NULL;

		switch (g_rand_int_range(rand, 0, 4)) {
		case 0:  keyval = (guint)g_rand_int_range(rand, 0, 0x100); break;
		case 1:  keyval = 0xff00 + (guint)g_rand_int_range(rand, 0, 0x100); break;
		case 2:  keyval = 0xfe00 + (guint)g_rand_int_range(rand, 0, 0x100); break;
		default: keyval = 0x01000000 + (guint)g_rand_int_range(rand, 0, 0x3000); break;
		}
		name = gsurf_wmux_keys_from_event(keyval, mods);
		if (name != NULL) {
			g_autofree gchar *norm = gsurf_wmux_keys_normalize(name, NULL);

			if (g_strcmp0(norm, name) != 0)
				g_error("seed %u, event 0x%x/0x%x: \"%s\" normalizes to \"%s\"", seed,
					keyval, mods, name, norm != NULL ? norm : "(null)");
			if ((mods & GSURF_WMUX_MOD_SUPER) != 0)
				g_error("seed %u: event with Super has a name", seed);
		}
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/wmux-keys/tmux-notation", test_tmux_notation);
	g_test_add_func("/gsurf/wmux-keys/gsurf-notation", test_gsurf_notation);
	g_test_add_func("/gsurf/wmux-keys/errors", test_errors);
	g_test_add_func("/gsurf/wmux-keys/events", test_events);
	g_test_add_func("/gsurf/wmux-keys/mods", test_mods);
	g_test_add_func("/gsurf/wmux-keys/fuzz", test_fuzz);
	return g_test_run();
}
