/*
 * gsurf-keys.c - Keybinding string helpers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "util/gsurf-keys.h"

#include <gdk/gdk.h>

/* Canonical spelling of a key token, folding the case of a single ASCII
 * letter into an explicit Shift: "N" -> ("n", shift). This is what lets
 * GDK's Shift+n event (keyval "N" plus the Shift mask) meet a config
 * string written "N", "Shift+n" or "Shift+N". GDK reports Shift+Tab as
 * ISO_Left_Tab, which canonicalizes to ("Tab", shift). Writes the token
 * into @buf when it changes and returns it. */
/* US-layout punctuation: the unshifted GDK keysym name, its single
 * character spelling, and the keysym GDK reports while Shift is held. */
typedef struct {
	const gchar *keysym;
	const gchar *literal;
	const gchar *shifted;
} PunctuationKey;

static const PunctuationKey punctuation_keys[] = {
	{ "period",       ".",  "greater"      },
	{ "comma",        ",",  "less"         },
	{ "slash",        "/",  "question"     },
	{ "semicolon",    ";",  "colon"        },
	{ "apostrophe",   "'",  "quotedbl"     },
	{ "bracketleft",  "[",  "braceleft"    },
	{ "bracketright", "]",  "braceright"   },
	{ "minus",        "-",  "underscore"   },
	{ "equal",        "=",  "plus"         },
	{ "grave",        "`",  "asciitilde"   },
	{ "backslash",    "\\", "bar"          },
};

/* Map a one-character punctuation spelling ("." ",") to its keysym name;
 * anything else is returned unchanged. */
static const gchar *
punctuation_name(const gchar *key)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(punctuation_keys); i++)
		if (g_strcmp0(key, punctuation_keys[i].literal) == 0)
			return punctuation_keys[i].keysym;
	return key;
}

/* The keysym GDK reports for @key while Shift is held, or @key itself. */
static const gchar *
shifted_keysym(const gchar *key)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(punctuation_keys); i++)
		if (g_strcmp0(key, punctuation_keys[i].keysym) == 0)
			return punctuation_keys[i].shifted;
	return key;
}

static const gchar *
canonical_key(const gchar *key, gboolean *shift, gchar buf[2])
{
	if (key[0] != '\0' && key[1] == '\0' && g_ascii_isalpha(key[0])) {
		if (g_ascii_isupper(key[0]))
			*shift = TRUE;
		buf[0] = g_ascii_tolower(key[0]);
		buf[1] = '\0';
		return buf;
	}
	if (g_strcmp0(key, "ISO_Left_Tab") == 0) {
		*shift = TRUE;
		return "Tab";
	}
	/* "?" is the user-facing name for GDK's "question" keyval. */
	if (g_strcmp0(key, "?") == 0)
		return "question";
	key = punctuation_name(key);
	/* With Shift held, GDK reports the shifted keysym (Shift+period
	 * arrives as "greater"), so a binding written against the unshifted
	 * key ("Ctrl+Shift+period", "Ctrl+Shift+.") must name that keysym to
	 * ever match. Without Shift the name is left alone. */
	if (*shift)
		key = shifted_keysym(key);
	return key;
}

/* Assemble "Ctrl+Alt+Shift+Super+key" in the one canonical order. */
static gchar *
assemble(gboolean ctrl, gboolean alt, gboolean shift, gboolean super, const gchar *key)
{
	GString *out = g_string_new(NULL);

	if (ctrl)  g_string_append(out, "Ctrl+");
	if (alt)   g_string_append(out, "Alt+");
	if (shift) g_string_append(out, "Shift+");
	if (super) g_string_append(out, "Super+");
	if (key != NULL)
		g_string_append(out, key);
	return g_string_free(out, FALSE);
}

gchar *
gsurf_keys_to_string(guint keyval, guint mods)
{
	const gchar *name = gdk_keyval_name(keyval);
	gboolean shift = (mods & GSURF_MOD_SHIFT) != 0;
	gchar buf[2];

	if (name == NULL)
		return NULL;

	name = canonical_key(name, &shift, buf);
	return assemble((mods & GSURF_MOD_CTRL) != 0, (mods & GSURF_MOD_ALT) != 0,
		shift, (mods & GSURF_MOD_SUPER) != 0, name);
}

gchar *
gsurf_keys_to_name(guint keyval, guint mods)
{
	const gchar *name = gdk_keyval_name(keyval);

	if (name == NULL)
		return NULL;

	/* Letters: uppercase when the keyval is (Shift or Caps Lock) or the
	 * Shift mask is set on a lowercase keyval (synthesized events). */
	if (name[0] != '\0' && name[1] == '\0' && g_ascii_isalpha(name[0])) {
		gchar c = (mods & GSURF_MOD_SHIFT) != 0
			? g_ascii_toupper(name[0]) : name[0];
		return g_strndup(&c, 1);
	}
	if (g_strcmp0(name, "ISO_Left_Tab") == 0)
		return g_strdup("Tab");
	return g_strdup(name);
}

gchar *
gsurf_keys_normalize(const gchar *keystring)
{
	gchar **parts;
	gboolean ctrl = FALSE, alt = FALSE, shift = FALSE, super = FALSE;
	const gchar *key = NULL;
	gchar buf[2];
	gchar *out;
	guint i;

	if (keystring == NULL || *keystring == '\0')
		return NULL;

	parts = g_strsplit(keystring, "+", -1);
	for (i = 0; parts[i] != NULL; i++) {
		gchar *tok = g_strstrip(parts[i]);

		if (*tok == '\0')
			continue;
		if (g_ascii_strcasecmp(tok, "ctrl") == 0 ||
		    g_ascii_strcasecmp(tok, "control") == 0)
			ctrl = TRUE;
		else if (g_ascii_strcasecmp(tok, "alt") == 0 ||
		         g_ascii_strcasecmp(tok, "mod1") == 0)
			alt = TRUE;
		else if (g_ascii_strcasecmp(tok, "shift") == 0)
			shift = TRUE;
		else if (g_ascii_strcasecmp(tok, "super") == 0 ||
		         g_ascii_strcasecmp(tok, "logo") == 0 ||
		         g_ascii_strcasecmp(tok, "mod4") == 0)
			super = TRUE;
		else
			key = tok;
	}

	if (key != NULL)
		key = canonical_key(key, &shift, buf);
	out = assemble(ctrl, alt, shift, super, key);

	g_strfreev(parts);
	return out;
}

gboolean
gsurf_keys_match(guint keyval, guint mods, const gchar *keystring)
{
	g_autofree gchar *event = gsurf_keys_to_string(keyval, mods);
	g_autofree gchar *norm = gsurf_keys_normalize(keystring);

	if (event == NULL || norm == NULL)
		return FALSE;
	return g_strcmp0(event, norm) == 0;
}
