/*
 * gsurf-tabs-module.c - Tab bar + keyboard tab management
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Drives the library's multi-view GsurfWindow (a GtkStack) with both a
 * visible clickable tab strip (packed into the window chrome) and keyboard
 * binds to open/close/switch/reopen views. Implements #GsurfInputHandler.
 * GTK3.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <gtk/gtk.h>

#define GSURF_TYPE_TABS_MODULE (gsurf_tabs_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfTabsModule, gsurf_tabs_module, GSURF, TABS_MODULE, GsurfModule)

struct _GsurfTabsModule
{
	GsurfModule parent_instance;
	gchar *key_new;
	gchar *key_close;
	gchar *key_next;
	gchar *key_prev;
	gchar *key_reopen;

	/* Visible strip. */
	GtkWidget       *bar;       /* horizontal GtkBox of tab buttons */
	GsurfWindow     *window;    /* tracked window (weak) */
	GtkCssProvider  *css;
	gboolean         position_top;
	gboolean         show_single;
	gchar           *bg_color;
	gchar           *active_color;

	GQueue *closed_uris;        /* recently closed view URIs (head = most recent) */
};

static void gsurf_tabs_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfTabsModule, gsurf_tabs_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_tabs_input_init))

static void rebuild_strip(GsurfTabsModule *self);

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

static GsurfView *
open_tab_uri(GsurfWindow *window, const gchar *uri)
{
	GsurfConfig *config = gsurf_config_get_default();
	GsurfView *view = gsurf_view_new();

	if (config != NULL && config->settings != NULL)
		gsurf_view_apply_settings(view, config->settings);
	gsurf_window_add_view(window, view);
	gsurf_window_set_active_view(window, view);
	gsurf_view_load_uri(view, (uri != NULL && *uri != '\0') ? uri
		: ((config && config->new_view_uri) ? config->new_view_uri : "about:blank"));
	g_object_unref(view);
	return view;
}

static void
remember_closed(GsurfTabsModule *self, GsurfView *view)
{
	const gchar *uri = gsurf_view_get_uri(view);

	if (uri != NULL && *uri != '\0' && g_strcmp0(uri, "about:blank") != 0)
		g_queue_push_head(self->closed_uris, g_strdup(uri));
}

static void
close_active_tab(GsurfTabsModule *self, GsurfWindow *window)
{
	GsurfView *active = gsurf_window_get_active_view(window);

	/* Keep at least one view; closing the last is the host's call. */
	if (active != NULL && gsurf_window_get_n_views(window) > 1) {
		remember_closed(self, active);
		gsurf_window_remove_view(window, active);
	}
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

/* --- Visible tab strip --- */

static void
on_tab_clicked(GtkButton *button, gpointer user_data)
{
	GsurfTabsModule *self = user_data;
	GsurfView *view = g_object_get_data(G_OBJECT(button), "gsurf-view");

	if (self->window != NULL && view != NULL)
		gsurf_window_set_active_view(self->window, view);
}

static void
on_tab_close_clicked(GtkButton *button, gpointer user_data)
{
	GsurfTabsModule *self = user_data;
	GsurfView *view = g_object_get_data(G_OBJECT(button), "gsurf-view");

	if (self->window == NULL || view == NULL)
		return;
	if (gsurf_window_get_n_views(self->window) <= 1)
		return;
	remember_closed(self, view);
	gsurf_window_remove_view(self->window, view);
}

static void
on_view_title_changed(GsurfView *view, const gchar *title, gpointer user_data)
{
	rebuild_strip(GSURF_TABS_MODULE(user_data));
}

static void
update_visibility(GsurfTabsModule *self)
{
	guint n;

	if (self->bar == NULL || self->window == NULL)
		return;
	n = gsurf_window_get_n_views(self->window);
	if (n > 1 || self->show_single)
		gtk_widget_show(self->bar);
	else
		gtk_widget_hide(self->bar);
}

static void
rebuild_strip(GsurfTabsModule *self)
{
	GsurfWindow *window = self->window;
	GList *kids, *l;
	guint i, n;
	GsurfView *active;

	if (self->bar == NULL || window == NULL)
		return;

	kids = gtk_container_get_children(GTK_CONTAINER(self->bar));
	for (l = kids; l != NULL; l = l->next)
		gtk_widget_destroy(GTK_WIDGET(l->data));
	g_list_free(kids);

	n = gsurf_window_get_n_views(window);
	active = gsurf_window_get_active_view(window);

	for (i = 0; i < n; i++) {
		GsurfView *view = gsurf_window_get_nth_view(window, i);
		const gchar *title = gsurf_view_get_title(view);
		const gchar *uri = gsurf_view_get_uri(view);
		g_autofree gchar *label = NULL;
		GtkWidget *tab, *btn, *close;
		GtkStyleContext *ctx;

		if (title == NULL || *title == '\0')
			title = (uri != NULL && *uri != '\0') ? uri : "New Tab";
		label = (g_utf8_strlen(title, -1) > 24)
			? g_strdup_printf("%.*s…", (int)(g_utf8_offset_to_pointer(title, 24) - title), title)
			: g_strdup(title);

		tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		ctx = gtk_widget_get_style_context(tab);
		gtk_style_context_add_class(ctx, "gsurf-tab");
		if (view == active)
			gtk_style_context_add_class(ctx, "gsurf-tab-active");

		btn = gtk_button_new_with_label(label);
		gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
		g_object_set_data(G_OBJECT(btn), "gsurf-view", view);
		g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_clicked), self);

		close = gtk_button_new_with_label("×");
		gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
		g_object_set_data(G_OBJECT(close), "gsurf-view", view);
		g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close_clicked), self);

		gtk_box_pack_start(GTK_BOX(tab), btn, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(tab), close, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(self->bar), tab, FALSE, FALSE, 0);

		/* Refresh the strip when a view's title changes (connect once). */
		if (g_object_get_data(G_OBJECT(view), "gsurf-tabstrip-wired") == NULL) {
			g_object_set_data(G_OBJECT(view), "gsurf-tabstrip-wired", GINT_TO_POINTER(1));
			g_signal_connect(view, "title-changed",
				G_CALLBACK(on_view_title_changed), self);
		}
	}

	gtk_widget_show_all(self->bar);
	update_visibility(self);
}

static void
on_window_views_changed(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	rebuild_strip(GSURF_TABS_MODULE(user_data));
}

static void
install_css(GsurfTabsModule *self)
{
	g_autofree gchar *css = NULL;

	self->css = gtk_css_provider_new();
	css = g_strdup_printf(
		".gsurf-tabbar { background-color: %s; }\n"
		".gsurf-tab button { padding: 2px 8px; }\n"
		".gsurf-tab-active { background-color: %s; }\n"
		".gsurf-tab-active button { font-weight: bold; }\n",
		self->bg_color ? self->bg_color : "#181825",
		self->active_color ? self->active_color : "#313244");
	gtk_css_provider_load_from_data(self->css, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(self->css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* --- Key handling --- */

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
		open_tab_uri(window, NULL);
		return TRUE;
	}
	if (self->key_close && gsurf_keys_match(keyval, state, self->key_close)) {
		close_active_tab(self, window);
		return TRUE;
	}
	if (self->key_reopen && gsurf_keys_match(keyval, state, self->key_reopen)) {
		if (!g_queue_is_empty(self->closed_uris)) {
			g_autofree gchar *uri = g_queue_pop_head(self->closed_uris);
			open_tab_uri(window, uri);
		}
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
	return "Tab bar + keyboard tab management (new/close/switch/reopen)";
}

static gboolean
gsurf_tabs_activate(GsurfModule *module)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(module);
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());

	if (window == NULL)
		return TRUE;

	self->window = window;
	g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&self->window);

	install_css(self);

	self->bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_style_context_add_class(gtk_widget_get_style_context(self->bar), "gsurf-tabbar");

	if (self->position_top)
		gsurf_window_add_top_widget(window, self->bar);
	else
		gsurf_window_add_bottom_widget(window, self->bar);

	g_signal_connect(window, "view-added", G_CALLBACK(on_window_views_changed), self);
	g_signal_connect(window, "view-removed", G_CALLBACK(on_window_views_changed), self);
	g_signal_connect(window, "active-view-changed", G_CALLBACK(on_window_views_changed), self);

	rebuild_strip(self);
	return TRUE;
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

	g_free(self->key_new);    self->key_new    = dup_member(m, "key_new", "Ctrl+t");
	g_free(self->key_close);  self->key_close  = dup_member(m, "key_close", "Ctrl+w");
	g_free(self->key_next);   self->key_next   = dup_member(m, "key_next", "Ctrl+Tab");
	g_free(self->key_prev);   self->key_prev   = dup_member(m, "key_prev", "Ctrl+Shift+Tab");
	g_free(self->key_reopen); self->key_reopen = dup_member(m, "key_reopen", "Ctrl+Shift+t");

	if (yaml_mapping_has_member(m, "position"))
		self->position_top = g_strcmp0(yaml_mapping_get_string_member(m, "position"), "bottom") != 0;
	if (yaml_mapping_has_member(m, "show_single"))
		self->show_single = yaml_mapping_get_boolean_member(m, "show_single");
	if (yaml_mapping_has_member(m, "bg_color")) {
		g_free(self->bg_color);
		self->bg_color = g_strdup(yaml_mapping_get_string_member(m, "bg_color"));
	}
	if (yaml_mapping_has_member(m, "active_color")) {
		g_free(self->active_color);
		self->active_color = g_strdup(yaml_mapping_get_string_member(m, "active_color"));
	}
}

static void
gsurf_tabs_module_finalize(GObject *object)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(object);

	if (self->window != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->window), (gpointer *)&self->window);
	g_clear_object(&self->css);
	g_clear_pointer(&self->key_new, g_free);
	g_clear_pointer(&self->key_close, g_free);
	g_clear_pointer(&self->key_next, g_free);
	g_clear_pointer(&self->key_prev, g_free);
	g_clear_pointer(&self->key_reopen, g_free);
	g_clear_pointer(&self->bg_color, g_free);
	g_clear_pointer(&self->active_color, g_free);
	if (self->closed_uris != NULL) {
		g_queue_free_full(self->closed_uris, g_free);
		self->closed_uris = NULL;
	}

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
	self->key_reopen = g_strdup("Ctrl+Shift+t");
	self->position_top = TRUE;
	self->show_single = FALSE;
	self->closed_uris = g_queue_new();
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_TABS_MODULE;
}
