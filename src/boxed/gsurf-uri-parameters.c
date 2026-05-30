/*
 * gsurf-uri-parameters.c - GSURF URI Parameters Boxed Type Implementation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of the GsurfUriParameters boxed type pairing a URI
 * pattern with a string -> string map of per-URI web-engine overrides.
 */

#include "gsurf-uri-parameters.h"

G_DEFINE_BOXED_TYPE(GsurfUriParameters, gsurf_uri_parameters, gsurf_uri_parameters_copy, gsurf_uri_parameters_free)

/*
 * gsurf_uri_parameters_new:
 * @pattern: (nullable): the regex/glob pattern matched against URIs
 *
 * Creates a new URI parameter set with an empty overrides table whose
 * keys and values are owned (freed with g_free()).
 *
 * Returns: (transfer full): a newly allocated #GsurfUriParameters
 */
GsurfUriParameters *
gsurf_uri_parameters_new(const gchar *pattern)
{
	GsurfUriParameters *params;

	params = g_slice_new(GsurfUriParameters);
	params->pattern = g_strdup(pattern);
	params->overrides = g_hash_table_new_full(
		g_str_hash,
		g_str_equal,
		g_free,
		g_free
	);

	return params;
}

/*
 * gsurf_uri_parameters_copy:
 * @params: a #GsurfUriParameters to copy
 *
 * Creates a deep copy of the URI parameter set, duplicating the pattern
 * and every override entry into a fresh hash table.
 *
 * Returns: (transfer full): a copy of @params
 */
GsurfUriParameters *
gsurf_uri_parameters_copy(const GsurfUriParameters *params)
{
	GsurfUriParameters	*copy;
	GHashTableIter		iter;
	gpointer		key;
	gpointer		value;

	g_return_val_if_fail(params != NULL, NULL);

	copy = gsurf_uri_parameters_new(params->pattern);

	g_hash_table_iter_init(&iter, params->overrides);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		g_hash_table_insert(
			copy->overrides,
			g_strdup((const gchar *)key),
			g_strdup((const gchar *)value)
		);
	}

	return copy;
}

/*
 * gsurf_uri_parameters_free:
 * @params: a #GsurfUriParameters to free
 *
 * Frees the URI parameter set, its pattern and its overrides table.
 */
void
gsurf_uri_parameters_free(GsurfUriParameters *params)
{
	if (params != NULL) {
		g_free(params->pattern);
		g_hash_table_unref(params->overrides);
		g_slice_free(GsurfUriParameters, params);
	}
}

/*
 * gsurf_uri_parameters_get_pattern:
 * @params: a #GsurfUriParameters
 *
 * Returns: (nullable) (transfer none): the URI match pattern, or %NULL
 */
const gchar *
gsurf_uri_parameters_get_pattern(const GsurfUriParameters *params)
{
	g_return_val_if_fail(params != NULL, NULL);

	return params->pattern;
}

/*
 * gsurf_uri_parameters_set_pattern:
 * @params: a #GsurfUriParameters
 * @pattern: (nullable): the URI match pattern (deep-copied)
 *
 * Sets the URI match pattern, freeing any previous value.
 */
void
gsurf_uri_parameters_set_pattern(
	GsurfUriParameters	*params,
	const gchar		*pattern
){
	g_return_if_fail(params != NULL);

	g_free(params->pattern);
	params->pattern = g_strdup(pattern);
}

/*
 * gsurf_uri_parameters_set_override:
 * @params: a #GsurfUriParameters
 * @key: the setting name
 * @value: the setting value (deep-copied)
 *
 * Sets a per-URI setting override, replacing any existing value for @key.
 */
void
gsurf_uri_parameters_set_override(
	GsurfUriParameters	*params,
	const gchar		*key,
	const gchar		*value
){
	g_return_if_fail(params != NULL);
	g_return_if_fail(key != NULL);

	g_hash_table_insert(params->overrides, g_strdup(key), g_strdup(value));
}

/*
 * gsurf_uri_parameters_get_override:
 * @params: a #GsurfUriParameters
 * @key: the setting name
 *
 * Returns: (nullable) (transfer none): the override value for @key, or
 *   %NULL if not set
 */
const gchar *
gsurf_uri_parameters_get_override(
	GsurfUriParameters	*params,
	const gchar		*key
){
	g_return_val_if_fail(params != NULL, NULL);
	g_return_val_if_fail(key != NULL, NULL);

	return g_hash_table_lookup(params->overrides, key);
}

/*
 * gsurf_uri_parameters_get_overrides:
 * @params: a #GsurfUriParameters
 *
 * Returns: (transfer none) (element-type utf8 utf8): the overrides table
 */
GHashTable *
gsurf_uri_parameters_get_overrides(GsurfUriParameters *params)
{
	g_return_val_if_fail(params != NULL, NULL);

	return params->overrides;
}
