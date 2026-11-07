/*
 * gsurf-languages.h - The languages pages see (navigator.language,
 * Accept-Language)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GSURF_LANGUAGES_H
#define GSURF_LANGUAGES_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GSURF_LANGUAGES_FALLBACK:
 *
 * The language used when the locale names none (the C / POSIX locale),
 * as Firefox and Chromium do.
 */
#define GSURF_LANGUAGES_FALLBACK "en-US"

/**
 * gsurf_languages_normalize_tag:
 * @name: a language: a BCP 47 tag ("en-US") or a POSIX locale name
 *   ("en_US.UTF-8", "de_DE@euro")
 *
 * Turns @name into a BCP 47 language tag: the codeset (".UTF-8") and
 * modifier ("@euro") go, "_" becomes "-". "C" and "POSIX" are not
 * languages, and neither is anything but letters and digits in subtags of
 * 1-8 characters with a first subtag of 2-3 letters (or "i" / "x").
 *
 * Returns: (transfer full) (nullable): the tag, or %NULL when @name is
 *   not a language
 */
gchar  *gsurf_languages_normalize_tag(const gchar *name);

/**
 * gsurf_languages_from_names:
 * @names: (array zero-terminated=1) (nullable): languages, most preferred
 *   first (g_get_language_names(), or a config list)
 *
 * Normalizes every entry (gsurf_languages_normalize_tag()), drops the ones
 * that are not languages and duplicates, and keeps the order.
 *
 * Returns: (transfer full) (array zero-terminated=1): the tags; never
 *   empty: #GSURF_LANGUAGES_FALLBACK when none is left
 */
gchar **gsurf_languages_from_names(const gchar * const *names);

/**
 * gsurf_languages_resolve:
 * @configured: (array zero-terminated=1) (nullable): `browser.languages`
 *
 * The languages to give the web engine: @configured when it names at
 * least one language, else the process locale's (LANGUAGE, LC_ALL,
 * LC_MESSAGES, LANG); #GSURF_LANGUAGES_FALLBACK under the C locale.
 *
 * Returns: (transfer full) (array zero-terminated=1): the tags, never empty
 */
gchar **gsurf_languages_resolve(const gchar * const *configured);

G_END_DECLS

#endif /* GSURF_LANGUAGES_H */
