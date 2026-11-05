/*
 * gsurf-chromebar-module.c - Address bar
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A GtkEntry address bar packed into the window chrome. Enter loads the
 * (URI-handler-rewritten) input in the active view; the bar tracks the
 * active view's URI. Focus it with the open-prompt keybind (o) or the
 * configured key (Ctrl+l); Escape restores the URI and returns focus to
 * the page, as does loading a URI with Enter. Builds against GTK3 and
 * GTK4 through the module-private modules/include/gsurf-chrome-compat.h.
 *
 * Left of the entry, a vim-style mode indicator ("-- INSERT --") follows
 * the input mode the modal module publishes on the module manager
 * (GsurfModuleManager::mode-changed); mode_indicator: false leaves it
 * out, mode_hide_normal: true hides it in NORMAL mode, and each mode has
 * its own colour.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <gtk/gtk.h>
#include "modules/include/gsurf-chrome-compat.h"

#define GSURF_TYPE_CHROMEBAR_MODULE (gsurf_chromebar_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfChromebarModule, gsurf_chromebar_module,
                     GSURF, CHROMEBAR_MODULE, GsurfModule)

/* Defaults of the mode indicator (Catppuccin Mocha: blue, green, yellow,
 * and peach for find / passthrough). */
#define DEFAULT_MODE_NORMAL_COLOR "#89b4fa"
#define DEFAULT_MODE_INSERT_COLOR "#a6e3a1"
#define DEFAULT_MODE_FOLLOW_COLOR "#f9e2af"
#define DEFAULT_MODE_OTHER_COLOR  "#fab387"

/* Widget names the tests (and user CSS) can rely on. */
#define CHROMEBAR_NAME      "gsurf-chromebar"
#define CHROMEBAR_MODE_NAME "gsurf-chromebar-mode"

struct _GsurfChromebarModule
{
	GsurfModule parent_instance;
	GtkWidget *bar;         /* [mode][entry], the chrome widget (retained) */
	GtkWidget *mode;        /* the mode label, or NULL without an indicator */
	GtkCssProvider *css;    /* the indicator's colours */
	GsurfModuleManager *manager; /* whose mode we follow (ref) */
	gboolean   mode_indicator;
	gboolean   mode_hide_normal;
	gchar     *mode_normal_color;
	gchar     *mode_insert_color;
	gchar     *mode_follow_color;
	GtkWidget *entry;
	GsurfWindow *window; /* weak; the host may destroy it before the module */
	gchar     *key_focus;
	GsurfView *tracked;     /* view whose uri-changed we follow */
	gulong     tracked_id;
};

static void gsurf_chromebar_input_init(GsurfInputHandlerInterface *iface);
static void gsurf_chromebar_keybind_init(GsurfKeybindProviderInterface *iface);
static void gsurf_chromebar_action_init(GsurfActionHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfChromebarModule, gsurf_chromebar_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_chromebar_input_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_KEYBIND_PROVIDER, gsurf_chromebar_keybind_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_ACTION_HANDLER, gsurf_chromebar_action_init))

static GsurfView *
active_view(void)
{
	return gsurf_module_manager_get_active_view(gsurf_module_manager_get_default());
}

/* Show the tracked view's current URI in the bar. */
static void
sync_entry_text(GsurfChromebarModule *self)
{
	const gchar *uri = self->tracked != NULL ? gsurf_view_get_uri(self->tracked) : NULL;

	if (self->entry != NULL)
		gsurf_chrome_editable_set_text(self->entry, uri);
}

static void
on_view_uri_changed(GsurfView *view, const gchar *uri, gpointer user_data)
{
	GsurfChromebarModule *self = user_data;

	/* Don't clobber what the user is typing; Escape restores it. */
	if (self->entry != NULL && !gsurf_chrome_widget_has_focus(self->entry))
		gsurf_chrome_editable_set_text(self->entry, uri);
}

/* Put the cursor in the bar with its text selected, ready to replace. */
static void
focus_entry(GsurfChromebarModule *self)
{
	gtk_widget_grab_focus(self->entry);
	gtk_editable_select_region(GTK_EDITABLE(self->entry), 0, -1);
}

static void
track_view(GsurfChromebarModule *self, GsurfView *view)
{
	if (view != NULL && self->tracked == view)
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
	} else if (self->entry != NULL) {
		gsurf_chrome_editable_set_text(self->entry, NULL);
	}
}

static void
on_entry_activate(GtkEntry *entry, gpointer user_data)
{
	GsurfView *view = active_view();
	const gchar *text = gsurf_chrome_editable_get_text(GTK_WIDGET(entry));
	g_autofree gchar *rewritten = NULL;

	if (view == NULL || text == NULL || *text == '\0')
		return;
	rewritten = gsurf_module_manager_dispatch_rewrite_uri(
		gsurf_module_manager_get_default(), text);
	gsurf_view_load_uri(view, rewritten ? rewritten : text);
	/* Hand the keyboard back to the page so j/k/f work on what loads. */
	gsurf_view_focus(view);
}

/* Escape abandons the edit: restore the URI and return to the page.
 * Every other key is left to the entry. */
static gboolean
on_entry_key_press(GtkWidget *widget, guint keyval, GdkModifierType state,
                   gpointer user_data)
{
	GsurfChromebarModule *self = GSURF_CHROMEBAR_MODULE(user_data);
	GsurfView *view;

	if (keyval != GDK_KEY_Escape)
		return FALSE;
	sync_entry_text(self);
	view = active_view();
	if (view != NULL)
		gsurf_view_focus(view);
	return TRUE;
}

static void
on_active_view_changed(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	track_view(GSURF_CHROMEBAR_MODULE(user_data), view);
}

/* --- Mode indicator --- */

/* "-- INSERT --" with the mode's class (gsurf-mode-insert, ...), hidden
 * in NORMAL with mode_hide_normal. */
static void
update_mode(GsurfChromebarModule *self, GsurfModePolicy mode)
{
	static const gchar *const classes[] = {
		"gsurf-mode-normal", "gsurf-mode-insert", "gsurf-mode-follow",
		"gsurf-mode-find", "gsurf-mode-passthrough", NULL
	};
	const gchar *label = gsurf_mode_policy_get_label(mode);
	g_autofree gchar *text = NULL;
	guint i;

	if (self->mode == NULL)
		return;
	text = g_strdup_printf("-- %s --", label != NULL ? label : "?");
	gtk_label_set_text(GTK_LABEL(self->mode), text);
	for (i = 0; classes[i] != NULL; i++)
		gsurf_chrome_widget_set_class(self->mode, classes[i], (guint)mode == i);
	gtk_widget_set_visible(self->mode,
		!(self->mode_hide_normal && mode == GSURF_MODE_NORMAL));
}

static void
on_mode_changed(GsurfModuleManager *manager, GsurfModePolicy mode, gpointer user_data)
{
	(void)manager;
	update_mode(GSURF_CHROMEBAR_MODULE(user_data), mode);
}

static gchar *
build_css(GsurfChromebarModule *self)
{
	return g_strdup_printf(
		"label.gsurf-chromebar-mode { font-weight: bold; font-family: monospace;"
		" padding: 0px 6px; }\n"
		"label.gsurf-mode-normal { color: %s; }\n"
		"label.gsurf-mode-insert { color: %s; }\n"
		"label.gsurf-mode-follow { color: %s; }\n"
		"label.gsurf-mode-find, label.gsurf-mode-passthrough { color: "
		DEFAULT_MODE_OTHER_COLOR "; }\n",
		self->mode_normal_color, self->mode_insert_color, self->mode_follow_color);
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

	focus_entry(self);
	return TRUE;
}

static void
gsurf_chromebar_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_chromebar_handle_key_event;
}

/* The open-prompt keybind (o by default) focuses the bar. */
static gboolean
gsurf_chromebar_handle_action(GsurfActionHandler *handler, GsurfView *view,
                              GsurfAction action)
{
	GsurfChromebarModule *self = GSURF_CHROMEBAR_MODULE(handler);

	if (action != GSURF_ACTION_OPEN_PROMPT || self->entry == NULL)
		return FALSE;
	focus_entry(self);
	return TRUE;
}

static void
gsurf_chromebar_action_init(GsurfActionHandlerInterface *iface)
{
	iface->handle_action = gsurf_chromebar_handle_action;
}

static void
gsurf_chromebar_list_keybinds(GsurfKeybindProvider *provider, GPtrArray *entries)
{
	GsurfChromebarModule *self = GSURF_CHROMEBAR_MODULE(provider);

	gsurf_keybind_help_append(entries, self->key_focus,
		"Focus the address bar", "chromebar", "open-prompt");
}

static void
gsurf_chromebar_keybind_init(GsurfKeybindProviderInterface *iface)
{
	iface->list_keybinds = gsurf_chromebar_list_keybinds;
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

	/* Kiosk is a PWA-style chrome hide: keep the module loaded, but do
	 * not create an address bar that could take URL-entry focus. */
	if (gsurf_kiosk_is_enabled())
		return TRUE;

	/* 1. The bar: [mode indicator][entry]. Retain the widgets even if
	 *    the host destroys its container first. */
	self->bar = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
	gtk_widget_set_name(self->bar, CHROMEBAR_NAME);
	self->entry = gtk_entry_new();
	g_object_ref_sink(self->entry);
	gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry), "Enter URL or search...");
	g_signal_connect(self->entry, "activate", G_CALLBACK(on_entry_activate), self);
	gsurf_chrome_connect_key_press(self->entry, on_entry_key_press, self);
	if (self->mode_indicator) {
		g_autofree gchar *css = build_css(self);

		self->css = gsurf_chrome_css_provider_new(css);
		self->mode = g_object_ref_sink(gtk_label_new(""));
		gtk_widget_set_name(self->mode, CHROMEBAR_MODE_NAME);
		gsurf_chrome_widget_add_class(self->mode, "gsurf-chromebar-mode");
		gsurf_chrome_box_pack(self->bar, self->mode, FALSE, 0);
	}
	gsurf_chrome_box_pack(self->bar, self->entry, TRUE, 0);
	gsurf_window_add_top_widget(window, self->bar);
	if (self->mode != NULL)
		gsurf_chrome_widget_manage_visibility(self->mode);

	/* 2. Follow the published input mode. */
	self->manager = g_object_ref(gsurf_module_manager_get_default());
	if (self->mode != NULL) {
		g_signal_connect(self->manager, "mode-changed", G_CALLBACK(on_mode_changed), self);
		update_mode(self, gsurf_module_manager_get_mode(self->manager));
	}

	self->window = window;
	g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&self->window);
	g_signal_connect_object(window, "active-view-changed",
		G_CALLBACK(on_active_view_changed), self, 0);
	track_view(self, gsurf_window_get_active_view(window));
	return TRUE;
}

/* A CSS colour, validated with GdkRGBA and stored normalized; an
 * invalid one warns and keeps the current value. */
static void
read_color(YamlMapping *m, const gchar *member, gchar **field)
{
	const gchar *text;
	GdkRGBA rgba;

	if (!yaml_mapping_has_member(m, member))
		return;
	text = yaml_mapping_get_string_member(m, member);
	if (text == NULL || !gdk_rgba_parse(&rgba, text)) {
		g_warning("chromebar.%s: '%s' is not a CSS colour; keeping %s",
		          member, text != NULL ? text : "", *field);
		return;
	}
	g_free(*field);
	*field = gdk_rgba_to_string(&rgba);
}

/* The indicator's options back to their defaults. */
static void
reset_mode_config(GsurfChromebarModule *self)
{
	self->mode_indicator = TRUE;
	self->mode_hide_normal = FALSE;
	g_free(self->mode_normal_color);
	g_free(self->mode_insert_color);
	g_free(self->mode_follow_color);
	self->mode_normal_color = g_strdup(DEFAULT_MODE_NORMAL_COLOR);
	self->mode_insert_color = g_strdup(DEFAULT_MODE_INSERT_COLOR);
	self->mode_follow_color = g_strdup(DEFAULT_MODE_FOLLOW_COLOR);
}

/* The indicator options start from their defaults on every call; they
 * take effect at the next activation. */
static void
gsurf_chromebar_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfChromebarModule *self = GSURF_CHROMEBAR_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	reset_mode_config(self);
	node = gsurf_config_get_module_node(config, "chromebar");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);
	if (yaml_mapping_has_member(m, "key_focus")) {
		g_free(self->key_focus);
		self->key_focus = g_strdup(yaml_mapping_get_string_member(m, "key_focus"));
	}
	if (yaml_mapping_has_member(m, "mode_indicator"))
		self->mode_indicator = yaml_mapping_get_boolean_member(m, "mode_indicator");
	if (yaml_mapping_has_member(m, "mode_hide_normal"))
		self->mode_hide_normal = yaml_mapping_get_boolean_member(m, "mode_hide_normal");
	read_color(m, "mode_normal_color", &self->mode_normal_color);
	read_color(m, "mode_insert_color", &self->mode_insert_color);
	read_color(m, "mode_follow_color", &self->mode_follow_color);
}

/* Tear down callbacks and chrome before releasing the module's storage. */
static void
gsurf_chromebar_deactivate(GsurfModule *module)
{
	GsurfChromebarModule *self = GSURF_CHROMEBAR_MODULE(module);

	if (self->window != NULL) {
		g_signal_handlers_disconnect_by_data(self->window, self);
		g_object_remove_weak_pointer(G_OBJECT(self->window), (gpointer *)&self->window);
		self->window = NULL;
	}
	track_view(self, NULL);
	if (self->manager != NULL) {
		g_signal_handlers_disconnect_by_data(self->manager, self);
		g_clear_object(&self->manager);
	}
	if (self->entry != NULL) {
		gsurf_chrome_disconnect_key_press(self->entry, self);
		g_signal_handlers_disconnect_by_data(self->entry, self);
	}
	if (self->bar != NULL) {
		gsurf_chrome_widget_detach(self->bar);
		g_clear_object(&self->bar);
	}
	g_clear_object(&self->entry);
	g_clear_object(&self->mode);
	if (self->css != NULL) {
		gsurf_chrome_css_provider_remove(self->css);
		g_clear_object(&self->css);
	}
}

/* Idempotent disposal covers hosts retaining windows or native widgets. */
static void
gsurf_chromebar_module_dispose(GObject *object)
{
	gsurf_chromebar_deactivate(GSURF_MODULE(object));
	G_OBJECT_CLASS(gsurf_chromebar_module_parent_class)->dispose(object);
}

static void
gsurf_chromebar_module_finalize(GObject *object)
{
	GsurfChromebarModule *self = GSURF_CHROMEBAR_MODULE(object);
	g_clear_pointer(&self->key_focus, g_free);
	g_clear_pointer(&self->mode_normal_color, g_free);
	g_clear_pointer(&self->mode_insert_color, g_free);
	g_clear_pointer(&self->mode_follow_color, g_free);
	G_OBJECT_CLASS(gsurf_chromebar_module_parent_class)->finalize(object);
}

static void
gsurf_chromebar_module_class_init(GsurfChromebarModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_chromebar_module_finalize;
	object_class->dispose = gsurf_chromebar_module_dispose;
	module_class->activate = gsurf_chromebar_activate;
	module_class->deactivate = gsurf_chromebar_deactivate;
	module_class->get_name = gsurf_chromebar_get_name;
	module_class->get_description = gsurf_chromebar_get_description;
	module_class->configure = gsurf_chromebar_configure;
}

static void
gsurf_chromebar_module_init(GsurfChromebarModule *self)
{
	self->key_focus = g_strdup("Ctrl+l");
	reset_mode_config(self);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_CHROMEBAR_MODULE;
}
