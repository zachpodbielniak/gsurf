/*
 * gsurf-geolocation-module.c - Geolocation permission policy
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfPermissionHandler for geolocation requests.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_GEOLOCATION_MODULE (gsurf_geolocation_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfGeolocationModule, gsurf_geolocation_module,
                     GSURF, GEOLOCATION_MODULE, GsurfModule)

struct _GsurfGeolocationModule
{
	GsurfModule parent_instance;
	GsurfPermissionVerdict default_verdict;
};

static void gsurf_geolocation_perm_init(GsurfPermissionHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfGeolocationModule, gsurf_geolocation_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_PERMISSION_HANDLER, gsurf_geolocation_perm_init))

static GsurfPermissionVerdict
gsurf_geolocation_decide(GsurfPermissionHandler *handler, GsurfView *view,
                         const gchar *type, const gchar *origin)
{
	GsurfGeolocationModule *self = GSURF_GEOLOCATION_MODULE(handler);

	if (g_strcmp0(type, "geolocation") == 0)
		return self->default_verdict;
	return GSURF_PERMISSION_PROMPT;
}

static void
gsurf_geolocation_perm_init(GsurfPermissionHandlerInterface *iface)
{
	iface->decide = gsurf_geolocation_decide;
}

static const gchar *gsurf_geolocation_get_name(GsurfModule *m) { return "geolocation"; }
static const gchar *gsurf_geolocation_get_description(GsurfModule *m)
{
	return "Geolocation permission policy";
}

static void
gsurf_geolocation_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfGeolocationModule *self = GSURF_GEOLOCATION_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	const gchar *def;

	node = gsurf_config_get_module_node(config, "geolocation");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);
	def = yaml_mapping_get_string_member(m, "default");
	if (g_strcmp0(def, "allow") == 0)
		self->default_verdict = GSURF_PERMISSION_ALLOW;
	else if (g_strcmp0(def, "ask") == 0)
		self->default_verdict = GSURF_PERMISSION_PROMPT;
	else
		self->default_verdict = GSURF_PERMISSION_DENY;
}

static gboolean gsurf_geolocation_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_geolocation_module_class_init(GsurfGeolocationModuleClass *klass)
{
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);
	module_class->activate = gsurf_geolocation_activate;
	module_class->get_name = gsurf_geolocation_get_name;
	module_class->get_description = gsurf_geolocation_get_description;
	module_class->configure = gsurf_geolocation_configure;
}

static void
gsurf_geolocation_module_init(GsurfGeolocationModule *self)
{
	self->default_verdict = GSURF_PERMISSION_DENY;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_GEOLOCATION_MODULE;
}
