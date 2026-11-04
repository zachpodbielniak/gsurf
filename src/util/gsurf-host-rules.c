/*
 * gsurf-host-rules.c - Ordered per-host regex rules with attached data
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "util/gsurf-host-rules.h"

#include <yaml-glib.h>

#include <json-glib/json-glib.h>
#include <string.h>

G_DEFINE_QUARK(gsurf-host-rules-error-quark, gsurf_host_rules_error)

/* One compiled rule. */
typedef struct {
	gchar       *pattern;
	GRegex      *regex;
	YamlMapping *data;     /* nullable, owned ref */
} HostRule;

struct _GsurfHostRules
{
	GObject    parent_instance;
	GPtrArray *rules;      /* HostRule*, owned */
};

G_DEFINE_FINAL_TYPE(GsurfHostRules, gsurf_host_rules, G_TYPE_OBJECT)

static void
host_rule_free(gpointer data)
{
	HostRule *rule = data;

	g_free(rule->pattern);
	g_clear_pointer(&rule->regex, g_regex_unref);
	g_clear_pointer(&rule->data, yaml_mapping_unref);
	g_free(rule);
}

static void
gsurf_host_rules_finalize(GObject *object)
{
	GsurfHostRules *self = GSURF_HOST_RULES(object);

	g_clear_pointer(&self->rules, g_ptr_array_unref);
	G_OBJECT_CLASS(gsurf_host_rules_parent_class)->finalize(object);
}

static void
gsurf_host_rules_class_init(GsurfHostRulesClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gsurf_host_rules_finalize;
}

static void
gsurf_host_rules_init(GsurfHostRules *self)
{
	self->rules = g_ptr_array_new_with_free_func(host_rule_free);
}

GsurfHostRules *
gsurf_host_rules_new(void)
{
	return g_object_new(GSURF_TYPE_HOST_RULES, NULL);
}

gboolean
gsurf_host_rules_add(GsurfHostRules *self, const gchar *pattern,
                     gpointer data, GError **error)
{
	g_autoptr(GError) local = NULL;
	GRegex *regex;
	HostRule *rule;

	g_return_val_if_fail(GSURF_IS_HOST_RULES(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	if (pattern == NULL || *pattern == '\0') {
		g_set_error_literal(error, GSURF_HOST_RULES_ERROR,
			GSURF_HOST_RULES_ERROR_PATTERN, "empty host pattern");
		return FALSE;
	}

	/* Hosts are case-insensitive; the pattern is compiled once here and
	 * matched many times (every navigation / request). */
	regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, &local);
	if (regex == NULL) {
		g_set_error(error, GSURF_HOST_RULES_ERROR,
			GSURF_HOST_RULES_ERROR_PATTERN, "invalid host pattern '%s': %s",
			pattern, local->message);
		return FALSE;
	}

	rule = g_new0(HostRule, 1);
	rule->pattern = g_strdup(pattern);
	rule->regex = regex;
	rule->data = (data != NULL) ? yaml_mapping_ref(data) : NULL;
	g_ptr_array_add(self->rules, rule);
	return TRUE;
}

gboolean
gsurf_host_rules_load(GsurfHostRules *self, gpointer node_ptr,
                      const gchar *match_key, GError **error)
{
	YamlNode *node = node_ptr;
	YamlSequence *seq;
	guint i, n;

	g_return_val_if_fail(GSURF_IS_HOST_RULES(self), FALSE);
	g_return_val_if_fail(match_key != NULL, FALSE);

	if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
		return TRUE;
	if (yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE) {
		g_set_error_literal(error, GSURF_HOST_RULES_ERROR,
			GSURF_HOST_RULES_ERROR_SHAPE, "host rules must be a list");
		return FALSE;
	}

	seq = yaml_node_get_sequence(node);
	n = yaml_sequence_get_length(seq);
	for (i = 0; i < n; i++) {
		YamlNode *element = yaml_sequence_get_element(seq, i);
		YamlMapping *mapping;
		const gchar *pattern;

		/* Shorthand: a bare string is a rule without settings. */
		if (yaml_node_get_node_type(element) == YAML_NODE_SCALAR) {
			if (!gsurf_host_rules_add(self, yaml_node_get_string(element), NULL, error))
				return FALSE;
			continue;
		}
		if (yaml_node_get_node_type(element) != YAML_NODE_MAPPING) {
			g_set_error(error, GSURF_HOST_RULES_ERROR,
				GSURF_HOST_RULES_ERROR_SHAPE,
				"host rule %u must be a pattern or a mapping", i + 1);
			return FALSE;
		}
		mapping = yaml_node_get_mapping(element);
		pattern = yaml_mapping_has_member(mapping, match_key)
			? yaml_mapping_get_string_member(mapping, match_key) : NULL;
		if (pattern == NULL) {
			g_set_error(error, GSURF_HOST_RULES_ERROR,
				GSURF_HOST_RULES_ERROR_PATTERN,
				"host rule %u has no '%s' pattern", i + 1, match_key);
			return FALSE;
		}
		if (!gsurf_host_rules_add(self, pattern, mapping, error))
			return FALSE;
	}
	return TRUE;
}

void
gsurf_host_rules_clear(GsurfHostRules *self)
{
	g_return_if_fail(GSURF_IS_HOST_RULES(self));
	g_ptr_array_set_size(self->rules, 0);
}

guint
gsurf_host_rules_get_n_rules(GsurfHostRules *self)
{
	g_return_val_if_fail(GSURF_IS_HOST_RULES(self), 0);
	return self->rules->len;
}

const gchar *
gsurf_host_rules_get_pattern(GsurfHostRules *self, guint index)
{
	g_return_val_if_fail(GSURF_IS_HOST_RULES(self), NULL);
	if (index >= self->rules->len)
		return NULL;
	return ((HostRule *)g_ptr_array_index(self->rules, index))->pattern;
}

gpointer
gsurf_host_rules_get_data(GsurfHostRules *self, guint index)
{
	g_return_val_if_fail(GSURF_IS_HOST_RULES(self), NULL);
	if (index >= self->rules->len)
		return NULL;
	return ((HostRule *)g_ptr_array_index(self->rules, index))->data;
}

gint
gsurf_host_rules_find(GsurfHostRules *self, const gchar *host)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_HOST_RULES(self), -1);

	if (host == NULL || *host == '\0')
		return -1;
	for (i = 0; i < self->rules->len; i++) {
		HostRule *rule = g_ptr_array_index(self->rules, i);

		if (g_regex_match(rule->regex, host, 0, NULL))
			return (gint)i;
	}
	return -1;
}

gboolean
gsurf_host_rules_matches(GsurfHostRules *self, const gchar *host)
{
	return gsurf_host_rules_find(self, host) >= 0;
}

gpointer
gsurf_host_rules_lookup(GsurfHostRules *self, const gchar *host)
{
	gint index = gsurf_host_rules_find(self, host);

	return (index < 0) ? NULL : gsurf_host_rules_get_data(self, (guint)index);
}

GPtrArray *
gsurf_host_rules_lookup_all(GsurfHostRules *self, const gchar *host)
{
	GPtrArray *out;
	guint i;

	g_return_val_if_fail(GSURF_IS_HOST_RULES(self), NULL);

	out = g_ptr_array_new();
	if (host == NULL || *host == '\0')
		return out;
	for (i = 0; i < self->rules->len; i++) {
		HostRule *rule = g_ptr_array_index(self->rules, i);

		if (rule->data != NULL && g_regex_match(rule->regex, host, 0, NULL))
			g_ptr_array_add(out, rule->data);
	}
	return out;
}

/* --- serialization --- */

gchar *
gsurf_host_rules_to_json(GsurfHostRules *self)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	guint i;

	g_return_val_if_fail(GSURF_IS_HOST_RULES(self), NULL);

	json_builder_begin_array(builder);
	for (i = 0; i < self->rules->len; i++) {
		HostRule *rule = g_ptr_array_index(self->rules, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "pattern");
		json_builder_add_string_value(builder, rule->pattern);
		json_builder_set_member_name(builder, "data");
		if (rule->data != NULL) {
			/* yaml-glib converts a whole node; wrap the mapping in one. */
			g_autoptr(YamlNode) node = yaml_node_new_mapping(rule->data);

			json_builder_add_value(builder, yaml_node_to_json_node(node));
		} else {
			json_builder_add_null_value(builder);
		}
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);

	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	return json_generator_to_data(generator, NULL);
}

/* JSON text of an arbitrary YAML node (used for nested values). */
static gchar *
node_to_json_text(YamlNode *node)
{
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) json = yaml_node_to_json_node(node);

	json_generator_set_root(generator, json);
	return json_generator_to_data(generator, NULL);
}

/* Convert one YAML data member into the #GVariant documented for
 * gsurf_host_rules_to_variant(). Scalars keep their YAML type by asking
 * JSON (which the yaml-glib schema has already resolved). */
static GVariant *
node_to_variant(YamlNode *node)
{
	YamlNodeType type = yaml_node_get_node_type(node);

	if (type == YAML_NODE_SCALAR) {
		g_autoptr(JsonNode) json = yaml_node_to_json_node(node);
		GType value_type = json_node_get_value_type(json);

		if (value_type == G_TYPE_BOOLEAN)
			return g_variant_new_boolean(json_node_get_boolean(json));
		if (value_type == G_TYPE_INT64)
			return g_variant_new_int64(json_node_get_int(json));
		if (value_type == G_TYPE_DOUBLE)
			return g_variant_new_double(json_node_get_double(json));
		return g_variant_new_string(yaml_node_get_string(node) != NULL
			? yaml_node_get_string(node) : "");
	}

	if (type == YAML_NODE_SEQUENCE) {
		YamlSequence *seq = yaml_node_get_sequence(node);
		g_autoptr(GVariantBuilder) builder = g_variant_builder_new(G_VARIANT_TYPE("as"));
		guint i, n = yaml_sequence_get_length(seq);
		gboolean scalars = TRUE;

		for (i = 0; i < n && scalars; i++)
			scalars = yaml_node_get_node_type(yaml_sequence_get_element(seq, i))
				== YAML_NODE_SCALAR;
		if (scalars) {
			for (i = 0; i < n; i++) {
				const gchar *text = yaml_node_get_string(yaml_sequence_get_element(seq, i));

				g_variant_builder_add(builder, "s", text != NULL ? text : "");
			}
			return g_variant_builder_end(builder);
		}
	}

	if (type == YAML_NODE_NULL)
		return g_variant_new_string("");

	{
		g_autofree gchar *text = node_to_json_text(node);

		return g_variant_new_string(text);
	}
}

GVariant *
gsurf_host_rules_to_variant(GsurfHostRules *self)
{
	GVariantBuilder rules;
	guint i;

	g_return_val_if_fail(GSURF_IS_HOST_RULES(self), NULL);

	g_variant_builder_init(&rules, G_VARIANT_TYPE("a(sa{sv})"));
	for (i = 0; i < self->rules->len; i++) {
		HostRule *rule = g_ptr_array_index(self->rules, i);
		GVariantBuilder data;

		g_variant_builder_init(&data, G_VARIANT_TYPE("a{sv}"));
		if (rule->data != NULL) {
			guint j, n = yaml_mapping_get_size(rule->data);

			for (j = 0; j < n; j++)
				g_variant_builder_add(&data, "{sv}",
					yaml_mapping_get_key(rule->data, j),
					node_to_variant(yaml_mapping_get_value(rule->data, j)));
		}
		g_variant_builder_add(&rules, "(sa{sv})", rule->pattern, &data);
	}
	return g_variant_builder_end(&rules);
}

gchar *
gsurf_host_from_uri(const gchar *uri)
{
	g_autoptr(GUri) parsed = NULL;
	const gchar *host;
	gchar *out;
	gsize len;

	if (uri == NULL || *uri == '\0')
		return NULL;
	parsed = g_uri_parse(uri, G_URI_FLAGS_NON_DNS | G_URI_FLAGS_ENCODED, NULL);
	if (parsed == NULL)
		return NULL;
	host = g_uri_get_host(parsed);
	if (host == NULL || *host == '\0')
		return NULL;

	/* "[::1]" -> "::1"; GUri usually strips brackets already. */
	if (host[0] == '[') {
		const gchar *end = strchr(host, ']');

		out = g_ascii_strdown(host + 1, end != NULL ? end - host - 1 : -1);
	} else {
		out = g_ascii_strdown(host, -1);
	}

	/* "example.com." and "example.com" are the same host. */
	len = strlen(out);
	while (len > 0 && out[len - 1] == '.')
		out[--len] = '\0';
	if (len == 0) {
		g_free(out);
		return NULL;
	}
	return out;
}
