/*
 * gsurf-keys.h - Keybinding string helpers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Canonicalize and match keybinding strings of the form
 * "Ctrl+Shift+key", where `key` is a GDK keyval name ("r", "slash",
 * "Return"). Used by main.c and by input-handling modules so config
 * strings and live key events compare consistently. GDK is common to
 * both the GTK3 and GTK4 backends, so this stays backend-agnostic at the
 * API level (it takes/returns plain integers and strings).
 */

#ifndef GSURF_KEYS_H
#define GSURF_KEYS_H

#include <glib.h>

#include "gsurf-enums.h"

G_BEGIN_DECLS

/**
 * gsurf_keys_to_string:
 * @keyval: a GDK keyval
 * @mods: a #GsurfKeyMod modifier mask
 *
 * Builds the canonical "Ctrl+Alt+Shift+Super+key" string for a key event.
 * A letter key is always spelled lowercase with an explicit Shift for an
 * uppercase keyval or the Shift mask: GDK's Shift+n event ("N" with Shift)
 * gives "Shift+n". ISO_Left_Tab (GDK's Shift+Tab) gives "Shift+Tab".
 *
 * Returns: (transfer full) (nullable): the string, or %NULL if @keyval
 *   has no name
 */
gchar *gsurf_keys_to_string(guint keyval, guint mods);

/**
 * gsurf_keys_to_name:
 * @keyval: a GDK keyval
 * @mods: a #GsurfKeyMod modifier mask
 *
 * The bare key name of an event without modifier prefixes, for vi-style
 * keymaps that tell `g` from `G`. A letter is uppercase when @keyval is
 * or when @mods carries Shift; ISO_Left_Tab is "Tab"; other keys keep
 * their GDK name ("Escape", "slash").
 *
 * Returns: (transfer full) (nullable): the name, or %NULL if @keyval has
 *   no name
 */
gchar *gsurf_keys_to_name(guint keyval, guint mods);

/**
 * gsurf_keys_normalize:
 * @keystring: a possibly unordered "shift+CTRL+r" string
 *
 * Reorders modifiers and normalizes case so the result matches what
 * gsurf_keys_to_string() produces. An uppercase letter means its shifted
 * key ("N", "Shift+N" and "Shift+n" all give "Shift+n"); "?" gives
 * "question" and "ISO_Left_Tab" gives "Shift+Tab". Any other key token
 * keeps its spelling.
 *
 * Returns: (transfer full) (nullable): the normalized string
 */
gchar *gsurf_keys_normalize(const gchar *keystring);

/**
 * gsurf_keys_match:
 * @keyval: a GDK keyval
 * @mods: a #GsurfKeyMod modifier mask
 * @keystring: a configured key string (any modifier order/case)
 *
 * Returns: %TRUE if the event matches the configured key string.
 */
gboolean gsurf_keys_match(guint keyval, guint mods, const gchar *keystring);

G_END_DECLS

#endif /* GSURF_KEYS_H */
