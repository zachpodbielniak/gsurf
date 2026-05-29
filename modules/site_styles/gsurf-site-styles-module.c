/*
 * gsurf-site-styles-module.c - Inject a global user stylesheet
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfScriptInjector: applies a default user CSS file to
 * every view. Ports surf's styles[] (the default.css case).
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_SITE_STYLES_MODULE (gsurf_site_styles_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfSiteStylesModule, gsurf_site_styles_module,
                     GSURF, SITE_STYLES_MODULE, GsurfModule)

struct _GsurfSiteStylesModule
{
	GsurfModule parent_instance;
	gchar *css;   /* cached default stylesheet */
};

static void gsurf_site_styles_injector_init(GsurfScriptInjectorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfSiteStylesModule, gsurf_site_styles_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SCRIPT_INJECTOR, gsurf_site_styles_injector_init))

static void
gsurf_site_styles_inject(GsurfScriptInjector *injector, GsurfView *view, const gchar *uri)
{
	GsurfSiteStylesModule *self = GSURF_SITE_STYLES_MODULE(injector);

	if (self->css != NULL && view != NULL)
		gsurf_view_add_user_style(view, self->css);
}

static void
gsurf_site_styles_injector_init(GsurfScriptInjectorInterface *iface)
{
	iface->inject = gsurf_site_styles_inject;
}

static const gchar *gsurf_site_styles_get_name(GsurfModule *m) { return "site_styles"; }
static const gchar *gsurf_site_styles_get_description(GsurfModule *m)
{
	return "Apply a default user stylesheet to every page";
}

static void
gsurf_site_styles_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfSiteStylesModule *self = GSURF_SITE_STYLES_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	const gchar *path = NULL;

	node = gsurf_config_get_module_node(config, "site_styles");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "default"))
		path = yaml_mapping_get_string_member(m, "default");

	if (path != NULL && *path != '\0') {
		g_autofree gchar *expanded = (path[0] == '~' && path[1] == '/')
			? g_build_filename(g_get_home_dir(), path + 2, NULL)
			: g_strdup(path);
		g_clear_pointer(&self->css, g_free);
		g_file_get_contents(expanded, &self->css, NULL, NULL);
	}
}

static gboolean gsurf_site_styles_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_site_styles_module_finalize(GObject *object)
{
	GsurfSiteStylesModule *self = GSURF_SITE_STYLES_MODULE(object);
	g_clear_pointer(&self->css, g_free);
	G_OBJECT_CLASS(gsurf_site_styles_module_parent_class)->finalize(object);
}

static void
gsurf_site_styles_module_class_init(GsurfSiteStylesModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_site_styles_module_finalize;
	module_class->activate = gsurf_site_styles_activate;
	module_class->get_name = gsurf_site_styles_get_name;
	module_class->get_description = gsurf_site_styles_get_description;
	module_class->configure = gsurf_site_styles_configure;
}

static void
gsurf_site_styles_module_init(GsurfSiteStylesModule *self)
{
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_SITE_STYLES_MODULE;
}
