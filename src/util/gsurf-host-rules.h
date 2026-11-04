/*
 * gsurf-host-rules.h - Ordered per-host regex rules with attached data
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #GsurfHostRules is an ordered list of rules. Each rule is a
 * case-insensitive PCRE pattern matched against a bare host name
 * ("www.example.com", no scheme or port), plus an optional YAML mapping of
 * per-rule settings. Modules use it for per-site configuration
 * (containers' host assignment, hardening exemptions, dark_mode site
 * overrides, form_autofill exclusions) and hand it to the page or a
 * web-process extension as JSON or a #GVariant.
 *
 * Like the rest of the public API (see gsurf_config_get_module_node()),
 * YAML objects are passed as gpointer so this header does not require
 * yaml-glib: "data" is a YamlMapping*, "node" a YamlNode*.
 */

#ifndef GSURF_HOST_RULES_H
#define GSURF_HOST_RULES_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GSURF_HOST_RULES_ERROR (gsurf_host_rules_error_quark())

/**
 * GsurfHostRulesError:
 * @GSURF_HOST_RULES_ERROR_PATTERN: a pattern is missing, empty or not a
 *   valid regular expression
 * @GSURF_HOST_RULES_ERROR_SHAPE: a rule list element has the wrong YAML shape
 *
 * Errors reported while building a #GsurfHostRules.
 */
typedef enum {
	GSURF_HOST_RULES_ERROR_PATTERN,
	GSURF_HOST_RULES_ERROR_SHAPE
} GsurfHostRulesError;

GQuark gsurf_host_rules_error_quark(void);

#define GSURF_TYPE_HOST_RULES (gsurf_host_rules_get_type())

G_DECLARE_FINAL_TYPE(GsurfHostRules, gsurf_host_rules, GSURF, HOST_RULES, GObject)

/**
 * gsurf_host_rules_new:
 *
 * Returns: (transfer full): a new, empty rule list
 */
GsurfHostRules *gsurf_host_rules_new(void);

/**
 * gsurf_host_rules_add:
 * @self: a #GsurfHostRules
 * @pattern: a regular expression matched against the bare host
 * @data: (nullable) (type gpointer): per-rule settings (a YamlMapping*);
 *   a reference is taken
 * @error: return location for a #GError
 *
 * Appends one rule. Patterns are compiled case-insensitively and are not
 * anchored implicitly: write `(^|\.)example\.com$` to match a domain and
 * its subdomains. Patterns that are evaluated inside the page (see
 * gsurf_host_rules_to_json()) should stay within the syntax PCRE and
 * JavaScript share (no lookbehind, possessive quantifiers or `(?P<…>)`).
 *
 * Returns: %TRUE on success, %FALSE with @error set for a bad pattern
 */
gboolean gsurf_host_rules_add(GsurfHostRules *self, const gchar *pattern,
                              gpointer data, GError **error);

/**
 * gsurf_host_rules_load:
 * @self: a #GsurfHostRules
 * @node: (nullable) (type gpointer): a YamlNode* holding a sequence of rules
 * @match_key: the member naming the pattern in mapping elements (e.g. "match")
 * @error: return location for a #GError
 *
 * Appends every rule of @node. Each element is either a scalar pattern
 * (a rule without data) or a mapping whose @match_key member is the
 * pattern; the whole mapping becomes the rule's data. %NULL or a YAML
 * null adds nothing. On error the rules added before the bad element are
 * kept, so callers that want all-or-nothing should load into a fresh
 * object and swap it in on success.
 *
 * Returns: %TRUE on success
 */
gboolean gsurf_host_rules_load(GsurfHostRules *self, gpointer node,
                               const gchar *match_key, GError **error);

/**
 * gsurf_host_rules_clear:
 * @self: a #GsurfHostRules
 *
 * Removes every rule.
 */
void gsurf_host_rules_clear(GsurfHostRules *self);

/**
 * gsurf_host_rules_get_n_rules:
 * @self: a #GsurfHostRules
 *
 * Returns: the number of rules
 */
guint gsurf_host_rules_get_n_rules(GsurfHostRules *self);

/**
 * gsurf_host_rules_get_pattern:
 * @self: a #GsurfHostRules
 * @index: a rule index
 *
 * Returns: (transfer none) (nullable): the pattern of rule @index
 */
const gchar *gsurf_host_rules_get_pattern(GsurfHostRules *self, guint index);

/**
 * gsurf_host_rules_get_data:
 * @self: a #GsurfHostRules
 * @index: a rule index
 *
 * Returns: (transfer none) (nullable) (type gpointer): the data
 *   (YamlMapping*) of rule @index
 */
gpointer gsurf_host_rules_get_data(GsurfHostRules *self, guint index);

/**
 * gsurf_host_rules_find:
 * @self: a #GsurfHostRules
 * @host: (nullable): a bare host name
 *
 * Returns: the index of the first rule matching @host, or -1 (also for a
 *   %NULL or empty @host)
 */
gint gsurf_host_rules_find(GsurfHostRules *self, const gchar *host);

/**
 * gsurf_host_rules_matches:
 * @self: a #GsurfHostRules
 * @host: (nullable): a bare host name
 *
 * Returns: whether any rule matches @host
 */
gboolean gsurf_host_rules_matches(GsurfHostRules *self, const gchar *host);

/**
 * gsurf_host_rules_lookup:
 * @self: a #GsurfHostRules
 * @host: (nullable): a bare host name
 *
 * Returns: (transfer none) (nullable) (type gpointer): the data
 *   (YamlMapping*) of the first matching rule, or %NULL when nothing
 *   matches or that rule has no data
 */
gpointer gsurf_host_rules_lookup(GsurfHostRules *self, const gchar *host);

/**
 * gsurf_host_rules_lookup_all: (skip)
 * @self: a #GsurfHostRules
 * @host: (nullable): a bare host name
 *
 * Collects the data of every matching rule, in rule order, for callers
 * that merge settings (later rules override earlier ones). Not
 * introspectable (an array of raw YAML pointers); bindings can iterate
 * with gsurf_host_rules_get_n_rules() and gsurf_host_rules_get_data().
 *
 * Returns: (transfer container) (element-type gpointer): matching rule
 *   data (YamlMapping*); rules without data are skipped
 */
GPtrArray *gsurf_host_rules_lookup_all(GsurfHostRules *self, const gchar *host);

/**
 * gsurf_host_rules_to_json:
 * @self: a #GsurfHostRules
 *
 * Serializes the rules for evaluation inside a page, as a JSON array of
 * objects `{"pattern": "…", "data": {…}|null}` in rule order.
 *
 * Returns: (transfer full): the JSON text
 */
gchar *gsurf_host_rules_to_json(GsurfHostRules *self);

/**
 * gsurf_host_rules_to_variant:
 * @self: a #GsurfHostRules
 *
 * Serializes the rules for a web-process extension as `a(sa{sv})`:
 * (pattern, data). Scalar data members become `s`, `x`, `d` or `b`; a
 * sequence of scalars becomes `as`; nested mappings and other shapes are
 * carried as their JSON text (`s`) under the same key.
 *
 * Returns: (transfer floating): the #GVariant
 */
GVariant *gsurf_host_rules_to_variant(GsurfHostRules *self);

/**
 * gsurf_host_from_uri:
 * @uri: (nullable): a URI
 *
 * Extracts the lowercase host of a network URI. IPv6 literals lose their
 * brackets; trailing dots are dropped.
 *
 * Returns: (transfer full) (nullable): the host, or %NULL for URIs
 *   without one (about:, data:, file:, unparsable input)
 */
gchar *gsurf_host_from_uri(const gchar *uri);

G_END_DECLS

#endif /* GSURF_HOST_RULES_H */
