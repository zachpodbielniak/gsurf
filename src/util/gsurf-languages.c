/*
 * gsurf-languages.c - The languages pages see (navigator.language,
 * Accept-Language)
 *
 * WebKit derives them from the locale when it is told nothing, and under
 * the C locale it reports "C", which is not a language tag: Intl APIs
 * throw RangeError on it and break pages. gsurf always tells it.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "util/gsurf-languages.h"

#include <string.h>

/* Whether @subtag (one "-" piece) is 1-8 letters or digits. */
static gboolean
subtag_is_valid(const gchar *subtag)
{
	gsize len = strlen(subtag);
	gsize i;

	if (len < 1 || len > 8)
		return FALSE;
	for (i = 0; i < len; i++) {
		if (!g_ascii_isalnum(subtag[i]))
			return FALSE;
	}
	return TRUE;
}

gchar *
gsurf_languages_normalize_tag(const gchar *name)
{
	g_autofree gchar *tag = NULL;
	g_auto(GStrv) subtags = NULL;
	gchar *cut;
	gsize first_len;
	guint i;

	if (name == NULL)
		return NULL;

	/* 1. A POSIX locale name: drop ".codeset" and "@modifier", "_" -> "-". */
	tag = g_strstrip(g_strdup(name));
	cut = strpbrk(tag, ".@");
	if (cut != NULL)
		*cut = '\0';
	g_strdelimit(tag, "_", '-');
	if (*tag == '\0' || g_ascii_strcasecmp(tag, "C") == 0 ||
	    g_ascii_strcasecmp(tag, "POSIX") == 0)
		return NULL;

	/* 2. BCP 47 shape: alphanumeric subtags; the first is a 2-3 letter
	 *    language (or the "i" / "x" prefixes). */
	subtags = g_strsplit(tag, "-", -1);
	for (i = 0; subtags[i] != NULL; i++) {
		if (!subtag_is_valid(subtags[i]))
			return NULL;
	}
	first_len = strlen(subtags[0]);
	if (!((first_len == 2 || first_len == 3) ||
	      g_ascii_strcasecmp(subtags[0], "i") == 0 ||
	      g_ascii_strcasecmp(subtags[0], "x") == 0))
		return NULL;
	for (i = 0; i < first_len; i++) {
		if (!g_ascii_isalpha(subtags[0][i]))
			return NULL;
	}
	return g_steal_pointer(&tag);
}

/* The normalized, de-duplicated languages of @names, in order; may be
 * empty (no fallback). NULL-terminated. */
static GPtrArray *
collect(const gchar * const *names)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	guint i;
	guint j;

	/* The locale lists "en_US.UTF-8", "en_US", "en.UTF-8", "en": that is
	 * en-US, en. */
	for (i = 0; names != NULL && names[i] != NULL; i++) {
		g_autofree gchar *tag = gsurf_languages_normalize_tag(names[i]);
		gboolean seen = FALSE;

		if (tag == NULL)
			continue;
		for (j = 0; j < out->len && !seen; j++)
			seen = g_ascii_strcasecmp(g_ptr_array_index(out, j), tag) == 0;
		if (!seen)
			g_ptr_array_add(out, g_steal_pointer(&tag));
	}
	return out;
}

/* @list as a NULL-terminated vector (consumed). */
static gchar **
to_strv(GPtrArray *list)
{
	g_ptr_array_add(list, NULL);
	return (gchar **)g_ptr_array_free(list, FALSE);
}

gchar **
gsurf_languages_from_names(const gchar * const *names)
{
	GPtrArray *list = collect(names);

	if (list->len == 0)
		g_ptr_array_add(list, g_strdup(GSURF_LANGUAGES_FALLBACK));
	return to_strv(list);
}

gchar **
gsurf_languages_resolve(const gchar * const *configured)
{
	/* A configured list wins when it names at least one language. */
	if (configured != NULL && configured[0] != NULL) {
		GPtrArray *list = collect(configured);

		if (list->len > 0)
			return to_strv(list);
		g_ptr_array_unref(list);
		g_warning("gsurf: browser.languages names no language; using the locale's");
	}
	return gsurf_languages_from_names(g_get_language_names());
}
