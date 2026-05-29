/*
 * gsurf-tabs-module.c - Keyboard tab management
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler: open/close/switch views in the active
 * window via keybinds. The library's GsurfWindow already holds multiple
 * views (a GtkStack); this module drives them. A visible tab-bar widget
 * is a planned addition (it would use the window's native widget).
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_TABS_MODULE (gsurf_tabs_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfTabsModule, gsurf_tabs_module, GSURF, TABS_MODULE, GsurfModule)

struct _GsurfTabsModule
{
	GsurfModule parent_instance;
	gchar *key_new;
	gchar *key_close;
	gchar *key_next;
	gchar *key_prev;
};

static void gsurf_tabs_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfTabsModule, gsurf_tabs_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_tabs_input_init))

static gint
index_of_active(GsurfWindow *window)
{
	GsurfView *active = gsurf_window_get_active_view(window);
	guint i, n = gsurf_window_get_n_views(window);

	for (i = 0; i < n; i++) {
		if (gsurf_window_get_nth_view(window, i) == active)
			return (gint)i;
	}
	return -1;
}

static void
open_new_tab(GsurfWindow *window)
{
	GsurfConfig *config = gsurf_config_get_default();
	GsurfView *view = gsurf_view_new();

	if (config != NULL && config->settings != NULL)
		gsurf_view_apply_settings(view, config->settings);
	gsurf_window_add_view(window, view);
	gsurf_window_set_active_view(window, view);
	gsurf_view_load_uri(view,
		(config && config->new_view_uri) ? config->new_view_uri : "about:blank");
	g_object_unref(view);
}

static void
close_active_tab(GsurfWindow *window)
{
	GsurfView *active = gsurf_window_get_active_view(window);

	/* Keep at least one view; closing the last is the host's call. */
	if (active != NULL && gsurf_window_get_n_views(window) > 1)
		gsurf_window_remove_view(window, active);
}

static void
switch_tab(GsurfWindow *window, gint delta)
{
	gint n = (gint)gsurf_window_get_n_views(window);
	gint cur = index_of_active(window);
	gint next;

	if (n <= 1 || cur < 0)
		return;
	next = ((cur + delta) % n + n) % n;
	gsurf_window_set_active_view(window, gsurf_window_get_nth_view(window, next));
}

static gboolean
gsurf_tabs_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                            guint keyval, guint keycode, guint state,
                            GsurfModePolicy mode)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(handler);
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());

	if (window == NULL)
		return FALSE;

	if (self->key_new && gsurf_keys_match(keyval, state, self->key_new)) {
		open_new_tab(window);
		return TRUE;
	}
	if (self->key_close && gsurf_keys_match(keyval, state, self->key_close)) {
		close_active_tab(window);
		return TRUE;
	}
	if (self->key_next && gsurf_keys_match(keyval, state, self->key_next)) {
		switch_tab(window, +1);
		return TRUE;
	}
	if (self->key_prev && gsurf_keys_match(keyval, state, self->key_prev)) {
		switch_tab(window, -1);
		return TRUE;
	}
	return FALSE;
}

static void
gsurf_tabs_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_tabs_handle_key_event;
}

static const gchar *gsurf_tabs_get_name(GsurfModule *m) { return "tabs"; }
static const gchar *gsurf_tabs_get_description(GsurfModule *m)
{
	return "Keyboard tab management (new/close/switch views)";
}

static gchar *
dup_member(YamlMapping *m, const gchar *key, const gchar *fallback)
{
	if (yaml_mapping_has_member(m, key))
		return g_strdup(yaml_mapping_get_string_member(m, key));
	return g_strdup(fallback);
}

static void
gsurf_tabs_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "tabs");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	g_free(self->key_new);   self->key_new   = dup_member(m, "key_new", "Ctrl+t");
	g_free(self->key_close); self->key_close = dup_member(m, "key_close", "Ctrl+w");
	g_free(self->key_next);  self->key_next  = dup_member(m, "key_next", "Ctrl+Tab");
	g_free(self->key_prev);  self->key_prev  = dup_member(m, "key_prev", "Ctrl+Shift+Tab");
}

static gboolean gsurf_tabs_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_tabs_module_finalize(GObject *object)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(object);

	g_clear_pointer(&self->key_new, g_free);
	g_clear_pointer(&self->key_close, g_free);
	g_clear_pointer(&self->key_next, g_free);
	g_clear_pointer(&self->key_prev, g_free);

	G_OBJECT_CLASS(gsurf_tabs_module_parent_class)->finalize(object);
}

static void
gsurf_tabs_module_class_init(GsurfTabsModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_tabs_module_finalize;
	module_class->activate = gsurf_tabs_activate;
	module_class->get_name = gsurf_tabs_get_name;
	module_class->get_description = gsurf_tabs_get_description;
	module_class->configure = gsurf_tabs_configure;
}

static void
gsurf_tabs_module_init(GsurfTabsModule *self)
{
	self->key_new = g_strdup("Ctrl+t");
	self->key_close = g_strdup("Ctrl+w");
	self->key_next = g_strdup("Ctrl+Tab");
	self->key_prev = g_strdup("Ctrl+Shift+Tab");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_TABS_MODULE;
}
