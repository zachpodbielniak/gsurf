/*
 * gsurf-wmux-command.h - The wmux command language (a tmux subset)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every wmux binding and every line typed at the command prompt is a
 * command in tmux syntax: "split-window -h -p 30", "select-pane -t +1",
 * "confirm-before -p 'Kill?' kill-pane". This file tokenizes such a line
 * (single quotes, double quotes, backslash escapes), parses it into a
 * GsurfWmuxCommand (command id + flags + flag values + positional
 * arguments + a nested command), validates it against the command's
 * synopsis, and prints it back in one canonical spelling.
 *
 * Nothing is executed here; the module (Phase 3) interprets the parsed
 * struct. Placeholders such as "#W" (window name) are kept verbatim for
 * the module to expand. See docs/wmux.org, "Command reference", for every
 * command.
 *
 * Pure GLib, module-private (hidden symbols), compiled straight into
 * tests/test-wmux-command.c.
 */

#ifndef GSURF_WMUX_COMMAND_H
#define GSURF_WMUX_COMMAND_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GSURF_WMUX_COMMAND_ERROR:
 *
 * Error domain for command tokenizing and parsing.
 */
#define GSURF_WMUX_COMMAND_ERROR (gsurf_wmux_command_error_quark())

/**
 * GsurfWmuxCommandError:
 * @GSURF_WMUX_COMMAND_ERROR_EMPTY: the line holds no command
 * @GSURF_WMUX_COMMAND_ERROR_SYNTAX: an unterminated quote, a trailing
 *   backslash, invalid UTF-8 or a control character
 * @GSURF_WMUX_COMMAND_ERROR_UNKNOWN_COMMAND: no such command
 * @GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG: the command has no such flag
 * @GSURF_WMUX_COMMAND_ERROR_MISSING_VALUE: a flag that takes a value has
 *   none
 * @GSURF_WMUX_COMMAND_ERROR_BAD_VALUE: a flag value or argument is out of
 *   range or malformed
 * @GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT: a required flag or argument
 *   is absent
 * @GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS: more positional arguments
 *   than the command takes
 * @GSURF_WMUX_COMMAND_ERROR_CONFLICT: flags that exclude each other
 * @GSURF_WMUX_COMMAND_ERROR_TOO_DEEP: commands nested too deeply
 *
 * Error codes in %GSURF_WMUX_COMMAND_ERROR. Every message starts with
 * "position N: " (a 0-based byte offset into the parsed line), which is
 * also returned through the @error_pos argument of
 * gsurf_wmux_command_parse().
 */
typedef enum {
	GSURF_WMUX_COMMAND_ERROR_EMPTY,
	GSURF_WMUX_COMMAND_ERROR_SYNTAX,
	GSURF_WMUX_COMMAND_ERROR_UNKNOWN_COMMAND,
	GSURF_WMUX_COMMAND_ERROR_UNKNOWN_FLAG,
	GSURF_WMUX_COMMAND_ERROR_MISSING_VALUE,
	GSURF_WMUX_COMMAND_ERROR_BAD_VALUE,
	GSURF_WMUX_COMMAND_ERROR_MISSING_ARGUMENT,
	GSURF_WMUX_COMMAND_ERROR_TOO_MANY_ARGUMENTS,
	GSURF_WMUX_COMMAND_ERROR_CONFLICT,
	GSURF_WMUX_COMMAND_ERROR_TOO_DEEP
} GsurfWmuxCommandError;

G_GNUC_INTERNAL GQuark gsurf_wmux_command_error_quark(void);

/**
 * GsurfWmuxCommandId:
 * @GSURF_WMUX_CMD_SPLIT_WINDOW: split-window [-h|-v] [-b] [-d] [-c] [-p pct] [uri]
 * @GSURF_WMUX_CMD_SELECT_PANE: select-pane -L|-R|-U|-D|-l|-t target
 * @GSURF_WMUX_CMD_LAST_PANE: last-pane
 * @GSURF_WMUX_CMD_KILL_PANE: kill-pane
 * @GSURF_WMUX_CMD_RESIZE_PANE: resize-pane -L|-R|-U|-D [n] | -Z
 * @GSURF_WMUX_CMD_SWAP_PANE: swap-pane -U|-D
 * @GSURF_WMUX_CMD_ROTATE_WINDOW: rotate-window [-U|-D]
 * @GSURF_WMUX_CMD_SELECT_LAYOUT: select-layout name | -E
 * @GSURF_WMUX_CMD_NEXT_LAYOUT: next-layout
 * @GSURF_WMUX_CMD_BREAK_PANE: break-pane
 * @GSURF_WMUX_CMD_DISPLAY_PANES: display-panes
 * @GSURF_WMUX_CMD_NEW_WINDOW: new-window [-n name] [uri]
 * @GSURF_WMUX_CMD_NEXT_WINDOW: next-window
 * @GSURF_WMUX_CMD_PREVIOUS_WINDOW: previous-window
 * @GSURF_WMUX_CMD_LAST_WINDOW: last-window
 * @GSURF_WMUX_CMD_SELECT_WINDOW: select-window -t target
 * @GSURF_WMUX_CMD_KILL_WINDOW: kill-window
 * @GSURF_WMUX_CMD_RENAME_WINDOW: rename-window [name]
 * @GSURF_WMUX_CMD_CHOOSE_TREE: choose-tree
 * @GSURF_WMUX_CMD_COMMAND_PROMPT: command-prompt [-p prompt] [-I initial] [template]
 * @GSURF_WMUX_CMD_CONFIRM_BEFORE: confirm-before [-p prompt] command
 * @GSURF_WMUX_CMD_SEND_PREFIX: send-prefix
 * @GSURF_WMUX_CMD_LIST_KEYS: list-keys
 * @GSURF_WMUX_CMD_COUNT: the number of commands (not a command)
 *
 * The commands wmux understands.
 */
typedef enum {
	GSURF_WMUX_CMD_SPLIT_WINDOW,
	GSURF_WMUX_CMD_SELECT_PANE,
	GSURF_WMUX_CMD_LAST_PANE,
	GSURF_WMUX_CMD_KILL_PANE,
	GSURF_WMUX_CMD_RESIZE_PANE,
	GSURF_WMUX_CMD_SWAP_PANE,
	GSURF_WMUX_CMD_ROTATE_WINDOW,
	GSURF_WMUX_CMD_SELECT_LAYOUT,
	GSURF_WMUX_CMD_NEXT_LAYOUT,
	GSURF_WMUX_CMD_BREAK_PANE,
	GSURF_WMUX_CMD_DISPLAY_PANES,
	GSURF_WMUX_CMD_NEW_WINDOW,
	GSURF_WMUX_CMD_NEXT_WINDOW,
	GSURF_WMUX_CMD_PREVIOUS_WINDOW,
	GSURF_WMUX_CMD_LAST_WINDOW,
	GSURF_WMUX_CMD_SELECT_WINDOW,
	GSURF_WMUX_CMD_KILL_WINDOW,
	GSURF_WMUX_CMD_RENAME_WINDOW,
	GSURF_WMUX_CMD_CHOOSE_TREE,
	GSURF_WMUX_CMD_COMMAND_PROMPT,
	GSURF_WMUX_CMD_CONFIRM_BEFORE,
	GSURF_WMUX_CMD_SEND_PREFIX,
	GSURF_WMUX_CMD_LIST_KEYS,
	GSURF_WMUX_CMD_COUNT
} GsurfWmuxCommandId;

/**
 * GSURF_WMUX_COMMAND_MAX_DEPTH:
 *
 * How deeply confirm-before and command-prompt may nest commands
 * ("confirm-before confirm-before kill-pane" is depth 2).
 */
#define GSURF_WMUX_COMMAND_MAX_DEPTH (4)

/* Flags are single ASCII letters; each has a slot 0..51 (A-Z, a-z). */
#define GSURF_WMUX_COMMAND_N_FLAGS (52)

typedef struct _GsurfWmuxCommand GsurfWmuxCommand;

/**
 * GsurfWmuxCommand:
 * @id: which command
 * @flags: one bit per flag present, bit gsurf_wmux_command_flag_slot()
 * @values: per flag slot, the value of a flag that takes one (%NULL
 *   otherwise)
 * @args: (array zero-terminated=1): positional arguments, never %NULL
 * @n_args: the number of @args
 * @nested: (nullable): the command confirm-before runs
 * @template_str: (nullable): command-prompt's template, verbatim; the
 *   module replaces "%%" (or "%1") with the typed text, or appends the
 *   text as one quoted argument when the template has no placeholder
 * @number: the numeric value of the command's number: split-window -p,
 *   resize-pane's amount (default 1), select-pane / select-window -t
 * @has_number: whether @number was given
 * @relative: for -t, whether it was "+N" or "-N" (@number then signed)
 *
 * A parsed, validated command. Use the accessors or read the fields
 * directly; treat it as immutable.
 */
struct _GsurfWmuxCommand {
	GsurfWmuxCommandId  id;
	guint64             flags;
	gchar              *values[GSURF_WMUX_COMMAND_N_FLAGS];
	gchar             **args;
	guint               n_args;
	GsurfWmuxCommand   *nested;
	gchar              *template_str;
	gint                number;
	gboolean            has_number;
	gboolean            relative;
};

/**
 * gsurf_wmux_command_tokenize:
 * @text: a command line
 * @starts: (out) (optional) (transfer full) (element-type guint): the
 *   byte offset in @text where each token starts
 * @error: return location for a #GError
 *
 * Splits @text into words. Whitespace separates words; 'single quotes'
 * take everything literally; "double quotes" honour \" \\ \n \t; outside
 * quotes a backslash makes the next character literal. Quoted and plain
 * parts next to each other join into one word ("a"'b'c is "abc"); "" is
 * an empty word.
 *
 * Returns: (transfer full) (nullable): a %NULL-terminated word vector
 *   (empty for a blank line), or %NULL with @error set
 */
G_GNUC_INTERNAL gchar **gsurf_wmux_command_tokenize(const gchar *text,
                                                    GArray **starts,
                                                    GError **error);

/**
 * gsurf_wmux_command_parse:
 * @text: a command line, e.g. "split-window -h -p 30"
 * @error_pos: (out) (optional): on error, the byte offset of the
 *   offending token or character (-1 on success)
 * @error: return location for a #GError
 *
 * Tokenizes and parses one command. Flags follow getopt rules: "-hb" is
 * "-h -b", "-p30" and "-p 30" are equal, "--" ends the flags and the
 * first positional argument also ends them (so "confirm-before
 * split-window -h" nests "split-window -h"). tmux's short aliases
 * (splitw, selectp, neww, ...) are accepted.
 *
 * Returns: (transfer full) (nullable): the command, or %NULL with @error
 *   set
 */
G_GNUC_INTERNAL GsurfWmuxCommand *gsurf_wmux_command_parse(const gchar *text,
                                                           gint *error_pos,
                                                           GError **error);

/**
 * gsurf_wmux_command_free:
 * @cmd: (nullable): a command
 *
 * Frees @cmd and its nested command.
 */
G_GNUC_INTERNAL void gsurf_wmux_command_free(GsurfWmuxCommand *cmd);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfWmuxCommand, gsurf_wmux_command_free)

/**
 * gsurf_wmux_command_to_string:
 * @cmd: a command
 *
 * Prints @cmd canonically: the full command name, the boolean flags in
 * the synopsis order, valued flags as "-x value", "--" when the first
 * argument starts with "-", then the arguments (and the nested command).
 * Words are double-quoted when they need it. Parsing the result gives an
 * equal command.
 *
 * Returns: (transfer full): the canonical text
 */
G_GNUC_INTERNAL gchar *gsurf_wmux_command_to_string(const GsurfWmuxCommand *cmd);

/**
 * gsurf_wmux_command_quote:
 * @word: a word
 *
 * Quotes @word so the tokenizer reads it back as one word: unchanged when
 * it is non-empty and holds no whitespace, quote, backslash or
 * control character, double-quoted with escapes otherwise.
 *
 * Returns: (transfer full): the quoted word
 */
G_GNUC_INTERNAL gchar *gsurf_wmux_command_quote(const gchar *word);

/* --- Names --- */

/* The full command name for @id ("split-window"); NULL when out of range. */
G_GNUC_INTERNAL const gchar *gsurf_wmux_command_id_to_name(GsurfWmuxCommandId id);

/* The id for a full name or tmux alias; FALSE when unknown. */
G_GNUC_INTERNAL gboolean gsurf_wmux_command_id_from_name(const gchar *name,
                                                         GsurfWmuxCommandId *id);

/* The synopsis line for @id, as in docs/wmux.org. */
G_GNUC_INTERNAL const gchar *gsurf_wmux_command_id_synopsis(GsurfWmuxCommandId id);

/* --- Accessors --- */

/* The flag bit slot for letter @flag (0..51), or -1 when not a letter. */
G_GNUC_INTERNAL gint         gsurf_wmux_command_flag_slot(gchar flag);
G_GNUC_INTERNAL gboolean     gsurf_wmux_command_has_flag(const GsurfWmuxCommand *cmd,
                                                         gchar flag);
/* The value of a valued flag, NULL when the flag is absent. */
G_GNUC_INTERNAL const gchar *gsurf_wmux_command_get_value(const GsurfWmuxCommand *cmd,
                                                          gchar flag);
/* Positional argument @index, NULL when out of range. */
G_GNUC_INTERNAL const gchar *gsurf_wmux_command_get_arg(const GsurfWmuxCommand *cmd,
                                                        guint index);

/**
 * gsurf_wmux_command_direction:
 * @cmd: a select-pane, resize-pane or swap-pane command
 *
 * Returns: the direction letter given ('L', 'R', 'U' or 'D'), or 0
 */
G_GNUC_INTERNAL gchar gsurf_wmux_command_direction(const GsurfWmuxCommand *cmd);

/**
 * gsurf_wmux_command_equal:
 * @a: a command
 * @b: a command
 *
 * Returns: %TRUE when both print the same canonical text
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_command_equal(const GsurfWmuxCommand *a,
                                                  const GsurfWmuxCommand *b);

G_END_DECLS

#endif /* GSURF_WMUX_COMMAND_H */
