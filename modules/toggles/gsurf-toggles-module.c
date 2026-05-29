/*
 * gsurf-toggles-module.c - Runtime web-engine setting toggles
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler: configurable keys toggle individual
 * #GsurfSettings fields on the active view at runtime (JavaScript,
 * images, WebGL, caret browsing, ...). Ports surf's toggle keybinds.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <string.h>

#define GSURF_TYPE_TOGGLES_MODULE (gsurf_toggles_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfTogglesModule, gsurf_toggles_module,
                     GSURF, TOGGLES_MODULE, GsurfModule)

struct _GsurfTogglesModule
{
	GsurfModule parent_instance;
	GHashTable *keys;        /* normalized keystring -> setting name (gchar*) */
	gboolean    notify;
};

static void gsurf_toggles_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfTogglesModule, gsurf_toggles_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_toggles_input_init))

/* Toggle a named boolean field; returns -1 if unknown, else new value. */
static gint
flip_setting(GsurfSettings *s, const gchar *name)
{
#define TOG(field) do { s->field = !s->field; return s->field; } while (0)
	if (g_strcmp0(name, "javascript") == 0) TOG(javascript);
	if (g_strcmp0(name, "images") == 0) TOG(images);
	if (g_strcmp0(name, "webgl") == 0) TOG(webgl);
	if (g_strcmp0(name, "webaudio") == 0) TOG(webaudio);
	if (g_strcmp0(name, "media_stream") == 0) TOG(media_stream);
	if (g_strcmp0(name, "webrtc") == 0) TOG(webrtc);
	if (g_strcmp0(name, "plugins") == 0) TOG(plugins);
	if (g_strcmp0(name, "smooth_scrolling") == 0) TOG(smooth_scrolling);
	if (g_strcmp0(name, "caret_browsing") == 0) TOG(caret_browsing);
	if (g_strcmp0(name, "site_quirks") == 0) TOG(site_quirks);
	if (g_strcmp0(name, "developer_extras") == 0) TOG(developer_extras);
#undef TOG
	return -1;
}

static gboolean
gsurf_toggles_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                               guint keyval, guint keycode, guint state,
                               GsurfModePolicy mode)
{
	GsurfTogglesModule *self = GSURF_TOGGLES_MODULE(handler);
	GsurfConfig *config = gsurf_config_get_default();
	g_autofree gchar *event = NULL;
	const gchar *name;
	gint value;

	if (view == NULL || config == NULL)
		return FALSE;

	event = gsurf_keys_to_string(keyval, state);
	if (event == NULL)
		return FALSE;

	name = g_hash_table_lookup(self->keys, event);
	if (name == NULL)
		return FALSE;

	value = flip_setting(config->settings, name);
	if (value < 0)
		return FALSE;

	gsurf_view_apply_settings(view, config->settings);
	if (self->notify)
		g_message("gsurf: %s %s", name, value ? "on" : "off");
	return TRUE;
}

static void
gsurf_toggles_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_toggles_handle_key_event;
}

static const gchar *gsurf_toggles_get_name(GsurfModule *m) { return "toggles"; }
static const gchar *gsurf_toggles_get_description(GsurfModule *m)
{
	return "Runtime toggles for JavaScript, images, WebGL, etc.";
}

static void
gsurf_toggles_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfTogglesModule *self = GSURF_TOGGLES_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m, *keys;

	node = gsurf_config_get_module_node(config, "toggles");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "notify_on_toggle"))
		self->notify = yaml_mapping_get_boolean_member(m, "notify_on_toggle");

	keys = yaml_mapping_get_mapping_member(m, "keys");
	if (keys != NULL) {
		GList *names = yaml_mapping_get_members(keys), *l;
		for (l = names; l != NULL; l = l->next) {
			const gchar *setting = l->data;
			const gchar *keystr = yaml_mapping_get_string_member(keys, setting);
			gchar *norm;

			if (keystr == NULL)
				continue;
			norm = gsurf_keys_normalize(keystr);
			if (norm != NULL)
				g_hash_table_replace(self->keys, norm, g_strdup(setting));
		}
		g_list_free(names);
	}
}

static gboolean gsurf_toggles_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_toggles_module_finalize(GObject *object)
{
	GsurfTogglesModule *self = GSURF_TOGGLES_MODULE(object);

	g_clear_pointer(&self->keys, g_hash_table_unref);

	G_OBJECT_CLASS(gsurf_toggles_module_parent_class)->finalize(object);
}

static void
gsurf_toggles_module_class_init(GsurfTogglesModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_toggles_module_finalize;
	module_class->activate = gsurf_toggles_activate;
	module_class->get_name = gsurf_toggles_get_name;
	module_class->get_description = gsurf_toggles_get_description;
	module_class->configure = gsurf_toggles_configure;
}

static void
gsurf_toggles_module_init(GsurfTogglesModule *self)
{
	self->keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->notify = TRUE;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_TOGGLES_MODULE;
}
