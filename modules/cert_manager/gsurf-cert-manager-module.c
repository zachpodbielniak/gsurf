/*
 * gsurf-cert-manager-module.c - TLS certificate exceptions
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfCertHandler: proceeds past TLS errors for hosts listed
 * as exceptions/pins. Ports surf's certs[].
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_CERT_MANAGER_MODULE (gsurf_cert_manager_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfCertManagerModule, gsurf_cert_manager_module,
                     GSURF, CERT_MANAGER_MODULE, GsurfModule)

struct _GsurfCertManagerModule
{
	GsurfModule parent_instance;
	GHashTable *exceptions;  /* host -> 1 */
};

static void gsurf_cert_manager_handler_init(GsurfCertHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfCertManagerModule, gsurf_cert_manager_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_CERT_HANDLER, gsurf_cert_manager_handler_init))

static GsurfTlsDecision
gsurf_cert_manager_verify(GsurfCertHandler *handler, const gchar *host, guint tls_errors)
{
	GsurfCertManagerModule *self = GSURF_CERT_MANAGER_MODULE(handler);

	if (host != NULL && g_hash_table_contains(self->exceptions, host))
		return GSURF_TLS_PROCEED;
	return GSURF_TLS_PROMPT;
}

static void
gsurf_cert_manager_handler_init(GsurfCertHandlerInterface *iface)
{
	iface->verify = gsurf_cert_manager_verify;
}

static const gchar *gsurf_cert_manager_get_name(GsurfModule *m) { return "cert_manager"; }
static const gchar *gsurf_cert_manager_get_description(GsurfModule *m)
{
	return "Per-host TLS certificate exceptions";
}

static void
gsurf_cert_manager_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfCertManagerModule *self = GSURF_CERT_MANAGER_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m, *pins;

	node = gsurf_config_get_module_node(config, "cert_manager");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	pins = yaml_mapping_get_mapping_member(m, "pins");
	if (pins != NULL) {
		GList *hosts = yaml_mapping_get_members(pins), *l;
		for (l = hosts; l != NULL; l = l->next)
			g_hash_table_add(self->exceptions, g_strdup(l->data));
		g_list_free(hosts);
	}
}

static gboolean gsurf_cert_manager_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_cert_manager_module_finalize(GObject *object)
{
	GsurfCertManagerModule *self = GSURF_CERT_MANAGER_MODULE(object);
	g_clear_pointer(&self->exceptions, g_hash_table_unref);
	G_OBJECT_CLASS(gsurf_cert_manager_module_parent_class)->finalize(object);
}

static void
gsurf_cert_manager_module_class_init(GsurfCertManagerModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_cert_manager_module_finalize;
	module_class->activate = gsurf_cert_manager_activate;
	module_class->get_name = gsurf_cert_manager_get_name;
	module_class->get_description = gsurf_cert_manager_get_description;
	module_class->configure = gsurf_cert_manager_configure;
}

static void
gsurf_cert_manager_module_init(GsurfCertManagerModule *self)
{
	self->exceptions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_CERT_MANAGER_MODULE;
}
