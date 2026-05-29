/*
 * gsurf-dark-mode-module.c - Force a dark color scheme
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfScriptInjector: injects a user stylesheet that sets
 * the page color-scheme (and optionally a dark filter) on every view.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_DARK_MODE_MODULE (gsurf_dark_mode_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfDarkModeModule, gsurf_dark_mode_module,
                     GSURF, DARK_MODE_MODULE, GsurfModule)

struct _GsurfDarkModeModule
{
	GsurfModule parent_instance;
	gchar    *mode;        /* auto | light | dark */
	gboolean  force_css;
};

static void gsurf_dark_mode_injector_init(GsurfScriptInjectorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfDarkModeModule, gsurf_dark_mode_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SCRIPT_INJECTOR, gsurf_dark_mode_injector_init))

static void
gsurf_dark_mode_inject(GsurfScriptInjector *injector, GsurfView *view, const gchar *uri)
{
	GsurfDarkModeModule *self = GSURF_DARK_MODE_MODULE(injector);
	const gchar *css = NULL;

	if (view == NULL)
		return;

	if (g_strcmp0(self->mode, "dark") == 0) {
		css = self->force_css
			? "html{color-scheme:dark!important;filter:invert(1) hue-rotate(180deg);}"
			  "img,video,picture{filter:invert(1) hue-rotate(180deg);}"
			: ":root{color-scheme:dark!important;}";
	} else if (g_strcmp0(self->mode, "auto") == 0) {
		css = ":root{color-scheme:light dark;}";
	}

	if (css != NULL)
		gsurf_view_add_user_style(view, css);
}

static void
gsurf_dark_mode_injector_init(GsurfScriptInjectorInterface *iface)
{
	iface->inject = gsurf_dark_mode_inject;
}

static const gchar *gsurf_dark_mode_get_name(GsurfModule *m) { return "dark_mode"; }
static const gchar *gsurf_dark_mode_get_description(GsurfModule *m)
{
	return "Force a dark color scheme via a user stylesheet";
}

static void
gsurf_dark_mode_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfDarkModeModule *self = GSURF_DARK_MODE_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "dark_mode");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);
	if (yaml_mapping_has_member(m, "mode")) {
		g_free(self->mode);
		self->mode = g_strdup(yaml_mapping_get_string_member(m, "mode"));
	}
	if (yaml_mapping_has_member(m, "force_dark_css"))
		self->force_css = yaml_mapping_get_boolean_member(m, "force_dark_css");
}

static gboolean gsurf_dark_mode_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_dark_mode_module_finalize(GObject *object)
{
	GsurfDarkModeModule *self = GSURF_DARK_MODE_MODULE(object);
	g_clear_pointer(&self->mode, g_free);
	G_OBJECT_CLASS(gsurf_dark_mode_module_parent_class)->finalize(object);
}

static void
gsurf_dark_mode_module_class_init(GsurfDarkModeModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_dark_mode_module_finalize;
	module_class->activate = gsurf_dark_mode_activate;
	module_class->get_name = gsurf_dark_mode_get_name;
	module_class->get_description = gsurf_dark_mode_get_description;
	module_class->configure = gsurf_dark_mode_configure;
}

static void
gsurf_dark_mode_module_init(GsurfDarkModeModule *self)
{
	self->mode = g_strdup("auto");
	self->force_css = FALSE;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_DARK_MODE_MODULE;
}
