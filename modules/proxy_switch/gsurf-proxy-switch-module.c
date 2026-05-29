/*
 * gsurf-proxy-switch-module.c - Runtime proxy cycling
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler: a key cycles through configured proxy
 * presets and applies the selected one to the active view's context.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_PROXY_SWITCH_MODULE (gsurf_proxy_switch_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfProxySwitchModule, gsurf_proxy_switch_module,
                     GSURF, PROXY_SWITCH_MODULE, GsurfModule)

struct _GsurfProxySwitchModule
{
	GsurfModule parent_instance;
	GPtrArray *uris;   /* gchar* proxy URIs ("" = none) */
	gchar     *key;
	guint      current;
};

static void gsurf_proxy_switch_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfProxySwitchModule, gsurf_proxy_switch_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_proxy_switch_input_init))

static gboolean
gsurf_proxy_switch_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                                    guint keyval, guint keycode, guint state, GsurfModePolicy mode)
{
	GsurfProxySwitchModule *self = GSURF_PROXY_SWITCH_MODULE(handler);
	const gchar *uri;

	if (view == NULL || self->key == NULL || self->uris->len == 0)
		return FALSE;
	if (!gsurf_keys_match(keyval, state, self->key))
		return FALSE;

	self->current = (self->current + 1) % self->uris->len;
	uri = g_ptr_array_index(self->uris, self->current);
	gsurf_view_set_proxy(view, uri);
	g_message("gsurf proxy: %s", (uri && *uri) ? uri : "none");
	return TRUE;
}

static void
gsurf_proxy_switch_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_proxy_switch_handle_key_event;
}

static const gchar *gsurf_proxy_switch_get_name(GsurfModule *m) { return "proxy_switch"; }
static const gchar *gsurf_proxy_switch_get_description(GsurfModule *m)
{
	return "Cycle through proxy presets at runtime";
}

static void
gsurf_proxy_switch_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfProxySwitchModule *self = GSURF_PROXY_SWITCH_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m, *presets;

	node = gsurf_config_get_module_node(config, "proxy_switch");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "key_cycle")) {
		g_free(self->key);
		self->key = g_strdup(yaml_mapping_get_string_member(m, "key_cycle"));
	}

	presets = yaml_mapping_get_mapping_member(m, "presets");
	if (presets != NULL) {
		GList *names = yaml_mapping_get_members(presets), *l;
		for (l = names; l != NULL; l = l->next)
			g_ptr_array_add(self->uris,
				g_strdup(yaml_mapping_get_string_member(presets, l->data)));
		g_list_free(names);
	}
	if (self->uris->len == 0)
		g_ptr_array_add(self->uris, g_strdup(""));
}

static gboolean gsurf_proxy_switch_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_proxy_switch_module_finalize(GObject *object)
{
	GsurfProxySwitchModule *self = GSURF_PROXY_SWITCH_MODULE(object);
	g_clear_pointer(&self->uris, g_ptr_array_unref);
	g_clear_pointer(&self->key, g_free);
	G_OBJECT_CLASS(gsurf_proxy_switch_module_parent_class)->finalize(object);
}

static void
gsurf_proxy_switch_module_class_init(GsurfProxySwitchModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_proxy_switch_module_finalize;
	module_class->activate = gsurf_proxy_switch_activate;
	module_class->get_name = gsurf_proxy_switch_get_name;
	module_class->get_description = gsurf_proxy_switch_get_description;
	module_class->configure = gsurf_proxy_switch_configure;
}

static void
gsurf_proxy_switch_module_init(GsurfProxySwitchModule *self)
{
	self->uris = g_ptr_array_new_with_free_func(g_free);
	self->key = g_strdup("Ctrl+Shift+p");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_PROXY_SWITCH_MODULE;
}
