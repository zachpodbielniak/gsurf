/*
 * gsurf-wmux-bindings.c - Key binding tables for the wmux module
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A table is an ordered array of entries plus a hash from canonical key
 * to entry. Each entry carries the public GsurfWmuxBinding and a cache of
 * its parsed command.
 */

#include "gsurf-wmux-bindings.h"
#include "gsurf-wmux-keys.h"

#include <string.h>

G_DEFINE_QUARK(gsurf-wmux-bindings-error-quark, gsurf_wmux_bindings_error)

/* ------------------------------------------------------------------ */
/* Default tables (tmux's defaults, adapted; see docs/wmux.org)        */
/* ------------------------------------------------------------------ */

static const GsurfWmuxDefaultBinding default_prefix[] = {
	/* Panes: split, move, close, zoom, number. */
	{ "%",       "split-window -h",                    FALSE },
	{ "\"",      "split-window -v",                    FALSE },
	{ "C-%",     "split-window -h -c",                 FALSE },
	{ "C-\"",    "split-window -v -c",                 FALSE },
	{ "o",       "select-pane -t +1",                  FALSE },
	{ ";",       "last-pane",                          FALSE },
	{ "x",       "confirm-before kill-pane",           FALSE },
	{ "z",       "resize-pane -Z",                     FALSE },
	{ "q",       "display-panes",                      FALSE },
	{ "{",       "swap-pane -U",                       FALSE },
	{ "}",       "swap-pane -D",                       FALSE },
	{ "!",       "break-pane",                         FALSE },
	{ "C-o",     "rotate-window",                      FALSE },
	{ "M-o",     "rotate-window -D",                   FALSE },
	{ "Up",      "select-pane -U",                     TRUE  },
	{ "Down",    "select-pane -D",                     TRUE  },
	{ "Left",    "select-pane -L",                     TRUE  },
	{ "Right",   "select-pane -R",                     TRUE  },
	{ "C-Up",    "resize-pane -U 1",                   TRUE  },
	{ "C-Down",  "resize-pane -D 1",                   TRUE  },
	{ "C-Left",  "resize-pane -L 1",                   TRUE  },
	{ "C-Right", "resize-pane -R 1",                   TRUE  },
	{ "M-Up",    "resize-pane -U 5",                   TRUE  },
	{ "M-Down",  "resize-pane -D 5",                   TRUE  },
	{ "M-Left",  "resize-pane -L 5",                   TRUE  },
	{ "M-Right", "resize-pane -R 5",                   TRUE  },
	/* Layouts. */
	{ "Space",   "next-layout",                        FALSE },
	{ "M-1",     "select-layout even-horizontal",      FALSE },
	{ "M-2",     "select-layout even-vertical",        FALSE },
	{ "M-3",     "select-layout main-horizontal",      FALSE },
	{ "M-4",     "select-layout main-vertical",        FALSE },
	{ "M-5",     "select-layout tiled",                FALSE },
	{ "E",       "select-layout -E",                   FALSE },
	/* tmux windows. */
	{ "c",       "new-window",                         FALSE },
	{ "n",       "next-window",                        FALSE },
	{ "p",       "previous-window",                    FALSE },
	{ "l",       "last-window",                        FALSE },
	{ "0",       "select-window -t 0",                 FALSE },
	{ "1",       "select-window -t 1",                 FALSE },
	{ "2",       "select-window -t 2",                 FALSE },
	{ "3",       "select-window -t 3",                 FALSE },
	{ "4",       "select-window -t 4",                 FALSE },
	{ "5",       "select-window -t 5",                 FALSE },
	{ "6",       "select-window -t 6",                 FALSE },
	{ "7",       "select-window -t 7",                 FALSE },
	{ "8",       "select-window -t 8",                 FALSE },
	{ "9",       "select-window -t 9",                 FALSE },
	{ "w",       "choose-tree",                        FALSE },
	{ "&",       "confirm-before kill-window",         FALSE },
	{ ",",       "command-prompt -I #W rename-window", FALSE },
	/* Prompt, help, prefix. */
	{ ":",       "command-prompt",                     FALSE },
	{ "?",       "list-keys",                          FALSE },
	{ "C-b",     "send-prefix",                        FALSE },
};

static const GsurfWmuxDefaultBinding default_root[] = {
	{ "C-h", "select-pane -L", FALSE },
	{ "C-j", "select-pane -D", FALSE },
	{ "C-k", "select-pane -U", FALSE },
	{ "C-l", "select-pane -R", FALSE },
};

const GsurfWmuxDefaultBinding *
gsurf_wmux_bindings_default_prefix_table(guint *n)
{
	if (n != NULL)
		*n = G_N_ELEMENTS(default_prefix);
	return default_prefix;
}

const GsurfWmuxDefaultBinding *
gsurf_wmux_bindings_default_root_table(guint *n)
{
	if (n != NULL)
		*n = G_N_ELEMENTS(default_root);
	return default_root;
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
	GsurfWmuxBinding  pub;
	GsurfWmuxCommand *parsed;        /* cache, NULL until parsed */
	gboolean          parse_failed;  /* cache of a failed parse */
} Entry;

struct _GsurfWmuxBindings {
	GPtrArray  *entries;   /* Entry*, table order, owns */
	GHashTable *by_key;    /* canonical key -> Entry*, borrowed */
};

static void
entry_free(gpointer data)
{
	Entry *e = data;

	g_free(e->pub.key);
	g_free(e->pub.command);
	gsurf_wmux_command_free(e->parsed);
	g_free(e);
}

/* Forget a cached parse (the command changed). */
static void
entry_reset_cache(Entry *e)
{
	g_clear_pointer(&e->parsed, gsurf_wmux_command_free);
	e->parse_failed = FALSE;
}

GsurfWmuxBindings *
gsurf_wmux_bindings_new(void)
{
	GsurfWmuxBindings *b = g_new0(GsurfWmuxBindings, 1);

	b->entries = g_ptr_array_new_with_free_func(entry_free);
	b->by_key = g_hash_table_new(g_str_hash, g_str_equal);
	return b;
}

void
gsurf_wmux_bindings_free(GsurfWmuxBindings *bindings)
{
	if (bindings == NULL)
		return;
	g_hash_table_destroy(bindings->by_key);
	g_ptr_array_unref(bindings->entries);
	g_free(bindings);
}

/* Insert or update an entry for an already canonical key. */
static Entry *
put(GsurfWmuxBindings *b, const gchar *canonical, const gchar *command)
{
	Entry *e = g_hash_table_lookup(b->by_key, canonical);

	if (command != NULL && *command == '\0')
		command = NULL;
	if (e == NULL) {
		e = g_new0(Entry, 1);
		e->pub.key = g_strdup(canonical);
		g_ptr_array_add(b->entries, e);
		g_hash_table_insert(b->by_key, e->pub.key, e);
	}
	if (g_strcmp0(e->pub.command, command) != 0) {
		g_free(e->pub.command);
		e->pub.command = g_strdup(command);
		entry_reset_cache(e);
	}
	return e;
}

/* Remove an entry entirely (not a tombstone). */
static void
drop(GsurfWmuxBindings *b, Entry *e)
{
	g_hash_table_remove(b->by_key, e->pub.key);
	g_ptr_array_remove(b->entries, e);
}

static GsurfWmuxBindings *
from_table(const GsurfWmuxDefaultBinding *rows, guint n)
{
	GsurfWmuxBindings *b = gsurf_wmux_bindings_new();
	guint i;

	/* The defaults are written canonically (tested), so no parsing. */
	for (i = 0; i < n; i++)
		put(b, rows[i].key, rows[i].command)->pub.repeatable = rows[i].repeatable;
	return b;
}

GsurfWmuxBindings *
gsurf_wmux_bindings_new_prefix_defaults(void)
{
	return from_table(default_prefix, G_N_ELEMENTS(default_prefix));
}

GsurfWmuxBindings *
gsurf_wmux_bindings_new_root_defaults(void)
{
	return from_table(default_root, G_N_ELEMENTS(default_root));
}

GsurfWmuxBindings *
gsurf_wmux_bindings_copy(GsurfWmuxBindings *bindings)
{
	GsurfWmuxBindings *copy;
	guint i;

	g_return_val_if_fail(bindings != NULL, NULL);
	copy = gsurf_wmux_bindings_new();
	for (i = 0; i < bindings->entries->len; i++) {
		Entry *e = g_ptr_array_index(bindings->entries, i);

		put(copy, e->pub.key, e->pub.command)->pub.repeatable = e->pub.repeatable;
	}
	return copy;
}

/* Normalize @key for the table, wrapping the keys error. */
static gchar *
canonical_key(const gchar *key, GError **error)
{
	g_autoptr(GError) inner = NULL;
	gchar *canonical = gsurf_wmux_keys_normalize(key, &inner);

	if (canonical == NULL)
		g_set_error(error, GSURF_WMUX_BINDINGS_ERROR, GSURF_WMUX_BINDINGS_ERROR_BAD_KEY,
			"bad key \"%s\": %s", key != NULL ? key : "", inner->message);
	return canonical;
}

gboolean
gsurf_wmux_bindings_set(GsurfWmuxBindings *bindings, const gchar *key,
                        const gchar *command, GError **error)
{
	g_autofree gchar *canonical = NULL;

	g_return_val_if_fail(bindings != NULL, FALSE);
	canonical = canonical_key(key, error);
	if (canonical == NULL)
		return FALSE;
	put(bindings, canonical, command);
	return TRUE;
}

gboolean
gsurf_wmux_bindings_unbind(GsurfWmuxBindings *bindings, const gchar *key, GError **error)
{
	return gsurf_wmux_bindings_set(bindings, key, NULL, error);
}

gboolean
gsurf_wmux_bindings_set_repeatable(GsurfWmuxBindings *bindings, const gchar *key,
                                   gboolean repeatable, GError **error)
{
	g_autofree gchar *canonical = NULL;
	Entry *e;

	g_return_val_if_fail(bindings != NULL, FALSE);
	canonical = canonical_key(key, error);
	if (canonical == NULL)
		return FALSE;
	e = g_hash_table_lookup(bindings->by_key, canonical);
	if (e == NULL || e->pub.command == NULL) {
		g_set_error(error, GSURF_WMUX_BINDINGS_ERROR, GSURF_WMUX_BINDINGS_ERROR_NOT_BOUND,
			"key \"%s\" is not bound", canonical);
		return FALSE;
	}
	e->pub.repeatable = repeatable;
	return TRUE;
}

void
gsurf_wmux_bindings_merge(GsurfWmuxBindings *bindings, GsurfWmuxBindings *overrides)
{
	guint i;

	g_return_if_fail(bindings != NULL);
	g_return_if_fail(overrides != NULL);
	for (i = 0; i < overrides->entries->len; i++) {
		Entry *o = g_ptr_array_index(overrides->entries, i);
		Entry *e = g_hash_table_lookup(bindings->by_key, o->pub.key);

		if (o->pub.command == NULL) {
			/* A tombstone unbinds the key in the target. */
			if (e != NULL)
				drop(bindings, e);
			continue;
		}
		e = put(bindings, o->pub.key, o->pub.command);
		e->pub.repeatable = e->pub.repeatable || o->pub.repeatable;
	}
}

/* The entry for @key (canonical or not), tombstones included. */
static Entry *
find(GsurfWmuxBindings *b, const gchar *key)
{
	g_autofree gchar *canonical = NULL;
	Entry *e;

	if (key == NULL)
		return NULL;
	e = g_hash_table_lookup(b->by_key, key);
	if (e != NULL)
		return e;
	canonical = gsurf_wmux_keys_normalize(key, NULL);
	return canonical != NULL ? g_hash_table_lookup(b->by_key, canonical) : NULL;
}

const GsurfWmuxBinding *
gsurf_wmux_bindings_get_entry(GsurfWmuxBindings *bindings, const gchar *key)
{
	Entry *e;

	g_return_val_if_fail(bindings != NULL, NULL);
	e = find(bindings, key);
	return e != NULL ? &e->pub : NULL;
}

const GsurfWmuxBinding *
gsurf_wmux_bindings_lookup(GsurfWmuxBindings *bindings, const gchar *key)
{
	const GsurfWmuxBinding *b = gsurf_wmux_bindings_get_entry(bindings, key);

	return b != NULL && b->command != NULL ? b : NULL;
}

/* Parse (once) and cache an entry's command. */
static const GsurfWmuxCommand *
entry_command(Entry *e, GError **error)
{
	if (e->parsed == NULL && !e->parse_failed) {
		e->parsed = gsurf_wmux_command_parse(e->pub.command, NULL, error);
		e->parse_failed = e->parsed == NULL;
	} else if (e->parse_failed && error != NULL) {
		/* Re-parse only to report the error again. */
		g_autoptr(GsurfWmuxCommand) again = gsurf_wmux_command_parse(e->pub.command,
			NULL, error);
	}
	return e->parsed;
}

const GsurfWmuxCommand *
gsurf_wmux_bindings_get_command(GsurfWmuxBindings *bindings, const gchar *key)
{
	Entry *e;

	g_return_val_if_fail(bindings != NULL, NULL);
	e = find(bindings, key);
	if (e == NULL || e->pub.command == NULL)
		return NULL;
	return entry_command(e, NULL);
}

guint
gsurf_wmux_bindings_get_n(GsurfWmuxBindings *bindings)
{
	guint i;
	guint n = 0;

	g_return_val_if_fail(bindings != NULL, 0);
	for (i = 0; i < bindings->entries->len; i++)
		if (((Entry *)g_ptr_array_index(bindings->entries, i))->pub.command != NULL)
			n++;
	return n;
}

const GsurfWmuxBinding *
gsurf_wmux_bindings_get_nth(GsurfWmuxBindings *bindings, guint index)
{
	guint i;

	g_return_val_if_fail(bindings != NULL, NULL);
	for (i = 0; i < bindings->entries->len; i++) {
		Entry *e = g_ptr_array_index(bindings->entries, i);

		if (e->pub.command == NULL)
			continue;
		if (index-- == 0)
			return &e->pub;
	}
	return NULL;
}

gboolean
gsurf_wmux_bindings_validate(GsurfWmuxBindings *bindings, gboolean remove_invalid,
                             GPtrArray **errors)
{
	g_autoptr(GPtrArray) bad = NULL;
	g_autoptr(GPtrArray) found = NULL;
	guint i;

	g_return_val_if_fail(bindings != NULL, FALSE);
	if (errors != NULL)
		*errors = NULL;

	/* Parse every live binding; collect the failures. */
	bad = g_ptr_array_new();
	found = g_ptr_array_new_with_free_func((GDestroyNotify)g_error_free);
	for (i = 0; i < bindings->entries->len; i++) {
		Entry *e = g_ptr_array_index(bindings->entries, i);
		g_autoptr(GError) inner = NULL;

		if (e->pub.command == NULL)
			continue;
		if (entry_command(e, &inner) != NULL)
			continue;
		g_ptr_array_add(bad, e);
		g_ptr_array_add(found, g_error_new(GSURF_WMUX_BINDINGS_ERROR,
			GSURF_WMUX_BINDINGS_ERROR_BAD_COMMAND,
			"binding \"%s\": bad command \"%s\": %s", e->pub.key, e->pub.command,
			inner != NULL ? inner->message : "does not parse"));
	}

	if (remove_invalid)
		for (i = 0; i < bad->len; i++)
			drop(bindings, g_ptr_array_index(bad, i));

	if (found->len == 0)
		return TRUE;
	if (errors != NULL)
		*errors = g_steal_pointer(&found);
	return FALSE;
}
