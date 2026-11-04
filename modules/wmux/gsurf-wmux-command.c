/*
 * gsurf-wmux-command.c - The wmux command language (a tmux subset)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three stages:
 *   1. tokenize: a line becomes words, each with the byte offset where it
 *      starts (for error positions);
 *   2. parse_words: the first word picks a CommandSpec; the following
 *      words are read getopt-style against the spec's flag letters;
 *   3. validate: per-command rules (exactly one direction, numbers in
 *      range, layout names, nested commands) and normalization of numbers
 *      so the printer is canonical.
 */

#include "gsurf-wmux-command.h"

#include <string.h>

G_DEFINE_QUARK(gsurf-wmux-command-error-quark, gsurf_wmux_command_error)

/* ------------------------------------------------------------------ */
/* Command table                                                       */
/* ------------------------------------------------------------------ */

/* How many positional arguments a command takes beyond its flags. REST
 * means "all remaining words form a nested command or template". */
#define ARGS_REST G_MAXUINT

typedef struct {
	GsurfWmuxCommandId id;
	const gchar       *name;
	const gchar       *aliases[3];
	const gchar       *bool_flags;   /* letters, in synopsis (print) order */
	const gchar       *value_flags;  /* letters that take a value */
	guint              max_args;
	const gchar       *synopsis;
} CommandSpec;

static const CommandSpec command_specs[GSURF_WMUX_CMD_COUNT] = {
	{ GSURF_WMUX_CMD_SPLIT_WINDOW, "split-window", { "splitw", NULL }, "hvbdc", "p", 1,
	  "split-window [-h|-v] [-b] [-d] [-c] [-p pct] [uri]" },
	{ GSURF_WMUX_CMD_SELECT_PANE, "select-pane", { "selectp", NULL }, "LRUDl", "t", 0,
	  "select-pane -L|-R|-U|-D|-l|-t N|+N|-N" },
	{ GSURF_WMUX_CMD_LAST_PANE, "last-pane", { "lastp", NULL }, "", "", 0,
	  "last-pane" },
	{ GSURF_WMUX_CMD_KILL_PANE, "kill-pane", { "killp", NULL }, "", "", 0,
	  "kill-pane" },
	{ GSURF_WMUX_CMD_RESIZE_PANE, "resize-pane", { "resizep", NULL }, "LRUDZ", "", 1,
	  "resize-pane -L|-R|-U|-D [n] | -Z" },
	{ GSURF_WMUX_CMD_SWAP_PANE, "swap-pane", { "swapp", NULL }, "UD", "", 0,
	  "swap-pane -U|-D" },
	{ GSURF_WMUX_CMD_ROTATE_WINDOW, "rotate-window", { "rotatew", NULL }, "UD", "", 0,
	  "rotate-window [-U|-D]" },
	{ GSURF_WMUX_CMD_SELECT_LAYOUT, "select-layout", { "selectl", NULL }, "E", "", 1,
	  "select-layout name | -E" },
	{ GSURF_WMUX_CMD_NEXT_LAYOUT, "next-layout", { "nextl", NULL }, "", "", 0,
	  "next-layout" },
	{ GSURF_WMUX_CMD_BREAK_PANE, "break-pane", { "breakp", NULL }, "", "", 0,
	  "break-pane" },
	{ GSURF_WMUX_CMD_DISPLAY_PANES, "display-panes", { "displayp", NULL }, "", "", 0,
	  "display-panes" },
	{ GSURF_WMUX_CMD_NEW_WINDOW, "new-window", { "neww", NULL }, "", "n", 1,
	  "new-window [-n name] [uri]" },
	{ GSURF_WMUX_CMD_NEXT_WINDOW, "next-window", { "next", NULL }, "", "", 0,
	  "next-window" },
	{ GSURF_WMUX_CMD_PREVIOUS_WINDOW, "previous-window", { "prev", NULL }, "", "", 0,
	  "previous-window" },
	{ GSURF_WMUX_CMD_LAST_WINDOW, "last-window", { "last", NULL }, "", "", 0,
	  "last-window" },
	{ GSURF_WMUX_CMD_SELECT_WINDOW, "select-window", { "selectw", NULL }, "", "t", 0,
	  "select-window -t N|+N|-N" },
	{ GSURF_WMUX_CMD_KILL_WINDOW, "kill-window", { "killw", NULL }, "", "", 0,
	  "kill-window" },
	{ GSURF_WMUX_CMD_RENAME_WINDOW, "rename-window", { "renamew", NULL }, "", "", 1,
	  "rename-window [name]" },
	{ GSURF_WMUX_CMD_CHOOSE_TREE, "choose-tree", { NULL }, "", "", 0,
	  "choose-tree" },
	{ GSURF_WMUX_CMD_COMMAND_PROMPT, "command-prompt", { NULL }, "", "pI", ARGS_REST,
	  "command-prompt [-p prompt] [-I initial] [template]" },
	{ GSURF_WMUX_CMD_CONFIRM_BEFORE, "confirm-before", { "confirm", NULL }, "", "p", ARGS_REST,
	  "confirm-before [-p prompt] command" },
	{ GSURF_WMUX_CMD_SEND_PREFIX, "send-prefix", { NULL }, "", "", 0,
	  "send-prefix" },
	{ GSURF_WMUX_CMD_LIST_KEYS, "list-keys", { "lsk", NULL }, "", "", 0,
	  "list-keys" },
};

/* The layout names select-layout accepts (the presets of
 * gsurf-wmux-layout.c, in next-layout order). */
static const gchar *const layout_names[] = {
	"even-horizontal", "even-vertical", "main-horizontal", "main-vertical", "tiled",
};

const gchar *
gsurf_wmux_command_id_to_name(GsurfWmuxCommandId id)
{
	if ((guint)id >= GSURF_WMUX_CMD_COUNT)
		return NULL;
	return command_specs[id].name;
}

const gchar *
gsurf_wmux_command_id_synopsis(GsurfWmuxCommandId id)
{
	if ((guint)id >= GSURF_WMUX_CMD_COUNT)
		return NULL;
	return command_specs[id].synopsis;
}

gboolean
gsurf_wmux_command_id_from_name(const gchar *name, GsurfWmuxCommandId *id)
{
	guint i;
	guint j;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < GSURF_WMUX_CMD_COUNT; i++) {
		gboolean hit = strcmp(name, command_specs[i].name) == 0;

		for (j = 0; !hit && command_specs[i].aliases[j] != NULL; j++)
			hit = strcmp(name, command_specs[i].aliases[j]) == 0;
		if (hit) {
			if (id != NULL)
				*id = command_specs[i].id;
			return TRUE;
		}
	}
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

/* Set @error with a "position N: " prefix and record N in @pos. */
G_GNUC_PRINTF(5, 6)
static void
set_error_at(GError **error, gint *pos, gint at, GsurfWmuxCommandError code,
             const gchar *format, ...)
{
	g_autofree gchar *msg = NULL;
	va_list ap;

	if (pos != NULL)
		*pos = at;
	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	g_set_error(error, GSURF_WMUX_COMMAND_ERROR, (gint)code, "position %d: %s", at, msg);
}

/* ------------------------------------------------------------------ */
/* Tokenizer                                                           */
/* ------------------------------------------------------------------ */

/* Raw characters that may never appear (outside of the separators tab,
 * newline and carriage return): the other C0 controls and DEL. */
static gboolean
is_bad_control(guchar c)
{
	return (c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7f;
}

/* The tokenizer proper; @pos receives the error offset. */
static gchar **
tokenize(const gchar *text, gint base, GArray **starts_out, gint *pos, GError **error)
{
	g_autoptr(GPtrArray) words = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GArray) starts = g_array_new(FALSE, FALSE, sizeof(guint));
	g_autoptr(GString) word = g_string_new(NULL);
	gboolean in_word = FALSE;
	const gchar *p = text;

	if (!g_utf8_validate(text, -1, NULL)) {
		const gchar *end = NULL;

		g_utf8_validate(text, -1, &end);
		set_error_at(error, pos, base + (gint)(end - text), GSURF_WMUX_COMMAND_ERROR_SYNTAX,
			"invalid UTF-8");
		return NULL;
	}

	while (*p != '\0') {
		guchar c = (guchar)*p;

		/* Whitespace ends the current word. */
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			if (in_word) {
				g_ptr_array_add(words, g_strdup(word->str));
				g_string_truncate(word, 0);
				in_word = FALSE;
			}
			p++;
			continue;
		}
		if (is_bad_control(c)) {
			set_error_at(error, pos, base + (gint)(p - text), GSURF_WMUX_COMMAND_ERROR_SYNTAX,
				"control character 0x%02x", c);
			return NULL;
		}
		if (!in_word) {
			guint start = (guint)(p - text);

			g_array_append_val(starts, start);
			in_word = TRUE;
		}

		if (c == '\'') {
			/* Single quotes: literal up to the closing quote. */
			const gchar *open = p++;

			while (*p != '\0' && *p != '\'') {
				if (is_bad_control((guchar)*p)) {
					set_error_at(error, pos, base + (gint)(p - text),
						GSURF_WMUX_COMMAND_ERROR_SYNTAX,
						"control character 0x%02x", (guchar)*p);
					return NULL;
				}
				g_string_append_c(word, *p++);
			}
			if (*p == '\0') {
				set_error_at(error, pos, base + (gint)(open - text),
					GSURF_WMUX_COMMAND_ERROR_SYNTAX, "unterminated single quote");
				return NULL;
			}
			p++;
		} else if (c == '"') {
			/* Double quotes: \" \\ \n \t are escapes; any other
			 * backslash is kept as written. */
			const gchar *open = p++;

			while (*p != '\0' && *p != '"') {
				if (is_bad_control((guchar)*p)) {
					set_error_at(error, pos, base + (gint)(p - text),
						GSURF_WMUX_COMMAND_ERROR_SYNTAX,
						"control character 0x%02x", (guchar)*p);
					return NULL;
				}
				if (*p == '\\' && p[1] != '\0') {
					switch (p[1]) {
					case '"':  g_string_append_c(word, '"');  p += 2; continue;
					case '\\': g_string_append_c(word, '\\'); p += 2; continue;
					case 'n':  g_string_append_c(word, '\n'); p += 2; continue;
					case 't':  g_string_append_c(word, '\t'); p += 2; continue;
					default:   break;
					}
				}
				g_string_append_c(word, *p++);
			}
			if (*p == '\0') {
				set_error_at(error, pos, base + (gint)(open - text),
					GSURF_WMUX_COMMAND_ERROR_SYNTAX, "unterminated double quote");
				return NULL;
			}
			p++;
		} else if (c == '\\') {
			/* Outside quotes a backslash makes the next character
			 * (a whole UTF-8 sequence) literal. */
			const gchar *next;

			if (p[1] == '\0') {
				set_error_at(error, pos, base + (gint)(p - text),
					GSURF_WMUX_COMMAND_ERROR_SYNTAX, "trailing backslash");
				return NULL;
			}
			if (is_bad_control((guchar)p[1])) {
				set_error_at(error, pos, base + (gint)(p + 1 - text),
					GSURF_WMUX_COMMAND_ERROR_SYNTAX,
					"control character 0x%02x", (guchar)p[1]);
				return NULL;
			}
			next = g_utf8_next_char(p + 1);
			g_string_append_len(word, p + 1, next - (p + 1));
			p = next;
		} else {
			g_string_append_c(word, *p++);
		}
	}
	if (in_word)
		g_ptr_array_add(words, g_strdup(word->str));
	g_ptr_array_add(words, NULL);

	if (starts_out != NULL)
		*starts_out = g_steal_pointer(&starts);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&words), FALSE);
}

gchar **
gsurf_wmux_command_tokenize(const gchar *text, GArray **starts, GError **error)
{
	g_return_val_if_fail(text != NULL, NULL);

	if (starts != NULL)
		*starts = NULL;
	return tokenize(text, 0, starts, NULL, error);
}

/* ------------------------------------------------------------------ */
/* Quoting and printing                                                */
/* ------------------------------------------------------------------ */

gchar *
gsurf_wmux_command_quote(const gchar *word)
{
	GString *out;
	const gchar *p;
	gboolean plain = word != NULL && *word != '\0';

	if (word == NULL)
		word = "";
	for (p = word; plain && *p != '\0'; p++)
		if (strchr(" \t\n\r'\"\\", *p) != NULL || is_bad_control((guchar)*p))
			plain = FALSE;
	if (plain)
		return g_strdup(word);

	out = g_string_new("\"");
	for (p = word; *p != '\0'; p++) {
		switch (*p) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\n': g_string_append(out, "\\n");  break;
		case '\t': g_string_append(out, "\\t");  break;
		case '\r': g_string_append(out, " ");    break;
		default:   g_string_append_c(out, *p);   break;
		}
	}
	g_string_append_c(out, '"');
	return g_string_free(out, FALSE);
}

gint
gsurf_wmux_command_flag_slot(gchar flag)
{
	if (flag >= 'A' && flag <= 'Z')
		return flag - 'A';
	if (flag >= 'a' && flag <= 'z')
		return 26 + (flag - 'a');
	return -1;
}

gboolean
gsurf_wmux_command_has_flag(const GsurfWmuxCommand *cmd, gchar flag)
{
	gint slot = gsurf_wmux_command_flag_slot(flag);

	g_return_val_if_fail(cmd != NULL, FALSE);
	return slot >= 0 && (cmd->flags & (G_GUINT64_CONSTANT(1) << slot)) != 0;
}

const gchar *
gsurf_wmux_command_get_value(const GsurfWmuxCommand *cmd, gchar flag)
{
	gint slot = gsurf_wmux_command_flag_slot(flag);

	g_return_val_if_fail(cmd != NULL, NULL);
	return slot >= 0 ? cmd->values[slot] : NULL;
}

const gchar *
gsurf_wmux_command_get_arg(const GsurfWmuxCommand *cmd, guint index)
{
	g_return_val_if_fail(cmd != NULL, NULL);
	return index < cmd->n_args ? cmd->args[index] : NULL;
}

gchar
gsurf_wmux_command_direction(const GsurfWmuxCommand *cmd)
{
	const gchar *d;

	g_return_val_if_fail(cmd != NULL, 0);
	for (d = "LRUD"; *d != '\0'; d++)
		if (gsurf_wmux_command_has_flag(cmd, *d))
			return *d;
	return 0;
}

gchar *
gsurf_wmux_command_to_string(const GsurfWmuxCommand *cmd)
{
	const CommandSpec *spec;
	GString *out;
	const gchar *f;
	guint i;

	g_return_val_if_fail(cmd != NULL, NULL);
	spec = &command_specs[cmd->id];
	out = g_string_new(spec->name);

	/* Boolean flags, then valued flags, each in synopsis order. */
	for (f = spec->bool_flags; *f != '\0'; f++)
		if (gsurf_wmux_command_has_flag(cmd, *f))
			g_string_append_printf(out, " -%c", *f);
	for (f = spec->value_flags; *f != '\0'; f++) {
		const gchar *value = gsurf_wmux_command_get_value(cmd, *f);

		if (value != NULL) {
			g_autofree gchar *q = gsurf_wmux_command_quote(value);

			g_string_append_printf(out, " -%c %s", *f, q);
		}
	}

	/* The nested command is printed inline: its first word is a command
	 * name, so it ends the flags by itself. */
	if (cmd->nested != NULL) {
		g_autofree gchar *inner = gsurf_wmux_command_to_string(cmd->nested);

		g_string_append_printf(out, " %s", inner);
		return g_string_free(out, FALSE);
	}
	if (cmd->template_str != NULL) {
		g_autofree gchar *q = gsurf_wmux_command_quote(cmd->template_str);

		if (cmd->template_str[0] == '-' && cmd->template_str[1] != '\0')
			g_string_append(out, " --");
		g_string_append_printf(out, " %s", q);
		return g_string_free(out, FALSE);
	}

	/* Positional arguments; "--" protects one that looks like a flag (a
	 * lone "-" is an argument already). */
	if (cmd->n_args > 0 && cmd->args[0][0] == '-' && cmd->args[0][1] != '\0')
		g_string_append(out, " --");
	for (i = 0; i < cmd->n_args; i++) {
		g_autofree gchar *q = gsurf_wmux_command_quote(cmd->args[i]);

		g_string_append_printf(out, " %s", q);
	}
	return g_string_free(out, FALSE);
}

gboolean
gsurf_wmux_command_equal(const GsurfWmuxCommand *a, const GsurfWmuxCommand *b)
{
	g_autofree gchar *sa = NULL;
	g_autofree gchar *sb = NULL;

	if (a == NULL || b == NULL)
		return a == b;
	sa = gsurf_wmux_command_to_string(a);
	sb = gsurf_wmux_command_to_string(b);
	return strcmp(sa, sb) == 0;
}

void
gsurf_wmux_command_free(GsurfWmuxCommand *cmd)
{
	guint i;

	if (cmd == NULL)
		return;
	for (i = 0; i < GSURF_WMUX_COMMAND_N_FLAGS; i++)
		g_free(cmd->values[i]);
	g_strfreev(cmd->args);
	gsurf_wmux_command_free(cmd->nested);
	g_free(cmd->template_str);
	g_free(cmd);
}

/* ------------------------------------------------------------------ */
/* Numbers                                                             */
/* ------------------------------------------------------------------ */

/* Parse a strict decimal number: optional sign (only when @allow_sign),
 * 1..9 digits, nothing else. */
static gboolean
parse_number(const gchar *s, gboolean allow_sign, gint *value, gboolean *signed_out)
{
	gboolean negative = FALSE;
	gboolean has_sign = FALSE;
	gint64 v = 0;
	guint digits = 0;

	if (*s == '+' || *s == '-') {
		if (!allow_sign)
			return FALSE;
		has_sign = TRUE;
		negative = *s == '-';
		s++;
	}
	for (; *s != '\0'; s++) {
		if (!g_ascii_isdigit(*s) || ++digits > 9)
			return FALSE;
		v = v * 10 + (*s - '0');
	}
	if (digits == 0)
		return FALSE;
	*value = (gint)(negative ? -v : v);
	if (signed_out != NULL)
		*signed_out = has_sign;
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
	const gchar *line;     /* the text the offsets in @starts refer to */
	gchar      **words;
	const guint *starts;   /* byte offset of each word */
	guint        n;
	gint         base;     /* added to every reported position */
	gint         end;      /* position just past the last word */
} Words;

static GsurfWmuxCommand *parse_words(const Words *w, guint first, guint depth,
                                     gint *pos, GError **error);

/* Parse one string as a nested command line, with error positions moved
 * to where that string sits in the outer line. */
static GsurfWmuxCommand *
parse_line(const gchar *text, gint base, guint depth, gint *pos, GError **error)
{
	g_auto(GStrv) words = NULL;
	g_autoptr(GArray) starts = NULL;
	Words w;

	words = tokenize(text, base, &starts, pos, error);
	if (words == NULL)
		return NULL;
	w.line = text;
	w.words = words;
	w.starts = (const guint *)(void *)starts->data;
	w.n = g_strv_length(words);
	w.base = base;
	w.end = base + (gint)strlen(text);
	return parse_words(&w, 0, depth, pos, error);
}

/* Position of word @i (or the end of the line past the last word). */
static gint
word_pos(const Words *w, guint i)
{
	return i < w->n ? w->base + (gint)w->starts[i] : w->end;
}

/* Count how many of @letters are set on @cmd. */
static guint
count_flags(const GsurfWmuxCommand *cmd, const gchar *letters)
{
	guint n = 0;

	for (; *letters != '\0'; letters++)
		if (gsurf_wmux_command_has_flag(cmd, *letters))
			n++;
	return n;
}

/* Replace a valued flag's text with its normalized number. */
static void
set_number_value(GsurfWmuxCommand *cmd, gchar flag, gint number, gboolean sign)
{
	gint slot = gsurf_wmux_command_flag_slot(flag);

	g_free(cmd->values[slot]);
	cmd->values[slot] = sign ? g_strdup_printf("%+d", number)
	                         : g_strdup_printf("%d", number);
}

/* Read a -t target ("3", "+1", "-1") into @cmd. */
static gboolean
read_target(GsurfWmuxCommand *cmd, gint at, gint *pos, GError **error)
{
	const gchar *value = gsurf_wmux_command_get_value(cmd, 't');
	gboolean sign = FALSE;
	gint number = 0;

	if (!parse_number(value, TRUE, &number, &sign)) {
		set_error_at(error, pos, at, GSURF_WMUX_COMMAND_ERROR_BAD_VALUE,
			"%s: -t needs N, +N or -N, not \"%s\"",
			command_specs[cmd->id].name, value);
		return FALSE;
	}
	cmd->number = number;
	cmd->has_number = TRUE;
	cmd->relative = sign;
	set_number_value(cmd, 't', number, sign);
	return TRUE;
}

/* Validate a template: with each "%%"/"%1" replaced by a word, or with a
 * word appended when it has none, it must parse as a command. */
static gboolean
check_template(const gchar *tmpl, gint at, guint depth, gint *pos, GError **error)
{
	g_autoptr(GsurfWmuxCommand) probe = NULL;
	g_autoptr(GError) inner = NULL;
	g_autofree gchar *line = NULL;
	gint inner_pos = -1;

	if (strstr(tmpl, "%%") != NULL || strstr(tmpl, "%1") != NULL) {
		g_autofree gchar *a = NULL;
		gchar **parts = g_strsplit(tmpl, "%%", -1);

		a = g_strjoinv("x", parts);
		g_strfreev(parts);
		parts = g_strsplit(a, "%1", -1);
		line = g_strjoinv("x", parts);
		g_strfreev(parts);
	} else {
		line = g_strconcat(tmpl, " x", NULL);
	}
	probe = parse_line(line, 0, depth + 1, &inner_pos, &inner);
	if (probe == NULL) {
		if (inner != NULL && inner->code == GSURF_WMUX_COMMAND_ERROR_TOO_DEEP) {
			set_error_at(error, pos, at, GSURF_WMUX_COMMAND_ERROR_TOO_DEEP,
				"commands nested more than %d deep", GSURF_WMUX_COMMAND_MAX_DEPTH);
			return FALSE;
		}
		set_error_at(error, pos, at, GSURF_WMUX_COMMAND_ERROR_BAD_VALUE,
			"command-prompt: template \"%s\" is not a command (%s)", tmpl,
			inner != NULL ? inner->message : "invalid");
		return FALSE;
	}
	return TRUE;
}

/* The per-command rules, run once flags and arguments are collected.
 * @first is the index of the command word, @arg0 that of the first
 * positional word. */
static gboolean
validate(GsurfWmuxCommand *cmd, const Words *w, guint first, guint arg0, guint depth,
         gint *pos, GError **error)
{
	gint at_args = word_pos(w, arg0);
	gint at_cmd = word_pos(w, first);

	switch (cmd->id) {
	case GSURF_WMUX_CMD_SPLIT_WINDOW:
		if (count_flags(cmd, "hv") > 1) {
			set_error_at(error, pos, at_cmd, GSURF_WMUX_COMMAND_ERROR_CONFLICT,
				"split-window: -h and -v exclude each other");
			return FALSE;
		}
		if (gsurf_wmux_command_get_value(cmd, 'p') != NULL) {
			gint pct = 0;

			if (!parse_number(gsurf_wmux_command_get_value(cmd, 'p'), FALSE, &pct, NULL) ||
			    pct < 1 || pct > 99) {
				set_error_at(error, pos, at_cmd, GSURF_WMUX_COMMAND_ERROR_BAD_VALUE,
					"split-window: -p needs a percentage from 1 to 99, not \"%s\"",
					gsurf_wmux_command_get_value(cmd, 'p'));
				return FALSE;
			}
			cmd->number = pct;
			cmd->has_number = TRUE;
			set_number_value(cmd, 'p', pct, FALSE);
		}
		return TRUE;

	case GSURF_WMUX_CMD_SELECT_PANE:
		if (count_flags(cmd, "LRUDlt") != 1) {
			set_error_at(error, pos, at_cmd,
				count_flags(cmd, "LRUDlt") == 0
					? GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT
					: GSURF_WMUX_COMMAND_ERROR_CONFLICT,
				"select-pane needs exactly one of -L, -R, -U, -D, -l, -t");
			return FALSE;
		}
		if (gsurf_wmux_command_has_flag(cmd, 't'))
			return read_target(cmd, at_cmd, pos, error);
		return TRUE;

	case GSURF_WMUX_CMD_RESIZE_PANE:
		if (count_flags(cmd, "LRUDZ") != 1) {
			set_error_at(error, pos, at_cmd,
				count_flags(cmd, "LRUDZ") == 0
					? GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT
					: GSURF_WMUX_COMMAND_ERROR_CONFLICT,
				"resize-pane needs exactly one of -L, -R, -U, -D, -Z");
			return FALSE;
		}
		if (gsurf_wmux_command_has_flag(cmd, 'Z')) {
			if (cmd->n_args > 0) {
				set_error_at(error, pos, at_args,
					GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS,
					"resize-pane -Z takes no amount");
				return FALSE;
			}
			return TRUE;
		}
		cmd->number = 1;
		if (cmd->n_args == 1) {
			gint n = 0;

			if (!parse_number(cmd->args[0], FALSE, &n, NULL) || n < 1 || n > 100) {
				set_error_at(error, pos, at_args, GSURF_WMUX_COMMAND_ERROR_BAD_VALUE,
					"resize-pane: the amount must be 1 to 100 (percent), not \"%s\"",
					cmd->args[0]);
				return FALSE;
			}
			cmd->number = n;
			cmd->has_number = TRUE;
			g_free(cmd->args[0]);
			cmd->args[0] = g_strdup_printf("%d", n);
		}
		return TRUE;

	case GSURF_WMUX_CMD_SWAP_PANE:
		if (count_flags(cmd, "UD") != 1) {
			set_error_at(error, pos, at_cmd,
				count_flags(cmd, "UD") == 0
					? GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT
					: GSURF_WMUX_COMMAND_ERROR_CONFLICT,
				"swap-pane needs exactly one of -U, -D");
			return FALSE;
		}
		return TRUE;

	case GSURF_WMUX_CMD_ROTATE_WINDOW:
		if (count_flags(cmd, "UD") > 1) {
			set_error_at(error, pos, at_cmd, GSURF_WMUX_COMMAND_ERROR_CONFLICT,
				"rotate-window: -U and -D exclude each other");
			return FALSE;
		}
		return TRUE;

	case GSURF_WMUX_CMD_SELECT_LAYOUT: {
		guint i;

		if (gsurf_wmux_command_has_flag(cmd, 'E')) {
			if (cmd->n_args > 0) {
				set_error_at(error, pos, at_args, GSURF_WMUX_COMMAND_ERROR_CONFLICT,
					"select-layout: -E takes no layout name");
				return FALSE;
			}
			return TRUE;
		}
		if (cmd->n_args == 0) {
			set_error_at(error, pos, at_args, GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT,
				"select-layout needs a layout name or -E");
			return FALSE;
		}
		for (i = 0; i < G_N_ELEMENTS(layout_names); i++)
			if (strcmp(cmd->args[0], layout_names[i]) == 0)
				return TRUE;
		set_error_at(error, pos, at_args, GSURF_WMUX_COMMAND_ERROR_BAD_VALUE,
			"select-layout: unknown layout \"%s\" (even-horizontal, even-vertical, "
			"main-horizontal, main-vertical, tiled)", cmd->args[0]);
		return FALSE;
	}

	case GSURF_WMUX_CMD_SELECT_WINDOW:
		if (!gsurf_wmux_command_has_flag(cmd, 't')) {
			set_error_at(error, pos, at_cmd, GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT,
				"select-window needs -t");
			return FALSE;
		}
		return read_target(cmd, at_cmd, pos, error);

	case GSURF_WMUX_CMD_COMMAND_PROMPT:
		if (cmd->n_args == 0)
			return TRUE;
		/* One word is the template; several words are joined
		 * (quoted) into one. */
		if (cmd->n_args == 1) {
			cmd->template_str = g_strdup(cmd->args[0]);
		} else {
			GString *t = g_string_new(NULL);
			guint i;

			for (i = 0; i < cmd->n_args; i++) {
				g_autofree gchar *q = gsurf_wmux_command_quote(cmd->args[i]);

				g_string_append_printf(t, "%s%s", i > 0 ? " " : "", q);
			}
			cmd->template_str = g_string_free(t, FALSE);
		}
		g_strfreev(cmd->args);
		cmd->args = g_new0(gchar *, 1);
		cmd->n_args = 0;
		return check_template(cmd->template_str, at_args, depth, pos, error);

	case GSURF_WMUX_CMD_CONFIRM_BEFORE:
		if (w->n <= arg0) {
			set_error_at(error, pos, at_args, GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT,
				"confirm-before needs a command");
			return FALSE;
		}
		/* One word is a whole command line (it was quoted); several
		 * words are the command itself. */
		if (w->n == arg0 + 1) {
			gint base = word_pos(w, arg0);
			gchar lead = w->line[w->starts[arg0]];

			/* A quoted word's text begins one byte later. */
			if (lead == '\'' || lead == '"')
				base++;
			cmd->nested = parse_line(w->words[arg0], base, depth + 1, pos, error);
		} else {
			cmd->nested = parse_words(w, arg0, depth + 1, pos, error);
		}
		g_strfreev(cmd->args);
		cmd->args = g_new0(gchar *, 1);
		cmd->n_args = 0;
		return cmd->nested != NULL;

	default:
		return TRUE;
	}
}

/* Parse words[first..] as one command. */
static GsurfWmuxCommand *
parse_words(const Words *w, guint first, guint depth, gint *pos, GError **error)
{
	g_autoptr(GsurfWmuxCommand) cmd = NULL;
	g_autoptr(GPtrArray) args = NULL;
	const CommandSpec *spec;
	GsurfWmuxCommandId id;
	guint i;
	guint arg0;

	if (depth > GSURF_WMUX_COMMAND_MAX_DEPTH) {
		set_error_at(error, pos, word_pos(w, first), GSURF_WMUX_COMMAND_ERROR_TOO_DEEP,
			"commands nested more than %d deep", GSURF_WMUX_COMMAND_MAX_DEPTH);
		return NULL;
	}
	if (first >= w->n) {
		set_error_at(error, pos, word_pos(w, first), GSURF_WMUX_COMMAND_ERROR_EMPTY,
			"no command");
		return NULL;
	}
	if (!gsurf_wmux_command_id_from_name(w->words[first], &id)) {
		set_error_at(error, pos, word_pos(w, first),
			GSURF_WMUX_COMMAND_ERROR_UNKNOWN_COMMAND,
			"unknown command \"%s\"", w->words[first]);
		return NULL;
	}
	spec = &command_specs[id];
	cmd = g_new0(GsurfWmuxCommand, 1);
	cmd->id = id;

	/* Flags, getopt-style: a word "-abc" is a cluster of letters; a
	 * letter that takes a value consumes the rest of the cluster or the
	 * next word. "--" and the first non-flag word end the flags. A lone
	 * "-" is an argument. */
	for (i = first + 1; i < w->n; i++) {
		const gchar *word = w->words[i];
		const gchar *f;

		if (word[0] != '-' || word[1] == '\0')
			break;
		if (strcmp(word, "--") == 0) {
			i++;
			break;
		}
		for (f = word + 1; *f != '\0'; f++) {
			gint slot = gsurf_wmux_command_flag_slot(*f);
			gint at = word_pos(w, i) + (gint)(f - word);

			if (slot >= 0 && strchr(spec->value_flags, *f) != NULL) {
				const gchar *value;

				if (f[1] != '\0') {
					value = f + 1;
				} else if (i + 1 < w->n) {
					value = w->words[++i];
				} else {
					set_error_at(error, pos, at,
						GSURF_WMUX_COMMAND_ERROR_MISSING_VALUE,
						"%s: -%c needs a value", spec->name, *f);
					return NULL;
				}
				g_free(cmd->values[slot]);
				cmd->values[slot] = g_strdup(value);
				cmd->flags |= G_GUINT64_CONSTANT(1) << slot;
				break;
			}
			if (slot < 0 || strchr(spec->bool_flags, *f) == NULL) {
				if (g_ascii_isprint(*f))
					set_error_at(error, pos, at,
						GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG,
						"%s has no flag -%c", spec->name, *f);
				else
					set_error_at(error, pos, at,
						GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG,
						"%s: \"%s\" is not a flag", spec->name, word);
				return NULL;
			}
			cmd->flags |= G_GUINT64_CONSTANT(1) << slot;
		}
	}
	arg0 = i;

	/* Positional arguments. REST commands keep the words for validate()
	 * (a nested command or a template); others are bounded. */
	args = g_ptr_array_new_with_free_func(g_free);
	if (spec->max_args != ARGS_REST && w->n - arg0 > spec->max_args) {
		set_error_at(error, pos, word_pos(w, arg0 + spec->max_args),
			GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS,
			spec->max_args == 0 ? "%s takes no arguments"
			                    : "%s takes at most one argument",
			spec->name);
		return NULL;
	}
	for (i = arg0; i < w->n; i++)
		g_ptr_array_add(args, g_strdup(w->words[i]));
	cmd->n_args = args->len;
	g_ptr_array_add(args, NULL);
	cmd->args = (gchar **)g_ptr_array_free(g_steal_pointer(&args), FALSE);

	if (!validate(cmd, w, first, arg0, depth, pos, error))
		return NULL;
	return g_steal_pointer(&cmd);
}

GsurfWmuxCommand *
gsurf_wmux_command_parse(const gchar *text, gint *error_pos, GError **error)
{
	GsurfWmuxCommand *cmd;
	gint pos = -1;

	if (error_pos != NULL)
		*error_pos = -1;
	if (text == NULL) {
		set_error_at(error, error_pos, 0, GSURF_WMUX_COMMAND_ERROR_EMPTY, "no command");
		return NULL;
	}
	cmd = parse_line(text, 0, 1, &pos, error);
	if (cmd == NULL && error_pos != NULL)
		*error_pos = pos;
	return cmd;
}
