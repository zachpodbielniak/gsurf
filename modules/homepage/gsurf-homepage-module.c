/*
 * gsurf-homepage-module.c - Go-home keybind + new-view homepage
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler: a configurable key loads the homepage
 * in the active view. Ports surf's homepage patch.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_HOMEPAGE_MODULE (gsurf_homepage_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfHomepageModule, gsurf_homepage_module,
                     GSURF, HOMEPAGE_MODULE, GsurfModule)

struct _GsurfHomepageModule
{
	GsurfModule parent_instance;
	gchar *uri;
	gchar *key_home;
};

static void gsurf_homepage_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfHomepageModule, gsurf_homepage_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_homepage_input_init))

static gboolean
gsurf_homepage_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                                guint keyval, guint keycode, guint state,
                                GsurfModePolicy mode)
{
	GsurfHomepageModule *self = GSURF_HOMEPAGE_MODULE(handler);

	if (self->key_home == NULL || view == NULL)
		return FALSE;
	if (!gsurf_keys_match(keyval, state, self->key_home))
		return FALSE;

	gsurf_view_load_uri(view, self->uri ? self->uri : "about:blank");
	return TRUE;
}

static void
gsurf_homepage_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_homepage_handle_key_event;
}

static const gchar *
gsurf_homepage_get_name(GsurfModule *m)
{
	return "homepage";
}

static const gchar *
gsurf_homepage_get_description(GsurfModule *m)
{
	return "Go-home keybind and new-view homepage";
}

static void
gsurf_homepage_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfHomepageModule *self = GSURF_HOMEPAGE_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "homepage");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "uri")) {
		g_free(self->uri);
		self->uri = g_strdup(yaml_mapping_get_string_member(m, "uri"));
	}
	if (yaml_mapping_has_member(m, "key_home")) {
		g_free(self->key_home);
		self->key_home = g_strdup(yaml_mapping_get_string_member(m, "key_home"));
	}
}

static gboolean
gsurf_homepage_activate(GsurfModule *m)
{
	return TRUE;
}

static void
gsurf_homepage_module_finalize(GObject *object)
{
	GsurfHomepageModule *self = GSURF_HOMEPAGE_MODULE(object);

	g_clear_pointer(&self->uri, g_free);
	g_clear_pointer(&self->key_home, g_free);

	G_OBJECT_CLASS(gsurf_homepage_module_parent_class)->finalize(object);
}

static void
gsurf_homepage_module_class_init(GsurfHomepageModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_homepage_module_finalize;
	module_class->activate = gsurf_homepage_activate;
	module_class->get_name = gsurf_homepage_get_name;
	module_class->get_description = gsurf_homepage_get_description;
	module_class->configure = gsurf_homepage_configure;
}

static void
gsurf_homepage_module_init(GsurfHomepageModule *self)
{
	self->uri = g_strdup("about:blank");
	self->key_home = g_strdup("Ctrl+Home");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_HOMEPAGE_MODULE;
}
