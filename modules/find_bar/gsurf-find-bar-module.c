/*
 * gsurf-find-bar-module.c - In-page find navigation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler: next/previous-match keys over the
 * view's find controller. (A search-term entry widget is a planned
 * addition; the MCP find_text tool and gsurf_view_find() drive searches
 * programmatically today.)
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_FIND_BAR_MODULE (gsurf_find_bar_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfFindBarModule, gsurf_find_bar_module,
                     GSURF, FIND_BAR_MODULE, GsurfModule)

struct _GsurfFindBarModule
{
	GsurfModule parent_instance;
	gchar *key_next;
	gchar *key_prev;
};

static void gsurf_find_bar_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfFindBarModule, gsurf_find_bar_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_find_bar_input_init))

static gboolean
gsurf_find_bar_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                                guint keyval, guint keycode, guint state, GsurfModePolicy mode)
{
	GsurfFindBarModule *self = GSURF_FIND_BAR_MODULE(handler);

	if (view == NULL)
		return FALSE;
	if (self->key_next && gsurf_keys_match(keyval, state, self->key_next)) {
		gsurf_view_find_next(view);
		return TRUE;
	}
	if (self->key_prev && gsurf_keys_match(keyval, state, self->key_prev)) {
		gsurf_view_find_previous(view);
		return TRUE;
	}
	return FALSE;
}

static void
gsurf_find_bar_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_find_bar_handle_key_event;
}

static const gchar *gsurf_find_bar_get_name(GsurfModule *m) { return "find_bar"; }
static const gchar *gsurf_find_bar_get_description(GsurfModule *m)
{
	return "In-page find next/previous navigation";
}

static void
gsurf_find_bar_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfFindBarModule *self = GSURF_FIND_BAR_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "find_bar");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);
	if (yaml_mapping_has_member(m, "key_next")) {
		g_free(self->key_next);
		self->key_next = g_strdup(yaml_mapping_get_string_member(m, "key_next"));
	}
	if (yaml_mapping_has_member(m, "key_prev")) {
		g_free(self->key_prev);
		self->key_prev = g_strdup(yaml_mapping_get_string_member(m, "key_prev"));
	}
}

static gboolean gsurf_find_bar_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_find_bar_module_finalize(GObject *object)
{
	GsurfFindBarModule *self = GSURF_FIND_BAR_MODULE(object);
	g_clear_pointer(&self->key_next, g_free);
	g_clear_pointer(&self->key_prev, g_free);
	G_OBJECT_CLASS(gsurf_find_bar_module_parent_class)->finalize(object);
}

static void
gsurf_find_bar_module_class_init(GsurfFindBarModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_find_bar_module_finalize;
	module_class->activate = gsurf_find_bar_activate;
	module_class->get_name = gsurf_find_bar_get_name;
	module_class->get_description = gsurf_find_bar_get_description;
	module_class->configure = gsurf_find_bar_configure;
}

static void
gsurf_find_bar_module_init(GsurfFindBarModule *self)
{
	self->key_next = g_strdup("n");
	self->key_prev = g_strdup("N");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_FIND_BAR_MODULE;
}
