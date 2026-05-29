/*
 * gsurf-notifications-module.c - Web notification permission policy
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfPermissionHandler for Web Notification requests.
 * (libnotify/gcr are unavailable on this platform, so OS-level notifying
 * is left to the engine; this module governs the permission policy.)
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_NOTIFICATIONS_MODULE (gsurf_notifications_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfNotificationsModule, gsurf_notifications_module,
                     GSURF, NOTIFICATIONS_MODULE, GsurfModule)

struct _GsurfNotificationsModule
{
	GsurfModule parent_instance;
	GsurfPermissionVerdict policy;
};

static void gsurf_notifications_perm_init(GsurfPermissionHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfNotificationsModule, gsurf_notifications_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_PERMISSION_HANDLER, gsurf_notifications_perm_init))

static GsurfPermissionVerdict
gsurf_notifications_decide(GsurfPermissionHandler *handler, GsurfView *view,
                           const gchar *type, const gchar *origin)
{
	GsurfNotificationsModule *self = GSURF_NOTIFICATIONS_MODULE(handler);

	if (g_strcmp0(type, "notification") == 0)
		return self->policy;
	return GSURF_PERMISSION_PROMPT;
}

static void
gsurf_notifications_perm_init(GsurfPermissionHandlerInterface *iface)
{
	iface->decide = gsurf_notifications_decide;
}

static const gchar *gsurf_notifications_get_name(GsurfModule *m) { return "notifications"; }
static const gchar *gsurf_notifications_get_description(GsurfModule *m)
{
	return "Web notification permission policy";
}

static void
gsurf_notifications_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfNotificationsModule *self = GSURF_NOTIFICATIONS_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	const gchar *p;

	node = gsurf_config_get_module_node(config, "notifications");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);
	p = yaml_mapping_get_string_member(m, "allow_web_notifications");
	if (g_strcmp0(p, "allow") == 0)
		self->policy = GSURF_PERMISSION_ALLOW;
	else if (g_strcmp0(p, "deny") == 0)
		self->policy = GSURF_PERMISSION_DENY;
	else
		self->policy = GSURF_PERMISSION_PROMPT;
}

static gboolean gsurf_notifications_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_notifications_module_class_init(GsurfNotificationsModuleClass *klass)
{
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);
	module_class->activate = gsurf_notifications_activate;
	module_class->get_name = gsurf_notifications_get_name;
	module_class->get_description = gsurf_notifications_get_description;
	module_class->configure = gsurf_notifications_configure;
}

static void
gsurf_notifications_module_init(GsurfNotificationsModule *self)
{
	self->policy = GSURF_PERMISSION_PROMPT;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_NOTIFICATIONS_MODULE;
}
