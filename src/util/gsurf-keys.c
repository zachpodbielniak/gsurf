/*
 * gsurf-keys.c - Keybinding string helpers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "util/gsurf-keys.h"

#include <gdk/gdk.h>

gchar *
gsurf_keys_to_string(guint keyval, guint mods)
{
	const gchar *name = gdk_keyval_name(keyval);
	GString *s;

	if (name == NULL)
		return NULL;

	s = g_string_new(NULL);
	if (mods & GSURF_MOD_CTRL)  g_string_append(s, "Ctrl+");
	if (mods & GSURF_MOD_ALT)   g_string_append(s, "Alt+");
	if (mods & GSURF_MOD_SHIFT) g_string_append(s, "Shift+");
	if (mods & GSURF_MOD_SUPER) g_string_append(s, "Super+");
	g_string_append(s, name);

	return g_string_free(s, FALSE);
}

gchar *
gsurf_keys_normalize(const gchar *keystring)
{
	gchar **parts;
	gboolean ctrl = FALSE, alt = FALSE, shift = FALSE, super = FALSE;
	const gchar *key = NULL;
	GString *out;
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

	out = g_string_new(NULL);
	if (ctrl)  g_string_append(out, "Ctrl+");
	if (alt)   g_string_append(out, "Alt+");
	if (shift) g_string_append(out, "Shift+");
	if (super) g_string_append(out, "Super+");
	if (key != NULL)
		g_string_append(out, key);

	g_strfreev(parts);
	return g_string_free(out, FALSE);
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
