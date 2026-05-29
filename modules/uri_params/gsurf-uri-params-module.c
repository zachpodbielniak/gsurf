/*
 * gsurf-uri-params-module.c - Per-URI web engine setting overrides
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfSettingProvider: applies regex-matched setting
 * overrides for the URI about to load. Ports surf's uriparams[].
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_URI_PARAMS_MODULE (gsurf_uri_params_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfUriParamsModule, gsurf_uri_params_module,
                     GSURF, URI_PARAMS_MODULE, GsurfModule)

typedef struct {
	GRegex     *regex;
	GHashTable *settings;  /* field name (gchar*) -> GINT bool */
} UriRule;

struct _GsurfUriParamsModule
{
	GsurfModule parent_instance;
	GPtrArray *rules;   /* UriRule* */
};

static void gsurf_uri_params_provider_init(GsurfSettingProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfUriParamsModule, gsurf_uri_params_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SETTING_PROVIDER, gsurf_uri_params_provider_init))

static void
set_bool_field(GsurfSettings *s, const gchar *name, gboolean v)
{
#define SET(field) do { if (g_strcmp0(name, #field) == 0) { s->field = v; return; } } while (0)
	SET(javascript); SET(images); SET(webgl); SET(webaudio); SET(media_stream);
	SET(webrtc); SET(plugins); SET(smooth_scrolling); SET(caret_browsing);
	SET(site_quirks); SET(developer_extras);
#undef SET
}

static void
gsurf_uri_params_apply(GsurfSettingProvider *provider, GsurfView *view,
                       GsurfSettings *settings, const gchar *uri)
{
	GsurfUriParamsModule *self = GSURF_URI_PARAMS_MODULE(provider);
	guint i;

	if (uri == NULL)
		return;

	for (i = 0; i < self->rules->len; i++) {
		UriRule *rule = g_ptr_array_index(self->rules, i);
		GHashTableIter it;
		gpointer k, v;

		if (rule->regex == NULL || !g_regex_match(rule->regex, uri, 0, NULL))
			continue;
		g_hash_table_iter_init(&it, rule->settings);
		while (g_hash_table_iter_next(&it, &k, &v))
			set_bool_field(settings, k, GPOINTER_TO_INT(v));
	}
}

static void
gsurf_uri_params_provider_init(GsurfSettingProviderInterface *iface)
{
	iface->apply_settings = gsurf_uri_params_apply;
}

static const gchar *gsurf_uri_params_get_name(GsurfModule *m) { return "uri_params"; }
static const gchar *gsurf_uri_params_get_description(GsurfModule *m)
{
	return "Per-URI web engine setting overrides";
}

static void
uri_rule_free(gpointer p)
{
	UriRule *r = p;
	if (r->regex != NULL)
		g_regex_unref(r->regex);
	g_hash_table_unref(r->settings);
	g_free(r);
}

static void
gsurf_uri_params_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfUriParamsModule *self = GSURF_URI_PARAMS_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	YamlSequence *rules;
	guint i, n;

	node = gsurf_config_get_module_node(config, "uri_params");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	rules = yaml_mapping_get_sequence_member(m, "rules");
	if (rules == NULL)
		return;
	n = yaml_sequence_get_length(rules);
	for (i = 0; i < n; i++) {
		YamlNode *en = yaml_sequence_get_element(rules, i);
		YamlMapping *rm, *sm;
		const gchar *regex_str;
		UriRule *rule;
		GList *fields, *l;

		if (en == NULL || yaml_node_get_node_type(en) != YAML_NODE_MAPPING)
			continue;
		rm = yaml_node_get_mapping(en);
		regex_str = yaml_mapping_get_string_member(rm, "regex");
		sm = yaml_mapping_get_mapping_member(rm, "settings");
		if (regex_str == NULL || sm == NULL)
			continue;

		rule = g_new0(UriRule, 1);
		rule->regex = g_regex_new(regex_str, 0, 0, NULL);
		rule->settings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		fields = yaml_mapping_get_members(sm);
		for (l = fields; l != NULL; l = l->next)
			g_hash_table_replace(rule->settings, g_strdup(l->data),
				GINT_TO_POINTER(yaml_mapping_get_boolean_member(sm, l->data) ? 1 : 0));
		g_list_free(fields);
		g_ptr_array_add(self->rules, rule);
	}
}

static gboolean gsurf_uri_params_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_uri_params_module_finalize(GObject *object)
{
	GsurfUriParamsModule *self = GSURF_URI_PARAMS_MODULE(object);
	g_clear_pointer(&self->rules, g_ptr_array_unref);
	G_OBJECT_CLASS(gsurf_uri_params_module_parent_class)->finalize(object);
}

static void
gsurf_uri_params_module_class_init(GsurfUriParamsModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_uri_params_module_finalize;
	module_class->activate = gsurf_uri_params_activate;
	module_class->get_name = gsurf_uri_params_get_name;
	module_class->get_description = gsurf_uri_params_get_description;
	module_class->configure = gsurf_uri_params_configure;
}

static void
gsurf_uri_params_module_init(GsurfUriParamsModule *self)
{
	self->rules = g_ptr_array_new_with_free_func(uri_rule_free);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_URI_PARAMS_MODULE;
}
