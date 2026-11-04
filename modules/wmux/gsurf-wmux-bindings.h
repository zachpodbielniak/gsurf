/*
 * gsurf-wmux-bindings.h - Key binding tables for the wmux module
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * wmux has two tables, like tmux: the *prefix* table (keys pressed after
 * the prefix, C-b by default) and the *root* table (keys that work
 * without the prefix: C-h/j/k/l). A GsurfWmuxBindings is one such table:
 * canonical key name (gsurf-wmux-keys.h) -> command line
 * (gsurf-wmux-command.h) plus a "repeatable" flag (tmux bind -r: the key
 * works again without the prefix for repeat_time ms).
 *
 * A table keeps insertion order, which is the order keybind help lists
 * it in. An entry may be a *tombstone* (command %NULL): the key is
 * explicitly unbound. Tombstones are invisible to lookup and iteration;
 * merging a table with tombstones into another removes those keys there,
 * which is how config overrides (`bindings: {x: ""}`) unbind defaults.
 *
 * Commands are stored as text; gsurf_wmux_bindings_validate() parses
 * every one, reports each bad one as a #GError, and can drop them.
 *
 * Pure GLib, module-private (hidden symbols), compiled straight into
 * tests/test-wmux-bindings.c.
 */

#ifndef GSURF_WMUX_BINDINGS_H
#define GSURF_WMUX_BINDINGS_H

#include <glib.h>

#include "gsurf-wmux-command.h"

G_BEGIN_DECLS

/**
 * GSURF_WMUX_DEFAULT_PREFIX:
 *
 * The default prefix key, as in tmux.
 */
#define GSURF_WMUX_DEFAULT_PREFIX "C-b"

/**
 * GSURF_WMUX_BINDINGS_ERROR:
 *
 * Error domain for binding tables.
 */
#define GSURF_WMUX_BINDINGS_ERROR (gsurf_wmux_bindings_error_quark())

/**
 * GsurfWmuxBindingsError:
 * @GSURF_WMUX_BINDINGS_ERROR_BAD_KEY: the key does not parse (the message
 *   carries the key notation error)
 * @GSURF_WMUX_BINDINGS_ERROR_BAD_COMMAND: the command does not parse (the
 *   message carries the command error and its position)
 * @GSURF_WMUX_BINDINGS_ERROR_NOT_BOUND: the key has no binding
 *
 * Error codes in %GSURF_WMUX_BINDINGS_ERROR.
 */
typedef enum {
	GSURF_WMUX_BINDINGS_ERROR_BAD_KEY,
	GSURF_WMUX_BINDINGS_ERROR_BAD_COMMAND,
	GSURF_WMUX_BINDINGS_ERROR_NOT_BOUND
} GsurfWmuxBindingsError;

G_GNUC_INTERNAL GQuark gsurf_wmux_bindings_error_quark(void);

/**
 * GsurfWmuxDefaultBinding:
 * @key: canonical key name
 * @command: command line
 * @repeatable: whether the key repeats without the prefix
 *
 * One row of a built-in default table.
 */
typedef struct {
	const gchar *key;
	const gchar *command;
	gboolean     repeatable;
} GsurfWmuxDefaultBinding;

/**
 * GsurfWmuxBinding:
 * @key: canonical key name
 * @command: (nullable): command line; %NULL marks an unbound key (only
 *   seen through gsurf_wmux_bindings_get_entry())
 * @repeatable: whether the key repeats without the prefix
 *
 * One binding, owned by its table.
 */
typedef struct {
	gchar    *key;
	gchar    *command;
	gboolean  repeatable;
} GsurfWmuxBinding;

typedef struct _GsurfWmuxBindings GsurfWmuxBindings;

/* The built-in tables (static data, in help order). */
G_GNUC_INTERNAL const GsurfWmuxDefaultBinding *gsurf_wmux_bindings_default_prefix_table(guint *n);
G_GNUC_INTERNAL const GsurfWmuxDefaultBinding *gsurf_wmux_bindings_default_root_table(guint *n);

/* An empty table; one filled from the default prefix / root table. */
G_GNUC_INTERNAL GsurfWmuxBindings *gsurf_wmux_bindings_new(void);
G_GNUC_INTERNAL GsurfWmuxBindings *gsurf_wmux_bindings_new_prefix_defaults(void);
G_GNUC_INTERNAL GsurfWmuxBindings *gsurf_wmux_bindings_new_root_defaults(void);
G_GNUC_INTERNAL GsurfWmuxBindings *gsurf_wmux_bindings_copy(GsurfWmuxBindings *bindings);
G_GNUC_INTERNAL void               gsurf_wmux_bindings_free(GsurfWmuxBindings *bindings);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfWmuxBindings, gsurf_wmux_bindings_free)

/**
 * gsurf_wmux_bindings_set:
 * @bindings: a table
 * @key: a key in any notation gsurf_wmux_keys_normalize() accepts
 * @command: (nullable): the command line; %NULL or "" unbinds the key
 * @error: return location for a #GError
 *
 * Binds @key. Rebinding keeps the key's place and its repeatable flag; a
 * new key is appended. The command is not parsed here (see
 * gsurf_wmux_bindings_validate()).
 *
 * Returns: %FALSE (with %GSURF_WMUX_BINDINGS_ERROR_BAD_KEY) when @key
 *   does not parse
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_bindings_set(GsurfWmuxBindings *bindings,
                                                 const gchar *key,
                                                 const gchar *command,
                                                 GError **error);

/* Same as set() with a NULL command. */
G_GNUC_INTERNAL gboolean gsurf_wmux_bindings_unbind(GsurfWmuxBindings *bindings,
                                                    const gchar *key,
                                                    GError **error);

/**
 * gsurf_wmux_bindings_set_repeatable:
 * @bindings: a table
 * @key: a key in any accepted notation
 * @repeatable: the new flag
 * @error: return location for a #GError
 *
 * Returns: %FALSE when @key does not parse or is not bound
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_bindings_set_repeatable(GsurfWmuxBindings *bindings,
                                                            const gchar *key,
                                                            gboolean repeatable,
                                                            GError **error);

/**
 * gsurf_wmux_bindings_merge:
 * @bindings: the table to change (e.g. the defaults)
 * @overrides: the table to apply (e.g. built from config)
 *
 * Applies @overrides in its order: a bound key replaces (or appends) the
 * command in @bindings, and becomes repeatable when either side says so;
 * an unbound key (tombstone) removes the key from @bindings.
 */
G_GNUC_INTERNAL void gsurf_wmux_bindings_merge(GsurfWmuxBindings *bindings,
                                               GsurfWmuxBindings *overrides);

/**
 * gsurf_wmux_bindings_lookup:
 * @bindings: a table
 * @key: a canonical key name (as from gsurf_wmux_keys_from_event()); a
 *   non-canonical spelling is normalized first
 *
 * Returns: (transfer none) (nullable): the live binding, or %NULL when
 *   the key is absent or unbound
 */
G_GNUC_INTERNAL const GsurfWmuxBinding *gsurf_wmux_bindings_lookup(GsurfWmuxBindings *bindings,
                                                                   const gchar *key);

/* Like lookup(), but also returns tombstones (command NULL). */
G_GNUC_INTERNAL const GsurfWmuxBinding *gsurf_wmux_bindings_get_entry(GsurfWmuxBindings *bindings,
                                                                      const gchar *key);

/**
 * gsurf_wmux_bindings_get_command:
 * @bindings: a table
 * @key: a key, as for gsurf_wmux_bindings_lookup()
 *
 * The parsed command of a live binding, parsed on first use and cached
 * until the binding changes.
 *
 * Returns: (transfer none) (nullable): the command, or %NULL when unbound
 *   or when the command does not parse
 */
G_GNUC_INTERNAL const GsurfWmuxCommand *gsurf_wmux_bindings_get_command(GsurfWmuxBindings *bindings,
                                                                        const gchar *key);

/* Number of live bindings; the @index-th live binding in table order
 * (NULL when out of range). Together they iterate for keybind help. */
G_GNUC_INTERNAL guint                   gsurf_wmux_bindings_get_n(GsurfWmuxBindings *bindings);
G_GNUC_INTERNAL const GsurfWmuxBinding *gsurf_wmux_bindings_get_nth(GsurfWmuxBindings *bindings,
                                                                    guint index);

/**
 * gsurf_wmux_bindings_validate:
 * @bindings: a table
 * @remove_invalid: drop every binding whose command does not parse
 * @errors: (out) (optional) (transfer full) (element-type GError): one
 *   %GSURF_WMUX_BINDINGS_ERROR_BAD_COMMAND error per bad binding, in
 *   table order; the message names the key, the command and the command
 *   error; %NULL when every command parses
 *
 * Parses every live binding's command.
 *
 * Returns: %TRUE when every command parses
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_bindings_validate(GsurfWmuxBindings *bindings,
                                                      gboolean remove_invalid,
                                                      GPtrArray **errors);

G_END_DECLS

#endif /* GSURF_WMUX_BINDINGS_H */
