/*
 * gsurf-wmux-ui.c - The wmux widgets: panes, status line, prompt, chooser
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The layout's root widget, which takes the place of the window's view
 * stack, is a vertical box:
 *
 *   root (GtkBox, "gsurf-wmux-root")
 *   +-- [status line, when status_position is top]
 *   +-- overlay (GtkOverlay)
 *   |   +-- grid ("gsurf-wmux-grid"): one frame per visible pane
 *   |   |   +-- frame (GtkBox, class wmux-pane [wmux-pane-active] [wmux-ctr-<id>])
 *   |   |       +-- GtkOverlay
 *   |   |           +-- the view's web view
 *   |   |           +-- the display-panes number label
 *   |   +-- chooser ("gsurf-wmux-chooser"), shown by choose-tree
 *   +-- [status line, when status_position is bottom]
 *
 * The status line holds "[gsurf]", one button per tmux window ("0:name*"),
 * a message label (command errors), the prompt (label + entry) used by
 * command-prompt and confirm-before, and the pane counter on the right.
 *
 * Every view of the window has a frame while the layout is installed:
 * gsurf_wmux_ui_sync() tags the frames of the current window's panes with
 * their pane and *parks* the others (tagged GSURF_WMUX_GRID_PARKED: kept
 * in the grid, not shown), like the window's own view stack keeps hidden
 * tabs. A web view is reparented only when its frame is made (the view
 * joins) or dropped (the view leaves the window, the layout is removed). The
 * GsurfView keeps its own reference to its web view; it is never
 * destroyed here.
 *
 * The stylesheet (one provider, regenerated on config and container
 * changes) gives each frame its border: the container's colour (or
 * border_color) at inactive_opacity, and for the active pane the
 * brightened colour (or active_border_color), wider.
 */

#include "modules/include/gsurf-chrome-compat.h"

#include "gsurf-wmux-private.h"

#define CTR_CLASS_DATA     "gsurf-wmux-ctr-class"
#define CHOOSER_CTR_PREFIX "wmux-chooser-ctr-"

/* ------------------------------------------------------------------ */
/* Colours and stylesheet                                              */
/* ------------------------------------------------------------------ */

gchar *
gsurf_wmux_brighten(const gchar *color, gdouble amount)
{
	GdkRGBA rgba;

	if (color == NULL || !gdk_rgba_parse(&rgba, color))
		return g_strdup(color != NULL ? color : "");
	amount = CLAMP(amount, 0.0, 1.0);
	rgba.red += (1.0 - rgba.red) * amount;
	rgba.green += (1.0 - rgba.green) * amount;
	rgba.blue += (1.0 - rgba.blue) * amount;
	return gdk_rgba_to_string(&rgba);
}

gchar *
gsurf_wmux_with_alpha(const gchar *color, gdouble opacity)
{
	GdkRGBA rgba;

	if (color == NULL || !gdk_rgba_parse(&rgba, color))
		return g_strdup(color != NULL ? color : "");
	rgba.alpha *= CLAMP(opacity, 0.0, 1.0);
	return gdk_rgba_to_string(&rgba);
}

/* Whether a container id may be used inside a CSS class name. */
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

/* The module stylesheet: pane borders, the per-container border colours,
 * the pane numbers, the status line and the chooser. */
static gchar *
build_css(GsurfWmuxModule *self)
{
	GString *css = g_string_new(NULL);
	g_autofree gchar *inactive = gsurf_wmux_with_alpha(self->border_color, self->inactive_opacity);
	g_autoptr(GPtrArray) containers = NULL;
	guint i;

	/* 1. Panes in the default container, and the pane numbers. */
	g_string_append_printf(css,
		"box.wmux-pane { border: %dpx solid %s; }\n"
		"box.wmux-pane.wmux-pane-active { border: %dpx solid %s; }\n"
		"label.wmux-pane-number { font-size: 64px; font-weight: bold; color: %s;"
		" background-color: rgba(0,0,0,0.65); padding: 8px 28px; border-radius: 8px; }\n",
		self->border_width, inactive, self->active_border_width, self->active_border_color,
		self->active_border_color);

	/* 2. The status line and the chooser. */
	g_string_append_printf(css,
		".wmux-status { background-color: %s; color: %s; padding: 0px 4px; }\n"
		".wmux-status label, .wmux-status button { color: %s; }\n"
		".wmux-status button { padding: 0px 4px; min-height: 0px; min-width: 0px; }\n"
		".wmux-status .wmux-status-current, .wmux-status .wmux-status-current label"
		" { color: %s; font-weight: bold; }\n"
		".wmux-chooser { background-color: %s; color: %s; border: 1px solid %s;"
		" padding: 8px; }\n"
		".wmux-chooser label { color: %s; }\n"
		".wmux-chooser-row { padding: 1px 6px; }\n"
		".wmux-chooser-selected { background-color: %s; }\n"
		".wmux-chooser-selected label { color: %s; }\n",
		self->status_bg, self->status_fg, self->status_fg, self->status_current_fg,
		self->status_bg, self->status_fg, self->active_border_color, self->status_fg,
		self->active_border_color, self->status_bg);

	/* 3. Container colours: the frame border (dim, or brightened and full
	 *    for the active pane) and the chooser stripe. */
	if (self->container_colors && self->registry != NULL) {
		containers = gsurf_container_registry_list(self->registry);
		for (i = 0; i < containers->len; i++) {
			GsurfContainer *container = g_ptr_array_index(containers, i);
			const gchar *id = gsurf_container_get_id(container);
			const gchar *color = gsurf_container_get_color(container);
			g_autofree gchar *dim = NULL;
			g_autofree gchar *bright = NULL;
			GdkRGBA rgba;

			if (!id_is_css_safe(id) || color == NULL || !gdk_rgba_parse(&rgba, color))
				continue;
			dim = gsurf_wmux_with_alpha(color, self->inactive_opacity);
			bright = gsurf_wmux_brighten(color, self->active_brighten);
			g_string_append_printf(css,
				"box.wmux-pane." GSURF_WMUX_CTR_CLASS_PREFIX "%s { border-color: %s; }\n"
				"box.wmux-pane." GSURF_WMUX_CTR_CLASS_PREFIX "%s.wmux-pane-active"
				" { border-color: %s; }\n"
				"." CHOOSER_CTR_PREFIX "%s { background-color: %s; }\n",
				id, dim, id, bright, id, color);
		}
	}
	return g_string_free(css, FALSE);
}

void
gsurf_wmux_ui_update_css(GsurfWmuxModule *self)
{
	g_autofree gchar *css = NULL;

	if (self->css == NULL)
		return;
	css = build_css(self);
	gsurf_chrome_css_provider_reload(self->css, css);
}

/* The CSS class for a view's container, or NULL (default container,
 * colours off, or an id unusable in CSS). */
static gchar *
container_class(GsurfWmuxModule *self, GsurfView *view)
{
	const gchar *id = view != NULL ? gsurf_view_get_container_id(view) : NULL;

	if (!self->container_colors || !id_is_css_safe(id))
		return NULL;
	return g_strconcat(GSURF_WMUX_CTR_CLASS_PREFIX, id, NULL);
}

/* ------------------------------------------------------------------ */
/* Small widget helpers                                                */
/* ------------------------------------------------------------------ */

static void
box_append(GtkWidget *box, GtkWidget *child, gboolean expand)
{
	gsurf_chrome_box_pack(box, child, expand, 0);
}

static void
overlay_set_main(GtkWidget *overlay, GtkWidget *child)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_overlay_set_child(GTK_OVERLAY(overlay), child);
#else
	gtk_container_add(GTK_CONTAINER(overlay), child);
#endif
}

/* Take @child (the overlay's main child) out without destroying it. */
static void
overlay_unset_main(GtkWidget *overlay, GtkWidget *child)
{
	if (child == NULL || gtk_widget_get_parent(child) != overlay)
		return;
#ifdef GSURF_BACKEND_GTK4
	gtk_overlay_set_child(GTK_OVERLAY(overlay), NULL);
#else
	gtk_container_remove(GTK_CONTAINER(overlay), child);
#endif
}

/* ------------------------------------------------------------------ */
/* Grid callbacks                                                      */
/* ------------------------------------------------------------------ */

static GArray *
grid_geometry(gpointer data, gint width, gint height)
{
	GsurfWmuxModule *self = data;
	GsurfWmuxWin *win = gsurf_wmux_current(self);

	if (win == NULL || !self->attached)
		return NULL;
	return gsurf_wmux_layout_geometry(win->layout, MAX(width, 0), MAX(height, 0), self->gap);
}

static gint
grid_hit_border(gpointer data, gint width, gint height, gint x, gint y)
{
	GsurfWmuxModule *self = data;
	GsurfWmuxWin *win = gsurf_wmux_current(self);
	GsurfWmuxBorder border;

	if (win == NULL || !self->mouse ||
	    !gsurf_wmux_layout_find_border(win->layout, width, height, self->gap, x, y, &border))
		return GSURF_WMUX_GRID_BORDER_NONE;
	return border.split == GSURF_WMUX_SPLIT_H ? GSURF_WMUX_GRID_BORDER_VERTICAL
		: GSURF_WMUX_GRID_BORDER_HORIZONTAL;
}

static gboolean
grid_drag_begin(gpointer data, gint width, gint height, gint x, gint y)
{
	GsurfWmuxModule *self = data;
	GsurfWmuxWin *win = gsurf_wmux_current(self);

	if (win == NULL || !self->mouse ||
	    !gsurf_wmux_layout_find_border(win->layout, width, height, self->gap, x, y, &self->drag))
		return FALSE;
	self->dragging = TRUE;
	return TRUE;
}

static void
grid_drag_update(gpointer data, gint width, gint height, gint x, gint y)
{
	GsurfWmuxModule *self = data;
	GsurfWmuxWin *win = gsurf_wmux_current(self);

	if (win == NULL || !self->dragging)
		return;
	if (gsurf_wmux_layout_move_border(win->layout, &self->drag, width, height, self->gap,
			self->drag.split == GSURF_WMUX_SPLIT_H ? x : y))
		gsurf_wmux_grid_relayout(self->grid);
}

static void
grid_drag_end(gpointer data)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(data);

	self->dragging = FALSE;
	/* The sizes changed: notify (the session saves the layout). */
	gsurf_wmux_ui_sync(self);
}

static const GsurfWmuxGridFuncs grid_funcs = {
	grid_geometry,
	grid_hit_border,
	grid_drag_begin,
	grid_drag_update,
	grid_drag_end
};

/* ------------------------------------------------------------------ */
/* Frames                                                              */
/* ------------------------------------------------------------------ */

/* The frame around @view: a bordered box holding an overlay with the web
 * view and the (hidden) pane number. */
static GsurfWmuxFrame *
frame_new(GsurfView *view)
{
	GsurfWmuxFrame *f = g_new0(GsurfWmuxFrame, 1);
	GtkWidget *webview = gsurf_view_get_native_widget(view);
	guint64 id = gsurf_view_get_id(view);

	f->id = id;
	f->frame = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
	gsurf_chrome_widget_add_class(f->frame, GSURF_WMUX_PANE_CLASS);
	g_object_set_data_full(G_OBJECT(f->frame), GSURF_WMUX_VIEW_ID_DATA,
		g_memdup2(&id, sizeof(id)), g_free);

	f->overlay = gtk_overlay_new();
	gtk_widget_set_hexpand(f->overlay, TRUE);
	gtk_widget_set_vexpand(f->overlay, TRUE);
	box_append(f->frame, f->overlay, TRUE);

	f->number = gtk_label_new("");
	gtk_widget_set_name(f->number, GSURF_WMUX_PANE_NUMBER_NAME);
	gsurf_chrome_widget_add_class(f->number, "wmux-pane-number");
	gtk_widget_set_halign(f->number, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(f->number, GTK_ALIGN_CENTER);
	gtk_overlay_add_overlay(GTK_OVERLAY(f->overlay), f->number);
	/* Clicks go through the number to the page. */
#ifdef GSURF_BACKEND_GTK4
	gtk_widget_set_can_target(f->number, FALSE);
#else
	gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(f->overlay), f->number, TRUE);
#endif

	if (webview != NULL && gtk_widget_get_parent(webview) == NULL) {
		f->webview = g_object_ref(webview);
		gtk_widget_set_hexpand(webview, TRUE);
		gtk_widget_set_vexpand(webview, TRUE);
		overlay_set_main(f->overlay, webview);
	} else if (webview != NULL) {
		g_warning("wmux: a view widget is still parented elsewhere; its pane stays empty");
	}

	/* GTK3 widgets start hidden; the number stays hidden until
	 * display-panes (no-show-all keeps an ancestor's show_all off it). */
	gsurf_chrome_widget_show_all(f->frame);
#ifdef GSURF_BACKEND_GTK3
	gtk_widget_show(f->overlay);
	if (f->webview != NULL)
		gtk_widget_show(f->webview);
	gtk_widget_set_no_show_all(f->number, TRUE);
#endif
	gtk_widget_set_visible(f->number, FALSE);
	return f;
}

/* Undo frame_new(): the web view leaves (alive), the frame goes. */
static void
frame_free(gpointer data)
{
	GsurfWmuxFrame *f = data;
	GtkWidget *parent;

	overlay_unset_main(f->overlay, f->webview);
	parent = gtk_widget_get_parent(f->frame);
	if (parent != NULL)
		gsurf_wmux_grid_remove(parent, f->frame);
#ifdef GSURF_BACKEND_GTK3
	gtk_widget_destroy(f->frame);
#endif
	g_clear_object(&f->frame);
	g_clear_object(&f->webview);
	g_free(f);
}

GsurfWmuxFrame *
gsurf_wmux_ui_frame(GsurfWmuxModule *self, guint64 id)
{
	if (self->frames == NULL)
		return NULL;
	return g_hash_table_lookup(self->frames, &id);
}

void
gsurf_wmux_ui_drop_frame(GsurfWmuxModule *self, guint64 id)
{
	if (self->frames != NULL)
		g_hash_table_remove(self->frames, &id);
}

void
gsurf_wmux_ui_clear_frames(GsurfWmuxModule *self)
{
	if (self->frames != NULL)
		g_hash_table_remove_all(self->frames);
}

/* Bring one frame's classes in line: active, container. */
static void
frame_style(GsurfWmuxModule *self, GsurfWmuxFrame *f, GsurfView *view, gboolean active)
{
	g_autofree gchar *want = container_class(self, view);
	const gchar *have = g_object_get_data(G_OBJECT(f->frame), CTR_CLASS_DATA);

	gsurf_chrome_widget_set_class(f->frame, GSURF_WMUX_PANE_ACTIVE_CLASS, active);
	if (g_strcmp0(want, have) == 0)
		return;
	if (have != NULL)
		gsurf_chrome_widget_remove_class(f->frame, have);
	if (want != NULL)
		gsurf_chrome_widget_add_class(f->frame, want);
	g_object_set_data_full(G_OBJECT(f->frame), CTR_CLASS_DATA, g_steal_pointer(&want), g_free);
}

/* ------------------------------------------------------------------ */
/* Sync                                                                */
/* ------------------------------------------------------------------ */

/* The ids of the views on screen, in window order, plus the whole
 * arrangement, as a string: any change notifies the window. */
static gchar *
visible_signature(GsurfWmuxModule *self)
{
	GString *sig = g_string_new(NULL);
	GPtrArray *views;
	guint i;

	if (self->window == NULL || !self->attached)
		return g_string_free(sig, FALSE);
	views = gsurf_window_get_views(self->window);
	for (i = 0; i < views->len; i++) {
		GsurfView *view = g_ptr_array_index(views, i);

		if (gsurf_wmux_view_visible(self, view))
			g_string_append_printf(sig, "%" G_GUINT64_FORMAT ",", gsurf_view_get_id(view));
	}
	/* The arrangement too (layouts, active and zoomed panes, names, the
	 * current window): the session module saves on every change. */
	g_string_append_printf(sig, "|%u|%d", self->current, self->last_win);
	for (i = 0; self->wins != NULL && i < self->wins->len; i++) {
		GsurfWmuxWin *win = g_ptr_array_index(self->wins, i);
		g_autofree gchar *layout = gsurf_wmux_layout_serialize(win->layout, TRUE);

		g_string_append_printf(sig, "|%s/%u/%u/%d/%s", layout,
			gsurf_wmux_layout_get_active(win->layout),
			gsurf_wmux_layout_get_zoomed(win->layout), win->renamed, win->name);
	}
	return g_string_free(sig, FALSE);
}

void
gsurf_wmux_ui_sync(GsurfWmuxModule *self)
{
	GsurfWmuxWin *win = gsurf_wmux_current(self);
	g_autoptr(GHashTable) desired = NULL;
	g_autoptr(GArray) panes = NULL;
	g_autofree gchar *sig = NULL;
	GHashTableIter iter;
	gpointer key;
	GPtrArray *views;
	guint active;
	guint i;

	if (self->grid == NULL || self->frames == NULL)
		return;

	/* 1. Which views the current window's panes show (id -> pane). */
	desired = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	if (win != NULL && self->attached) {
		panes = gsurf_wmux_layout_list_panes(win->layout);
		for (i = 0; i < panes->len; i++) {
			guint pane = g_array_index(panes, guint, i);
			GsurfView *view = gsurf_wmux_pane_view(self, win, pane);

			if (view != NULL) {
				guint64 id = gsurf_view_get_id(view);

				g_hash_table_insert(desired, g_memdup2(&id, sizeof(id)),
					GUINT_TO_POINTER(pane + 1));
			}
		}
	}

	/* 2. Frames of views no longer in the window go (their web views
	 *    unparented, alive). */
	g_hash_table_iter_init(&iter, self->frames);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		GsurfView *view = gsurf_view_lookup(*(guint64 *)key);

		if (view == NULL || self->window == NULL || !self->attached ||
		    *(guint64 *)key == self->removing ||
		    gsurf_window_get_view_index(self->window, view) < 0)
			g_hash_table_iter_remove(&iter);
	}

	/* 3. Every view of the window has a frame in the grid: tagged with
	 *    its pane (styled and numbered) when the current window shows it,
	 *    otherwise *parked* (tagged GSURF_WMUX_GRID_PARKED: kept, never
	 *    shown). Like the window's own view stack, this keeps every web
	 *    view realized: unparenting one that WebKit is still drawing
	 *    makes GTK3 WebKit hand an unrealized widget to GDK (a critical
	 *    from gdk_window_create_gl_context), and reparenting reloads its
	 *    surface (a flash) on every window or tab switch. */
	active = win != NULL ? gsurf_wmux_layout_get_active(win->layout) : GSURF_WMUX_PANE_NONE;
	views = (self->window != NULL && self->attached) ? gsurf_window_get_views(self->window) : NULL;
	for (i = 0; views != NULL && i < views->len; i++) {
		GsurfView *view = g_ptr_array_index(views, i);
		guint64 id = gsurf_view_get_id(view);
		gpointer tag = g_hash_table_lookup(desired, &id);
		GsurfWmuxFrame *f = g_hash_table_lookup(self->frames, &id);
		guint pane;

		/* The view the window is taking out right now gets none. */
		if (id == self->removing)
			continue;
		if (f == NULL) {
			f = frame_new(view);
			g_hash_table_insert(self->frames, g_memdup2(&id, sizeof(id)), f);
		}
		if (tag == NULL) {
			gsurf_wmux_grid_put(self->grid, f->frame, GSURF_WMUX_GRID_PARKED);
			frame_style(self, f, view, FALSE);
			gtk_widget_set_visible(f->number, FALSE);
			continue;
		}
		pane = GPOINTER_TO_UINT(tag) - 1;
		gsurf_wmux_grid_put(self->grid, f->frame, pane);
		frame_style(self, f, view, pane == active);
		{
			g_autofree gchar *number = g_strdup_printf("%d",
				gsurf_wmux_layout_get_pane_index(win->layout, pane));

			gtk_label_set_text(GTK_LABEL(f->number), number);
		}
		gtk_widget_set_visible(f->number, self->displaying);
	}
	gsurf_wmux_grid_relayout(self->grid);

	/* 4. Tell the window (the tab tree, the session module) when the
	 *    visible set or the arrangement changed. */
	sig = visible_signature(self);
	if (g_strcmp0(sig, self->visible_sig) != 0) {
		g_free(self->visible_sig);
		self->visible_sig = g_steal_pointer(&sig);
		if (self->window != NULL && self->installed)
			gsurf_window_layout_notify_visible(self->window);
	}

	/* 5. The status line (window names, pane counter) and the chooser. */
	gsurf_wmux_ui_update_status(self);
	if (self->chooser_open)
		gsurf_wmux_ui_chooser_refresh(self);
}

/* ------------------------------------------------------------------ */
/* Status line                                                         */
/* ------------------------------------------------------------------ */

static void
on_window_button_clicked(GtkButton *button, gpointer user_data)
{
	guint tag = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), GSURF_WMUX_INDEX_DATA));

	if (tag > 0)
		gsurf_wmux_select_window(GSURF_WMUX_MODULE(user_data), tag - 1);
}

/* "0:name*" for the current window, "1:name-" for the last one. */
static gchar *
window_entry_text(GsurfWmuxModule *self, guint index)
{
	g_autofree gchar *name = gsurf_wmux_win_display_name(self, index);
	const gchar *mark = "";

	if (index == self->current)
		mark = "*";
	else if ((gint)index == self->last_win)
		mark = "-";
	return g_strdup_printf("%u:%s%s", index, name, mark);
}

/* "2/3", plus " Z" while zoomed: the active pane's number (from 1) and
 * the number of panes. */
static gchar *
right_text(GsurfWmuxModule *self)
{
	GsurfWmuxWin *win = gsurf_wmux_current(self);
	gint index;

	if (win == NULL)
		return g_strdup("");
	index = gsurf_wmux_layout_get_pane_index(win->layout, gsurf_wmux_layout_get_active(win->layout));
	return g_strdup_printf("%d/%u%s", index + 1, gsurf_wmux_layout_get_n_panes(win->layout),
		gsurf_wmux_layout_get_zoomed(win->layout) != GSURF_WMUX_PANE_NONE ? " Z" : "");
}

void
gsurf_wmux_ui_update_status(GsurfWmuxModule *self)
{
	g_autofree gchar *right = NULL;
	guint i;

	if (self->status_windows == NULL)
		return;
	gsurf_chrome_box_clear(self->status_windows);
	if (!self->attached || self->wins == NULL) {
		gtk_label_set_text(GTK_LABEL(self->status_right), "");
		return;
	}
	for (i = 0; i < self->wins->len; i++) {
		g_autofree gchar *text = window_entry_text(self, i);
		GtkWidget *button = gsurf_chrome_button_new_flat(text);

		gsurf_chrome_widget_add_class(button, GSURF_WMUX_STATUS_WINDOW_CLASS);
		if (i == self->current)
			gsurf_chrome_widget_add_class(button, GSURF_WMUX_STATUS_CURRENT_CLASS);
		g_object_set_data(G_OBJECT(button), GSURF_WMUX_INDEX_DATA, GUINT_TO_POINTER(i + 1));
		g_signal_connect(button, "clicked", G_CALLBACK(on_window_button_clicked), self);
		box_append(self->status_windows, button, FALSE);
		gsurf_chrome_widget_show_all(button);
	}
	right = right_text(self);
	gtk_label_set_text(GTK_LABEL(self->status_right), right);
}

gchar *
gsurf_wmux_ui_status_text(GsurfWmuxModule *self)
{
	GString *out = g_string_new("[" GSURF_WMUX_SESSION_NAME "]");
	g_autofree gchar *right = NULL;
	guint i;

	if (!self->attached || self->wins == NULL)
		return g_string_free(out, FALSE);
	for (i = 0; i < self->wins->len; i++) {
		g_autofree gchar *text = window_entry_text(self, i);

		g_string_append_printf(out, " %s", text);
	}
	right = right_text(self);
	g_string_append_printf(out, "  %s", right);
	return g_string_free(out, FALSE);
}

static gboolean
on_message_timeout(gpointer user_data)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(user_data);

	self->message_id = 0;
	if (self->message != NULL) {
		gtk_widget_set_visible(self->message, FALSE);
		if (self->prompt_kind == GSURF_WMUX_PROMPT_NONE)
			gtk_widget_set_visible(self->status_windows, TRUE);
	}
	return G_SOURCE_REMOVE;
}

void
gsurf_wmux_ui_message(GsurfWmuxModule *self, const gchar *text)
{
	g_message("wmux: %s", text != NULL ? text : "");
	if (self->message == NULL)
		return;
	gtk_label_set_text(GTK_LABEL(self->message), text != NULL ? text : "");
	gtk_widget_set_visible(self->message, TRUE);
	gtk_widget_set_visible(self->status_windows, FALSE);
	if (self->message_id != 0)
		g_source_remove(self->message_id);
	self->message_id = g_timeout_add(GSURF_WMUX_MESSAGE_TIME, on_message_timeout, self);
}

/* ------------------------------------------------------------------ */
/* Prompt (command-prompt, confirm-before)                             */
/* ------------------------------------------------------------------ */

/* Run what was typed at a command prompt: the line itself, or the
 * template with %% / %1 replaced by it (or the text appended as one quoted
 * argument when the template has no placeholder). */
static void
submit_command(GsurfWmuxModule *self, const gchar *text, const gchar *template_str)
{
	g_autofree gchar *line = NULL;

	if (template_str == NULL || *template_str == '\0') {
		if (text == NULL || *text == '\0')
			return;
		line = g_strdup(text);
	} else if (strstr(template_str, "%%") != NULL || strstr(template_str, "%1") != NULL) {
		g_auto(GStrv) a = g_strsplit(template_str, "%%", -1);
		g_autofree gchar *first = g_strjoinv(text, a);
		g_auto(GStrv) b = g_strsplit(first, "%1", -1);

		line = g_strjoinv(text, b);
	} else {
		g_autofree gchar *quoted = gsurf_wmux_command_quote(text != NULL ? text : "");

		line = g_strdup_printf("%s %s", template_str, quoted);
	}
	gsurf_wmux_run_line(self, line);
}

/* Keys in the prompt entry, before the entry handles them. */
static gboolean
on_prompt_key(GtkWidget *widget, guint keyval, GdkModifierType state, gpointer user_data)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(user_data);
	gboolean enter = (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter);

	(void)state;
	switch (self->prompt_kind) {
	case GSURF_WMUX_PROMPT_COMMAND:
		if (enter) {
			g_autofree gchar *text = g_strdup(gsurf_chrome_editable_get_text(widget));
			g_autofree gchar *template_str = g_steal_pointer(&self->prompt_template);

			gsurf_wmux_ui_prompt_close(self);
			submit_command(self, text, template_str);
			return TRUE;
		}
		if (keyval == GDK_KEY_Escape) {
			gsurf_wmux_ui_prompt_close(self);
			return TRUE;
		}
		return FALSE;
	case GSURF_WMUX_PROMPT_CONFIRM:
		/* y runs the command; n, Escape and Enter cancel; nothing is
		 * typed. */
		if (keyval == GDK_KEY_y || keyval == GDK_KEY_Y) {
			g_autofree gchar *command = g_steal_pointer(&self->prompt_command);

			gsurf_wmux_ui_prompt_close(self);
			if (command != NULL)
				gsurf_wmux_run_line(self, command);
			return TRUE;
		}
		if (keyval == GDK_KEY_n || keyval == GDK_KEY_N || keyval == GDK_KEY_Escape || enter)
			gsurf_wmux_ui_prompt_close(self);
		return TRUE;
	case GSURF_WMUX_PROMPT_NONE:
	default:
		return FALSE;
	}
}

void
gsurf_wmux_ui_prompt(GsurfWmuxModule *self, GsurfWmuxPromptKind kind, const gchar *prompt,
                     const gchar *initial)
{
	if (self->prompt_box == NULL || kind == GSURF_WMUX_PROMPT_NONE)
		return;
	self->prompt_kind = kind;
	gtk_label_set_text(GTK_LABEL(self->prompt_label), prompt != NULL ? prompt : ":");
	gsurf_chrome_editable_set_text(self->prompt_entry, initial);
	gtk_widget_set_visible(self->status_box, TRUE);
	gtk_widget_set_visible(self->status_windows, FALSE);
	gtk_widget_set_visible(self->message, FALSE);
	gtk_widget_set_visible(self->prompt_box, TRUE);
	gtk_widget_grab_focus(self->prompt_entry);
	gtk_editable_set_position(GTK_EDITABLE(self->prompt_entry), -1);
}

void
gsurf_wmux_ui_prompt_close(GsurfWmuxModule *self)
{
	gboolean was_open = (self->prompt_kind != GSURF_WMUX_PROMPT_NONE);
	GsurfView *view;

	self->prompt_kind = GSURF_WMUX_PROMPT_NONE;
	g_clear_pointer(&self->prompt_template, g_free);
	g_clear_pointer(&self->prompt_command, g_free);
	if (self->prompt_box == NULL)
		return;
	gtk_widget_set_visible(self->prompt_box, FALSE);
	gsurf_chrome_editable_set_text(self->prompt_entry, "");
	gtk_widget_set_visible(self->status_windows, TRUE);
	gtk_widget_set_visible(self->status_box, self->status);
	/* The keyboard goes back to the active pane. */
	if (was_open && self->window != NULL && self->attached) {
		view = gsurf_window_get_active_view(self->window);
		if (view != NULL)
			gsurf_view_focus(view);
	}
}

/* ------------------------------------------------------------------ */
/* display-panes                                                       */
/* ------------------------------------------------------------------ */

static gboolean
on_display_timeout(gpointer user_data)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(user_data);

	self->display_id = 0;
	gsurf_wmux_ui_display_panes(self, FALSE);
	return G_SOURCE_REMOVE;
}

void
gsurf_wmux_ui_display_panes(GsurfWmuxModule *self, gboolean show)
{
	GHashTableIter iter;
	gpointer value;

	if (self->display_id != 0) {
		g_source_remove(self->display_id);
		self->display_id = 0;
	}
	if (!show && !self->displaying)
		return;
	self->displaying = show && self->attached;
	if (self->frames != NULL) {
		g_hash_table_iter_init(&iter, self->frames);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			GsurfWmuxFrame *f = value;

			gtk_widget_set_visible(f->number, self->displaying &&
				gsurf_wmux_grid_get_pane(f->frame) != GSURF_WMUX_GRID_PARKED);
		}
	}
	if (self->displaying)
		self->display_id = g_timeout_add((guint)self->display_panes_time, on_display_timeout, self);
	gsurf_wmux_update_grab(self);
}

/* ------------------------------------------------------------------ */
/* Chooser (choose-tree)                                               */
/* ------------------------------------------------------------------ */

static void
chooser_items_rebuild(GsurfWmuxModule *self)
{
	guint i;
	guint j;

	if (self->chooser_items == NULL)
		self->chooser_items = g_array_new(FALSE, FALSE, sizeof(GsurfWmuxChooserItem));
	g_array_set_size(self->chooser_items, 0);
	if (self->wins == NULL)
		return;
	for (i = 0; i < self->wins->len; i++) {
		GsurfWmuxWin *win = g_ptr_array_index(self->wins, i);
		g_autoptr(GArray) panes = gsurf_wmux_layout_list_panes(win->layout);
		GsurfWmuxChooserItem item = { i, GSURF_WMUX_PANE_NONE };

		g_array_append_val(self->chooser_items, item);
		for (j = 0; j < panes->len; j++) {
			item.pane = g_array_index(panes, guint, j);
			g_array_append_val(self->chooser_items, item);
		}
	}
}

static void
on_chooser_row_clicked(GtkWidget *widget, gpointer user_data)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(user_data);
	guint tag = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), GSURF_WMUX_INDEX_DATA));

	if (tag == 0)
		return;
	self->chooser_sel = tag - 1;
	gsurf_wmux_ui_chooser_choose(self);
}

/* One chooser line: a window ("0: name (2 panes)") or one of its panes
 * ("    0.1: title", with the container's colour stripe). */
static GtkWidget *
chooser_row(GsurfWmuxModule *self, guint index, const GsurfWmuxChooserItem *item)
{
	GsurfWmuxWin *win = g_ptr_array_index(self->wins, item->win);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *stripe = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *label;
	GtkWidget *row;
	g_autofree gchar *text = NULL;

	if (item->pane == GSURF_WMUX_PANE_NONE) {
		g_autofree gchar *name = gsurf_wmux_win_display_name(self, item->win);
		guint n = gsurf_wmux_layout_get_n_panes(win->layout);

		text = g_strdup_printf("%u: %s (%u pane%s)%s", item->win, name, n, n == 1 ? "" : "s",
			item->win == self->current ? " *" : "");
	} else {
		GsurfView *view = gsurf_wmux_pane_view(self, win, item->pane);
		const gchar *title = view != NULL ? gsurf_view_get_title(view) : NULL;
		g_autofree gchar *ctr = NULL;

		if ((title == NULL || *title == '\0') && view != NULL)
			title = gsurf_view_get_uri(view);
		text = g_strdup_printf("    %u.%d: %s", item->win,
			gsurf_wmux_layout_get_pane_index(win->layout, item->pane),
			title != NULL && *title != '\0' ? title : "(empty)");
		ctr = container_class(self, view);
		if (ctr != NULL) {
			g_autofree gchar *cls = g_strconcat(CHOOSER_CTR_PREFIX,
				ctr + strlen(GSURF_WMUX_CTR_CLASS_PREFIX), NULL);

			gsurf_chrome_widget_add_class(stripe, cls);
		}
	}
	gtk_widget_set_size_request(stripe, 4, -1);
	box_append(box, stripe, FALSE);
	label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
	box_append(box, label, TRUE);

	row = gsurf_chrome_clickable_new(box);
	gsurf_chrome_widget_add_class(box, GSURF_WMUX_CHOOSER_ROW_CLASS);
	if (index == self->chooser_sel)
		gsurf_chrome_widget_add_class(box, GSURF_WMUX_CHOOSER_SEL_CLASS);
	g_object_set_data(G_OBJECT(row), GSURF_WMUX_INDEX_DATA, GUINT_TO_POINTER(index + 1));
	gsurf_chrome_connect_click(row, 1, on_chooser_row_clicked, self);
	return row;
}

void
gsurf_wmux_ui_chooser_refresh(GsurfWmuxModule *self)
{
	guint i;

	if (self->chooser_list == NULL || !self->chooser_open)
		return;
	chooser_items_rebuild(self);
	if (self->chooser_items->len == 0) {
		gsurf_wmux_ui_chooser_close(self);
		return;
	}
	if (self->chooser_sel >= self->chooser_items->len)
		self->chooser_sel = self->chooser_items->len - 1;
	gsurf_chrome_box_clear(self->chooser_list);
	for (i = 0; i < self->chooser_items->len; i++) {
		GtkWidget *row = chooser_row(self, i,
			&g_array_index(self->chooser_items, GsurfWmuxChooserItem, i));

		box_append(self->chooser_list, row, FALSE);
		gsurf_chrome_widget_show_all(row);
	}
}

void
gsurf_wmux_ui_chooser_open(GsurfWmuxModule *self)
{
	GsurfWmuxWin *win = gsurf_wmux_current(self);
	guint i;

	if (self->chooser == NULL || win == NULL)
		return;
	self->chooser_open = TRUE;
	chooser_items_rebuild(self);
	/* Start on the active pane of the current window. */
	self->chooser_sel = 0;
	for (i = 0; i < self->chooser_items->len; i++) {
		GsurfWmuxChooserItem *item = &g_array_index(self->chooser_items, GsurfWmuxChooserItem, i);

		if (item->win == self->current &&
		    item->pane == gsurf_wmux_layout_get_active(win->layout))
			self->chooser_sel = i;
	}
	gsurf_wmux_ui_chooser_refresh(self);
	gtk_widget_set_visible(self->chooser, TRUE);
	gsurf_wmux_update_grab(self);
}

void
gsurf_wmux_ui_chooser_close(GsurfWmuxModule *self)
{
	if (!self->chooser_open)
		return;
	self->chooser_open = FALSE;
	if (self->chooser != NULL) {
		gtk_widget_set_visible(self->chooser, FALSE);
		gsurf_chrome_box_clear(self->chooser_list);
	}
	if (self->chooser_items != NULL)
		g_array_set_size(self->chooser_items, 0);
	gsurf_wmux_update_grab(self);
}

void
gsurf_wmux_ui_chooser_move(GsurfWmuxModule *self, gint delta)
{
	gint n;

	if (!self->chooser_open || self->chooser_items == NULL || self->chooser_items->len == 0)
		return;
	n = (gint)self->chooser_items->len;
	self->chooser_sel = (guint)((((gint)self->chooser_sel + delta) % n + n) % n);
	gsurf_wmux_ui_chooser_refresh(self);
}

void
gsurf_wmux_ui_chooser_choose(GsurfWmuxModule *self)
{
	GsurfWmuxChooserItem item;

	if (!self->chooser_open || self->chooser_items == NULL ||
	    self->chooser_sel >= self->chooser_items->len)
		return;
	item = g_array_index(self->chooser_items, GsurfWmuxChooserItem, self->chooser_sel);
	gsurf_wmux_ui_chooser_close(self);
	gsurf_wmux_select_window(self, item.win);
	if (item.pane != GSURF_WMUX_PANE_NONE)
		gsurf_wmux_select_pane(self, item.pane);
}

void
gsurf_wmux_ui_chooser_kill(GsurfWmuxModule *self)
{
	GsurfWmuxChooserItem item;

	if (!self->chooser_open || self->chooser_items == NULL ||
	    self->chooser_sel >= self->chooser_items->len)
		return;
	item = g_array_index(self->chooser_items, GsurfWmuxChooserItem, self->chooser_sel);
	if (item.pane == GSURF_WMUX_PANE_NONE)
		gsurf_wmux_kill_window(self, item.win);
	else
		gsurf_wmux_kill_pane(self, item.win, item.pane);
	gsurf_wmux_ui_chooser_refresh(self);
}

/* ------------------------------------------------------------------ */
/* Build and free                                                      */
/* ------------------------------------------------------------------ */

void
gsurf_wmux_ui_build(GsurfWmuxModule *self)
{
	g_autofree gchar *css = NULL;
	GtkWidget *session;
	GtkWidget *spacer;

	if (self->root != NULL)
		return;

	/* 1. The stylesheet (published for the tests) and the frame table. */
	css = build_css(self);
	self->css = gsurf_chrome_css_provider_new(css);
	g_object_set_data(G_OBJECT(self), GSURF_WMUX_CSS_DATA, self->css);
	self->frames = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, frame_free);

	/* 2. Root, overlay, grid and the chooser panel over the grid. */
	self->root = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
	gtk_widget_set_name(self->root, GSURF_WMUX_ROOT_NAME);
	gtk_widget_set_hexpand(self->root, TRUE);
	gtk_widget_set_vexpand(self->root, TRUE);
	self->overlay = gtk_overlay_new();
	gtk_widget_set_hexpand(self->overlay, TRUE);
	gtk_widget_set_vexpand(self->overlay, TRUE);
	self->grid = gsurf_wmux_grid_new(&grid_funcs, self);
	gtk_widget_set_name(self->grid, GSURF_WMUX_GRID_NAME);
	overlay_set_main(self->overlay, self->grid);

	self->chooser = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_name(self->chooser, GSURF_WMUX_CHOOSER_NAME);
	gsurf_chrome_widget_add_class(self->chooser, "wmux-chooser");
	gtk_widget_set_halign(self->chooser, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(self->chooser, GTK_ALIGN_CENTER);
	self->chooser_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	box_append(self->chooser, self->chooser_list, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->chooser);

	/* 3. The status line: [gsurf] windows message prompt ... counter. */
	self->status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_name(self->status_box, GSURF_WMUX_STATUS_NAME);
	gsurf_chrome_widget_add_class(self->status_box, GSURF_WMUX_STATUS_CLASS);
	session = gtk_label_new("[" GSURF_WMUX_SESSION_NAME "]");
	box_append(self->status_box, session, FALSE);
	self->status_windows = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_name(self->status_windows, GSURF_WMUX_STATUS_WINDOWS_NAME);
	box_append(self->status_box, self->status_windows, FALSE);
	self->message = gtk_label_new("");
	gtk_widget_set_name(self->message, GSURF_WMUX_MESSAGE_NAME);
	gtk_label_set_ellipsize(GTK_LABEL(self->message), PANGO_ELLIPSIZE_END);
	box_append(self->status_box, self->message, FALSE);
	self->prompt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_name(self->prompt_box, GSURF_WMUX_PROMPT_NAME);
	self->prompt_label = gtk_label_new(":");
	gtk_widget_set_name(self->prompt_label, GSURF_WMUX_PROMPT_LABEL_NAME);
	box_append(self->prompt_box, self->prompt_label, FALSE);
	self->prompt_entry = gtk_entry_new();
	gtk_widget_set_name(self->prompt_entry, GSURF_WMUX_PROMPT_ENTRY_NAME);
	gtk_widget_set_hexpand(self->prompt_entry, TRUE);
	gsurf_chrome_connect_key_press(self->prompt_entry, on_prompt_key, self);
	box_append(self->prompt_box, self->prompt_entry, TRUE);
	box_append(self->status_box, self->prompt_box, TRUE);
	spacer = gtk_label_new("");
	gtk_widget_set_hexpand(spacer, TRUE);
	box_append(self->status_box, spacer, TRUE);
	self->status_right = gtk_label_new("");
	gtk_widget_set_name(self->status_right, GSURF_WMUX_STATUS_RIGHT_NAME);
	box_append(self->status_box, self->status_right, FALSE);

	/* 4. Status line above or below the panes. */
	if (self->status_top)
		box_append(self->root, self->status_box, FALSE);
	box_append(self->root, self->overlay, TRUE);
	if (!self->status_top)
		box_append(self->root, self->status_box, FALSE);

	/* 5. GTK3 widgets start hidden: show the tree once, then hide (with
	 *    no-show-all) what only appears on demand. */
	gsurf_chrome_widget_show_all(self->root);
	gsurf_chrome_widget_start_hidden(self->chooser);
	gsurf_chrome_widget_start_hidden(self->message);
	gsurf_chrome_widget_start_hidden(self->prompt_box);
	gsurf_chrome_widget_manage_visibility(self->status_box);
	gtk_widget_set_visible(self->status_box, self->status);
}

void
gsurf_wmux_ui_free(GsurfWmuxModule *self)
{
	if (self->message_id != 0) {
		g_source_remove(self->message_id);
		self->message_id = 0;
	}
	if (self->display_id != 0) {
		g_source_remove(self->display_id);
		self->display_id = 0;
	}
	self->displaying = FALSE;
	self->chooser_open = FALSE;
	self->prompt_kind = GSURF_WMUX_PROMPT_NONE;
	g_clear_pointer(&self->prompt_template, g_free);
	g_clear_pointer(&self->prompt_command, g_free);
	g_clear_pointer(&self->chooser_items, g_array_unref);

	/* Frames first: their web views must never die with the root. */
	g_clear_pointer(&self->frames, g_hash_table_unref);
	if (self->css != NULL) {
		gsurf_chrome_css_provider_remove(self->css);
		g_clear_object(&self->css);
	}
	g_object_set_data(G_OBJECT(self), GSURF_WMUX_CSS_DATA, NULL);
	if (self->prompt_entry != NULL)
		gsurf_chrome_disconnect_key_press(self->prompt_entry, self);
	if (self->grid != NULL)
		gsurf_wmux_grid_set_owner(self->grid, NULL);
	if (self->root != NULL) {
		gsurf_chrome_widget_detach(self->root);
		g_clear_object(&self->root);
	}
	self->overlay = NULL;
	self->grid = NULL;
	self->status_box = NULL;
	self->status_windows = NULL;
	self->status_right = NULL;
	self->message = NULL;
	self->prompt_box = NULL;
	self->prompt_label = NULL;
	self->prompt_entry = NULL;
	self->chooser = NULL;
	self->chooser_list = NULL;
}
