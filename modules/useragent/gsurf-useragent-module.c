/*
 * gsurf-useragent-module.c - Per-URI / default user-agent override
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfSettingProvider: sets the user agent for the URI about
 * to load, from regex rules (resolving named presets) or a default.
 * Ports surf's useragent patch.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_USERAGENT_MODULE (gsurf_useragent_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfUseragentModule, gsurf_useragent_module,
                     GSURF, USERAGENT_MODULE, GsurfModule)

typedef struct { GRegex *regex; gchar *ua; } UaRule;

struct _GsurfUseragentModule
{
	GsurfModule parent_instance;
	gchar      *default_ua;
	GHashTable *presets;  /* name -> ua */
	GPtrArray  *rules;    /* UaRule* (ua already resolved) */
};

static void gsurf_useragent_provider_init(GsurfSettingProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfUseragentModule, gsurf_useragent_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SETTING_PROVIDER, gsurf_useragent_provider_init))

static void
gsurf_useragent_apply(GsurfSettingProvider *provider, GsurfView *view,
                      GsurfSettings *settings, const gchar *uri)
{
	GsurfUseragentModule *self = GSURF_USERAGENT_MODULE(provider);
	guint i;

	for (i = 0; uri != NULL && i < self->rules->len; i++) {
		UaRule *r = g_ptr_array_index(self->rules, i);
		if (r->regex != NULL && g_regex_match(r->regex, uri, 0, NULL)) {
			gsurf_settings_set_user_agent(settings, r->ua);
			return;
		}
	}
	if (self->default_ua != NULL && *self->default_ua != '\0')
		gsurf_settings_set_user_agent(settings, self->default_ua);
}

static void
gsurf_useragent_provider_init(GsurfSettingProviderInterface *iface)
{
	iface->apply_settings = gsurf_useragent_apply;
}

static const gchar *gsurf_useragent_get_name(GsurfModule *m) { return "useragent"; }
static const gchar *gsurf_useragent_get_description(GsurfModule *m)
{
	return "Per-URI and default user-agent override";
}

static void
ua_rule_free(gpointer p)
{
	UaRule *r = p;
	if (r->regex != NULL)
		g_regex_unref(r->regex);
	g_free(r->ua);
	g_free(r);
}

static void
gsurf_useragent_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfUseragentModule *self = GSURF_USERAGENT_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m, *presets;
	YamlSequence *rules;
	guint i, n;

	node = gsurf_config_get_module_node(config, "useragent");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "default")) {
		g_free(self->default_ua);
		self->default_ua = g_strdup(yaml_mapping_get_string_member(m, "default"));
	}

	presets = yaml_mapping_get_mapping_member(m, "presets");
	if (presets != NULL) {
		GList *names = yaml_mapping_get_members(presets), *l;
		for (l = names; l != NULL; l = l->next)
			g_hash_table_replace(self->presets, g_strdup(l->data),
				g_strdup(yaml_mapping_get_string_member(presets, l->data)));
		g_list_free(names);
	}

	rules = yaml_mapping_get_sequence_member(m, "rules");
	if (rules != NULL) {
		n = yaml_sequence_get_length(rules);
		for (i = 0; i < n; i++) {
			YamlNode *en = yaml_sequence_get_element(rules, i);
			YamlMapping *rm;
			const gchar *regex_str, *ua_ref, *ua;
			UaRule *rule;

			if (en == NULL || yaml_node_get_node_type(en) != YAML_NODE_MAPPING)
				continue;
			rm = yaml_node_get_mapping(en);
			regex_str = yaml_mapping_get_string_member(rm, "regex");
			ua_ref = yaml_mapping_get_string_member(rm, "ua");
			if (regex_str == NULL || ua_ref == NULL)
				continue;
			/* Resolve a preset name, else use the literal string. */
			ua = g_hash_table_lookup(self->presets, ua_ref);
			rule = g_new0(UaRule, 1);
			rule->regex = g_regex_new(regex_str, 0, 0, NULL);
			rule->ua = g_strdup(ua ? ua : ua_ref);
			g_ptr_array_add(self->rules, rule);
		}
	}
}

static gboolean gsurf_useragent_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_useragent_module_finalize(GObject *object)
{
	GsurfUseragentModule *self = GSURF_USERAGENT_MODULE(object);
	g_clear_pointer(&self->default_ua, g_free);
	g_clear_pointer(&self->presets, g_hash_table_unref);
	g_clear_pointer(&self->rules, g_ptr_array_unref);
	G_OBJECT_CLASS(gsurf_useragent_module_parent_class)->finalize(object);
}

static void
gsurf_useragent_module_class_init(GsurfUseragentModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_useragent_module_finalize;
	module_class->activate = gsurf_useragent_activate;
	module_class->get_name = gsurf_useragent_get_name;
	module_class->get_description = gsurf_useragent_get_description;
	module_class->configure = gsurf_useragent_configure;
}

static void
gsurf_useragent_module_init(GsurfUseragentModule *self)
{
	self->presets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->rules = g_ptr_array_new_with_free_func(ua_rule_free);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_USERAGENT_MODULE;
}
