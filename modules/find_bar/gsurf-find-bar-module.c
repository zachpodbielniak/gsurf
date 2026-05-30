/*
 * gsurf-find-bar-module.c - In-page find bar
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A GtkSearchEntry packed into the window chrome that drives the active
 * view's find controller. The configured open key (default "/") reveals
 * the bar and focuses it; typing searches incrementally; Enter / Shift+Enter
 * step through matches; Escape hides the bar and clears the highlight. The
 * bare next/previous keys (n/N) step matches while the page is focused.
 * Implements #GsurfInputHandler. GTK3.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#define GSURF_TYPE_FIND_BAR_MODULE (gsurf_find_bar_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfFindBarModule, gsurf_find_bar_module,
                     GSURF, FIND_BAR_MODULE, GsurfModule)

struct _GsurfFindBarModule
{
	GsurfModule parent_instance;
	GtkWidget *bar;     /* container packed into the window chrome */
	GtkWidget *entry;   /* GtkSearchEntry */
	GtkWidget *label;   /* small status/hint label */
	gchar    *key_open;
	gchar    *key_next;
	gchar    *key_prev;
	gboolean  match_case;
};

static void gsurf_find_bar_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfFindBarModule, gsurf_find_bar_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_find_bar_input_init))

static GsurfView *
active_view(void)
{
	return gsurf_module_manager_get_active_view(gsurf_module_manager_get_default());
}

static void
find_bar_search(GsurfFindBarModule *self, gboolean forward)
{
	GsurfView *view = active_view();
	const gchar *text;

	if (view == NULL || self->entry == NULL)
		return;
	text = gtk_entry_get_text(GTK_ENTRY(self->entry));
	if (text == NULL)
		text = "";
	gsurf_view_find(view, text, self->match_case, forward);
}

static void
find_bar_close(GsurfFindBarModule *self)
{
	GsurfView *view = active_view();

	if (self->entry != NULL)
		gtk_entry_set_text(GTK_ENTRY(self->entry), "");
	if (view != NULL) {
		/* Empty search clears the highlight, then return focus to the page. */
		gsurf_view_find(view, "", self->match_case, TRUE);
		GtkWidget *w = gsurf_view_get_native_widget(view);
		if (w != NULL)
			gtk_widget_grab_focus(w);
	}
	if (self->bar != NULL)
		gtk_widget_hide(self->bar);
}

static void
find_bar_open(GsurfFindBarModule *self)
{
	if (self->bar == NULL || self->entry == NULL)
		return;
	gtk_widget_show_all(self->bar);
	gtk_widget_grab_focus(self->entry);
	gtk_editable_select_region(GTK_EDITABLE(self->entry), 0, -1);
}

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
	find_bar_search(GSURF_FIND_BAR_MODULE(user_data), TRUE);
}

static void
on_entry_activate(GtkEntry *entry, gpointer user_data)
{
	/* Enter advances to the next match. */
	find_bar_search(GSURF_FIND_BAR_MODULE(user_data), TRUE);
	gsurf_view_find_next(active_view());
}

static gboolean
on_entry_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	GsurfFindBarModule *self = GSURF_FIND_BAR_MODULE(user_data);

	if (event->keyval == GDK_KEY_Escape) {
		find_bar_close(self);
		return TRUE;
	}
	/* Shift+Enter steps backwards through matches. */
	if ((event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) &&
	    (event->state & GDK_SHIFT_MASK)) {
		gsurf_view_find_previous(active_view());
		return TRUE;
	}
	return FALSE;
}

static gboolean
gsurf_find_bar_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                                guint keyval, guint keycode, guint state, GsurfModePolicy mode)
{
	GsurfFindBarModule *self = GSURF_FIND_BAR_MODULE(handler);

	if (self->key_open != NULL && gsurf_keys_match(keyval, state, self->key_open)) {
		find_bar_open(self);
		return TRUE;
	}
	if (view == NULL)
		return FALSE;
	if (self->key_next != NULL && gsurf_keys_match(keyval, state, self->key_next)) {
		gsurf_view_find_next(view);
		return TRUE;
	}
	if (self->key_prev != NULL && gsurf_keys_match(keyval, state, self->key_prev)) {
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
	return "In-page find bar with incremental search";
}

static gboolean
gsurf_find_bar_activate(GsurfModule *module)
{
	GsurfFindBarModule *self = GSURF_FIND_BAR_MODULE(module);
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());

	if (window == NULL)
		return TRUE;

	self->bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	self->label = gtk_label_new("Find:");
	self->entry = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry), "Find in page…");

	gtk_box_pack_start(GTK_BOX(self->bar), self->label, FALSE, FALSE, 4);
	gtk_box_pack_start(GTK_BOX(self->bar), self->entry, TRUE, TRUE, 0);

	g_signal_connect(self->entry, "search-changed", G_CALLBACK(on_search_changed), self);
	g_signal_connect(self->entry, "activate", G_CALLBACK(on_entry_activate), self);
	g_signal_connect(self->entry, "key-press-event", G_CALLBACK(on_entry_key_press), self);

	gsurf_window_add_bottom_widget(window, self->bar);
	gtk_widget_hide(self->bar);   /* hidden until the open key is pressed */
	return TRUE;
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
	if (yaml_mapping_has_member(m, "key_open")) {
		g_free(self->key_open);
		self->key_open = g_strdup(yaml_mapping_get_string_member(m, "key_open"));
	}
	if (yaml_mapping_has_member(m, "key_next")) {
		g_free(self->key_next);
		self->key_next = g_strdup(yaml_mapping_get_string_member(m, "key_next"));
	}
	if (yaml_mapping_has_member(m, "key_prev")) {
		g_free(self->key_prev);
		self->key_prev = g_strdup(yaml_mapping_get_string_member(m, "key_prev"));
	}
	if (yaml_mapping_has_member(m, "match_case"))
		self->match_case = yaml_mapping_get_boolean_member(m, "match_case");
}

static void
gsurf_find_bar_module_finalize(GObject *object)
{
	GsurfFindBarModule *self = GSURF_FIND_BAR_MODULE(object);
	g_clear_pointer(&self->key_open, g_free);
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
	self->key_open = g_strdup("slash");
	self->key_next = g_strdup("n");
	self->key_prev = g_strdup("N");
	self->match_case = FALSE;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_FIND_BAR_MODULE;
}
