/*
 * gsurf-uri-parameters.h - GSURF URI Parameters Boxed Type
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Associates a URI pattern (regex/glob) with a set of per-URI web-engine
 * setting overrides, expressed as a string -> string mapping. Mirrors
 * surf's per-site "parameters" feature.
 */

#ifndef GSURF_URI_PARAMETERS_H
#define GSURF_URI_PARAMETERS_H

#include <glib-object.h>
#include "../gsurf-types.h"
#include "../gsurf-enums.h"

G_BEGIN_DECLS

#define GSURF_TYPE_URI_PARAMETERS (gsurf_uri_parameters_get_type())

/**
 * GsurfUriParameters:
 * @pattern: the regex/glob pattern matched against URIs
 * @overrides: (element-type utf8 utf8): per-URI setting overrides
 *
 * A set of web-engine setting overrides applied to URIs matching
 * @pattern.
 */
struct _GsurfUriParameters {
	gchar		*pattern;	/* URI match pattern */
	GHashTable	*overrides;	/* gchar* key -> gchar* value */
};

/**
 * gsurf_uri_parameters_get_type:
 *
 * Gets the GType for the GsurfUriParameters boxed type.
 *
 * Returns: the GType
 */
GType gsurf_uri_parameters_get_type(void) G_GNUC_CONST;

/**
 * gsurf_uri_parameters_new:
 * @pattern: (nullable): the regex/glob pattern matched against URIs
 *
 * Creates a new URI parameter set with an empty overrides table.
 *
 * Returns: (transfer full): a new #GsurfUriParameters
 */
GsurfUriParameters *gsurf_uri_parameters_new(const gchar *pattern);

/**
 * gsurf_uri_parameters_copy:
 * @params: a #GsurfUriParameters
 *
 * Creates a deep copy of the URI parameter set, duplicating the pattern
 * and every override entry.
 *
 * Returns: (transfer full): a copy of @params
 */
GsurfUriParameters *gsurf_uri_parameters_copy(const GsurfUriParameters *params);

/**
 * gsurf_uri_parameters_free:
 * @params: a #GsurfUriParameters
 *
 * Frees the URI parameter set, its pattern and its overrides table.
 */
void gsurf_uri_parameters_free(GsurfUriParameters *params);

/**
 * gsurf_uri_parameters_get_pattern:
 * @params: a #GsurfUriParameters
 *
 * Returns: (nullable) (transfer none): the URI match pattern, or %NULL
 */
const gchar *gsurf_uri_parameters_get_pattern(const GsurfUriParameters *params);

/**
 * gsurf_uri_parameters_set_pattern:
 * @params: a #GsurfUriParameters
 * @pattern: (nullable): the URI match pattern (deep-copied)
 *
 * Sets the URI match pattern, freeing any previous value.
 */
void gsurf_uri_parameters_set_pattern(GsurfUriParameters *params, const gchar *pattern);

/**
 * gsurf_uri_parameters_set_override:
 * @params: a #GsurfUriParameters
 * @key: the setting name
 * @value: the setting value (deep-copied)
 *
 * Sets a per-URI setting override, replacing any existing value for @key.
 */
void gsurf_uri_parameters_set_override(GsurfUriParameters *params, const gchar *key, const gchar *value);

/**
 * gsurf_uri_parameters_get_override:
 * @params: a #GsurfUriParameters
 * @key: the setting name
 *
 * Returns: (nullable) (transfer none): the override value for @key, or
 *   %NULL if not set
 */
const gchar *gsurf_uri_parameters_get_override(GsurfUriParameters *params, const gchar *key);

/**
 * gsurf_uri_parameters_get_overrides:
 * @params: a #GsurfUriParameters
 *
 * Returns: (transfer none) (element-type utf8 utf8): the overrides table
 */
GHashTable *gsurf_uri_parameters_get_overrides(GsurfUriParameters *params);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfUriParameters, gsurf_uri_parameters_free)

G_END_DECLS

#endif /* GSURF_URI_PARAMETERS_H */
