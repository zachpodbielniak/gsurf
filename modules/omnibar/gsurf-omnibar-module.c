/*
 * gsurf-omnibar-module.c - History completion via dmenu
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler: a key runs dmenu over the history file
 * (most-recent first) and loads the chosen URI. Ports surf's omnibar.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <string.h>

#define GSURF_TYPE_OMNIBAR_MODULE (gsurf_omnibar_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfOmnibarModule, gsurf_omnibar_module,
                     GSURF, OMNIBAR_MODULE, GsurfModule)

struct _GsurfOmnibarModule
{
	GsurfModule parent_instance;
	gchar *key;
	gchar *history_file;
};

static void gsurf_omnibar_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfOmnibarModule, gsurf_omnibar_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_omnibar_input_init))

static gboolean
gsurf_omnibar_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                               guint keyval, guint keycode, guint state, GsurfModePolicy mode)
{
	GsurfOmnibarModule *self = GSURF_OMNIBAR_MODULE(handler);
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *cmd = NULL;
	g_autofree gchar *out = NULL;
	gchar *argv[] = { "sh", "-c", NULL, NULL };

	if (view == NULL || self->key == NULL || self->history_file == NULL)
		return FALSE;
	if (!gsurf_keys_match(keyval, state, self->key))
		return FALSE;

	/* Most-recent first, de-duplicated, piped into dmenu. */
	quoted = g_shell_quote(self->history_file);
	cmd = g_strdup_printf("tac %s | awk '!seen[$0]++' | dmenu -l 20 -i | awk '{print $1}'", quoted);
	argv[2] = cmd;
	if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			&out, NULL, NULL, NULL) && out != NULL) {
		g_strstrip(out);
		if (*out != '\0')
			gsurf_view_load_uri(view, out);
	}
	return TRUE;
}

static void
gsurf_omnibar_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_omnibar_handle_key_event;
}

static const gchar *gsurf_omnibar_get_name(GsurfModule *m) { return "omnibar"; }
static const gchar *gsurf_omnibar_get_description(GsurfModule *m)
{
	return "History completion popup via dmenu";
}

static void
gsurf_omnibar_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfOmnibarModule *self = GSURF_OMNIBAR_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node, *hist;
	YamlMapping *m;
	const gchar *hf = NULL;

	node = gsurf_config_get_module_node(config, "omnibar");
	if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING) {
		m = yaml_node_get_mapping(node);
		if (yaml_mapping_has_member(m, "key")) {
			g_free(self->key);
			self->key = g_strdup(yaml_mapping_get_string_member(m, "key"));
		}
	}

	/* Reuse the history module's file if configured. */
	hist = gsurf_config_get_module_node(config, "history");
	if (hist != NULL && yaml_node_get_node_type(hist) == YAML_NODE_MAPPING)
		hf = yaml_mapping_get_string_member(yaml_node_get_mapping(hist), "file");
	g_free(self->history_file);
	self->history_file = (hf && hf[0] == '~' && hf[1] == '/')
		? g_build_filename(g_get_home_dir(), hf + 2, NULL)
		: g_strdup(hf ? hf : g_build_filename(g_get_user_data_dir(), "gsurf", "history", NULL));
}

static gboolean gsurf_omnibar_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_omnibar_module_finalize(GObject *object)
{
	GsurfOmnibarModule *self = GSURF_OMNIBAR_MODULE(object);
	g_clear_pointer(&self->key, g_free);
	g_clear_pointer(&self->history_file, g_free);
	G_OBJECT_CLASS(gsurf_omnibar_module_parent_class)->finalize(object);
}

static void
gsurf_omnibar_module_class_init(GsurfOmnibarModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_omnibar_module_finalize;
	module_class->activate = gsurf_omnibar_activate;
	module_class->get_name = gsurf_omnibar_get_name;
	module_class->get_description = gsurf_omnibar_get_description;
	module_class->configure = gsurf_omnibar_configure;
}

static void
gsurf_omnibar_module_init(GsurfOmnibarModule *self)
{
	self->key = g_strdup("Ctrl+Shift+o");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_OMNIBAR_MODULE;
}
