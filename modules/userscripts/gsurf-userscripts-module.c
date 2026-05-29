/*
 * gsurf-userscripts-module.c - Inject a global user script
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfScriptInjector: injects ~/.config/gsurf/script.js into
 * every view. Ports surf's script.js behavior.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_USERSCRIPTS_MODULE (gsurf_userscripts_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfUserscriptsModule, gsurf_userscripts_module,
                     GSURF, USERSCRIPTS_MODULE, GsurfModule)

struct _GsurfUserscriptsModule
{
	GsurfModule parent_instance;
	gchar    *source;     /* cached script contents */
	gboolean  at_end;
};

static void gsurf_userscripts_injector_init(GsurfScriptInjectorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfUserscriptsModule, gsurf_userscripts_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SCRIPT_INJECTOR, gsurf_userscripts_injector_init))

static void
gsurf_userscripts_inject(GsurfScriptInjector *injector, GsurfView *view, const gchar *uri)
{
	GsurfUserscriptsModule *self = GSURF_USERSCRIPTS_MODULE(injector);

	if (self->source != NULL && view != NULL)
		gsurf_view_add_user_script(view, self->source, self->at_end);
}

static void
gsurf_userscripts_injector_init(GsurfScriptInjectorInterface *iface)
{
	iface->inject = gsurf_userscripts_inject;
}

static const gchar *gsurf_userscripts_get_name(GsurfModule *m) { return "userscripts"; }
static const gchar *gsurf_userscripts_get_description(GsurfModule *m)
{
	return "Inject a global user JavaScript file into every page";
}

static void
gsurf_userscripts_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfUserscriptsModule *self = GSURF_USERSCRIPTS_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	const gchar *path = NULL;

	node = gsurf_config_get_module_node(config, "userscripts");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "global"))
		path = yaml_mapping_get_string_member(m, "global");
	if (yaml_mapping_has_member(m, "inject_time"))
		self->at_end = g_strcmp0(yaml_mapping_get_string_member(m, "inject_time"), "start") != 0;

	if (path != NULL) {
		g_autofree gchar *expanded = (path[0] == '~' && path[1] == '/')
			? g_build_filename(g_get_home_dir(), path + 2, NULL)
			: g_strdup(path);
		g_clear_pointer(&self->source, g_free);
		g_file_get_contents(expanded, &self->source, NULL, NULL);
	}
}

static gboolean gsurf_userscripts_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_userscripts_module_finalize(GObject *object)
{
	GsurfUserscriptsModule *self = GSURF_USERSCRIPTS_MODULE(object);
	g_clear_pointer(&self->source, g_free);
	G_OBJECT_CLASS(gsurf_userscripts_module_parent_class)->finalize(object);
}

static void
gsurf_userscripts_module_class_init(GsurfUserscriptsModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_userscripts_module_finalize;
	module_class->activate = gsurf_userscripts_activate;
	module_class->get_name = gsurf_userscripts_get_name;
	module_class->get_description = gsurf_userscripts_get_description;
	module_class->configure = gsurf_userscripts_configure;
}

static void
gsurf_userscripts_module_init(GsurfUserscriptsModule *self)
{
	self->at_end = TRUE;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_USERSCRIPTS_MODULE;
}
