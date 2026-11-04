/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-wmux-bindings.c - wmux key binding tables
 *
 * Compiles the wmux model (keys, command language, layout, binding
 * tables) straight into the test and checks docs/wmux.org, "tmux mapping
 * table": the default prefix and root tables (every key canonical, every
 * command valid, the repeatable set), key events reaching their default
 * commands, the doc's mapping tables matching the defaults row for row,
 * set / unbind / repeatable, merging overrides with tombstones,
 * validation with a GError per bad command, the parsed-command cache,
 * copies, and a seeded random run against the table invariants.
 *
 * The seed defaults to a constant; set GSURF_WMUX_SEED to replay
 * another. Failures print it.
 */
#include <glib.h>
#include <string.h>

#include "modules/wmux/gsurf-wmux-keys.c"
#include "modules/wmux/gsurf-wmux-command.c"
#include "modules/wmux/gsurf-wmux-layout.c"
#include "modules/wmux/gsurf-wmux-bindings.c"

static guint32
test_seed(void)
{
	const gchar *env = g_getenv("GSURF_WMUX_SEED");
	guint32 seed = env != NULL ? (guint32)g_ascii_strtoull(env, NULL, 10) : 20261104u;

	g_test_message("GSURF_WMUX_SEED=%u", seed);
	return seed;
}

/* The command bound to the key event, or NULL. */
static const gchar *
command_for_event(GsurfWmuxBindings *b, guint keyval, guint mods)
{
	g_autofree gchar *name = gsurf_wmux_keys_from_event(keyval, mods);
	const GsurfWmuxBinding *binding;

	g_assert_nonnull(name);
	binding = gsurf_wmux_bindings_lookup(b, name);
	return binding != NULL ? binding->command : NULL;
}

/* The table's live keys as "k1 k2 ...". */
static gchar *
keys_string(GsurfWmuxBindings *b)
{
	GString *s = g_string_new(NULL);
	guint i;

	for (i = 0; i < gsurf_wmux_bindings_get_n(b); i++)
		g_string_append_printf(s, "%s%s", i > 0 ? " " : "",
			gsurf_wmux_bindings_get_nth(b, i)->key);
	return g_string_free(s, FALSE);
}

/* --- Defaults --- */

static void
test_default_tables(void)
{
	static const gchar *const repeatable[] = {
		"Up", "Down", "Left", "Right", "C-Up", "C-Down", "C-Left", "C-Right",
		"M-Up", "M-Down", "M-Left", "M-Right",
	};
	g_autoptr(GsurfWmuxBindings) prefix = gsurf_wmux_bindings_new_prefix_defaults();
	g_autoptr(GsurfWmuxBindings) root = gsurf_wmux_bindings_new_root_defaults();
	const GsurfWmuxDefaultBinding *rows;
	g_autoptr(GPtrArray) errors = NULL;
	guint n = 0;
	guint n_repeat = 0;
	guint i;

	/* Every default key is written canonically and every command parses;
	 * the table keeps the data's order and size. */
	rows = gsurf_wmux_bindings_default_prefix_table(&n);
	g_assert_cmpuint(n, ==, 53);
	g_assert_cmpuint(gsurf_wmux_bindings_get_n(prefix), ==, n);
	for (i = 0; i < n; i++) {
		g_autoptr(GsurfWmuxCommand) cmd = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *norm = gsurf_wmux_keys_normalize(rows[i].key, &error);

		g_assert_no_error(error);
		g_assert_cmpstr(norm, ==, rows[i].key);
		cmd = gsurf_wmux_command_parse(rows[i].command, NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(gsurf_wmux_bindings_get_nth(prefix, i)->key, ==, rows[i].key);
		if (rows[i].repeatable)
			n_repeat++;
	}
	/* Repeatable: exactly the arrows, C-arrows and M-arrows. */
	g_assert_cmpuint(n_repeat, ==, G_N_ELEMENTS(repeatable));
	for (i = 0; i < G_N_ELEMENTS(repeatable); i++)
		g_assert_true(gsurf_wmux_bindings_lookup(prefix, repeatable[i])->repeatable);
	g_assert_false(gsurf_wmux_bindings_lookup(prefix, "%")->repeatable);
	g_assert_true(gsurf_wmux_bindings_validate(prefix, FALSE, &errors));
	g_assert_null(errors);

	rows = gsurf_wmux_bindings_default_root_table(&n);
	g_assert_cmpuint(n, ==, 4);
	g_assert_cmpuint(gsurf_wmux_bindings_get_n(root), ==, 4);
	g_assert_true(gsurf_wmux_bindings_validate(root, FALSE, NULL));
	g_assert_cmpstr(gsurf_wmux_bindings_lookup(root, "C-h")->command, ==, "select-pane -L");
	g_assert_cmpstr(gsurf_wmux_bindings_lookup(root, "C-j")->command, ==, "select-pane -D");
	g_assert_cmpstr(gsurf_wmux_bindings_lookup(root, "C-k")->command, ==, "select-pane -U");
	g_assert_cmpstr(gsurf_wmux_bindings_lookup(root, "C-l")->command, ==, "select-pane -R");
	g_assert_nonnull(gsurf_wmux_bindings_default_root_table(NULL));
	g_assert_cmpstr(GSURF_WMUX_DEFAULT_PREFIX, ==, "C-b");

	/* select-layout accepts exactly the layout model's presets. */
	for (i = 0; i < GSURF_WMUX_PRESET_COUNT; i++) {
		g_autofree gchar *line = g_strdup_printf("select-layout %s",
			gsurf_wmux_preset_to_string((GsurfWmuxPreset)i));
		g_autoptr(GsurfWmuxCommand) cmd = gsurf_wmux_command_parse(line, NULL, NULL);

		g_assert_nonnull(cmd);
		g_assert_cmpstr(layout_names[i], ==, gsurf_wmux_preset_to_string((GsurfWmuxPreset)i));
	}
	g_assert_cmpuint(G_N_ELEMENTS(layout_names), ==, GSURF_WMUX_PRESET_COUNT);
}

/* The plan's tmux mapping, reached through real key events. */
static void
test_default_events(void)
{
	g_autoptr(GsurfWmuxBindings) b = gsurf_wmux_bindings_new_prefix_defaults();
	g_autoptr(GsurfWmuxBindings) root = gsurf_wmux_bindings_new_root_defaults();
	const guint S = GSURF_WMUX_MOD_SHIFT;
	const guint C = GSURF_WMUX_MOD_CTRL;
	const guint M = GSURF_WMUX_MOD_ALT;
	guint i;

	g_assert_cmpstr(command_for_event(b, GDK_KEY_percent, S), ==, "split-window -h");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_5, S), ==, "split-window -h");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_quotedbl, S), ==, "split-window -v");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_apostrophe, S), ==, "split-window -v");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_percent, C | S), ==, "split-window -h -c");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_quotedbl, C | S), ==, "split-window -v -c");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_o, 0), ==, "select-pane -t +1");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_semicolon, 0), ==, "last-pane");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_x, 0), ==, "confirm-before kill-pane");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_z, 0), ==, "resize-pane -Z");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_q, 0), ==, "display-panes");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_c, 0), ==, "new-window");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_n, 0), ==, "next-window");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_p, 0), ==, "previous-window");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_l, 0), ==, "last-window");
	for (i = 0; i <= 9; i++) {
		g_autofree gchar *want = g_strdup_printf("select-window -t %u", i);

		g_assert_cmpstr(command_for_event(b, GDK_KEY_0 + i, 0), ==, want);
	}
	g_assert_cmpstr(command_for_event(b, GDK_KEY_w, 0), ==, "choose-tree");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_ampersand, S), ==, "confirm-before kill-window");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_comma, 0), ==,
		"command-prompt -I #W rename-window");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_braceleft, S), ==, "swap-pane -U");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_braceright, S), ==, "swap-pane -D");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_space, 0), ==, "next-layout");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_1, M), ==, "select-layout even-horizontal");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_2, M), ==, "select-layout even-vertical");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_3, M), ==, "select-layout main-horizontal");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_4, M), ==, "select-layout main-vertical");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_5, M), ==, "select-layout tiled");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_E, S), ==, "select-layout -E");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_exclam, S), ==, "break-pane");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_o, C), ==, "rotate-window");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_o, M), ==, "rotate-window -D");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_Up, 0), ==, "select-pane -U");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_Down, 0), ==, "select-pane -D");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_Left, 0), ==, "select-pane -L");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_Right, 0), ==, "select-pane -R");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_Up, C), ==, "resize-pane -U 1");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_Left, C), ==, "resize-pane -L 1");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_Down, M), ==, "resize-pane -D 5");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_Right, M), ==, "resize-pane -R 5");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_colon, S), ==, "command-prompt");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_question, S), ==, "list-keys");
	g_assert_cmpstr(command_for_event(b, GDK_KEY_b, C), ==, "send-prefix");
	/* Unbound keys. */
	g_assert_null(command_for_event(b, GDK_KEY_Up, S));
	g_assert_null(command_for_event(b, GDK_KEY_e, 0));
	g_assert_null(command_for_event(b, GDK_KEY_h, C));
	/* Root table. */
	g_assert_cmpstr(command_for_event(root, GDK_KEY_h, C), ==, "select-pane -L");
	g_assert_cmpstr(command_for_event(root, GDK_KEY_l, C), ==, "select-pane -R");
	g_assert_null(command_for_event(root, GDK_KEY_h, 0));
}

/* docs/wmux.org, found next to the build tree (build/<mode>/test-*). */
static gchar *
read_doc(void)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;

	g_assert_nonnull(exe);
	dir = g_path_get_dirname(exe);
	path = g_build_filename(dir, "..", "..", "docs", "wmux.org", NULL);
	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read %s: %s", path, error->message);
	return text;
}

/* Strip the org verbatim markers of a table cell: " =x= " -> "x". */
static gchar *
cell_text(const gchar *cell)
{
	g_autofree gchar *t = g_strstrip(g_strdup(cell));
	gsize len = strlen(t);

	g_assert_true(len >= 2 && t[0] == '=' && t[len - 1] == '=');
	return g_strndup(t + 1, len - 2);
}

/* The "tmux mapping table" section lists exactly the defaults, in order,
 * with the right repeatable flag. */
static void
check_doc_table(gchar **lines, const gchar *heading, const GsurfWmuxDefaultBinding *rows,
                guint n)
{
	gboolean in_table = FALSE;
	guint seen = 0;
	guint i;

	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];
		g_auto(GStrv) cells = NULL;
		g_autofree gchar *key = NULL;
		g_autofree gchar *command = NULL;
		g_autofree gchar *repeat = NULL;

		if (g_str_has_prefix(line, "*")) {
			in_table = strcmp(line, heading) == 0;
			continue;
		}
		if (!in_table || !g_str_has_prefix(line, "| ="))
			continue;
		cells = g_strsplit(line, "|", -1);
		g_assert_cmpuint(g_strv_length(cells), >=, 5);
		key = cell_text(cells[1]);
		command = cell_text(cells[2]);
		repeat = g_strstrip(g_strdup(cells[3]));
		if (seen >= n)
			g_error("docs/wmux.org %s: extra row %s", heading, key);
		if (strcmp(key, rows[seen].key) != 0 || strcmp(command, rows[seen].command) != 0 ||
		    strcmp(repeat, rows[seen].repeatable ? "yes" : "no") != 0)
			g_error("docs/wmux.org %s row %u: \"%s\" \"%s\" %s, the default is "
				"\"%s\" \"%s\" %s", heading, seen, key, command, repeat, rows[seen].key,
				rows[seen].command, rows[seen].repeatable ? "yes" : "no");
		seen++;
	}
	g_assert_cmpuint(seen, ==, n);
}

static void
test_doc_tables(void)
{
	g_autofree gchar *doc = read_doc();
	g_auto(GStrv) lines = g_strsplit(doc, "\n", -1);
	const GsurfWmuxDefaultBinding *rows;
	guint n = 0;

	rows = gsurf_wmux_bindings_default_prefix_table(&n);
	check_doc_table(lines, "** Prefix table", rows, n);
	rows = gsurf_wmux_bindings_default_root_table(&n);
	check_doc_table(lines, "** Root table", rows, n);
}

/* --- Editing --- */

static void
test_set_unbind_repeatable(void)
{
	g_autoptr(GsurfWmuxBindings) b = gsurf_wmux_bindings_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *order = NULL;
	const GsurfWmuxBinding *e;

	g_assert_cmpuint(gsurf_wmux_bindings_get_n(b), ==, 0);
	g_assert_null(gsurf_wmux_bindings_get_nth(b, 0));
	g_assert_null(gsurf_wmux_bindings_lookup(b, "C-b"));

	/* New keys append, in any notation. */
	g_assert_true(gsurf_wmux_bindings_set(b, "Ctrl+Shift+percent", "split-window -h -c", NULL));
	g_assert_true(gsurf_wmux_bindings_set(b, "|", "split-window -h", NULL));
	g_assert_true(gsurf_wmux_bindings_set(b, "S-Up", "resize-pane -U 10", NULL));
	order = keys_string(b);
	g_assert_cmpstr(order, ==, "C-% | S-Up");
	g_assert_cmpstr(gsurf_wmux_bindings_lookup(b, "C-%")->command, ==, "split-window -h -c");
	/* Lookup accepts any spelling. */
	g_assert_nonnull(gsurf_wmux_bindings_lookup(b, "Shift+Up"));
	g_assert_null(gsurf_wmux_bindings_lookup(b, "Nope"));
	g_assert_null(gsurf_wmux_bindings_lookup(b, NULL));

	/* Repeatable: set, keep across a rebind, refuse unbound keys. */
	g_assert_true(gsurf_wmux_bindings_set_repeatable(b, "Shift+Up", TRUE, NULL));
	g_assert_true(gsurf_wmux_bindings_lookup(b, "S-Up")->repeatable);
	g_assert_true(gsurf_wmux_bindings_set(b, "S-Up", "resize-pane -U 20", NULL));
	g_assert_true(gsurf_wmux_bindings_lookup(b, "S-Up")->repeatable);
	g_clear_pointer(&order, g_free);
	order = keys_string(b);
	g_assert_cmpstr(order, ==, "C-% | S-Up");
	g_assert_true(gsurf_wmux_bindings_set_repeatable(b, "S-Up", FALSE, NULL));
	g_assert_false(gsurf_wmux_bindings_lookup(b, "S-Up")->repeatable);
	g_assert_false(gsurf_wmux_bindings_set_repeatable(b, "q", TRUE, &error));
	g_assert_error(error, GSURF_WMUX_BINDINGS_ERROR, GSURF_WMUX_BINDINGS_ERROR_NOT_BOUND);
	g_clear_error(&error);
	g_assert_false(gsurf_wmux_bindings_set_repeatable(b, "Super+q", TRUE, &error));
	g_assert_error(error, GSURF_WMUX_BINDINGS_ERROR, GSURF_WMUX_BINDINGS_ERROR_BAD_KEY);
	g_clear_error(&error);

	/* Bad keys. */
	g_assert_false(gsurf_wmux_bindings_set(b, "Ctrl+", "kill-pane", &error));
	g_assert_error(error, GSURF_WMUX_BINDINGS_ERROR, GSURF_WMUX_BINDINGS_ERROR_BAD_KEY);
	g_assert_nonnull(strstr(error->message, "Ctrl+"));
	g_clear_error(&error);
	g_assert_false(gsurf_wmux_bindings_set(b, NULL, "kill-pane", &error));
	g_assert_error(error, GSURF_WMUX_BINDINGS_ERROR, GSURF_WMUX_BINDINGS_ERROR_BAD_KEY);
	g_clear_error(&error);

	/* Unbinding ("" or NULL) leaves a tombstone: invisible to lookup and
	 * iteration, visible to get_entry; rebinding revives it in place. */
	g_assert_true(gsurf_wmux_bindings_set(b, "|", "", NULL));
	g_assert_null(gsurf_wmux_bindings_lookup(b, "|"));
	e = gsurf_wmux_bindings_get_entry(b, "|");
	g_assert_nonnull(e);
	g_assert_null(e->command);
	g_assert_cmpuint(gsurf_wmux_bindings_get_n(b), ==, 2);
	g_clear_pointer(&order, g_free);
	order = keys_string(b);
	g_assert_cmpstr(order, ==, "C-% S-Up");
	g_assert_true(gsurf_wmux_bindings_unbind(b, "C-%", NULL));
	g_assert_cmpuint(gsurf_wmux_bindings_get_n(b), ==, 1);
	g_assert_cmpstr(gsurf_wmux_bindings_get_nth(b, 0)->key, ==, "S-Up");
	g_assert_null(gsurf_wmux_bindings_get_nth(b, 1));
	g_assert_false(gsurf_wmux_bindings_set_repeatable(b, "|", TRUE, NULL));
	g_assert_true(gsurf_wmux_bindings_set(b, "|", "split-window -h", NULL));
	g_clear_pointer(&order, g_free);
	order = keys_string(b);
	g_assert_cmpstr(order, ==, "| S-Up");
	g_assert_null(gsurf_wmux_bindings_get_entry(b, "never"));
}

static void
test_merge(void)
{
	g_autoptr(GsurfWmuxBindings) b = gsurf_wmux_bindings_new_prefix_defaults();
	g_autoptr(GsurfWmuxBindings) o = gsurf_wmux_bindings_new();
	guint n = gsurf_wmux_bindings_get_n(b);
	gint idx_x = -1;
	guint i;

	/* vim-style splits instead of % and ", x without confirmation, an
	 * extra repeatable key, and Up rebound (stays repeatable). */
	gsurf_wmux_bindings_set(o, "|", "split-window -h", NULL);
	gsurf_wmux_bindings_set(o, "-", "split-window -v", NULL);
	gsurf_wmux_bindings_unbind(o, "%", NULL);
	gsurf_wmux_bindings_unbind(o, "Shift+apostrophe", NULL);
	gsurf_wmux_bindings_unbind(o, "never-bound-key-F9", NULL);
	gsurf_wmux_bindings_unbind(o, "F9", NULL);
	gsurf_wmux_bindings_set(o, "x", "kill-pane", NULL);
	gsurf_wmux_bindings_set(o, "H", "resize-pane -L 10", NULL);
	gsurf_wmux_bindings_set_repeatable(o, "H", TRUE, NULL);
	gsurf_wmux_bindings_set(o, "Up", "select-pane -U", NULL);
	gsurf_wmux_bindings_merge(b, o);

	g_assert_null(gsurf_wmux_bindings_lookup(b, "%"));
	g_assert_null(gsurf_wmux_bindings_lookup(b, "\""));
	/* Tombstones are removed outright in the target. */
	g_assert_null(gsurf_wmux_bindings_get_entry(b, "%"));
	g_assert_null(gsurf_wmux_bindings_get_entry(b, "F9"));
	g_assert_cmpstr(gsurf_wmux_bindings_lookup(b, "x")->command, ==, "kill-pane");
	g_assert_true(gsurf_wmux_bindings_lookup(b, "Up")->repeatable);
	g_assert_true(gsurf_wmux_bindings_lookup(b, "H")->repeatable);
	g_assert_false(gsurf_wmux_bindings_lookup(b, "|")->repeatable);
	/* Two removed, three appended. */
	g_assert_cmpuint(gsurf_wmux_bindings_get_n(b), ==, n - 2 + 3);
	/* Replaced keys keep their place; new ones are at the end. */
	for (i = 0; i < gsurf_wmux_bindings_get_n(b); i++)
		if (strcmp(gsurf_wmux_bindings_get_nth(b, i)->key, "x") == 0)
			idx_x = (gint)i;
	g_assert_cmpint(idx_x, ==, 4);
	g_assert_cmpstr(gsurf_wmux_bindings_get_nth(b, n - 2)->key, ==, "|");
	g_assert_cmpstr(gsurf_wmux_bindings_get_nth(b, n - 1)->key, ==, "-");
	g_assert_cmpstr(gsurf_wmux_bindings_get_nth(b, n)->key, ==, "H");
	g_assert_true(gsurf_wmux_bindings_validate(b, FALSE, NULL));

	/* A merged command's cache follows the new text. */
	g_assert_cmpint(gsurf_wmux_bindings_get_command(b, "x")->id, ==, GSURF_WMUX_CMD_KILL_PANE);

	/* Merging an empty table changes nothing. */
	{
		g_autoptr(GsurfWmuxBindings) empty = gsurf_wmux_bindings_new();
		g_autofree gchar *before = keys_string(b);
		g_autofree gchar *after = NULL;

		gsurf_wmux_bindings_merge(b, empty);
		after = keys_string(b);
		g_assert_cmpstr(before, ==, after);
	}
}

static void
test_validate_and_commands(void)
{
	g_autoptr(GsurfWmuxBindings) b = gsurf_wmux_bindings_new_root_defaults();
	g_autoptr(GPtrArray) errors = NULL;
	const GsurfWmuxCommand *cmd;
	const GsurfWmuxCommand *again;
	GError *e0;
	GError *e1;

	/* Parsed commands are cached per binding. */
	cmd = gsurf_wmux_bindings_get_command(b, "C-h");
	g_assert_nonnull(cmd);
	g_assert_cmpint(cmd->id, ==, GSURF_WMUX_CMD_SELECT_PANE);
	g_assert_cmpint(gsurf_wmux_command_direction(cmd), ==, 'L');
	again = gsurf_wmux_bindings_get_command(b, "Ctrl+h");
	g_assert_true(cmd == again);
	gsurf_wmux_bindings_set(b, "C-h", "select-pane -R", NULL);
	cmd = gsurf_wmux_bindings_get_command(b, "C-h");
	g_assert_cmpint(gsurf_wmux_command_direction(cmd), ==, 'R');
	g_assert_null(gsurf_wmux_bindings_get_command(b, "C-z"));

	/* Bad commands: reported (one GError each, in table order), kept
	 * until asked to remove them, never returned as commands. */
	gsurf_wmux_bindings_set(b, "M-x", "bogus", NULL);
	gsurf_wmux_bindings_set(b, "M-y", "split-window -x", NULL);
	gsurf_wmux_bindings_set(b, "M-z", "", NULL);
	g_assert_null(gsurf_wmux_bindings_get_command(b, "M-x"));
	g_assert_null(gsurf_wmux_bindings_get_command(b, "M-z"));
	g_assert_false(gsurf_wmux_bindings_validate(b, FALSE, &errors));
	g_assert_nonnull(errors);
	g_assert_cmpuint(errors->len, ==, 2);
	e0 = g_ptr_array_index(errors, 0);
	e1 = g_ptr_array_index(errors, 1);
	g_assert_error(e0, GSURF_WMUX_BINDINGS_ERROR, GSURF_WMUX_BINDINGS_ERROR_BAD_COMMAND);
	g_assert_error(e1, GSURF_WMUX_BINDINGS_ERROR, GSURF_WMUX_BINDINGS_ERROR_BAD_COMMAND);
	g_assert_nonnull(strstr(e0->message, "\"M-x\""));
	g_assert_nonnull(strstr(e0->message, "unknown command"));
	g_assert_nonnull(strstr(e1->message, "\"M-y\""));
	g_assert_nonnull(strstr(e1->message, "position 14"));
	g_assert_nonnull(gsurf_wmux_bindings_lookup(b, "M-x"));
	g_clear_pointer(&errors, g_ptr_array_unref);
	g_assert_false(gsurf_wmux_bindings_validate(b, FALSE, NULL));

	g_assert_false(gsurf_wmux_bindings_validate(b, TRUE, &errors));
	g_assert_cmpuint(errors->len, ==, 2);
	g_clear_pointer(&errors, g_ptr_array_unref);
	g_assert_null(gsurf_wmux_bindings_lookup(b, "M-x"));
	g_assert_null(gsurf_wmux_bindings_get_entry(b, "M-y"));
	g_assert_true(gsurf_wmux_bindings_validate(b, TRUE, &errors));
	g_assert_null(errors);
	g_assert_cmpuint(gsurf_wmux_bindings_get_n(b), ==, 4);

	/* Fixing a bad command clears its failed cache. */
	gsurf_wmux_bindings_set(b, "M-x", "bogus", NULL);
	g_assert_null(gsurf_wmux_bindings_get_command(b, "M-x"));
	gsurf_wmux_bindings_set(b, "M-x", "kill-pane", NULL);
	g_assert_nonnull(gsurf_wmux_bindings_get_command(b, "M-x"));
}

static void
test_copy(void)
{
	g_autoptr(GsurfWmuxBindings) b = gsurf_wmux_bindings_new_prefix_defaults();
	g_autoptr(GsurfWmuxBindings) c = NULL;
	g_autofree gchar *kb = NULL;
	g_autofree gchar *kc = NULL;

	gsurf_wmux_bindings_unbind(b, "z", NULL);
	c = gsurf_wmux_bindings_copy(b);
	kb = keys_string(b);
	kc = keys_string(c);
	g_assert_cmpstr(kb, ==, kc);
	g_assert_nonnull(gsurf_wmux_bindings_get_entry(c, "z"));
	g_assert_true(gsurf_wmux_bindings_lookup(c, "Up")->repeatable);
	gsurf_wmux_bindings_set(c, "%", "kill-pane", NULL);
	g_assert_cmpstr(gsurf_wmux_bindings_lookup(b, "%")->command, ==, "split-window -h");
	gsurf_wmux_bindings_free(NULL);
}

/* Random edits keep the table consistent: the live count matches
 * iteration, every live key looks itself up, tombstones are hidden, and
 * the cached command always matches the text. */
static void
test_stress(void)
{
	static const gchar *const keys[] = {
		"a", "B", "C-b", "Ctrl+b", "%", "Shift+5", "Up", "S-Up", "M-1", "Alt+1", "Space",
		"F5", "C-%", "\"", "Shift+apostrophe", "x", "q", "0", "Super+x", "Ctrl+",
	};
	static const gchar *const commands[] = {
		"kill-pane", "split-window -h", "bogus", "", NULL, "select-pane -t +1",
		"resize-pane -L 3", "confirm-before kill-window",
	};
	guint32 seed = test_seed();
	g_autoptr(GRand) rand = g_rand_new_with_seed(seed);
	g_autoptr(GsurfWmuxBindings) b = gsurf_wmux_bindings_new_prefix_defaults();
	guint i;

	for (i = 0; i < 5000; i++) {
		const gchar *key = keys[g_rand_int_range(rand, 0, G_N_ELEMENTS(keys))];
		const gchar *cmd = commands[g_rand_int_range(rand, 0, G_N_ELEMENTS(commands))];
		guint n;
		guint j;

		switch (g_rand_int_range(rand, 0, 6)) {
		case 0:
		case 1:
			gsurf_wmux_bindings_set(b, key, cmd, NULL);
			break;
		case 2:
			gsurf_wmux_bindings_set_repeatable(b, key, g_rand_boolean(rand), NULL);
			break;
		case 3: {
			g_autoptr(GsurfWmuxBindings) o = gsurf_wmux_bindings_new();

			gsurf_wmux_bindings_set(o, key, cmd, NULL);
			gsurf_wmux_bindings_merge(b, o);
			break;
		}
		case 4:
			gsurf_wmux_bindings_validate(b, g_rand_boolean(rand), NULL);
			break;
		default: {
			GsurfWmuxBindings *c = gsurf_wmux_bindings_copy(b);

			gsurf_wmux_bindings_free(b);
			b = c;
			break;
		}
		}

		n = gsurf_wmux_bindings_get_n(b);
		if (gsurf_wmux_bindings_get_nth(b, n) != NULL)
			g_error("seed %u op %u: get_nth past the end", seed, i);
		for (j = 0; j < n; j++) {
			const GsurfWmuxBinding *e = gsurf_wmux_bindings_get_nth(b, j);
			const GsurfWmuxCommand *parsed;
			g_autoptr(GsurfWmuxCommand) fresh = NULL;

			if (e == NULL || e->command == NULL || gsurf_wmux_bindings_lookup(b, e->key) != e)
				g_error("seed %u op %u: binding %u is inconsistent", seed, i, j);
			parsed = gsurf_wmux_bindings_get_command(b, e->key);
			fresh = gsurf_wmux_command_parse(e->command, NULL, NULL);
			if ((parsed == NULL) != (fresh == NULL) ||
			    (parsed != NULL && !gsurf_wmux_command_equal(parsed, fresh)))
				g_error("seed %u op %u: cached command of %s is stale", seed, i, e->key);
		}
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/wmux-bindings/defaults", test_default_tables);
	g_test_add_func("/gsurf/wmux-bindings/default-events", test_default_events);
	g_test_add_func("/gsurf/wmux-bindings/doc-tables", test_doc_tables);
	g_test_add_func("/gsurf/wmux-bindings/set-unbind-repeatable", test_set_unbind_repeatable);
	g_test_add_func("/gsurf/wmux-bindings/merge", test_merge);
	g_test_add_func("/gsurf/wmux-bindings/validate", test_validate_and_commands);
	g_test_add_func("/gsurf/wmux-bindings/copy", test_copy);
	g_test_add_func("/gsurf/wmux-bindings/stress", test_stress);
	return g_test_run();
}
