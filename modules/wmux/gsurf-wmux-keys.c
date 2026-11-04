/*
 * gsurf-wmux-keys.c - Key notation for the wmux module
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A key is held as a small value while it is parsed or decoded: modifier
 * bits plus either a printable character or a named key. Both entry
 * points (a config string, a key event) build that value and then apply
 * the same canonicalization (Shift folds into printables, Tab+Shift is
 * BTab) and printer, so the two always agree.
 */

#include "gsurf-wmux-keys.h"

#include <string.h>
#include <gdk/gdkkeysyms.h>

/* GDK modifier bits, hard-coded so this file needs no GDK headers beyond
 * the keysym #defines. The values are identical in GTK3 (gdktypes.h) and
 * GTK4 (gdkenums.h). */
#define GDK_BIT_SHIFT   (1u << 0)
#define GDK_BIT_CONTROL (1u << 2)
#define GDK_BIT_MOD1    (1u << 3)
#define GDK_BIT_SUPER   (1u << 26)
#define GDK_BIT_HYPER   (1u << 27)
#define GDK_BIT_META    (1u << 28)

G_DEFINE_QUARK(gsurf-wmux-keys-error-quark, gsurf_wmux_keys_error)

/* ------------------------------------------------------------------ */
/* Tables                                                              */
/* ------------------------------------------------------------------ */

/* A named (non-printable, or Space) key: its canonical tmux spelling, the
 * keyval GDK delivers for it, and other spellings accepted in config
 * (GDK keysym names, tmux aliases). Lookups ignore case. */
typedef struct {
	const gchar *name;
	guint        keyval;
	const gchar *aliases[5];
} NamedKey;

static const NamedKey named_keys[] = {
	{ "Space",   GDK_KEY_space,        { "space", "SPC", NULL } },
	{ "Enter",   GDK_KEY_Return,       { "Return", "RET", "ISO_Enter", NULL } },
	{ "Escape",  GDK_KEY_Escape,       { "Esc", NULL } },
	{ "Tab",     GDK_KEY_Tab,          { NULL } },
	{ "BTab",    GDK_KEY_ISO_Left_Tab, { "ISO_Left_Tab", "BackTab", NULL } },
	{ "BSpace",  GDK_KEY_BackSpace,    { "BackSpace", NULL } },
	{ "Up",      GDK_KEY_Up,           { NULL } },
	{ "Down",    GDK_KEY_Down,         { NULL } },
	{ "Left",    GDK_KEY_Left,         { NULL } },
	{ "Right",   GDK_KEY_Right,        { NULL } },
	{ "Home",    GDK_KEY_Home,         { NULL } },
	{ "End",     GDK_KEY_End,          { NULL } },
	{ "PPage",   GDK_KEY_Page_Up,      { "Page_Up", "PageUp", "PgUp", "Prior", NULL } },
	{ "NPage",   GDK_KEY_Page_Down,    { "Page_Down", "PageDown", "PgDn", "Next", NULL } },
	{ "IC",      GDK_KEY_Insert,       { "Insert", NULL } },
	{ "DC",      GDK_KEY_Delete,       { "Delete", NULL } },
	{ "F1",      GDK_KEY_F1,           { NULL } },
	{ "F2",      GDK_KEY_F2,           { NULL } },
	{ "F3",      GDK_KEY_F3,           { NULL } },
	{ "F4",      GDK_KEY_F4,           { NULL } },
	{ "F5",      GDK_KEY_F5,           { NULL } },
	{ "F6",      GDK_KEY_F6,           { NULL } },
	{ "F7",      GDK_KEY_F7,           { NULL } },
	{ "F8",      GDK_KEY_F8,           { NULL } },
	{ "F9",      GDK_KEY_F9,           { NULL } },
	{ "F10",     GDK_KEY_F10,          { NULL } },
	{ "F11",     GDK_KEY_F11,          { NULL } },
	{ "F12",     GDK_KEY_F12,          { NULL } },
	{ "KP0",     GDK_KEY_KP_0,         { "KP_0", NULL } },
	{ "KP1",     GDK_KEY_KP_1,         { "KP_1", NULL } },
	{ "KP2",     GDK_KEY_KP_2,         { "KP_2", NULL } },
	{ "KP3",     GDK_KEY_KP_3,         { "KP_3", NULL } },
	{ "KP4",     GDK_KEY_KP_4,         { "KP_4", NULL } },
	{ "KP5",     GDK_KEY_KP_5,         { "KP_5", NULL } },
	{ "KP6",     GDK_KEY_KP_6,         { "KP_6", NULL } },
	{ "KP7",     GDK_KEY_KP_7,         { "KP_7", NULL } },
	{ "KP8",     GDK_KEY_KP_8,         { "KP_8", NULL } },
	{ "KP9",     GDK_KEY_KP_9,         { "KP_9", NULL } },
	{ "KPEnter", GDK_KEY_KP_Enter,     { "KP_Enter", NULL } },
};

/* Keypad navigation keyvals (NumLock off) and their main-block names.
 * Events only: config strings name the main keys. */
static const struct {
	guint        keyval;
	const gchar *name;
} keypad_aliases[] = {
	{ GDK_KEY_KP_Up,        "Up"    },
	{ GDK_KEY_KP_Down,      "Down"  },
	{ GDK_KEY_KP_Left,      "Left"  },
	{ GDK_KEY_KP_Right,     "Right" },
	{ GDK_KEY_KP_Home,      "Home"  },
	{ GDK_KEY_KP_End,       "End"   },
	{ GDK_KEY_KP_Page_Up,   "PPage" },
	{ GDK_KEY_KP_Page_Down, "NPage" },
	{ GDK_KEY_KP_Insert,    "IC"    },
	{ GDK_KEY_KP_Delete,    "DC"    },
	{ GDK_KEY_KP_Tab,       "Tab"   },
};

/* GDK keysym names of the ASCII punctuation, so gsurf notation
 * ("Ctrl+Shift+percent", "Alt+period") reaches the character. */
static const struct {
	const gchar *name;
	gchar        ch;
} keysym_punct[] = {
	{ "exclam", '!' },       { "quotedbl", '"' },     { "numbersign", '#' },
	{ "dollar", '$' },       { "percent", '%' },      { "ampersand", '&' },
	{ "apostrophe", '\'' },  { "quoteright", '\'' },  { "parenleft", '(' },
	{ "parenright", ')' },   { "asterisk", '*' },     { "plus", '+' },
	{ "comma", ',' },        { "minus", '-' },        { "period", '.' },
	{ "slash", '/' },        { "colon", ':' },        { "semicolon", ';' },
	{ "less", '<' },         { "equal", '=' },        { "greater", '>' },
	{ "question", '?' },     { "at", '@' },           { "bracketleft", '[' },
	{ "backslash", '\\' },   { "bracketright", ']' }, { "asciicircum", '^' },
	{ "underscore", '_' },   { "grave", '`' },        { "quoteleft", '`' },
	{ "braceleft", '{' },    { "bar", '|' },          { "braceright", '}' },
	{ "asciitilde", '~' },
};

/* The US layout's Shift row: what Shift turns an unshifted character
 * into. Used for config strings ("S-5", "Shift+apostrophe") and for
 * synthesized events that carry Shift with the unshifted keyval. */
static const gchar us_unshifted[] = "1234567890-=[]\\;',./`";
static const gchar us_shifted[]   = "!@#$%^&*()_+{}|:\"<>?~";

/* ------------------------------------------------------------------ */
/* The intermediate key value                                          */
/* ------------------------------------------------------------------ */

/* A key: modifiers plus exactly one of a printable character (@ch != 0)
 * or a named key (@named != NULL, an entry of named_keys[]). */
typedef struct {
	guint           mods;
	gunichar        ch;
	const NamedKey *named;
} WmuxKey;

/* Find a named key by canonical name or alias, ignoring case. */
static const NamedKey *
find_named(const gchar *name)
{
	guint i;
	guint j;

	for (i = 0; i < G_N_ELEMENTS(named_keys); i++) {
		if (g_ascii_strcasecmp(name, named_keys[i].name) == 0)
			return &named_keys[i];
		for (j = 0; named_keys[i].aliases[j] != NULL; j++)
			if (g_ascii_strcasecmp(name, named_keys[i].aliases[j]) == 0)
				return &named_keys[i];
	}
	return NULL;
}

/* Find a named key by canonical name, exactly (for the keypad aliases). */
static const NamedKey *
find_named_exact(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(named_keys); i++)
		if (strcmp(name, named_keys[i].name) == 0)
			return &named_keys[i];
	return NULL;
}

/* Apply Shift to a printable character the way a US keyboard does:
 * letters uppercase, the digit and punctuation row shift, everything
 * else (already shifted, or with no shifted form) is unchanged. */
static gunichar
shift_char(gunichar ch)
{
	const gchar *p;

	if (ch < 0x80) {
		if (g_ascii_isalpha((gchar)ch))
			return (gunichar)g_ascii_toupper((gchar)ch);
		p = strchr(us_unshifted, (gint)ch);
		if (ch != 0 && p != NULL)
			return (gunichar)us_shifted[p - us_unshifted];
		return ch;
	}
	return g_unichar_toupper(ch);
}

/* Canonicalize in place. Shift is always consumed by a printable (it is
 * folded into the character); on named keys it stays, except Tab, where
 * Shift+Tab is BTab. Space is a named key, so S-Space keeps its S-. */
static void
canonicalize(WmuxKey *key)
{
	if (key->ch != 0) {
		if ((key->mods & GSURF_WMUX_MOD_SHIFT) != 0) {
			key->ch = shift_char(key->ch);
			key->mods &= ~(guint)GSURF_WMUX_MOD_SHIFT;
		}
		return;
	}
	if (key->named != NULL && strcmp(key->named->name, "Tab") == 0 &&
	    (key->mods & GSURF_WMUX_MOD_SHIFT) != 0) {
		key->named = find_named_exact("BTab");
		key->mods &= ~(guint)GSURF_WMUX_MOD_SHIFT;
	} else if (key->named != NULL && strcmp(key->named->name, "BTab") == 0) {
		key->mods &= ~(guint)GSURF_WMUX_MOD_SHIFT;
	}
}

/* Print "C-M-S-<key>" in the canonical order. */
static gchar *
key_to_string(const WmuxKey *key)
{
	GString *out = g_string_new(NULL);

	if ((key->mods & GSURF_WMUX_MOD_CTRL) != 0)
		g_string_append(out, "C-");
	if ((key->mods & GSURF_WMUX_MOD_ALT) != 0)
		g_string_append(out, "M-");
	if ((key->mods & GSURF_WMUX_MOD_SHIFT) != 0)
		g_string_append(out, "S-");
	if (key->ch != 0)
		g_string_append_unichar(out, key->ch);
	else
		g_string_append(out, key->named->name);
	return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------------ */
/* Parsing config strings                                              */
/* ------------------------------------------------------------------ */

/* Resolve the key part of a config string (after the modifiers): a
 * single character, a tmux or GDK name, or a GDK punctuation name. */
static gboolean
resolve_key_name(const gchar *name, WmuxKey *key, const gchar *whole, GError **error)
{
	const NamedKey *named;
	guint i;

	if (*name == '\0') {
		g_set_error(error, GSURF_WMUX_KEYS_ERROR, GSURF_WMUX_KEYS_ERROR_NO_KEY,
			"key \"%s\" has modifiers but no key", whole);
		return FALSE;
	}

	/* One character (any UTF-8 character) stands for itself; a literal
	 * space is the Space key. */
	if (g_utf8_validate(name, -1, NULL) && g_utf8_strlen(name, -1) == 1) {
		gunichar ch = g_utf8_get_char(name);

		if (ch == ' ') {
			key->named = find_named_exact("Space");
			return TRUE;
		}
		if (!g_unichar_isprint(ch)) {
			g_set_error(error, GSURF_WMUX_KEYS_ERROR,
				GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY,
				"key \"%s\" is not a printable character", whole);
			return FALSE;
		}
		key->ch = ch;
		return TRUE;
	}

	named = find_named(name);
	if (named != NULL) {
		key->named = named;
		return TRUE;
	}
	for (i = 0; i < G_N_ELEMENTS(keysym_punct); i++) {
		if (g_ascii_strcasecmp(name, keysym_punct[i].name) == 0) {
			key->ch = (gunichar)keysym_punct[i].ch;
			return TRUE;
		}
	}
	g_set_error(error, GSURF_WMUX_KEYS_ERROR, GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY,
		"unknown key name \"%s\" in \"%s\"", name, whole);
	return FALSE;
}

/* A gsurf modifier word ("Ctrl", "Alt", ...) to its bit; Super and Hyper
 * map to SUPER so the caller can reject them with a precise error. 0 when
 * @word is not a modifier. */
static guint
gsurf_modifier_word(const gchar *word, gsize len)
{
	static const struct {
		const gchar *word;
		guint        bit;
	} words[] = {
		{ "ctrl", GSURF_WMUX_MOD_CTRL },  { "control", GSURF_WMUX_MOD_CTRL },
		{ "alt", GSURF_WMUX_MOD_ALT },    { "mod1", GSURF_WMUX_MOD_ALT },
		{ "meta", GSURF_WMUX_MOD_ALT },   { "shift", GSURF_WMUX_MOD_SHIFT },
		{ "super", GSURF_WMUX_MOD_SUPER }, { "logo", GSURF_WMUX_MOD_SUPER },
		{ "mod4", GSURF_WMUX_MOD_SUPER },  { "hyper", GSURF_WMUX_MOD_SUPER },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(words); i++)
		if (strlen(words[i].word) == len &&
		    g_ascii_strncasecmp(word, words[i].word, len) == 0)
			return words[i].bit;
	return 0;
}

/* Length of the leading run of [A-Za-z0-9_] in @s. */
static gsize
word_length(const gchar *s)
{
	gsize n = 0;

	while (g_ascii_isalnum(s[n]) || s[n] == '_')
		n++;
	return n;
}

gchar *
gsurf_wmux_keys_normalize(const gchar *text, GError **error)
{
	g_autofree gchar *copy = NULL;
	WmuxKey key = { 0, 0, NULL };
	const gchar *s;
	gsize wlen;

	if (text == NULL || *text == '\0') {
		g_set_error_literal(error, GSURF_WMUX_KEYS_ERROR,
			GSURF_WMUX_KEYS_ERROR_EMPTY, "empty key");
		return NULL;
	}
	if (!g_utf8_validate(text, -1, NULL)) {
		g_set_error_literal(error, GSURF_WMUX_KEYS_ERROR,
			GSURF_WMUX_KEYS_ERROR_UNKNOWN_KEY, "key is not valid UTF-8");
		return NULL;
	}
	/* Surrounding whitespace is ignored, except a lone " " (Space). */
	copy = g_strdup(text);
	if (strcmp(copy, " ") != 0)
		g_strstrip(copy);
	if (*copy == '\0') {
		g_set_error_literal(error, GSURF_WMUX_KEYS_ERROR,
			GSURF_WMUX_KEYS_ERROR_EMPTY, "empty key");
		return NULL;
	}
	s = copy;

	/* gsurf notation: a word followed by '+' and something after it
	 * ("Ctrl+b", "Ctrl++"). Every word before a '+' must be a modifier. */
	wlen = word_length(s);
	if (wlen > 0 && s[wlen] == '+' && s[wlen + 1] != '\0') {
		while ((wlen = word_length(s)) > 0 && s[wlen] == '+' && s[wlen + 1] != '\0') {
			guint bit = gsurf_modifier_word(s, wlen);

			if (bit == 0) {
				/* The last segment may itself be a key name
				 * followed by nothing; anything else before a
				 * '+' must be a modifier. */
				g_set_error(error, GSURF_WMUX_KEYS_ERROR,
					GSURF_WMUX_KEYS_ERROR_UNKNOWN_MODIFIER,
					"unknown modifier \"%.*s\" in \"%s\"", (gint)wlen, s, text);
				return NULL;
			}
			key.mods |= bit;
			s += wlen + 1;
		}
		/* "Ctrl+" with nothing after was excluded above, but
		 * "Ctrl+Shift+" ends on a modifier word plus '+'. */
		wlen = word_length(s);
		if (wlen > 0 && s[wlen] == '+' && s[wlen + 1] == '\0' &&
		    gsurf_modifier_word(s, wlen) != 0) {
			g_set_error(error, GSURF_WMUX_KEYS_ERROR, GSURF_WMUX_KEYS_ERROR_NO_KEY,
				"key \"%s\" has modifiers but no key", text);
			return NULL;
		}
	} else if (wlen > 0 && s[wlen] == '+' && s[wlen + 1] == '\0' &&
	           gsurf_modifier_word(s, wlen) != 0) {
		/* "Ctrl+" */
		g_set_error(error, GSURF_WMUX_KEYS_ERROR, GSURF_WMUX_KEYS_ERROR_NO_KEY,
			"key \"%s\" has modifiers but no key", text);
		return NULL;
	} else {
		/* tmux notation: "^x" is C-x; then any run of "C-", "M-",
		 * "S-" (either case) before the key. "C--" is Ctrl+minus. */
		if (s[0] == '^' && s[1] != '\0' && g_utf8_strlen(s + 1, -1) == 1) {
			key.mods |= GSURF_WMUX_MOD_CTRL;
			s++;
		}
		while (s[0] != '\0' && s[1] == '-' && strchr("CcMmSs", s[0]) != NULL) {
			if (s[2] == '\0') {
				g_set_error(error, GSURF_WMUX_KEYS_ERROR,
					GSURF_WMUX_KEYS_ERROR_NO_KEY,
					"key \"%s\" has modifiers but no key", text);
				return NULL;
			}
			switch (s[0]) {
			case 'C': case 'c': key.mods |= GSURF_WMUX_MOD_CTRL;  break;
			case 'M': case 'm': key.mods |= GSURF_WMUX_MOD_ALT;   break;
			default:            key.mods |= GSURF_WMUX_MOD_SHIFT; break;
			}
			s += 2;
		}
	}

	if ((key.mods & GSURF_WMUX_MOD_SUPER) != 0) {
		g_set_error(error, GSURF_WMUX_KEYS_ERROR, GSURF_WMUX_KEYS_ERROR_SUPER,
			"key \"%s\" uses Super, which wmux leaves to the compositor", text);
		return NULL;
	}
	if (!resolve_key_name(s, &key, text, error))
		return NULL;

	canonicalize(&key);
	return key_to_string(&key);
}

/* ------------------------------------------------------------------ */
/* Key events                                                          */
/* ------------------------------------------------------------------ */

guint
gsurf_wmux_keys_mods_from_gdk(guint gdk_state)
{
	guint mods = GSURF_WMUX_MOD_NONE;

	if ((gdk_state & GDK_BIT_SHIFT) != 0)
		mods |= GSURF_WMUX_MOD_SHIFT;
	if ((gdk_state & GDK_BIT_CONTROL) != 0)
		mods |= GSURF_WMUX_MOD_CTRL;
	if ((gdk_state & (GDK_BIT_MOD1 | GDK_BIT_META)) != 0)
		mods |= GSURF_WMUX_MOD_ALT;
	if ((gdk_state & (GDK_BIT_SUPER | GDK_BIT_HYPER)) != 0)
		mods |= GSURF_WMUX_MOD_SUPER;
	return mods;
}

gchar *
gsurf_wmux_keys_from_event(guint keyval, guint mods)
{
	WmuxKey key = { 0, 0, NULL };
	guint i;

	/* Super belongs to the compositor; such a chord has no wmux name. */
	if ((mods & GSURF_WMUX_MOD_SUPER) != 0)
		return NULL;
	key.mods = mods & (GSURF_WMUX_MOD_SHIFT | GSURF_WMUX_MOD_CTRL | GSURF_WMUX_MOD_ALT);

	/* Named keys first (this includes Space, keyval 0x20). */
	for (i = 0; i < G_N_ELEMENTS(named_keys); i++)
		if (named_keys[i].keyval == keyval) {
			key.named = &named_keys[i];
			break;
		}
	for (i = 0; key.named == NULL && i < G_N_ELEMENTS(keypad_aliases); i++)
		if (keypad_aliases[i].keyval == keyval)
			key.named = find_named_exact(keypad_aliases[i].name);

	if (key.named == NULL) {
		/* Printables: Latin-1 keyvals are their code point; the
		 * 0x01000000 range carries any other Unicode character.
		 * Everything else (modifier keys, media keys, dead keys) has
		 * no wmux name. */
		gunichar ch = 0;

		if ((keyval >= 0x21 && keyval <= 0x7e) || (keyval >= 0xa1 && keyval <= 0xff))
			ch = (gunichar)keyval;
		else if (keyval >= 0x01000100 && keyval <= 0x0110ffff)
			ch = (gunichar)(keyval - 0x01000000);
		if (ch == 0 || !g_unichar_isprint(ch))
			return NULL;

		/* Letters: the case follows Shift, not the keyval, so Caps
		 * Lock (dropped from @mods) never turns "c" into "C". */
		if (ch < 0x80 && g_ascii_isalpha((gchar)ch)) {
			ch = (gunichar)((key.mods & GSURF_WMUX_MOD_SHIFT) != 0
				? g_ascii_toupper((gchar)ch) : g_ascii_tolower((gchar)ch));
			key.mods &= ~(guint)GSURF_WMUX_MOD_SHIFT;
		}
		key.ch = ch;
	}

	canonicalize(&key);
	return key_to_string(&key);
}

gboolean
gsurf_wmux_keys_equal(const gchar *a, const gchar *b)
{
	g_autofree gchar *na = gsurf_wmux_keys_normalize(a, NULL);
	g_autofree gchar *nb = gsurf_wmux_keys_normalize(b, NULL);

	return na != NULL && nb != NULL && strcmp(na, nb) == 0;
}
