/*
 * gsurf-chromebar-module.c - Address bar
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A GtkEntry address bar packed into the window chrome. Enter loads the
 * (URI-handler-rewritten) input in the active view; the bar tracks the
 * active view's URI. Focus it with the configured key (Ctrl+l). GTK3.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <gtk/gtk.h>

#define GSURF_TYPE_CHROMEBAR_MODULE (gsurf_chromebar_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfChromebarModule, gsurf_chromebar_module,
                     GSURF, CHROMEBAR_MODULE, GsurfModule)

struct _GsurfChromebarModule
{
	GsurfModule parent_instance;
	GtkWidget *entry;
	gchar     *key_focus;
	GsurfView *tracked;     /* view whose uri-changed we follow */
	gulong     tracked_id;
};

static void gsurf_chromebar_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfChromebarModule, gsurf_chromebar_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_chromebar_input_init))

static GsurfView *
active_view(void)
{
	return gsurf_module_manager_get_active_view(gsurf_module_manager_get_default());
}

static void
on_view_uri_changed(GsurfView *view, const gchar *uri, gpointer user_data)
{
	GsurfChromebarModule *self = user_data;
	if (self->entry != NULL && !gtk_widget_has_focus(self->entry))
		gtk_entry_set_text(GTK_ENTRY(self->entry), uri ? uri : "");
}

static void
track_view(GsurfChromebarModule *self, GsurfView *view)
{
	if (self->tracked == view)
		return;
	if (self->tracked != NULL && self->tracked_id != 0) {
		g_signal_handler_disconnect(self->tracked, self->tracked_id);
		g_object_remove_weak_pointer(G_OBJECT(self->tracked), (gpointer *)&self->tracked);
	}
	self->tracked = view;
	self->tracked_id = 0;
	if (view != NULL) {
		g_object_add_weak_pointer(G_OBJECT(view), (gpointer *)&self->tracked);
		self->tracked_id = g_signal_connect(view, "uri-changed",
			G_CALLBACK(on_view_uri_changed), self);
		on_view_uri_changed(view, gsurf_view_get_uri(view), self);
	}
}

static void
on_entry_activate(GtkEntry *entry, gpointer user_data)
{
	GsurfView *view = active_view();
	const gchar *text = gtk_entry_get_text(entry);
	g_autofree gchar *rewritten = NULL;

	if (view == NULL || text == NULL || *text == '\0')
		return;
	rewritten = gsurf_module_manager_dispatch_rewrite_uri(
		gsurf_module_manager_get_default(), text);
	gsurf_view_load_uri(view, rewritten ? rewritten : text);
}

static void
on_active_view_changed(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	track_view(GSURF_CHROMEBAR_MODULE(user_data), view);
}

static gboolean
gsurf_chromebar_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                                 guint keyval, guint keycode, guint state, GsurfModePolicy mode)
{
	GsurfChromebarModule *self = GSURF_CHROMEBAR_MODULE(handler);

	if (self->entry == NULL || self->key_focus == NULL)
		return FALSE;
	if (!gsurf_keys_match(keyval, state, self->key_focus))
		return FALSE;

	gtk_widget_grab_focus(self->entry);
	gtk_editable_select_region(GTK_EDITABLE(self->entry), 0, -1);
	return TRUE;
}

static void
gsurf_chromebar_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_chromebar_handle_key_event;
}

static const gchar *gsurf_chromebar_get_name(GsurfModule *m) { return "chromebar"; }
static const gchar *gsurf_chromebar_get_description(GsurfModule *m)
{
	return "Address bar with search fallback";
}

static gboolean
gsurf_chromebar_activate(GsurfModule *module)
{
	GsurfChromebarModule *self = GSURF_CHROMEBAR_MODULE(module);
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());

	if (window == NULL)
		return TRUE;

	self->entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry), "Enter URL or search...");
	g_signal_connect(self->entry, "activate", G_CALLBACK(on_entry_activate), self);
	gsurf_window_add_top_widget(window, self->entry);

	g_signal_connect(window, "active-view-changed",
		G_CALLBACK(on_active_view_changed), self);
	track_view(self, gsurf_window_get_active_view(window));
	return TRUE;
}

static void
gsurf_chromebar_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfChromebarModule *self = GSURF_CHROMEBAR_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "chromebar");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);
	if (yaml_mapping_has_member(m, "key_focus")) {
		g_free(self->key_focus);
		self->key_focus = g_strdup(yaml_mapping_get_string_member(m, "key_focus"));
	}
}

static void
gsurf_chromebar_module_finalize(GObject *object)
{
	GsurfChromebarModule *self = GSURF_CHROMEBAR_MODULE(object);
	track_view(self, NULL);
	g_clear_pointer(&self->key_focus, g_free);
	G_OBJECT_CLASS(gsurf_chromebar_module_parent_class)->finalize(object);
}

static void
gsurf_chromebar_module_class_init(GsurfChromebarModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_chromebar_module_finalize;
	module_class->activate = gsurf_chromebar_activate;
	module_class->get_name = gsurf_chromebar_get_name;
	module_class->get_description = gsurf_chromebar_get_description;
	module_class->configure = gsurf_chromebar_configure;
}

static void
gsurf_chromebar_module_init(GsurfChromebarModule *self)
{
	self->key_focus = g_strdup("Ctrl+l");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_CHROMEBAR_MODULE;
}
