/*
 * gsurf-wmux-keys.h - Key notation for the wmux module
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * wmux names keys the way tmux does: "C-b", "M-1", "S-Up", "C-M-x", "%",
 * "\"", "{", "Space", "Enter", "F5". A config string may be written in
 * tmux notation or in gsurf notation ("Ctrl+b", "Alt+1", "Shift+Up",
 * "Ctrl+Shift+percent"); both normalize to the one canonical tmux name.
 * A key event (a GDK keyval and a modifier set) maps to the same name, so
 * a binding table is a plain string lookup.
 *
 * Canonical form:
 *   - modifiers in the order C- M- S- (Ctrl, Meta/Alt, Shift);
 *   - a printable character stands for itself, shifted: Shift+5 is "%",
 *     Shift+apostrophe is "\"", Shift+a is "A" -- never "S-5" or "S-a";
 *     Ctrl+Shift+5 is "C-%";
 *   - named keys use tmux spellings: Space, Enter, Escape, Tab, BTab
 *     (Shift+Tab), BSpace, Up, Down, Left, Right, Home, End, PPage, NPage,
 *     IC, DC, F1..F12, KP0..KP9, KPEnter; S- stays on named keys
 *     ("S-Up", "C-S-F1");
 *   - Super is not supported: the compositor owns it (gowl binds Super),
 *     so a config key naming it is an error and an event with Super held
 *     has no wmux name and passes through.
 *
 * GLib plus the GDK keyval #defines, taken from <gdk/gdk.h> (GTK 4.24
 * refuses a direct <gdk/gdkkeysyms.h>). Module-private (hidden symbols) and compiled straight into
 * tests/test-wmux-keys.c.
 */

#ifndef GSURF_WMUX_KEYS_H
#define GSURF_WMUX_KEYS_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GsurfWmuxMod:
 * @GSURF_WMUX_MOD_NONE: no modifier
 * @GSURF_WMUX_MOD_SHIFT: Shift
 * @GSURF_WMUX_MOD_CTRL: Control
 * @GSURF_WMUX_MOD_ALT: Alt (Meta, Mod1); tmux's M-
 * @GSURF_WMUX_MOD_SUPER: Super (Logo, Mod4); never part of a wmux key
 *
 * Modifier bits for gsurf_wmux_keys_from_event(). The values are those of
 * the core's #GsurfKeyMod, which is what #GsurfInputHandler hooks receive,
 * so a module passes the hook's @state straight through. Raw GDK masks go
 * through gsurf_wmux_keys_mods_from_gdk() first.
 */
typedef enum {
	GSURF_WMUX_MOD_NONE  = 0,
	GSURF_WMUX_MOD_SHIFT = 1 << 0,
	GSURF_WMUX_MOD_CTRL  = 1 << 1,
	GSURF_WMUX_MOD_ALT   = 1 << 2,
	GSURF_WMUX_MOD_SUPER = 1 << 3
} GsurfWmuxMod;

/**
 * GSURF_WMUX_KEYS_ERROR:
 *
 * Error domain for key notation parsing.
 */
#define GSURF_WMUX_KEYS_ERROR (gsurf_wmux_keys_error_quark())

/**
 * GsurfWmuxKeysError:
 * @GSURF_WMUX_KEYS_ERROR_EMPTY: the key string is empty
 * @GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY: the key name is not known
 * @GSURF_WMUX_KEYS_ERROR_UNKNOWN_MODIFIER: a modifier is not known
 * @GSURF_WMUX_KEYS_ERROR_NO_KEY: modifiers without a key ("C-", "Ctrl+")
 * @GSURF_WMUX_KEYS_ERROR_SUPER: the string names Super, which wmux
 *   does not bind
 *
 * Error codes in %GSURF_WMUX_KEYS_ERROR.
 */
typedef enum {
	GSURF_WMUX_KEYS_ERROR_EMPTY,
	GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY,
	GSURF_WMUX_KEYS_ERROR_UNKNOWN_MODIFIER,
	GSURF_WMUX_KEYS_ERROR_NO_KEY,
	GSURF_WMUX_KEYS_ERROR_SUPER
} GsurfWmuxKeysError;

G_GNUC_INTERNAL GQuark gsurf_wmux_keys_error_quark(void);

/**
 * gsurf_wmux_keys_normalize:
 * @text: a key in tmux ("C-b", "M-S-Up", "^b") or gsurf ("Ctrl+b",
 *   "Alt+Shift+Up", "Ctrl+Shift+percent") notation; key names are the
 *   tmux names, GDK keysym names ("percent", "Return", "Page_Up") or a
 *   single character
 * @error: return location for a #GError
 *
 * Normalizes a configured key to its canonical wmux name.
 *
 * Returns: (transfer full) (nullable): the canonical name, or %NULL with
 *   @error set
 */
G_GNUC_INTERNAL gchar *gsurf_wmux_keys_normalize(const gchar *text, GError **error);

/**
 * gsurf_wmux_keys_from_event:
 * @keyval: a GDK keyval, as delivered (Shift+5 arrives as "percent")
 * @mods: #GsurfWmuxMod bits; any other bit is ignored
 *
 * Names a key press. Shifted printables name their character (Shift+5 ->
 * "%", Ctrl+Shift+5 -> "C-%"); Shift on a lowercase letter or unshifted
 * US punctuation (synthesized events) is applied as a US keyboard would.
 *
 * Returns: (transfer full) (nullable): the canonical name, or %NULL for a
 *   bare modifier press, a key wmux has no name for, or Super held
 */
G_GNUC_INTERNAL gchar *gsurf_wmux_keys_from_event(guint keyval, guint mods);

/**
 * gsurf_wmux_keys_mods_from_gdk:
 * @gdk_state: a #GdkModifierType mask (GTK3 or GTK4)
 *
 * Converts a raw GDK modifier mask to #GsurfWmuxMod bits. Shift, Control,
 * Alt (Mod1) and Meta map; Super and Hyper map to
 * %GSURF_WMUX_MOD_SUPER; Lock, NumLock (Mod2), the other ModN bits and the
 * mouse button bits are dropped.
 *
 * Returns: the #GsurfWmuxMod bits
 */
G_GNUC_INTERNAL guint  gsurf_wmux_keys_mods_from_gdk(guint gdk_state);

/**
 * gsurf_wmux_keys_equal:
 * @a: a key in any accepted notation
 * @b: a key in any accepted notation
 *
 * Returns: %TRUE when both parse and name the same key
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_keys_equal(const gchar *a, const gchar *b);

G_END_DECLS

#endif /* GSURF_WMUX_KEYS_H */
