/*
 * gsurf-tabs-ui.c - The tabs module's strip and tree sidebar
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two views of the same tabs:
 *   - the horizontal strip above or below the page: one flat button per
 *     view in window order, rebuilt from scratch on every change;
 *   - the tree sidebar beside the page: one row per view with indentation,
 *     a twisty, the favicon, the title, a close button and a container
 *     colour stripe. Rows are kept and updated in place: a change only
 *     touches the rows (and the properties) it affects.
 *
 * All toolkit calls go through modules/include/gsurf-chrome-compat.h, so
 * this file builds unchanged on GTK3 and GTK4.
 */

#include <string.h>

#include "gsurf-tabs-private.h"
#include "modules/include/gsurf-chrome-compat.h"

/* Twisty glyphs for an expanded and a collapsed subtree. */
#define TWISTY_EXPANDED  "▾"
#define TWISTY_COLLAPSED "▸"
#define CLOSE_LABEL      "×"

/* Strip labels are cut to this many characters. */
#define STRIP_LABEL_CHARS (24)

/* CSS class prefix for a container's stripe colour. */
#define CONTAINER_CLASS_PREFIX "gsurf-ctr-"

/* Twisty states, as last applied to a row. */
enum {
	TWISTY_UNSET = -1,
	TWISTY_NONE = 0,
	TWISTY_OPEN = 1,
	TWISTY_SHUT = 2
};

struct _GsurfTabsRow
{
	guint64    id;
	GtkWidget *row;      /* retained; the clickable (GTK3: an event box) */
	GtkWidget *box;      /* horizontal content box, indented */
	GtkWidget *stripe;
	GtkWidget *twisty;
	GtkWidget *icon;
	GtkWidget *title;
	GtkWidget *close;

	/* What was last applied, so a sync only touches what changed. */
	gint       indent;
	gint       twisty_state;
	gint       visible;
	gint       active;
	gchar     *container_class;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* The text a tab shows: its title, else its URI, else "New Tab". */
static const gchar *
view_label(GsurfView *view)
{
	const gchar *title = view != NULL ? gsurf_view_get_title(view) : NULL;
	const gchar *uri = view != NULL ? gsurf_view_get_uri(view) : NULL;

	if (title != NULL && *title != '\0')
		return title;
	if (uri != NULL && *uri != '\0')
		return uri;
	return "New Tab";
}

/* Whether a container id can be used inside a CSS class name (ids are
 * validated by the registry, but never trust input into CSS). */
static gboolean
id_is_css_safe(const gchar *id)
{
	const gchar *p;

	if (id == NULL || *id == '\0')
		return FALSE;
	for (p = id; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
			return FALSE;
	}
	return TRUE;
}

/* The container a view browses in, or NULL for the default one. */
static GsurfContainer *
view_container(GsurfTabsModule *self, GsurfView *view)
{
	const gchar *id;

	if (view == NULL)
		return NULL;
	id = gsurf_view_get_container_id(view);
	if (id == NULL)
		return NULL;
	return gsurf_container_registry_lookup(self->registry != NULL ? self->registry
		: gsurf_container_registry_get_default(), id);
}

/* The module stylesheet: colours from the config, then one rule per
 * container that has a (valid) colour. */
static gchar *
build_css(GsurfTabsModule *self)
{
	GString *css;
	g_autoptr(GPtrArray) containers = NULL;
	guint i;

	css = g_string_new(NULL);
	g_string_append_printf(css,
		".gsurf-tabbar, .gsurf-tab-sidebar { background-color: %s; color: %s; }\n"
		".gsurf-tabbar label, .gsurf-tab-sidebar label,"
		" .gsurf-tabbar button, .gsurf-tab-sidebar button { color: %s; }\n"
		".gsurf-tab button { padding: 2px 8px; }\n"
		".gsurf-tab-active { background-color: %s; }\n"
		".gsurf-tab-active button { font-weight: bold; }\n"
		".gsurf-tab-row { padding: 0px 2px; }\n"
		".gsurf-tab-row button { padding: 0px 4px; min-height: 0px; min-width: 0px; }\n"
		".gsurf-tab-row-active { background-color: %s; }\n"
		".gsurf-tab-row-active label { font-weight: bold; }\n"
		".gsurf-tab-stripe { min-width: 3px; }\n"
		".gsurf-tab-stripe-on { background-color: %s; }\n",
		self->bg_color, self->fg_color, self->fg_color,
		self->active_color, self->active_color, self->fg_color);

	/* Container colours come later, so they win over the plain stripe. */
	if (self->registry != NULL) {
		containers = gsurf_container_registry_list(self->registry);
		for (i = 0; i < containers->len; i++) {
			GsurfContainer *container = g_ptr_array_index(containers, i);
			const gchar *id = gsurf_container_get_id(container);
			const gchar *color = gsurf_container_get_color(container);
			g_autofree gchar *normalized = NULL;
			GdkRGBA rgba;

			if (!id_is_css_safe(id) || color == NULL || !gdk_rgba_parse(&rgba, color))
				continue;
			normalized = gdk_rgba_to_string(&rgba);
			g_string_append_printf(css, "." CONTAINER_CLASS_PREFIX "%s { background-color: %s; }\n",
				id, normalized);
		}
	}
	return g_string_free(css, FALSE);
}

/* ------------------------------------------------------------------ */
/* Strip                                                               */
/* ------------------------------------------------------------------ */

static void
on_strip_tab_clicked(GtkButton *button, gpointer user_data)
{
	gsurf_tabs_activate_tab(GSURF_TABS_MODULE(user_data),
		gsurf_tabs_get_widget_id(GTK_WIDGET(button)));
}

static void
on_strip_close_clicked(GtkButton *button, gpointer user_data)
{
	gsurf_tabs_close_tab(GSURF_TABS_MODULE(user_data),
		gsurf_tabs_get_widget_id(GTK_WIDGET(button)));
}

/* Rebuild the strip from the window order (cheap: a few buttons). */
static void
strip_rebuild(GsurfTabsModule *self)
{
	GsurfView *active;
	guint i;
	guint n;

	if (self->bar == NULL || self->window == NULL)
		return;
	gsurf_chrome_box_clear(self->bar);

	n = gsurf_window_get_n_views(self->window);
	active = gsurf_window_get_active_view(self->window);
	for (i = 0; i < n; i++) {
		GsurfView *view = gsurf_window_get_nth_view(self->window, i);
		const gchar *text = view_label(view);
		guint64 id = gsurf_view_get_id(view);
		g_autofree gchar *label = NULL;
		GtkWidget *tab;
		GtkWidget *button;
		GtkWidget *close;

		label = (g_utf8_strlen(text, -1) > STRIP_LABEL_CHARS)
			? g_strdup_printf("%.*s…",
				(int)(g_utf8_offset_to_pointer(text, STRIP_LABEL_CHARS) - text), text)
			: g_strdup(text);

		tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gsurf_chrome_widget_add_class(tab, "gsurf-tab");
		if (view == active)
			gsurf_chrome_widget_add_class(tab, "gsurf-tab-active");

		button = gsurf_chrome_button_new_flat(label);
		gsurf_tabs_set_widget_id(button, id);
		g_signal_connect(button, "clicked", G_CALLBACK(on_strip_tab_clicked), self);

		close = gsurf_chrome_button_new_flat(CLOSE_LABEL);
		gsurf_tabs_set_widget_id(close, id);
		g_signal_connect(close, "clicked", G_CALLBACK(on_strip_close_clicked), self);

		gsurf_chrome_box_pack(tab, button, FALSE, 0);
		gsurf_chrome_box_pack(tab, close, FALSE, 0);
		gsurf_chrome_box_pack(self->bar, tab, FALSE, 0);
		gsurf_chrome_widget_show_all(tab);
	}
}

/* ------------------------------------------------------------------ */
/* Sidebar rows                                                        */
/* ------------------------------------------------------------------ */

static void
on_row_click(GtkWidget *widget, gpointer user_data)
{
	gsurf_tabs_activate_tab(GSURF_TABS_MODULE(user_data), gsurf_tabs_get_widget_id(widget));
}

static void
on_row_middle_click(GtkWidget *widget, gpointer user_data)
{
	gsurf_tabs_close_tab(GSURF_TABS_MODULE(user_data), gsurf_tabs_get_widget_id(widget));
}

static void
on_row_twisty_clicked(GtkButton *button, gpointer user_data)
{
	gsurf_tabs_toggle_collapse(GSURF_TABS_MODULE(user_data),
		gsurf_tabs_get_widget_id(GTK_WIDGET(button)));
}

static void
on_row_close_clicked(GtkButton *button, gpointer user_data)
{
	gsurf_tabs_close_tab(GSURF_TABS_MODULE(user_data),
		gsurf_tabs_get_widget_id(GTK_WIDGET(button)));
}

/* Build one row: [stripe][twisty][favicon][title.........][close],
 * indented by the content box's start margin. */
static GsurfTabsRow *
row_new(GsurfTabsModule *self, guint64 id)
{
	GsurfTabsRow *row;

	row = g_new0(GsurfTabsRow, 1);
	row->id = id;
	row->indent = -1;
	row->twisty_state = TWISTY_UNSET;
	row->visible = -1;
	row->active = -1;

	row->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_set_name(row->box, GSURF_TABS_ROW_BOX_NAME);

	row->stripe = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_name(row->stripe, GSURF_TABS_ROW_STRIPE_NAME);
	gtk_widget_set_size_request(row->stripe, 3, -1);
	gsurf_chrome_widget_add_class(row->stripe, "gsurf-tab-stripe");

	row->twisty = gsurf_chrome_button_new_flat("");
	gtk_widget_set_name(row->twisty, GSURF_TABS_ROW_TWISTY_NAME);
	gsurf_tabs_set_widget_id(row->twisty, id);
	g_signal_connect(row->twisty, "clicked", G_CALLBACK(on_row_twisty_clicked), self);

	row->icon = gtk_image_new();
	gtk_widget_set_name(row->icon, GSURF_TABS_ROW_ICON_NAME);
	gtk_widget_set_size_request(row->icon, GSURF_TABS_FAVICON_SIZE, GSURF_TABS_FAVICON_SIZE);

	row->title = gtk_label_new("");
	gtk_widget_set_name(row->title, GSURF_TABS_ROW_TITLE_NAME);
	gtk_label_set_xalign(GTK_LABEL(row->title), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(row->title), PANGO_ELLIPSIZE_END);

	row->close = gsurf_chrome_button_new_flat(CLOSE_LABEL);
	gtk_widget_set_name(row->close, GSURF_TABS_ROW_CLOSE_NAME);
	gsurf_tabs_set_widget_id(row->close, id);
	g_signal_connect(row->close, "clicked", G_CALLBACK(on_row_close_clicked), self);

	gsurf_chrome_box_pack(row->box, row->stripe, FALSE, 0);
	gsurf_chrome_box_pack(row->box, row->twisty, FALSE, 0);
	gsurf_chrome_box_pack(row->box, row->icon, FALSE, 0);
	gsurf_chrome_box_pack(row->box, row->title, TRUE, 0);
	gsurf_chrome_box_pack(row->box, row->close, FALSE, 0);

	/* Clicks: primary activates, middle closes. The twisty and close
	 * buttons keep their own presses. */
	row->row = g_object_ref_sink(gsurf_chrome_clickable_new(row->box));
	gsurf_chrome_widget_add_class(row->row, "gsurf-tab-row");
	gsurf_tabs_set_widget_id(row->row, id);
	gsurf_chrome_connect_click(row->row, 1, on_row_click, self);
	gsurf_chrome_connect_click(row->row, 2, on_row_middle_click, self);
	return row;
}

/* Disconnect, detach and release a row. */
static void
row_free(GsurfTabsModule *self, GsurfTabsRow *row)
{
	gsurf_chrome_disconnect_click(row->row, self);
	g_signal_handlers_disconnect_by_data(row->twisty, self);
	g_signal_handlers_disconnect_by_data(row->close, self);
	gsurf_chrome_widget_detach(row->row);
	g_clear_object(&row->row);
	g_free(row->container_class);
	g_free(row);
}

/* Title, tooltip, favicon and container stripe of one row. */
static void
row_update_content(GsurfTabsModule *self, GsurfTabsRow *row)
{
	GsurfView *view = gsurf_view_lookup(row->id);
	GsurfContainer *container = view_container(self, view);
	const gchar *text = view_label(view);
	const gchar *container_id = container != NULL ? gsurf_container_get_id(container) : NULL;
	g_autofree gchar *wanted_class = NULL;
	g_autofree gchar *tooltip = NULL;

	gtk_label_set_text(GTK_LABEL(row->title), text);
	gsurf_chrome_image_set_native(row->icon,
		view != NULL ? gsurf_view_get_favicon(view) : NULL, GSURF_TABS_FAVICON_SIZE);

	/* The stripe carries the container's colour class; its tooltip and
	 * the row's name the container. */
	if (container_id != NULL && id_is_css_safe(container_id))
		wanted_class = g_strconcat(CONTAINER_CLASS_PREFIX, container_id, NULL);
	if (g_strcmp0(wanted_class, row->container_class) != 0) {
		if (row->container_class != NULL)
			gsurf_chrome_widget_remove_class(row->stripe, row->container_class);
		if (wanted_class != NULL)
			gsurf_chrome_widget_add_class(row->stripe, wanted_class);
		g_free(row->container_class);
		row->container_class = g_strdup(wanted_class);
	}
	gsurf_chrome_widget_set_class(row->stripe, "gsurf-tab-stripe-on", container != NULL);

	if (container != NULL) {
		const gchar *name = gsurf_container_get_name(container);

		gtk_widget_set_tooltip_text(row->stripe, name);
		tooltip = g_strdup_printf("%s\nContainer: %s", text, name);
	} else {
		gtk_widget_set_tooltip_text(row->stripe, NULL);
		tooltip = g_strdup(text);
	}
	gtk_widget_set_tooltip_text(row->row, tooltip);
}

/* Indentation, twisty, visibility and highlight of one row, touching
 * only what changed since the last sync. */
static void
row_update_tree(GsurfTabsModule *self, GsurfTabsRow *row, guint64 active)
{
	gint indent;
	gint twisty;
	gint visible;
	gint is_active;

	indent = (gint)gsurf_tab_tree_get_display_depth(self->tree, row->id,
		(guint)self->max_depth) * self->indent;
	if (indent != row->indent) {
		gtk_widget_set_margin_start(row->box, indent);
		row->indent = indent;
	}

	if (gsurf_tab_tree_get_n_children(self->tree, row->id) == 0)
		twisty = TWISTY_NONE;
	else
		twisty = gsurf_tab_tree_get_collapsed(self->tree, row->id) ? TWISTY_SHUT : TWISTY_OPEN;
	if (twisty != row->twisty_state) {
		gtk_button_set_label(GTK_BUTTON(row->twisty),
			twisty == TWISTY_OPEN ? TWISTY_EXPANDED
			: twisty == TWISTY_SHUT ? TWISTY_COLLAPSED : "");
		gtk_widget_set_sensitive(row->twisty, twisty != TWISTY_NONE);
		row->twisty_state = twisty;
	}

	visible = gsurf_tab_tree_is_visible(self->tree, row->id) ? 1 : 0;
	if (visible != row->visible) {
		gtk_widget_set_visible(row->row, visible);
		row->visible = visible;
	}

	is_active = (row->id == active) ? 1 : 0;
	if (is_active != row->active) {
		gsurf_chrome_widget_set_class(row->row, "gsurf-tab-row-active", is_active);
		row->active = is_active;
	}
}

/* Bring the rows in line with the tree: create missing rows, put every
 * row at its tree position, update what changed and drop rows of tabs
 * that left. */
static void
sidebar_sync(GsurfTabsModule *self)
{
	guint64 active = 0;
	guint i;
	guint n;

	if (self->sidebar_list == NULL || self->tree == NULL)
		return;
	if (self->window != NULL && gsurf_window_get_active_view(self->window) != NULL)
		active = gsurf_view_get_id(gsurf_window_get_active_view(self->window));

	n = gsurf_tab_tree_get_n_tabs(self->tree);
	for (i = 0; i < n; i++) {
		guint64 id = gsurf_tab_tree_get_nth(self->tree, i);
		GsurfTabsRow *row = g_hash_table_lookup(self->rows, &id);
		guint at;

		/* 1. A new tab gets a row (appended, then moved below). */
		if (row == NULL) {
			row = row_new(self, id);
			g_hash_table_insert(self->rows, &row->id, row);
			g_ptr_array_add(self->row_order, row);
			gsurf_chrome_box_pack(self->sidebar_list, row->row, FALSE, 0);
			gsurf_chrome_widget_manage_visibility(row->row);
			row_update_content(self, row);
		}

		/* 2. Move it to position i if it is elsewhere. */
		if (g_ptr_array_index(self->row_order, i) != row &&
		    g_ptr_array_find(self->row_order, row, &at)) {
			g_ptr_array_steal_index(self->row_order, at);
			g_ptr_array_insert(self->row_order, (gint)i, row);
			gsurf_chrome_box_move_child(self->sidebar_list, row->row, i);
		}

		/* 3. Tree state. */
		row_update_tree(self, row, active);
	}

	/* 4. Rows past the live tabs belong to closed tabs. */
	while (self->row_order->len > n) {
		GsurfTabsRow *row = g_ptr_array_steal_index(self->row_order, self->row_order->len - 1);

		g_hash_table_remove(self->rows, &row->id);
		row_free(self, row);
	}
}

/* ------------------------------------------------------------------ */
/* Entry points used by the core                                       */
/* ------------------------------------------------------------------ */

void
gsurf_tabs_ui_install(GsurfTabsModule *self)
{
	g_autofree gchar *css = NULL;
	GtkWidget *scrolled;

	if (self->window == NULL)
		return;
	gsurf_tabs_ui_uninstall(self);

	/* 1. Stylesheet. */
	css = build_css(self);
	self->css = gsurf_chrome_css_provider_new(css);
	g_object_set_data(G_OBJECT(self), GSURF_TABS_CSS_DATA, self->css);

	/* 2. The strip, hidden until update_visibility() decides. */
	self->bar = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2));
	gtk_widget_set_name(self->bar, GSURF_TABS_STRIP_NAME);
	gsurf_chrome_widget_add_class(self->bar, "gsurf-tabbar");
	if (self->position_top)
		gsurf_window_add_top_widget(self->window, self->bar);
	else
		gsurf_window_add_bottom_widget(self->window, self->bar);
	gsurf_chrome_widget_start_hidden(self->bar);

	/* 3. The sidebar: a fixed-width panel scrolling a list of rows. */
	self->sidebar_list = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
	gtk_widget_set_name(self->sidebar_list, GSURF_TABS_SIDEBAR_LIST_NAME);
	scrolled = gsurf_chrome_scrolled_new(self->sidebar_list);
	self->sidebar_box = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
	gtk_widget_set_name(self->sidebar_box, GSURF_TABS_SIDEBAR_NAME);
	gsurf_chrome_widget_add_class(self->sidebar_box, "gsurf-tab-sidebar");
	gtk_widget_set_size_request(self->sidebar_box, self->sidebar_width, -1);
	gsurf_chrome_box_pack(self->sidebar_box, scrolled, TRUE, 0);
	gsurf_window_add_side_widget(self->window, self->sidebar_box,
		self->sidebar_right ? GSURF_CHROME_EDGE_RIGHT : GSURF_CHROME_EDGE_LEFT);
	gsurf_chrome_widget_manage_visibility(self->sidebar_box);

	self->rows = g_hash_table_new(g_int64_hash, g_int64_equal);
	self->row_order = g_ptr_array_new();

	/* 4. Fill both from the current tabs. */
	gsurf_tabs_ui_sync(self);
}

void
gsurf_tabs_ui_uninstall(GsurfTabsModule *self)
{
	/* Rows first: they hold handlers with the module as data. */
	if (self->row_order != NULL) {
		guint i;

		for (i = 0; i < self->row_order->len; i++)
			row_free(self, g_ptr_array_index(self->row_order, i));
		g_clear_pointer(&self->row_order, g_ptr_array_unref);
	}
	g_clear_pointer(&self->rows, g_hash_table_unref);

	if (self->bar != NULL) {
		gsurf_chrome_box_clear(self->bar);
		gsurf_chrome_widget_detach(self->bar);
		g_clear_object(&self->bar);
	}
	if (self->sidebar_box != NULL) {
		gsurf_chrome_widget_detach(self->sidebar_box);
		g_clear_object(&self->sidebar_box);
	}
	g_clear_object(&self->sidebar_list);
	if (self->css != NULL) {
		g_object_set_data(G_OBJECT(self), GSURF_TABS_CSS_DATA, NULL);
		gsurf_chrome_css_provider_remove(self->css);
		g_clear_object(&self->css);
	}
}

void
gsurf_tabs_ui_sync(GsurfTabsModule *self)
{
	strip_rebuild(self);
	sidebar_sync(self);
	gsurf_tabs_ui_update_visibility(self);
}

void
gsurf_tabs_ui_update_tab(GsurfTabsModule *self, guint64 id)
{
	GsurfTabsRow *row;

	if (self->rows != NULL) {
		row = g_hash_table_lookup(self->rows, &id);
		if (row != NULL)
			row_update_content(self, row);
	}
	strip_rebuild(self);
}

void
gsurf_tabs_ui_update_containers(GsurfTabsModule *self)
{
	g_autofree gchar *css = NULL;
	guint i;

	if (self->css != NULL) {
		css = build_css(self);
		gsurf_chrome_css_provider_reload(self->css, css);
	}
	if (self->row_order != NULL) {
		for (i = 0; i < self->row_order->len; i++)
			row_update_content(self, g_ptr_array_index(self->row_order, i));
	}
}

void
gsurf_tabs_ui_update_visibility(GsurfTabsModule *self)
{
	guint n;

	if (self->bar != NULL && self->window != NULL) {
		n = gsurf_window_get_n_views(self->window);
		gtk_widget_set_visible(self->bar,
			(n > 1 || self->show_single) &&
			!(self->sidebar_shown && self->sidebar_hides_strip));
	}
	if (self->sidebar_box != NULL)
		gtk_widget_set_visible(self->sidebar_box, self->sidebar_shown);
}
