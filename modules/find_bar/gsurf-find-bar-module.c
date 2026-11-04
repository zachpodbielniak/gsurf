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
 * Implements #GsurfInputHandler. Builds against GTK3 and GTK4 through the
 * module-private modules/include/gsurf-chrome-compat.h (on GTK4 the
 * GtkSearchEntry is a GtkEditable, not a GtkEntry).
 *
 * The module keeps its own reference to the bar from activation until
 * deactivate()/dispose(), which detach it from the window chrome and are
 * idempotent, so a host may destroy the window before the module.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include "modules/include/gsurf-chrome-compat.h"

#define GSURF_TYPE_FIND_BAR_MODULE (gsurf_find_bar_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfFindBarModule, gsurf_find_bar_module,
                     GSURF, FIND_BAR_MODULE, GsurfModule)

struct _GsurfFindBarModule
{
	GsurfModule parent_instance;
	GtkWidget *bar;     /* container packed into the window chrome (owned ref) */
	GtkWidget *entry;   /* GtkSearchEntry, a child of @bar (owned ref) */
	GtkWidget *label;   /* small status/hint label, a child of @bar (borrowed) */
	gchar    *key_open;
	gchar    *key_next;
	gchar    *key_prev;
	gboolean  match_case;
	gboolean  active;   /* a search is in progress: n/N step through it */
};

static void gsurf_find_bar_input_init(GsurfInputHandlerInterface *iface);
static void gsurf_find_bar_keybind_init(GsurfKeybindProviderInterface *iface);
static void gsurf_find_bar_action_init(GsurfActionHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfFindBarModule, gsurf_find_bar_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_find_bar_input_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_KEYBIND_PROVIDER, gsurf_find_bar_keybind_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_ACTION_HANDLER, gsurf_find_bar_action_init))

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
	text = gsurf_chrome_editable_get_text(self->entry);
	self->active = *text != '\0';
	gsurf_view_find(view, text, self->match_case, forward);
}

static void
find_bar_close(GsurfFindBarModule *self)
{
	GsurfView *view = active_view();

	if (self->entry != NULL)
		gsurf_chrome_editable_set_text(self->entry, NULL);
	self->active = FALSE;
	if (view != NULL) {
		/* Empty search clears the highlight, then return focus to the page. */
		gsurf_view_find(view, "", self->match_case, TRUE);
		gsurf_view_focus(view);
	}
	if (self->bar != NULL)
		gtk_widget_set_visible(self->bar, FALSE);
}

static void
find_bar_open(GsurfFindBarModule *self)
{
	if (self->bar == NULL || self->entry == NULL)
		return;
	gtk_widget_set_visible(self->bar, TRUE);
	gtk_widget_grab_focus(self->entry);
	gtk_editable_select_region(GTK_EDITABLE(self->entry), 0, -1);
}

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
	find_bar_search(GSURF_FIND_BAR_MODULE(user_data), TRUE);
}

static void
on_entry_activate(GtkWidget *entry, gpointer user_data)
{
	GsurfView *view = active_view();

	/* Enter confirms the search (vim-style): the bar stays up showing the
	 * query, the page takes focus, and n/N step through the matches. */
	find_bar_search(GSURF_FIND_BAR_MODULE(user_data), TRUE);
	if (view != NULL)
		gsurf_view_focus(view);
}

/* Escape closes the bar; Shift+Enter steps backwards through matches.
 * Runs before the entry's own key handling (see gsurf-chrome-compat.h),
 * so Shift+Enter never reaches the entry's "activate". */
static gboolean
on_entry_key_press(GtkWidget *widget, guint keyval, GdkModifierType state,
                   gpointer user_data)
{
	GsurfFindBarModule *self = GSURF_FIND_BAR_MODULE(user_data);
	GsurfView *view;

	if (keyval == GDK_KEY_Escape) {
		find_bar_close(self);
		return TRUE;
	}
	if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
	    (state & GDK_SHIFT_MASK)) {
		view = active_view();
		if (view != NULL)
			gsurf_view_find_previous(view);
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
	/* n/N only mean "next/previous match" while a search is up; otherwise
	 * they stay free for the page and other modules. */
	if (view == NULL || !self->active)
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

/* The find keybind (Ctrl+f by default) opens the bar. */
static gboolean
gsurf_find_bar_handle_action(GsurfActionHandler *handler, GsurfView *view,
                             GsurfAction action)
{
	GsurfFindBarModule *self = GSURF_FIND_BAR_MODULE(handler);

	switch (action) {
	case GSURF_ACTION_FIND:
		if (self->bar == NULL)
			return FALSE;
		find_bar_open(self);
		return TRUE;
	default:
		return FALSE;
	}
}

static void
gsurf_find_bar_action_init(GsurfActionHandlerInterface *iface)
{
	iface->handle_action = gsurf_find_bar_handle_action;
}

static void
gsurf_find_bar_list_keybinds(GsurfKeybindProvider *provider, GPtrArray *entries)
{
	GsurfFindBarModule *self = GSURF_FIND_BAR_MODULE(provider);

	gsurf_keybind_help_append(entries, self->key_open,
		"Open find-in-page", "find_bar", "find");
	gsurf_keybind_help_append(entries, self->key_next,
		"Find next match", "find_bar", "find-next");
	gsurf_keybind_help_append(entries, self->key_prev,
		"Find previous match", "find_bar", "find-prev");
}

static void
gsurf_find_bar_keybind_init(GsurfKeybindProviderInterface *iface)
{
	iface->list_keybinds = gsurf_find_bar_list_keybinds;
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

	if (gsurf_kiosk_is_enabled())
		return TRUE;

	self->bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	/* Retain the bar even if the host destroys its container first. */
	g_object_ref_sink(self->bar);
	self->label = gtk_label_new("Find:");
	self->entry = gtk_search_entry_new();
	/* A property, not gtk_entry_set_placeholder_text(): on GTK4 the
	 * search entry is not a GtkEntry. */
	g_object_set(self->entry, "placeholder-text", "Find in page…", NULL);

	gsurf_chrome_box_pack(self->bar, self->label, FALSE, 4);
	gsurf_chrome_box_pack(self->bar, self->entry, TRUE, 0);
	/* GTK3 destroys a container's children with it, so a host that
	 * destroys the chrome first would otherwise leave the entry dangling
	 * before deactivate() disconnects it; keep our own reference. */
	g_object_ref(self->entry);

	g_signal_connect(self->entry, "search-changed", G_CALLBACK(on_search_changed), self);
	g_signal_connect(self->entry, "activate", G_CALLBACK(on_entry_activate), self);
	gsurf_chrome_connect_key_press(self->entry, on_entry_key_press, self);

	gsurf_window_add_bottom_widget(window, self->bar);
	/* Hidden until the open key is pressed, and kept hidden by the
	 * window's startup show-all on GTK3. */
	gsurf_chrome_widget_start_hidden(self->bar);
	return TRUE;
}

/* Tear down callbacks and chrome; reactivation then packs one new bar.
 * Idempotent: also run from dispose(). */
static void
gsurf_find_bar_deactivate(GsurfModule *module)
{
	GsurfFindBarModule *self = GSURF_FIND_BAR_MODULE(module);

	self->active = FALSE;
	if (self->entry != NULL) {
		gsurf_chrome_disconnect_key_press(self->entry, self);
		g_signal_handlers_disconnect_by_data(self->entry, self);
		g_clear_object(&self->entry);
	}
	self->label = NULL;
	if (self->bar != NULL) {
		/* The entry and label are children of the bar and go with it. */
		gsurf_chrome_widget_detach(self->bar);
		g_clear_object(&self->bar);
	}
}

/* Disposal also works when a caller never explicitly deactivated us. */
static void
gsurf_find_bar_module_dispose(GObject *object)
{
	gsurf_find_bar_deactivate(GSURF_MODULE(object));
	G_OBJECT_CLASS(gsurf_find_bar_module_parent_class)->dispose(object);
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

	object_class->dispose = gsurf_find_bar_module_dispose;
	object_class->finalize = gsurf_find_bar_module_finalize;
	module_class->activate = gsurf_find_bar_activate;
	module_class->deactivate = gsurf_find_bar_deactivate;
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
