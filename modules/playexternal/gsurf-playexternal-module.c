/*
 * gsurf-playexternal-module.c - Send the current URI to an external player
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler: a configurable key pipes the active
 * view's URI to an external command (mpv by default). Ports surf's
 * playexternal patch.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_PLAYEXTERNAL_MODULE (gsurf_playexternal_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfPlayexternalModule, gsurf_playexternal_module,
                     GSURF, PLAYEXTERNAL_MODULE, GsurfModule)

struct _GsurfPlayexternalModule
{
	GsurfModule parent_instance;
	gchar *command;
	gchar *key;
};

static void gsurf_playexternal_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfPlayexternalModule, gsurf_playexternal_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_playexternal_input_init))

static gboolean
gsurf_playexternal_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                                    guint keyval, guint keycode, guint state,
                                    GsurfModePolicy mode)
{
	GsurfPlayexternalModule *self = GSURF_PLAYEXTERNAL_MODULE(handler);
	const gchar *uri;
	gchar **argv = NULL;
	GPtrArray *args;
	g_autoptr(GError) error = NULL;
	gchar **cmd_parts;
	guint i;

	if (view == NULL || self->key == NULL)
		return FALSE;
	if (!gsurf_keys_match(keyval, state, self->key))
		return FALSE;

	uri = gsurf_view_get_uri(view);
	if (uri == NULL || *uri == '\0')
		return TRUE;

	/* Build argv from the command words plus the URI. */
	args = g_ptr_array_new();
	cmd_parts = g_strsplit(self->command ? self->command : "mpv --", " ", -1);
	for (i = 0; cmd_parts[i] != NULL; i++)
		if (*cmd_parts[i] != '\0')
			g_ptr_array_add(args, cmd_parts[i]);
	g_ptr_array_add(args, (gpointer)uri);
	g_ptr_array_add(args, NULL);
	argv = (gchar **)args->pdata;

	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
			NULL, NULL, NULL, &error))
		g_warning("gsurf playexternal: %s", error->message);

	g_ptr_array_free(args, TRUE);
	g_strfreev(cmd_parts);
	return TRUE;
}

static void
gsurf_playexternal_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_playexternal_handle_key_event;
}

static const gchar *gsurf_playexternal_get_name(GsurfModule *m) { return "playexternal"; }
static const gchar *gsurf_playexternal_get_description(GsurfModule *m)
{
	return "Send the current URI to an external player (mpv)";
}

static void
gsurf_playexternal_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfPlayexternalModule *self = GSURF_PLAYEXTERNAL_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "playexternal");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "command")) {
		g_free(self->command);
		self->command = g_strdup(yaml_mapping_get_string_member(m, "command"));
	}
	if (yaml_mapping_has_member(m, "key")) {
		g_free(self->key);
		self->key = g_strdup(yaml_mapping_get_string_member(m, "key"));
	}
}

static gboolean gsurf_playexternal_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_playexternal_module_finalize(GObject *object)
{
	GsurfPlayexternalModule *self = GSURF_PLAYEXTERNAL_MODULE(object);

	g_clear_pointer(&self->command, g_free);
	g_clear_pointer(&self->key, g_free);

	G_OBJECT_CLASS(gsurf_playexternal_module_parent_class)->finalize(object);
}

static void
gsurf_playexternal_module_class_init(GsurfPlayexternalModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_playexternal_module_finalize;
	module_class->activate = gsurf_playexternal_activate;
	module_class->get_name = gsurf_playexternal_get_name;
	module_class->get_description = gsurf_playexternal_get_description;
	module_class->configure = gsurf_playexternal_configure;
}

static void
gsurf_playexternal_module_init(GsurfPlayexternalModule *self)
{
	self->command = g_strdup("mpv --");
	self->key = g_strdup("Ctrl+m");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_PLAYEXTERNAL_MODULE;
}
