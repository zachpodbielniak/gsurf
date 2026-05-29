/*
 * gsurf-downloads-module.c - Choose download destinations
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfDownloadHandler: saves downloads into a configured
 * directory using the suggested filename.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_DOWNLOADS_MODULE (gsurf_downloads_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfDownloadsModule, gsurf_downloads_module,
                     GSURF, DOWNLOADS_MODULE, GsurfModule)

struct _GsurfDownloadsModule
{
	GsurfModule parent_instance;
	gchar *dir;
};

static void gsurf_downloads_handler_init(GsurfDownloadHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfDownloadsModule, gsurf_downloads_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_DOWNLOAD_HANDLER, gsurf_downloads_handler_init))

static gchar *
gsurf_downloads_decide_destination(GsurfDownloadHandler *handler,
                                   const gchar *uri, const gchar *suggested)
{
	GsurfDownloadsModule *self = GSURF_DOWNLOADS_MODULE(handler);
	const gchar *name = (suggested && *suggested) ? suggested : "download";

	if (self->dir == NULL)
		return NULL;
	g_mkdir_with_parents(self->dir, 0755);
	return g_build_filename(self->dir, name, NULL);
}

static void
gsurf_downloads_handler_init(GsurfDownloadHandlerInterface *iface)
{
	iface->decide_destination = gsurf_downloads_decide_destination;
}

static const gchar *gsurf_downloads_get_name(GsurfModule *m) { return "downloads"; }
static const gchar *gsurf_downloads_get_description(GsurfModule *m)
{
	return "Save downloads into a configured directory";
}

static void
gsurf_downloads_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfDownloadsModule *self = GSURF_DOWNLOADS_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	const gchar *dir;

	node = gsurf_config_get_module_node(config, "downloads");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);
	dir = yaml_mapping_get_string_member(m, "dir");
	if (dir == NULL)
		dir = "~/Downloads";
	g_free(self->dir);
	self->dir = (dir[0] == '~' && dir[1] == '/')
		? g_build_filename(g_get_home_dir(), dir + 2, NULL)
		: g_strdup(dir);
}

static gboolean gsurf_downloads_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_downloads_module_finalize(GObject *object)
{
	GsurfDownloadsModule *self = GSURF_DOWNLOADS_MODULE(object);
	g_clear_pointer(&self->dir, g_free);
	G_OBJECT_CLASS(gsurf_downloads_module_parent_class)->finalize(object);
}

static void
gsurf_downloads_module_class_init(GsurfDownloadsModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_downloads_module_finalize;
	module_class->activate = gsurf_downloads_activate;
	module_class->get_name = gsurf_downloads_get_name;
	module_class->get_description = gsurf_downloads_get_description;
	module_class->configure = gsurf_downloads_configure;
}

static void
gsurf_downloads_module_init(GsurfDownloadsModule *self)
{
	self->dir = g_build_filename(g_get_home_dir(), "Downloads", NULL);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_DOWNLOADS_MODULE;
}
