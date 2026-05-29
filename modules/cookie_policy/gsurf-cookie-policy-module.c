/*
 * gsurf-cookie-policy-module.c - Cookie accept policy + cycle toggle
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler: applies the configured cookie accept
 * policy and cycles it with a key (surf's @Aa toggle). 0=always,
 * 1=never, 2=no-third-party.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_COOKIE_POLICY_MODULE (gsurf_cookie_policy_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfCookiePolicyModule, gsurf_cookie_policy_module,
                     GSURF, COOKIE_POLICY_MODULE, GsurfModule)

struct _GsurfCookiePolicyModule
{
	GsurfModule parent_instance;
	gint   policy;
	gchar *key_toggle;
};

static void gsurf_cookie_policy_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfCookiePolicyModule, gsurf_cookie_policy_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_cookie_policy_input_init))

static gint
parse_policy(const gchar *p)
{
	if (g_strcmp0(p, "never") == 0) return 1;
	if (g_strcmp0(p, "no-third-party") == 0) return 2;
	return 0; /* always */
}

static gboolean
gsurf_cookie_policy_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                                     guint keyval, guint keycode, guint state, GsurfModePolicy mode)
{
	GsurfCookiePolicyModule *self = GSURF_COOKIE_POLICY_MODULE(handler);

	if (view == NULL || self->key_toggle == NULL)
		return FALSE;
	if (!gsurf_keys_match(keyval, state, self->key_toggle))
		return FALSE;

	self->policy = (self->policy + 1) % 3;
	gsurf_view_set_cookie_accept_policy(view, self->policy);
	g_message("gsurf cookie policy: %s",
		self->policy == 1 ? "never" : self->policy == 2 ? "no-third-party" : "always");
	return TRUE;
}

static void
gsurf_cookie_policy_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_cookie_policy_handle_key_event;
}

static const gchar *gsurf_cookie_policy_get_name(GsurfModule *m) { return "cookie_policy"; }
static const gchar *gsurf_cookie_policy_get_description(GsurfModule *m)
{
	return "Cookie accept policy with a cycle toggle";
}

static void
gsurf_cookie_policy_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfCookiePolicyModule *self = GSURF_COOKIE_POLICY_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "cookie_policy");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);
	if (yaml_mapping_has_member(m, "policy"))
		self->policy = parse_policy(yaml_mapping_get_string_member(m, "policy"));
	if (yaml_mapping_has_member(m, "key_toggle")) {
		g_free(self->key_toggle);
		self->key_toggle = g_strdup(yaml_mapping_get_string_member(m, "key_toggle"));
	}
}

static gboolean
gsurf_cookie_policy_activate(GsurfModule *module)
{
	GsurfCookiePolicyModule *self = GSURF_COOKIE_POLICY_MODULE(module);
	GsurfView *view = gsurf_module_manager_get_active_view(gsurf_module_manager_get_default());

	if (view != NULL)
		gsurf_view_set_cookie_accept_policy(view, self->policy);
	return TRUE;
}

static void
gsurf_cookie_policy_module_finalize(GObject *object)
{
	GsurfCookiePolicyModule *self = GSURF_COOKIE_POLICY_MODULE(object);
	g_clear_pointer(&self->key_toggle, g_free);
	G_OBJECT_CLASS(gsurf_cookie_policy_module_parent_class)->finalize(object);
}

static void
gsurf_cookie_policy_module_class_init(GsurfCookiePolicyModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_cookie_policy_module_finalize;
	module_class->activate = gsurf_cookie_policy_activate;
	module_class->get_name = gsurf_cookie_policy_get_name;
	module_class->get_description = gsurf_cookie_policy_get_description;
	module_class->configure = gsurf_cookie_policy_configure;
}

static void
gsurf_cookie_policy_module_init(GsurfCookiePolicyModule *self)
{
	self->policy = 2; /* no-third-party */
	self->key_toggle = g_strdup("Ctrl+Shift+at");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_COOKIE_POLICY_MODULE;
}
