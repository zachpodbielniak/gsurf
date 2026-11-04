/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-wmux-command.c - the wmux command language
 *
 * Compiles modules/wmux/gsurf-wmux-command.c straight into the test and
 * checks docs/wmux.org, "Command reference": the tokenizer (quotes,
 * escapes, joins, errors with positions), every command with every flag
 * and its parsed fields, tmux aliases, the canonical printer and round
 * trips, every validation error with its code and position, nesting
 * limits, that every example in the doc's command reference parses (and
 * every command has a heading there), and seeded fuzz runs over random,
 * mutated and truncated lines (always a command or a GError, never a
 * crash).
 *
 * The seed defaults to a constant; set GSURF_WMUX_SEED to replay
 * another. Failures print it.
 */
#include <glib.h>
#include <string.h>

#include "modules/wmux/gsurf-wmux-command.c"

static guint32
test_seed(void)
{
	const gchar *env = g_getenv("GSURF_WMUX_SEED");
	guint32 seed = env != NULL ? (guint32)g_ascii_strtoull(env, NULL, 10) : 20261104u;

	g_test_message("GSURF_WMUX_SEED=%u", seed);
	return seed;
}

/* Parse @in, which must succeed and print as @canonical; the printed
 * form must parse back to an equal command. */
static GsurfWmuxCommand *
assert_parse(const gchar *in, const gchar *canonical)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfWmuxCommand) again = NULL;
	g_autofree gchar *printed = NULL;
	g_autofree gchar *reprinted = NULL;
	GsurfWmuxCommand *cmd;
	gint pos = 99;

	cmd = gsurf_wmux_command_parse(in, &pos, &error);
	if (cmd == NULL)
		g_error("parse(\"%s\") failed: %s", in, error->message);
	g_assert_cmpint(pos, ==, -1);
	printed = gsurf_wmux_command_to_string(cmd);
	if (strcmp(printed, canonical) != 0)
		g_error("parse(\"%s\") prints \"%s\", expected \"%s\"", in, printed, canonical);
	again = gsurf_wmux_command_parse(printed, NULL, &error);
	if (again == NULL)
		g_error("reparse(\"%s\") failed: %s", printed, error->message);
	reprinted = gsurf_wmux_command_to_string(again);
	g_assert_cmpstr(reprinted, ==, printed);
	g_assert_true(gsurf_wmux_command_equal(cmd, again));
	return cmd;
}

/* Parse @in, which must fail with @code at @pos. */
static void
assert_fail(const gchar *in, GsurfWmuxCommandError code, gint pos)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *prefix = NULL;
	GsurfWmuxCommand *cmd;
	gint got = 99;

	cmd = gsurf_wmux_command_parse(in, &got, &error);
	if (cmd != NULL) {
		g_autofree gchar *s = gsurf_wmux_command_to_string(cmd);

		g_error("parse(\"%s\") should fail but gave \"%s\"", in, s);
	}
	if (!g_error_matches(error, GSURF_WMUX_COMMAND_ERROR, (gint)code))
		g_error("parse(\"%s\"): expected code %d, got %d (%s)", in, code,
			error != NULL ? error->code : -1, error != NULL ? error->message : "no error");
	if (got != pos)
		g_error("parse(\"%s\"): expected position %d, got %d (%s)", in, pos, got,
			error->message);
	prefix = g_strdup_printf("position %d: ", pos);
	g_assert_true(g_str_has_prefix(error->message, prefix));
}

/* --- Tokenizer --- */

static void
assert_tokens(const gchar *in, const gchar *const *words, const guint *starts)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) offsets = NULL;
	g_auto(GStrv) got = gsurf_wmux_command_tokenize(in, &offsets, &error);
	guint i;

	g_assert_no_error(error);
	g_assert_nonnull(got);
	for (i = 0; words[i] != NULL; i++) {
		g_assert_nonnull(got[i]);
		g_assert_cmpstr(got[i], ==, words[i]);
		g_assert_cmpuint(g_array_index(offsets, guint, i), ==, starts[i]);
	}
	g_assert_null(got[i]);
	g_assert_cmpuint(offsets->len, ==, i);
}

static void
test_tokenize(void)
{
	static const guint s_abc[] = { 0, 2, 5 };
	static const guint s0[] = { 0 };
	static const guint s1[] = { 1 };
	static const guint s_two[] = { 0, 6 };

	assert_tokens("a b  c", (const gchar *const[]){ "a", "b", "c", NULL }, s_abc);
	assert_tokens("a\tb\n c", (const gchar *const[]){ "a", "b", "c", NULL }, s_abc);
	assert_tokens("'a b'", (const gchar *const[]){ "a b", NULL }, s0);
	assert_tokens("'a\\b\"c'", (const gchar *const[]){ "a\\b\"c", NULL }, s0);
	assert_tokens("\"a\\\"b\"", (const gchar *const[]){ "a\"b", NULL }, s0);
	assert_tokens("\"a\\\\b\"", (const gchar *const[]){ "a\\b", NULL }, s0);
	assert_tokens("\"a\\nb\\tc\"", (const gchar *const[]){ "a\nb\tc", NULL }, s0);
	assert_tokens("\"a\\xb\"", (const gchar *const[]){ "a\\xb", NULL }, s0);
	assert_tokens("a\\ b", (const gchar *const[]){ "a b", NULL }, s0);
	assert_tokens("\\\"x", (const gchar *const[]){ "\"x", NULL }, s0);
	assert_tokens("\\é", (const gchar *const[]){ "é", NULL }, s0);
	assert_tokens("a\"b c\"'d'e", (const gchar *const[]){ "ab cde", NULL }, s0);
	assert_tokens("\"\"", (const gchar *const[]){ "", NULL }, s0);
	assert_tokens(" ''", (const gchar *const[]){ "", NULL }, s1);
	assert_tokens("#W 'é ü'", (const gchar *const[]){ "#W", "é ü", NULL }, (const guint[]){ 0, 3 });
	assert_tokens("first second", (const gchar *const[]){ "first", "second", NULL }, s_two);
	assert_tokens("", (const gchar *const[]){ NULL }, NULL);
	assert_tokens(" \t\r\n ", (const gchar *const[]){ NULL }, NULL);
	{
		/* starts is optional */
		g_auto(GStrv) w = gsurf_wmux_command_tokenize("x y", NULL, NULL);

		g_assert_cmpuint(g_strv_length(w), ==, 2);
	}

	/* Errors carry the position of the problem. */
	{
		static const struct {
			const gchar *in;
			gint         pos;
		} bad[] = {
			{ "'abc", 0 },          { "a 'bc", 2 },       { "a \"bc", 2 },
			{ "abc\\", 3 },         { "\x01", 0 },        { "a\x7f", 1 },
			{ "\"a\x01\"", 2 },     { "'a\x02'", 2 },     { "\\\x03", 1 },
			{ "ab\xff", 2 },        { "\"\\", 0 },
		};
		guint i;

		for (i = 0; i < G_N_ELEMENTS(bad); i++) {
			g_autoptr(GError) error = NULL;
			g_autoptr(GArray) offsets = NULL;
			g_auto(GStrv) got = gsurf_wmux_command_tokenize(bad[i].in, &offsets, &error);
			g_autofree gchar *prefix = g_strdup_printf("position %d: ", bad[i].pos);

			g_assert_null(got);
			g_assert_null(offsets);
			g_assert_error(error, GSURF_WMUX_COMMAND_ERROR, GSURF_WMUX_COMMAND_ERROR_SYNTAX);
			if (!g_str_has_prefix(error->message, prefix))
				g_error("tokenize(%u): \"%s\" lacks \"%s\"", i, error->message, prefix);
		}
	}
}

static void
test_quote(void)
{
	static const struct {
		const gchar *in;
		const gchar *out;
	} cases[] = {
		{ "plain", "plain" },        { "", "\"\"" },
		{ "a b", "\"a b\"" },        { "a\"b", "\"a\\\"b\"" },
		{ "a\\b", "\"a\\\\b\"" },    { "it's", "\"it's\"" },
		{ "a\nb", "\"a\\nb\"" },     { "a\tb", "\"a\\tb\"" },
		{ "#W", "#W" },              { "-x", "-x" },
		{ "é", "é" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *q = gsurf_wmux_command_quote(cases[i].in);
		g_auto(GStrv) back = NULL;

		g_assert_cmpstr(q, ==, cases[i].out);
		back = gsurf_wmux_command_tokenize(q, NULL, NULL);
		g_assert_cmpuint(g_strv_length(back), ==, 1);
		g_assert_cmpstr(back[0], ==, cases[i].in);
	}
	{
		g_autofree gchar *q = gsurf_wmux_command_quote(NULL);

		g_assert_cmpstr(q, ==, "\"\"");
	}
}

/* --- Names --- */

static void
test_names(void)
{
	static const gchar *const aliases[][2] = {
		{ "splitw", "split-window" },   { "selectp", "select-pane" },
		{ "lastp", "last-pane" },       { "killp", "kill-pane" },
		{ "resizep", "resize-pane" },   { "swapp", "swap-pane" },
		{ "rotatew", "rotate-window" }, { "selectl", "select-layout" },
		{ "nextl", "next-layout" },     { "breakp", "break-pane" },
		{ "displayp", "display-panes" },{ "neww", "new-window" },
		{ "next", "next-window" },      { "prev", "previous-window" },
		{ "last", "last-window" },      { "selectw", "select-window" },
		{ "killw", "kill-window" },     { "renamew", "rename-window" },
		{ "confirm", "confirm-before" },{ "lsk", "list-keys" },
	};
	guint i;

	for (i = 0; i < GSURF_WMUX_CMD_COUNT; i++) {
		const gchar *name = gsurf_wmux_command_id_to_name((GsurfWmuxCommandId)i);
		GsurfWmuxCommandId id = GSURF_WMUX_CMD_COUNT;

		g_assert_nonnull(name);
		g_assert_true(gsurf_wmux_command_id_from_name(name, &id));
		g_assert_cmpint(id, ==, (gint)i);
		g_assert_true(g_str_has_prefix(gsurf_wmux_command_id_synopsis(id), name));
		g_assert_cmpint(command_specs[i].id, ==, (gint)i);
	}
	for (i = 0; i < G_N_ELEMENTS(aliases); i++) {
		GsurfWmuxCommandId id = GSURF_WMUX_CMD_COUNT;

		g_assert_true(gsurf_wmux_command_id_from_name(aliases[i][0], &id));
		g_assert_cmpstr(gsurf_wmux_command_id_to_name(id), ==, aliases[i][1]);
	}
	g_assert_false(gsurf_wmux_command_id_from_name("nope", NULL));
	g_assert_false(gsurf_wmux_command_id_from_name(NULL, NULL));
	g_assert_true(gsurf_wmux_command_id_from_name("kill-pane", NULL));
	g_assert_null(gsurf_wmux_command_id_to_name(GSURF_WMUX_CMD_COUNT));
	g_assert_null(gsurf_wmux_command_id_synopsis(GSURF_WMUX_CMD_COUNT));
	g_assert_null(gsurf_wmux_command_id_to_name((GsurfWmuxCommandId)-1));

	g_assert_cmpint(gsurf_wmux_command_flag_slot('A'), ==, 0);
	g_assert_cmpint(gsurf_wmux_command_flag_slot('Z'), ==, 25);
	g_assert_cmpint(gsurf_wmux_command_flag_slot('a'), ==, 26);
	g_assert_cmpint(gsurf_wmux_command_flag_slot('z'), ==, 51);
	g_assert_cmpint(gsurf_wmux_command_flag_slot('-'), ==, -1);
	g_assert_cmpint(gsurf_wmux_command_flag_slot('0'), ==, -1);
}

/* --- Every command --- */

static void
test_split_window(void)
{
	g_autoptr(GsurfWmuxCommand) a = assert_parse("split-window", "split-window");
	g_autoptr(GsurfWmuxCommand) b = assert_parse("split-window -h", "split-window -h");
	g_autoptr(GsurfWmuxCommand) c = assert_parse("splitw -hbdc -p 30 https://example.org/a?b=c",
		"split-window -h -b -d -c -p 30 https://example.org/a?b=c");
	g_autoptr(GsurfWmuxCommand) d = assert_parse("split-window -v -p030", "split-window -v -p 30");
	g_autoptr(GsurfWmuxCommand) e = assert_parse("split-window -- -odd", "split-window -- -odd");
	g_autoptr(GsurfWmuxCommand) f = assert_parse("split-window -c -h", "split-window -h -c");
	g_autoptr(GsurfWmuxCommand) g = assert_parse("split-window 'a b'", "split-window \"a b\"");
	g_autoptr(GsurfWmuxCommand) h = assert_parse("split-window -", "split-window -");

	g_assert_cmpint(a->id, ==, GSURF_WMUX_CMD_SPLIT_WINDOW);
	g_assert_cmpuint(a->flags, ==, 0);
	g_assert_cmpuint(a->n_args, ==, 0);
	g_assert_null(gsurf_wmux_command_get_arg(a, 0));
	g_assert_false(a->has_number);
	g_assert_true(gsurf_wmux_command_has_flag(b, 'h'));
	g_assert_false(gsurf_wmux_command_has_flag(b, 'v'));
	g_assert_false(gsurf_wmux_command_has_flag(b, '?'));
	g_assert_true(gsurf_wmux_command_has_flag(c, 'b'));
	g_assert_true(gsurf_wmux_command_has_flag(c, 'd'));
	g_assert_true(gsurf_wmux_command_has_flag(c, 'c'));
	g_assert_true(gsurf_wmux_command_has_flag(c, 'p'));
	g_assert_cmpstr(gsurf_wmux_command_get_value(c, 'p'), ==, "30");
	g_assert_null(gsurf_wmux_command_get_value(c, 'h'));
	g_assert_null(gsurf_wmux_command_get_value(c, '!'));
	g_assert_true(c->has_number);
	g_assert_cmpint(c->number, ==, 30);
	g_assert_cmpstr(gsurf_wmux_command_get_arg(c, 0), ==, "https://example.org/a?b=c");
	g_assert_cmpint(d->number, ==, 30);
	g_assert_true(gsurf_wmux_command_has_flag(d, 'v'));
	g_assert_cmpstr(e->args[0], ==, "-odd");
	g_assert_cmpstr(g->args[0], ==, "a b");
	g_assert_cmpstr(h->args[0], ==, "-");
	g_assert_true(gsurf_wmux_command_equal(b, b));
	g_assert_false(gsurf_wmux_command_equal(a, b));
	g_assert_true(gsurf_wmux_command_equal(NULL, NULL));
	g_assert_false(gsurf_wmux_command_equal(a, NULL));
	g_assert_true(gsurf_wmux_command_has_flag(f, 'c'));

	assert_fail("split-window -x", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG, 14);
	assert_fail("split-window -hx", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG, 15);
	assert_fail("split-window -p", GSURF_WMUX_COMMAND_ERROR_MISSING_VALUE, 14);
	assert_fail("split-window -h -v", GSURF_WMUX_COMMAND_ERROR_CONFLICT, 0);
	assert_fail("split-window -p 0", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 0);
	assert_fail("split-window -p 100", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 0);
	assert_fail("split-window -p abc", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 0);
	assert_fail("split-window -p -5", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 0);
	assert_fail("split-window -p 9999999999", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 0);
	assert_fail("split-window a b", GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS, 15);
	assert_fail("split-window -h\x01", GSURF_WMUX_COMMAND_ERROR_SYNTAX, 15);
	assert_fail("split-window -h 'x", GSURF_WMUX_COMMAND_ERROR_SYNTAX, 16);
}

static void
test_select_pane(void)
{
	static const gchar *const dirs[] = { "-L", "-R", "-U", "-D" };
	g_autoptr(GsurfWmuxCommand) next = assert_parse("select-pane -t +1", "select-pane -t +1");
	g_autoptr(GsurfWmuxCommand) prev = assert_parse("selectp -t -1", "select-pane -t -1");
	g_autoptr(GsurfWmuxCommand) abs = assert_parse("select-pane -t3", "select-pane -t 3");
	g_autoptr(GsurfWmuxCommand) pad = assert_parse("select-pane -t +02", "select-pane -t +2");
	g_autoptr(GsurfWmuxCommand) last = assert_parse("select-pane -l", "select-pane -l");
	guint i;

	for (i = 0; i < G_N_ELEMENTS(dirs); i++) {
		g_autofree gchar *in = g_strdup_printf("select-pane %s", dirs[i]);
		g_autoptr(GsurfWmuxCommand) cmd = assert_parse(in, in);

		g_assert_cmpint(gsurf_wmux_command_direction(cmd), ==, dirs[i][1]);
	}
	g_assert_true(next->relative);
	g_assert_cmpint(next->number, ==, 1);
	g_assert_true(prev->relative);
	g_assert_cmpint(prev->number, ==, -1);
	g_assert_false(abs->relative);
	g_assert_cmpint(abs->number, ==, 3);
	g_assert_cmpint(pad->number, ==, 2);
	g_assert_cmpint(gsurf_wmux_command_direction(next), ==, 0);
	g_assert_true(gsurf_wmux_command_has_flag(last, 'l'));

	assert_fail("select-pane", GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT, 0);
	assert_fail("select-pane -L -R", GSURF_WMUX_COMMAND_ERROR_CONFLICT, 0);
	assert_fail("select-pane -l -t 1", GSURF_WMUX_COMMAND_ERROR_CONFLICT, 0);
	assert_fail("select-pane -t x", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 0);
	assert_fail("select-pane -t +", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 0);
	assert_fail("select-pane -t", GSURF_WMUX_COMMAND_ERROR_MISSING_VALUE, 13);
	assert_fail("select-pane -t 1 extra", GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS, 17);
	assert_fail("select-pane -X", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG, 13);
}

/* Commands without flags or arguments. */
static void
test_plain_commands(void)
{
	static const gchar *const names[] = {
		"last-pane", "kill-pane", "next-layout", "break-pane", "display-panes",
		"next-window", "previous-window", "last-window", "kill-window",
		"choose-tree", "send-prefix", "list-keys",
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		g_autoptr(GsurfWmuxCommand) cmd = assert_parse(names[i], names[i]);
		g_autofree gchar *with_arg = g_strdup_printf("%s extra", names[i]);
		g_autofree gchar *with_flag = g_strdup_printf("%s -a", names[i]);
		gint len = (gint)strlen(names[i]);

		g_assert_cmpstr(gsurf_wmux_command_id_to_name(cmd->id), ==, names[i]);
		g_assert_cmpuint(cmd->n_args, ==, 0);
		assert_fail(with_arg, GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS, len + 1);
		assert_fail(with_flag, GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG, len + 2);
	}
	/* Surrounding whitespace is fine. */
	{
		g_autoptr(GsurfWmuxCommand) cmd = assert_parse("  \tkill-pane \n", "kill-pane");
	}
}

static void
test_resize_swap_rotate(void)
{
	g_autoptr(GsurfWmuxCommand) l = assert_parse("resize-pane -L", "resize-pane -L");
	g_autoptr(GsurfWmuxCommand) u5 = assert_parse("resizep -U 05", "resize-pane -U 5");
	g_autoptr(GsurfWmuxCommand) z = assert_parse("resize-pane -Z", "resize-pane -Z");
	g_autoptr(GsurfWmuxCommand) r = assert_parse("resize-pane -R 100", "resize-pane -R 100");
	g_autoptr(GsurfWmuxCommand) d = assert_parse("resize-pane -D 1", "resize-pane -D 1");
	g_autoptr(GsurfWmuxCommand) su = assert_parse("swap-pane -U", "swap-pane -U");
	g_autoptr(GsurfWmuxCommand) sd = assert_parse("swapp -D", "swap-pane -D");
	g_autoptr(GsurfWmuxCommand) rw = assert_parse("rotate-window", "rotate-window");
	g_autoptr(GsurfWmuxCommand) rwd = assert_parse("rotatew -D", "rotate-window -D");
	g_autoptr(GsurfWmuxCommand) rwu = assert_parse("rotate-window -U", "rotate-window -U");

	g_assert_cmpint(gsurf_wmux_command_direction(l), ==, 'L');
	g_assert_cmpint(l->number, ==, 1);
	g_assert_false(l->has_number);
	g_assert_cmpint(u5->number, ==, 5);
	g_assert_true(u5->has_number);
	g_assert_cmpint(gsurf_wmux_command_direction(u5), ==, 'U');
	g_assert_true(gsurf_wmux_command_has_flag(z, 'Z'));
	g_assert_cmpint(gsurf_wmux_command_direction(z), ==, 0);
	g_assert_cmpint(r->number, ==, 100);
	g_assert_cmpint(d->number, ==, 1);
	g_assert_cmpint(gsurf_wmux_command_direction(su), ==, 'U');
	g_assert_cmpint(gsurf_wmux_command_direction(sd), ==, 'D');
	g_assert_cmpuint(rw->flags, ==, 0);
	g_assert_true(gsurf_wmux_command_has_flag(rwd, 'D'));
	g_assert_true(gsurf_wmux_command_has_flag(rwu, 'U'));

	assert_fail("resize-pane", GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT, 0);
	assert_fail("resize-pane -L -Z", GSURF_WMUX_COMMAND_ERROR_CONFLICT, 0);
	assert_fail("resize-pane -L -R", GSURF_WMUX_COMMAND_ERROR_CONFLICT, 0);
	assert_fail("resize-pane -Z 3", GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS, 15);
	assert_fail("resize-pane -L 0", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 15);
	assert_fail("resize-pane -L 101", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 15);
	assert_fail("resize-pane -L x", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 15);
	assert_fail("resize-pane -L +1", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 15);
	assert_fail("resize-pane -L 1 2", GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS, 17);
	assert_fail("swap-pane", GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT, 0);
	assert_fail("swap-pane -U -D", GSURF_WMUX_COMMAND_ERROR_CONFLICT, 0);
	assert_fail("swap-pane -L", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG, 11);
	assert_fail("rotate-window -UD", GSURF_WMUX_COMMAND_ERROR_CONFLICT, 0);
	assert_fail("rotate-window x", GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS, 14);
}

static void
test_layouts_and_windows(void)
{
	static const gchar *const presets[] = {
		"even-horizontal", "even-vertical", "main-horizontal", "main-vertical", "tiled",
	};
	g_autoptr(GsurfWmuxCommand) e = assert_parse("select-layout -E", "select-layout -E");
	g_autoptr(GsurfWmuxCommand) nw = assert_parse("new-window", "new-window");
	g_autoptr(GsurfWmuxCommand) nwn = assert_parse("neww -n 'my docs' https://example.org",
		"new-window -n \"my docs\" https://example.org");
	g_autoptr(GsurfWmuxCommand) sw = assert_parse("select-window -t 0", "select-window -t 0");
	g_autoptr(GsurfWmuxCommand) swr = assert_parse("selectw -t-2", "select-window -t -2");
	g_autoptr(GsurfWmuxCommand) rn = assert_parse("rename-window", "rename-window");
	g_autoptr(GsurfWmuxCommand) rn2 = assert_parse("renamew \"a \\\"b\\\"\"",
		"rename-window \"a \\\"b\\\"\"");
	g_autoptr(GsurfWmuxCommand) rn3 = assert_parse("rename-window ''", "rename-window \"\"");
	guint i;

	for (i = 0; i < G_N_ELEMENTS(presets); i++) {
		g_autofree gchar *in = g_strdup_printf("select-layout %s", presets[i]);
		g_autoptr(GsurfWmuxCommand) cmd = assert_parse(in, in);

		g_assert_cmpstr(cmd->args[0], ==, presets[i]);
	}
	g_assert_true(gsurf_wmux_command_has_flag(e, 'E'));
	g_assert_cmpuint(nw->n_args, ==, 0);
	g_assert_cmpstr(gsurf_wmux_command_get_value(nwn, 'n'), ==, "my docs");
	g_assert_cmpstr(nwn->args[0], ==, "https://example.org");
	g_assert_cmpint(sw->number, ==, 0);
	g_assert_false(sw->relative);
	g_assert_cmpint(swr->number, ==, -2);
	g_assert_true(swr->relative);
	g_assert_cmpuint(rn->n_args, ==, 0);
	g_assert_cmpstr(rn2->args[0], ==, "a \"b\"");
	g_assert_cmpstr(rn3->args[0], ==, "");

	assert_fail("select-layout", GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT, 13);
	assert_fail("select-layout bogus", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 14);
	assert_fail("select-layout -E tiled", GSURF_WMUX_COMMAND_ERROR_CONFLICT, 17);
	assert_fail("select-layout tiled tiled", GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS, 20);
	assert_fail("select-window", GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT, 0);
	assert_fail("select-window -t a", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 0);
	assert_fail("new-window a b", GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS, 13);
	assert_fail("new-window -n", GSURF_WMUX_COMMAND_ERROR_MISSING_VALUE, 12);
	assert_fail("rename-window a b", GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS, 16);
}

static void
test_prompt_and_confirm(void)
{
	g_autoptr(GsurfWmuxCommand) cp = assert_parse("command-prompt", "command-prompt");
	g_autoptr(GsurfWmuxCommand) rename = assert_parse("command-prompt -I #W rename-window",
		"command-prompt -I #W rename-window");
	g_autoptr(GsurfWmuxCommand) full = assert_parse(
		"command-prompt -p 'New name:' -I '#W' 'rename-window %%'",
		"command-prompt -p \"New name:\" -I #W \"rename-window %%\"");
	g_autoptr(GsurfWmuxCommand) joined = assert_parse("command-prompt split-window -h %1",
		"command-prompt \"split-window -h %1\"");
	g_autoptr(GsurfWmuxCommand) open = assert_parse("command-prompt -p URL: new-window",
		"command-prompt -p URL: new-window");
	g_autoptr(GsurfWmuxCommand) kp = assert_parse("confirm-before kill-pane",
		"confirm-before kill-pane");
	g_autoptr(GsurfWmuxCommand) kpp = assert_parse("confirm -p 'Kill pane?' kill-pane",
		"confirm-before -p \"Kill pane?\" kill-pane");
	g_autoptr(GsurfWmuxCommand) quoted = assert_parse("confirm-before 'split-window -h -p 30'",
		"confirm-before split-window -h -p 30");
	g_autoptr(GsurfWmuxCommand) inline_cmd = assert_parse("confirm-before split-window -h",
		"confirm-before split-window -h");
	g_autoptr(GsurfWmuxCommand) nested = assert_parse(
		"confirm-before confirm-before confirm-before kill-pane",
		"confirm-before confirm-before confirm-before kill-pane");

	g_assert_null(cp->template_str);
	g_assert_null(cp->nested);
	g_assert_cmpstr(gsurf_wmux_command_get_value(rename, 'I'), ==, "#W");
	g_assert_cmpstr(rename->template_str, ==, "rename-window");
	g_assert_cmpuint(rename->n_args, ==, 0);
	g_assert_null(rename->nested);
	g_assert_cmpstr(gsurf_wmux_command_get_value(full, 'p'), ==, "New name:");
	g_assert_cmpstr(full->template_str, ==, "rename-window %%");
	g_assert_cmpstr(joined->template_str, ==, "split-window -h %1");
	g_assert_cmpstr(open->template_str, ==, "new-window");
	g_assert_nonnull(kp->nested);
	g_assert_cmpint(kp->nested->id, ==, GSURF_WMUX_CMD_KILL_PANE);
	g_assert_cmpuint(kp->n_args, ==, 0);
	g_assert_cmpstr(gsurf_wmux_command_get_value(kpp, 'p'), ==, "Kill pane?");
	g_assert_cmpint(quoted->nested->id, ==, GSURF_WMUX_CMD_SPLIT_WINDOW);
	g_assert_cmpint(quoted->nested->number, ==, 30);
	g_assert_true(gsurf_wmux_command_has_flag(inline_cmd->nested, 'h'));
	g_assert_cmpint(nested->nested->nested->nested->id, ==, GSURF_WMUX_CMD_KILL_PANE);

	/* Errors, with positions inside the nested command. */
	assert_fail("confirm-before", GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT, 14);
	assert_fail("confirm-before -p", GSURF_WMUX_COMMAND_ERROR_MISSING_VALUE, 16);
	assert_fail("confirm-before -p x", GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT, 19);
	assert_fail("confirm-before bogus", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_COMMAND, 15);
	assert_fail("confirm-before 'bogus'", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_COMMAND, 16);
	assert_fail("confirm-before 'split-window -x'", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG, 30);
	assert_fail("confirm-before split-window -x", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG, 29);
	assert_fail("confirm-before ''", GSURF_WMUX_COMMAND_ERROR_EMPTY, 16);
	assert_fail("confirm-before 'kill-pane \"x'", GSURF_WMUX_COMMAND_ERROR_SYNTAX, 26);
	assert_fail("confirm-before confirm-before confirm-before confirm-before kill-pane",
		GSURF_WMUX_COMMAND_ERROR_TOO_DEEP, 60);
	assert_fail("command-prompt bogus", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 15);
	assert_fail("command-prompt kill-pane", GSURF_WMUX_COMMAND_ERROR_BAD_VALUE, 15);
	assert_fail("command-prompt -I", GSURF_WMUX_COMMAND_ERROR_MISSING_VALUE, 16);
	assert_fail("command-prompt -x", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG, 16);
	assert_fail("command-prompt 'confirm-before confirm-before confirm-before'",
		GSURF_WMUX_COMMAND_ERROR_TOO_DEEP, 15);
}

static void
test_general_errors(void)
{
	assert_fail("", GSURF_WMUX_COMMAND_ERROR_EMPTY, 0);
	assert_fail("   ", GSURF_WMUX_COMMAND_ERROR_EMPTY, 3);
	assert_fail("bogus", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_COMMAND, 0);
	assert_fail("  bogus -h", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_COMMAND, 2);
	assert_fail("Split-Window", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_COMMAND, 0);
	assert_fail("-h", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_COMMAND, 0);
	assert_fail("'kill-pane", GSURF_WMUX_COMMAND_ERROR_SYNTAX, 0);
	assert_fail("kill-pane \xc3", GSURF_WMUX_COMMAND_ERROR_SYNTAX, 10);
	assert_fail("split-window -\xc3\xa9", GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG, 14);
	{
		g_autoptr(GError) error = NULL;
		gint pos = 99;

		g_assert_null(gsurf_wmux_command_parse(NULL, &pos, &error));
		g_assert_error(error, GSURF_WMUX_COMMAND_ERROR, GSURF_WMUX_COMMAND_ERROR_EMPTY);
		g_assert_cmpint(pos, ==, 0);
	}
	/* Optional out-parameters. */
	g_assert_null(gsurf_wmux_command_parse("bogus", NULL, NULL));
	gsurf_wmux_command_free(NULL);
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

/* Every line of every example block in the "Command reference" section
 * parses, and every command has its own heading there. */
static void
test_doc_examples(void)
{
	g_autofree gchar *doc = read_doc();
	g_auto(GStrv) lines = g_strsplit(doc, "\n", -1);
	g_autoptr(GString) headings = g_string_new(NULL);
	gboolean in_ref = FALSE;
	gboolean in_example = FALSE;
	guint n = 0;
	guint i;

	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];

		if (g_str_has_prefix(line, "* ")) {
			in_ref = strcmp(line, "* Command reference") == 0;
			continue;
		}
		if (!in_ref)
			continue;
		if (g_str_has_prefix(line, "** "))
			g_string_append_printf(headings, "%s\n", line);
		if (strcmp(line, "#+begin_example") == 0) {
			in_example = TRUE;
		} else if (strcmp(line, "#+end_example") == 0) {
			in_example = FALSE;
		} else if (in_example && *line != '\0') {
			g_autoptr(GError) error = NULL;
			g_autoptr(GsurfWmuxCommand) cmd = gsurf_wmux_command_parse(line, NULL, &error);

			if (cmd == NULL)
				g_error("docs/wmux.org example \"%s\": %s", line, error->message);
			n++;
		}
	}
	g_assert_false(in_example);
	g_assert_cmpuint(n, >=, 50);
	for (i = 0; i < GSURF_WMUX_CMD_COUNT; i++) {
		g_autofree gchar *needle = g_strdup_printf(" %s", gsurf_wmux_command_id_to_name(i));

		if (strstr(headings->str, needle) == NULL)
			g_error("docs/wmux.org has no heading for %s", needle + 1);
	}
}

/* --- Fuzz --- */

/* Every outcome must be a command (that round-trips) or a command error
 * with a position inside the line. */
static void
check_any(const gchar *line, guint32 seed)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfWmuxCommand) cmd = NULL;
	gint pos = 99;

	cmd = gsurf_wmux_command_parse(line, &pos, &error);
	if (cmd != NULL) {
		g_autofree gchar *printed = gsurf_wmux_command_to_string(cmd);
		g_autoptr(GsurfWmuxCommand) again = gsurf_wmux_command_parse(printed, NULL, NULL);
		g_autofree gchar *reprinted = NULL;

		if (again == NULL)
			g_error("seed %u: \"%s\" printed \"%s\", which does not parse", seed, line,
				printed);
		reprinted = gsurf_wmux_command_to_string(again);
		if (strcmp(printed, reprinted) != 0)
			g_error("seed %u: \"%s\" does not round-trip (\"%s\" vs \"%s\")", seed, line,
				printed, reprinted);
		if (pos != -1)
			g_error("seed %u: success with position %d", seed, pos);
		return;
	}
	if (error == NULL || error->domain != GSURF_WMUX_COMMAND_ERROR ||
	    error->code < 0 || error->code > GSURF_WMUX_COMMAND_ERROR_TOO_DEEP)
		g_error("seed %u: \"%s\" failed without a command error", seed, line);
	if (pos < 0 || pos > (gint)strlen(line))
		g_error("seed %u: \"%s\": position %d outside the line (%s)", seed, line, pos,
			error->message);
}

static const gchar *const samples[] = {
	"split-window -h -c -p 30 https://example.org",
	"select-pane -t +1",
	"resize-pane -L 5",
	"confirm-before -p 'Kill?' kill-pane",
	"confirm-before 'split-window -v -b'",
	"command-prompt -p 'Name:' -I '#W' 'rename-window %%'",
	"new-window -n \"a \\\"b\\\"\" https://x",
	"select-layout main-vertical",
	"select-window -t 3",
	"rotate-window -D",
};

static void
test_fuzz_random(void)
{
	static const gchar *const atoms[] = {
		"split-window", "select-pane", "resize-pane", "confirm-before", "command-prompt",
		"select-layout", "new-window", "kill-pane", "rename-window", "select-window",
		"splitw", "-", "--", "-h", "-v", "-p", "-t", "-L", "-Z", "-E", "-I", "-x", "+1",
		"-1", "30", "0", "100", "tiled", "bogus", "'", "\"", "\\", " ", " ", " ", "\t",
		"%%", "#W", "é", "\x01", "\xff", "''", "\"\"", "\\n",
	};
	guint32 seed = test_seed();
	g_autoptr(GRand) rand = g_rand_new_with_seed(seed);
	guint i;

	for (i = 0; i < 30000; i++) {
		g_autoptr(GString) s = g_string_new(NULL);
		guint n = (guint)g_rand_int_range(rand, 0, 8);
		guint j;

		for (j = 0; j < n; j++)
			g_string_append(s, atoms[g_rand_int_range(rand, 0, G_N_ELEMENTS(atoms))]);
		check_any(s->str, seed);
	}
}

/* Valid lines, every prefix of them, and random byte mutations. */
static void
test_fuzz_truncate_mutate(void)
{
	guint32 seed = test_seed();
	g_autoptr(GRand) rand = g_rand_new_with_seed(seed ^ 0x5a5a5a5au);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(samples); i++) {
		gsize len = strlen(samples[i]);
		gsize cut;
		guint m;

		for (cut = 0; cut <= len; cut++) {
			g_autofree gchar *prefix = g_strndup(samples[i], cut);

			check_any(prefix, seed);
		}
		for (m = 0; m < 2000; m++) {
			g_autofree gchar *line = g_strdup(samples[i]);
			guint edits = (guint)g_rand_int_range(rand, 1, 4);
			guint e;

			for (e = 0; e < edits; e++) {
				gsize at = (gsize)g_rand_int_range(rand, 0, (gint32)strlen(line) + 1);
				gchar c = (gchar)g_rand_int_range(rand, 1, 256);
				gchar *next;

				switch (g_rand_int_range(rand, 0, 3)) {
				case 0:   /* replace */
					if (line[at] != '\0')
						line[at] = c;
					break;
				case 1:   /* delete */
					if (line[at] != '\0')
						memmove(line + at, line + at + 1, strlen(line + at));
					break;
				default:  /* insert */
					next = g_strdup_printf("%.*s%c%s", (gint)at, line, c, line + at);
					g_free(line);
					line = next;
					break;
				}
			}
			check_any(line, seed);
		}
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/wmux-command/tokenize", test_tokenize);
	g_test_add_func("/gsurf/wmux-command/quote", test_quote);
	g_test_add_func("/gsurf/wmux-command/names", test_names);
	g_test_add_func("/gsurf/wmux-command/split-window", test_split_window);
	g_test_add_func("/gsurf/wmux-command/select-pane", test_select_pane);
	g_test_add_func("/gsurf/wmux-command/plain", test_plain_commands);
	g_test_add_func("/gsurf/wmux-command/resize-swap-rotate", test_resize_swap_rotate);
	g_test_add_func("/gsurf/wmux-command/layouts-windows", test_layouts_and_windows);
	g_test_add_func("/gsurf/wmux-command/prompt-confirm", test_prompt_and_confirm);
	g_test_add_func("/gsurf/wmux-command/errors", test_general_errors);
	g_test_add_func("/gsurf/wmux-command/doc-examples", test_doc_examples);
	g_test_add_func("/gsurf/wmux-command/fuzz-random", test_fuzz_random);
	g_test_add_func("/gsurf/wmux-command/fuzz-truncate-mutate", test_fuzz_truncate_mutate);
	return g_test_run();
}
