/*
 * gsurf-wmux-session.c - The tmux windows of a browser window
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A browser window under wmux is a tmux session: a list of tmux windows
 * (GsurfWmuxWin), one of them current. Each tmux window has a pane layout
 * (GsurfWmuxLayout) whose pane payloads are the ids of the views the panes
 * show (0 for an empty pane). A view of the browser window that no pane
 * shows is a *background tab*: it stays in the window (and the tab tree),
 * not shown (its frame is parked in the grid).
 *
 * Invariants kept by every operation here:
 *   - a view id is in at most one pane of all tmux windows;
 *   - the window's active view is the view of the current tmux window's
 *     active pane (whenever that pane shows one);
 *   - the widgets (gsurf-wmux-ui.c) show exactly the panes of the current
 *     window, after gsurf_wmux_ui_sync().
 *
 * This file also implements #GsurfWindowLayout for the module: the window
 * hands views in (add_view), takes them out (remove_view), makes one
 * active (activate_view) and asks for visibility and a saved state.
 */

#include <yaml-glib.h>

#include "gsurf-wmux-private.h"

static void on_view_title_changed(GsurfView *view, const gchar *title, gpointer user_data);

/* ------------------------------------------------------------------ */
/* Windows and lookups                                                 */
/* ------------------------------------------------------------------ */

GsurfWmuxWin *
gsurf_wmux_win_new(GsurfWmuxModule *self, const gchar *name)
{
	GsurfWmuxWin *win = g_new0(GsurfWmuxWin, 1);

	win->name = g_strdup(name != NULL ? name : "");
	win->renamed = (name != NULL && *name != '\0');
	win->layout = gsurf_wmux_layout_new();
	gsurf_wmux_layout_set_min_pct(win->layout, (guint)self->min_pane_pct);
	return win;
}

void
gsurf_wmux_win_free(GsurfWmuxWin *win)
{
	if (win == NULL)
		return;
	g_free(win->name);
	g_clear_pointer(&win->layout, gsurf_wmux_layout_free);
	g_free(win);
}

GsurfWmuxWin *
gsurf_wmux_current(GsurfWmuxModule *self)
{
	if (self->wins == NULL || self->current >= self->wins->len)
		return NULL;
	return g_ptr_array_index(self->wins, self->current);
}

/* The view with @id while it belongs to the tracked window, else NULL. */
static GsurfView *
window_view(GsurfWmuxModule *self, guint64 id)
{
	GsurfView *view;

	if (self->window == NULL || id == 0)
		return NULL;
	view = gsurf_view_lookup(id);
	if (view == NULL || gsurf_window_get_view_index(self->window, view) < 0)
		return NULL;
	return view;
}

GsurfView *
gsurf_wmux_pane_view(GsurfWmuxModule *self, GsurfWmuxWin *win, guint pane)
{
	if (win == NULL || pane == GSURF_WMUX_PANE_NONE)
		return NULL;
	return window_view(self, gsurf_wmux_layout_get_payload(win->layout, pane));
}

gboolean
gsurf_wmux_find_view(GsurfWmuxModule *self, guint64 id, guint *win, guint *pane)
{
	guint i;

	if (self->wins == NULL || id == 0)
		return FALSE;
	for (i = 0; i < self->wins->len; i++) {
		GsurfWmuxWin *w = g_ptr_array_index(self->wins, i);
		guint p = gsurf_wmux_layout_find_payload(w->layout, id);

		if (p != GSURF_WMUX_PANE_NONE) {
			if (win != NULL)
				*win = i;
			if (pane != NULL)
				*pane = p;
			return TRUE;
		}
	}
	return FALSE;
}

gboolean
gsurf_wmux_view_visible(GsurfWmuxModule *self, GsurfView *view)
{
	GsurfWmuxWin *win;
	guint w;
	guint p;
	guint zoomed;

	if (view == NULL || !gsurf_wmux_find_view(self, gsurf_view_get_id(view), &w, &p) ||
	    w != self->current)
		return FALSE;
	win = gsurf_wmux_current(self);
	zoomed = gsurf_wmux_layout_get_zoomed(win->layout);
	return zoomed == GSURF_WMUX_PANE_NONE || zoomed == p;
}

gchar *
gsurf_wmux_win_display_name(GsurfWmuxModule *self, guint index)
{
	GsurfWmuxWin *win;
	GsurfView *view;
	const gchar *name = NULL;
	glong max = MAX(self->window_name_max, 1);

	if (self->wins == NULL || index >= self->wins->len)
		return g_strdup("");
	win = g_ptr_array_index(self->wins, index);

	/* A renamed window keeps its name; otherwise it follows the page in
	 * its active pane (title, else address). */
	if (win->renamed) {
		name = win->name;
	} else {
		view = gsurf_wmux_pane_view(self, win, gsurf_wmux_layout_get_active(win->layout));
		if (view != NULL)
			name = gsurf_view_get_title(view);
		if ((name == NULL || *name == '\0') && view != NULL)
			name = gsurf_view_get_uri(view);
	}
	if (name == NULL || *name == '\0')
		name = "-";
	if (!g_utf8_validate(name, -1, NULL))
		return g_strdup("?");
	if (g_utf8_strlen(name, -1) <= max)
		return g_strdup(name);
	return g_utf8_substring(name, 0, max);
}

/* The next view in window order (after @exclude, wrapping) that no pane
 * shows, other than @exclude itself; NULL when there is none. */
static GsurfView *
next_background(GsurfWmuxModule *self, guint64 exclude)
{
	GPtrArray *views;
	gint start;
	guint n;
	guint k;

	if (self->window == NULL)
		return NULL;
	views = gsurf_window_get_views(self->window);
	n = views->len;
	start = gsurf_window_get_view_index(self->window, gsurf_view_lookup(exclude));
	for (k = 0; k < n; k++) {
		guint i = start >= 0 ? ((guint)start + 1 + k) % n : k;
		GsurfView *view = g_ptr_array_index(views, i);
		guint64 id = gsurf_view_get_id(view);

		if (id != exclude && !gsurf_wmux_find_view(self, id, NULL, NULL))
			return view;
	}
	return NULL;
}

/* Drop tmux window @index from the list, keeping current and last_win
 * pointing at the same windows. The caller has moved current away. */
static void
remove_win_at(GsurfWmuxModule *self, guint index)
{
	if (index >= self->wins->len || self->wins->len <= 1)
		return;
	g_ptr_array_remove_index(self->wins, index);
	if (self->current > index)
		self->current--;
	else if (self->current >= self->wins->len)
		self->current = self->wins->len - 1;
	if (self->last_win == (gint)index)
		self->last_win = -1;
	else if (self->last_win > (gint)index)
		self->last_win--;
	if (self->last_win == (gint)self->current)
		self->last_win = -1;
}

/* Take view @id out of whatever pane shows it: the pane closes when its
 * window has others, a non-current window left without panes closes,
 * else the pane is left empty. */
static void
release_view(GsurfWmuxModule *self, guint64 id)
{
	GsurfWmuxWin *win;
	guint w;
	guint p;

	if (!gsurf_wmux_find_view(self, id, &w, &p))
		return;
	win = g_ptr_array_index(self->wins, w);
	if (gsurf_wmux_layout_get_n_panes(win->layout) > 1)
		gsurf_wmux_layout_remove(win->layout, p, NULL);
	else if (self->wins->len > 1 && w != self->current)
		remove_win_at(self, w);
	else
		gsurf_wmux_layout_set_payload(win->layout, p, 0);
}

/* ------------------------------------------------------------------ */
/* New views                                                           */
/* ------------------------------------------------------------------ */

/* A new view placed like Ctrl+t (the containers policy picks its
 * container from @uri), or a duplicate of @source (same address, same
 * container, a child of @source in the tab tree). Not yet in the window.
 * @load_out receives the address to load once it is. */
static GsurfView *
new_view(GsurfWmuxModule *self, GsurfView *source, const gchar *uri, gchar **load_out)
{
	GsurfConfig *config = gsurf_config_get_default();
	const gchar *fallback = (config != NULL && config->new_view_uri != NULL)
		? config->new_view_uri : "about:blank";
	GsurfView *view;

	(void)self;
	if (source != NULL) {
		const gchar *container = gsurf_view_get_container_id(source);
		const gchar *src_uri = gsurf_view_get_uri(source);

		if (src_uri == NULL || *src_uri == '\0')
			src_uri = gsurf_view_get_initial_uri(source);
		if (src_uri == NULL || *src_uri == '\0')
			src_uri = fallback;
		/* An explicit container (default included), so the containers
		 * module's new-tab policy does not pick another one. */
		view = gsurf_view_new_full(source,
			container != NULL ? container : GSURF_CONTAINER_DEFAULT, src_uri, FALSE);
		*load_out = g_strdup(src_uri);
	} else {
		const gchar *target = (uri != NULL && *uri != '\0') ? uri : fallback;
		g_autofree gchar *rewritten = NULL;

		/* A typed address goes through the URI handlers (search
		 * engines) like the address bar does; the default does not. */
		if (uri != NULL && *uri != '\0')
			rewritten = gsurf_module_manager_dispatch_rewrite_uri(
				gsurf_module_manager_get_default(), uri);
		if (rewritten != NULL)
			target = rewritten;
		view = gsurf_view_new_full(NULL, NULL, target, FALSE);
		*load_out = g_strdup(target);
	}
	if (view == NULL) {
		g_clear_pointer(load_out, g_free);
		return NULL;
	}
	if (config != NULL && config->settings != NULL)
		gsurf_view_apply_settings(view, config->settings);
	return view;
}

/* ------------------------------------------------------------------ */
/* Showing the active pane                                             */
/* ------------------------------------------------------------------ */

void
gsurf_wmux_show_active(GsurfWmuxModule *self)
{
	GsurfWmuxWin *win = gsurf_wmux_current(self);
	GsurfView *view;

	if (win == NULL || self->window == NULL)
		return;
	view = gsurf_wmux_pane_view(self, win, gsurf_wmux_layout_get_active(win->layout));

	/* The window's active view follows the active pane; activation comes
	 * back through activate_view, which syncs and focuses. */
	if (view != NULL && gsurf_window_get_active_view(self->window) != view) {
		gsurf_window_set_active_view(self->window, view);
		return;
	}
	gsurf_wmux_ui_sync(self);
	if (view != NULL)
		gsurf_view_focus(view);
}

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */

void
gsurf_wmux_select_pane(GsurfWmuxModule *self, guint pane)
{
	GsurfWmuxWin *win = gsurf_wmux_current(self);

	if (win == NULL || !gsurf_wmux_layout_set_active(win->layout, pane))
		return;
	gsurf_wmux_show_active(self);
}

void
gsurf_wmux_select_window(GsurfWmuxModule *self, guint index)
{
	if (self->wins == NULL || index >= self->wins->len || index == self->current)
		return;
	self->last_win = (gint)self->current;
	self->current = index;
	gsurf_wmux_show_active(self);
}

/*
 * gsurf_wmux_split:
 * @pane: the pane to split (of the current window)
 * @keep_active: split-window -d: the active pane stays active
 * @pct: the new pane's share, 0 for split_ratio
 * @duplicate: split-window -c: the new pane shows a copy of @pane's page
 * @uri: (nullable): the address for a new tab
 * @existing: (nullable): show this view in the new pane instead of a new
 *   tab (pane-split-* for a tab that is not on screen)
 *
 * Splits @pane. The new view goes into the new pane *before* it joins the
 * window, so the window's add_view and the activation that follows find
 * it in the new pane (not the old one).
 *
 * Returns: whether the split happened
 */
gboolean
gsurf_wmux_split(GsurfWmuxModule *self, guint pane, GsurfWmuxSplit split, gboolean before,
                 gboolean keep_active, guint pct, gboolean duplicate, const gchar *uri,
                 GsurfView *existing)
{
	GsurfWmuxWin *win = gsurf_wmux_current(self);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *load = NULL;
	g_autoptr(GsurfView) view = NULL;
	guint new_pane;

	if (win == NULL || self->window == NULL)
		return FALSE;

	/* 1. A view shown elsewhere leaves its old pane first. */
	if (existing != NULL)
		release_view(self, gsurf_view_get_id(existing));

	/* 2. The new pane, refused when either side would get too small. */
	new_pane = gsurf_wmux_layout_split(win->layout, pane, split, before,
		pct > 0 ? pct : (guint)self->split_ratio, &error);
	if (new_pane == GSURF_WMUX_PANE_NONE) {
		gsurf_wmux_ui_message(self, error->message);
		gsurf_wmux_ui_sync(self);
		return FALSE;
	}

	/* 3. What it shows: the given view, a duplicate, or a new tab. */
	if (existing != NULL) {
		view = g_object_ref(existing);
	} else {
		view = new_view(self, duplicate ? gsurf_wmux_pane_view(self, win, pane) : NULL,
			uri, &load);
		if (view == NULL) {
			gsurf_wmux_layout_remove(win->layout, new_pane, NULL);
			gsurf_wmux_ui_sync(self);
			return FALSE;
		}
	}
	gsurf_wmux_layout_set_payload(win->layout, new_pane, gsurf_view_get_id(view));

	/* 4. Into the window (a new tab), then activate the new pane. */
	if (existing == NULL)
		gsurf_window_add_view(self->window, view);
	if (!keep_active)
		gsurf_wmux_layout_set_active(win->layout, new_pane);
	gsurf_wmux_show_active(self);
	if (load != NULL)
		gsurf_view_load_uri(view, load);
	return TRUE;
}

void
gsurf_wmux_kill_window(GsurfWmuxModule *self, guint index)
{
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GArray) panes = NULL;
	GsurfWmuxWin *win;
	guint i;

	if (self->wins == NULL || index >= self->wins->len || self->window == NULL)
		return;

	/* 1. The last window is replaced by a fresh one with a blank tab. */
	if (self->wins->len == 1)
		gsurf_wmux_new_window(self, NULL, NULL);

	/* 2. Remember the views it shows (their tabs close afterwards). */
	win = g_ptr_array_index(self->wins, index);
	ids = g_array_new(FALSE, FALSE, sizeof(guint64));
	panes = gsurf_wmux_layout_list_panes(win->layout);
	for (i = 0; i < panes->len; i++) {
		guint64 id = gsurf_wmux_layout_get_payload(win->layout,
			g_array_index(panes, guint, i));

		if (id != 0)
			g_array_append_val(ids, id);
	}

	/* 3. Leave it (to the last window, or a neighbour) and drop it. */
	if (index == self->current) {
		if (self->last_win >= 0 && (guint)self->last_win != index &&
		    (guint)self->last_win < self->wins->len)
			self->current = (guint)self->last_win;
		else
			self->current = index + 1 < self->wins->len ? index + 1 : index - 1;
		self->last_win = -1;
	}
	remove_win_at(self, index);
	gsurf_wmux_show_active(self);

	/* 4. on_pane_kill: close_tab closes the tabs too. */
	if (self->keep_tab)
		return;
	for (i = 0; i < ids->len; i++) {
		GsurfView *view = window_view(self, g_array_index(ids, guint64, i));

		if (view != NULL && !gsurf_wmux_find_view(self, gsurf_view_get_id(view), NULL, NULL))
			gsurf_window_remove_view(self->window, view);
	}
}

void
gsurf_wmux_kill_pane(GsurfWmuxModule *self, guint index, guint pane)
{
	GsurfWmuxWin *win;
	GsurfView *view;

	if (self->wins == NULL || index >= self->wins->len || self->window == NULL)
		return;
	win = g_ptr_array_index(self->wins, index);
	if (!gsurf_wmux_layout_has_pane(win->layout, pane))
		return;
	view = gsurf_wmux_pane_view(self, win, pane);

	/* 1. The pane goes; its window's next pane becomes active. */
	if (gsurf_wmux_layout_get_n_panes(win->layout) > 1) {
		gsurf_wmux_layout_remove(win->layout, pane, NULL);
		gsurf_wmux_show_active(self);
	} else if (self->wins->len > 1) {
		/* 2. The only pane of a window: the window goes (tmux). */
		gsurf_wmux_kill_window(self, index);
		return;
	} else {
		/* 3. The last pane of the last window stays: a new blank tab
		 *    takes its place. */
		g_autofree gchar *load = NULL;
		g_autoptr(GsurfView) blank = new_view(self, NULL, NULL, &load);

		if (blank == NULL)
			return;
		gsurf_wmux_layout_set_payload(win->layout, pane, gsurf_view_get_id(blank));
		gsurf_window_add_view(self->window, blank);
		gsurf_wmux_show_active(self);
		gsurf_view_load_uri(blank, load);
	}

	/* 4. on_pane_kill: close_tab closes the pane's tab as well. */
	if (view != NULL && !self->keep_tab &&
	    !gsurf_wmux_find_view(self, gsurf_view_get_id(view), NULL, NULL))
		gsurf_window_remove_view(self->window, view);
}

void
gsurf_wmux_new_window(GsurfWmuxModule *self, const gchar *name, const gchar *uri)
{
	GsurfWmuxWin *win;
	g_autofree gchar *load = NULL;
	g_autoptr(GsurfView) view = NULL;

	if (self->wins == NULL || self->window == NULL)
		return;
	win = gsurf_wmux_win_new(self, name);
	g_ptr_array_add(self->wins, win);
	self->last_win = (gint)self->current;
	self->current = self->wins->len - 1;

	/* Its one pane shows a new tab (placed like Ctrl+t). */
	view = new_view(self, NULL, uri, &load);
	if (view == NULL) {
		gsurf_wmux_ui_sync(self);
		return;
	}
	gsurf_wmux_layout_set_payload(win->layout, gsurf_wmux_layout_get_active(win->layout),
		gsurf_view_get_id(view));
	gsurf_window_add_view(self->window, view);
	gsurf_wmux_show_active(self);
	gsurf_view_load_uri(view, load);
}

void
gsurf_wmux_break_pane(GsurfWmuxModule *self)
{
	GsurfWmuxWin *win = gsurf_wmux_current(self);
	GsurfWmuxWin *fresh;
	guint pane;
	guint64 id;

	if (win == NULL)
		return;
	if (gsurf_wmux_layout_get_n_panes(win->layout) <= 1) {
		gsurf_wmux_ui_message(self, "break-pane: the window has only one pane");
		return;
	}

	/* The active pane's view leaves its window for a new one of its own;
	 * it stays the window's active view. */
	pane = gsurf_wmux_layout_get_active(win->layout);
	id = gsurf_wmux_layout_get_payload(win->layout, pane);
	gsurf_wmux_layout_remove(win->layout, pane, NULL);
	fresh = gsurf_wmux_win_new(self, NULL);
	gsurf_wmux_layout_set_payload(fresh->layout, gsurf_wmux_layout_get_active(fresh->layout), id);
	g_ptr_array_add(self->wins, fresh);
	self->last_win = (gint)self->current;
	self->current = self->wins->len - 1;
	gsurf_wmux_show_active(self);
}

void
gsurf_wmux_rename_window(GsurfWmuxModule *self, const gchar *name)
{
	GsurfWmuxWin *win = gsurf_wmux_current(self);

	if (win == NULL)
		return;
	g_free(win->name);
	win->name = g_strdup(name != NULL ? name : "");
	win->renamed = (name != NULL && *name != '\0');
	gsurf_wmux_ui_sync(self);
}

/* ------------------------------------------------------------------ */
/* View signals                                                        */
/* ------------------------------------------------------------------ */

static void
on_view_title_changed(GsurfView *view, const gchar *title, gpointer user_data)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(user_data);

	(void)view;
	(void)title;
	gsurf_wmux_ui_update_status(self);
	if (self->chooser_open)
		gsurf_wmux_ui_chooser_refresh(self);
}

static void
watch_view(GsurfWmuxModule *self, GsurfView *view)
{
	if (g_signal_handler_find(view, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL,
			on_view_title_changed, self) != 0)
		return;
	g_signal_connect(view, "title-changed", G_CALLBACK(on_view_title_changed), self);
}

static void
unwatch_view(GsurfWmuxModule *self, GsurfView *view)
{
	g_signal_handlers_disconnect_by_func(view, on_view_title_changed, self);
}

void
gsurf_wmux_session_clear(GsurfWmuxModule *self)
{
	guint i;

	if (self->window != NULL) {
		GPtrArray *views = gsurf_window_get_views(self->window);

		for (i = 0; i < views->len; i++)
			unwatch_view(self, g_ptr_array_index(views, i));
	}
	g_clear_pointer(&self->wins, g_ptr_array_unref);
	self->current = 0;
	self->last_win = -1;
	self->in_activate = 0;
	g_clear_pointer(&self->visible_sig, g_free);
}

/* ------------------------------------------------------------------ */
/* GsurfWindowLayout                                                   */
/* ------------------------------------------------------------------ */

static gpointer
wmux_get_root(GsurfWindowLayout *layout)
{
	return GSURF_WMUX_MODULE(layout)->root;
}

/* The window installs us: one tmux window with one empty pane; the
 * window's views arrive next (add_view), then the active one is
 * activated into that pane. */
static void
wmux_attach(GsurfWindowLayout *layout, GsurfWindow *window)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(layout);

	if (window != self->window) {
		g_warning("wmux: the layout serves the window it was activated for only");
		return;
	}
	gsurf_wmux_session_clear(self);
	self->wins = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_wmux_win_free);
	g_ptr_array_add(self->wins, gsurf_wmux_win_new(self, NULL));
	self->current = 0;
	self->last_win = -1;
	self->attached = TRUE;
}

/* The window removes us (deactivate, or the window going away): every
 * web view leaves its frame; the window stacks them again. */
static void
wmux_detach(GsurfWindowLayout *layout, GsurfWindow *window)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(layout);

	if (!self->attached || window != self->window)
		return;
	gsurf_wmux_ui_prompt_close(self);
	gsurf_wmux_ui_chooser_close(self);
	gsurf_wmux_ui_display_panes(self, FALSE);
	gsurf_wmux_ui_clear_frames(self);
	gsurf_wmux_session_clear(self);
	self->attached = FALSE;
	self->prefix_active = FALSE;
	self->dragging = FALSE;
	gsurf_window_set_key_grab(window, FALSE);
}

/* A new view is a background tab unless an operation already put it in
 * a pane (a split or a new window set the pane's payload first). */
static void
wmux_add_view(GsurfWindowLayout *layout, GsurfWindow *window, GsurfView *view)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(layout);

	if (!self->attached || window != self->window)
		return;
	watch_view(self, view);
	gsurf_wmux_ui_sync(self);
}

/* A tab closes. If a pane showed it: on_tab_close close_pane closes the
 * pane (the last pane of the last window shows the next background tab
 * instead), keep_pane keeps the pane with the next background tab. When
 * it was the active view, the view of the new active pane takes over
 * right away (before the window picks a neighbour by list order). */
static void
wmux_remove_view(GsurfWindowLayout *layout, GsurfWindow *window, GsurfView *view)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(layout);
	guint64 id = gsurf_view_get_id(view);
	GsurfWmuxWin *win;
	GsurfView *next;
	gboolean was_active;
	guint w;
	guint p;

	if (!self->attached || window != self->window)
		return;
	unwatch_view(self, view);
	/* The view leaves the window: its web view leaves its frame (and no
	 * sync below makes it a new one; it is still in the window's list
	 * until this returns). */
	gsurf_wmux_ui_drop_frame(self, id);
	self->removing = id;
	if (!gsurf_wmux_find_view(self, id, &w, &p)) {
		gsurf_wmux_ui_sync(self);
		self->removing = 0;
		return;
	}
	win = g_ptr_array_index(self->wins, w);
	was_active = (gsurf_window_get_active_view(window) == view);

	if (!self->keep_pane && gsurf_wmux_layout_get_n_panes(win->layout) > 1) {
		gsurf_wmux_layout_remove(win->layout, p, NULL);
	} else if (!self->keep_pane && self->wins->len > 1) {
		/* The window's only pane: the window closes (tmux). */
		if (w == self->current) {
			if (self->last_win >= 0 && (guint)self->last_win != w &&
			    (guint)self->last_win < self->wins->len)
				self->current = (guint)self->last_win;
			else
				self->current = w + 1 < self->wins->len ? w + 1 : w - 1;
			self->last_win = -1;
		}
		remove_win_at(self, w);
	} else {
		next = next_background(self, id);
		gsurf_wmux_layout_set_payload(win->layout, p, next != NULL ? gsurf_view_get_id(next) : 0);
	}

	gsurf_wmux_ui_sync(self);
	if (was_active) {
		win = gsurf_wmux_current(self);
		next = gsurf_wmux_pane_view(self, win, gsurf_wmux_layout_get_active(win->layout));
		if (next != NULL && next != view)
			gsurf_window_set_active_view(window, next);
	}
	self->removing = 0;
}

/* The window made @view active (a tab was selected, a new tab, a popup,
 * focus followed a click): a pane of the current window that shows it
 * becomes active; a pane of another window switches to that window;
 * otherwise the active pane shows it and its old view becomes a
 * background tab. */
static void
wmux_activate_view(GsurfWindowLayout *layout, GsurfWindow *window, GsurfView *view)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(layout);
	GsurfWmuxWin *win;
	guint w;
	guint p;
	guint i;

	if (!self->attached || window != self->window)
		return;
	/* A focus change caused by our own reparenting (a sync below) must
	 * not rearrange the panes; the outer activation sets things right. */
	if (self->in_activate > 0)
		return;

	self->in_activate++;
	if (gsurf_wmux_find_view(self, gsurf_view_get_id(view), &w, &p)) {
		if (w != self->current) {
			self->last_win = (gint)self->current;
			self->current = w;
		}
		win = gsurf_wmux_current(self);
		gsurf_wmux_layout_set_active(win->layout, p);
	} else {
		win = gsurf_wmux_current(self);
		gsurf_wmux_layout_set_payload(win->layout, gsurf_wmux_layout_get_active(win->layout),
			gsurf_view_get_id(view));
	}
	gsurf_wmux_ui_sync(self);
	gsurf_view_focus(view);
	self->in_activate--;

	/* A stray activation during the sync (GTK moved the focus into
	 * another pane) left the window pointing elsewhere: point it back,
	 * a bounded number of times. */
	for (i = 0; i < 2 && gsurf_window_get_active_view(window) != view &&
	            gsurf_window_get_view_index(window, view) >= 0; i++)
		gsurf_window_set_active_view(window, view);
}

static gboolean
wmux_is_view_visible(GsurfWindowLayout *layout, GsurfWindow *window, GsurfView *view)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(layout);

	if (!self->attached || window != self->window)
		return FALSE;
	return gsurf_wmux_view_visible(self, view);
}

/* The state string ("wmux1 current=... [ ... layout=...]", see
 * docs/wmux.org) with each pane's payload the index @map gives its view.
 * Panes whose view is not saved are left out (a window left without
 * panes too); NULL when nothing is left or remember_layout is off. */
static gchar *
wmux_serialize(GsurfWindowLayout *layout, GsurfWindow *window,
               GsurfWindowLayoutIndexFunc map, gpointer user_data)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(layout);
	g_autoptr(GsurfWmuxState) state = NULL;
	gint current = 0;
	gint last = -1;
	guint i;
	guint j;

	if (!self->attached || window != self->window || !self->remember_layout || map == NULL)
		return NULL;
	state = gsurf_wmux_state_new();
	for (i = 0; i < self->wins->len; i++) {
		GsurfWmuxWin *win = g_ptr_array_index(self->wins, i);
		g_autoptr(GsurfWmuxLayout) copy = gsurf_wmux_layout_copy(win->layout);
		g_autoptr(GArray) panes = gsurf_wmux_layout_list_panes(copy);
		gboolean empty = FALSE;
		guint index;

		/* 1. Each pane's payload becomes its view's saved index; a pane
		 *    whose view is not saved goes. */
		for (j = 0; j < panes->len && !empty; j++) {
			guint pane = g_array_index(panes, guint, j);
			GsurfView *view = gsurf_wmux_pane_view(self, win, pane);
			gint saved = view != NULL ? map(view, user_data) : -1;

			if (saved >= 0)
				gsurf_wmux_layout_set_payload(copy, pane, (guint64)saved);
			else if (!gsurf_wmux_layout_remove(copy, pane, NULL))
				empty = TRUE;
		}
		if (empty)
			continue;

		/* 2. Keep current and last pointing at the same windows. */
		index = gsurf_wmux_state_add_window(state, win->name, win->renamed,
			g_steal_pointer(&copy));
		if (i == self->current)
			current = (gint)index;
		if ((gint)i == self->last_win)
			last = (gint)index;
	}
	if (state->windows->len == 0)
		return NULL;
	state->current = current;
	state->last = last != current ? last : -1;
	return gsurf_wmux_state_serialize(state);
}

/* Rebuild the tmux windows from a saved state; @views[i] is the view
 * saved at index i. Every pane must name a distinct live view of the
 * window, or nothing changes. */
static gboolean
wmux_restore(GsurfWindowLayout *layout, GsurfWindow *window, const gchar *text,
             GsurfView **views, guint n_views)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(layout);
	g_autoptr(GsurfWmuxState) state = NULL;
	g_autoptr(GPtrArray) wins = NULL;
	g_autoptr(GHashTable) used = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	guint j;

	if (!self->attached || window != self->window || text == NULL)
		return FALSE;

	/* 1. Parse strictly. */
	state = gsurf_wmux_state_parse(text, NULL, &error);
	if (state == NULL) {
		g_debug("wmux: cannot restore the layout: %s", error->message);
		return FALSE;
	}

	/* 2. Map every pane's saved index to a live, unused view. */
	used = g_hash_table_new(g_direct_hash, g_direct_equal);
	wins = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_wmux_win_free);
	for (i = 0; i < state->windows->len; i++) {
		GsurfWmuxWindowState *ws = g_ptr_array_index(state->windows, i);
		GsurfWmuxWin *win = g_new0(GsurfWmuxWin, 1);
		g_autoptr(GArray) panes = NULL;

		win->name = g_strdup(ws->name != NULL ? ws->name : "");
		win->renamed = ws->renamed;
		win->layout = gsurf_wmux_layout_copy(ws->layout);
		gsurf_wmux_layout_set_min_pct(win->layout, (guint)self->min_pane_pct);
		g_ptr_array_add(wins, win);
		panes = gsurf_wmux_layout_list_panes(win->layout);
		for (j = 0; j < panes->len; j++) {
			guint pane = g_array_index(panes, guint, j);
			guint64 saved = gsurf_wmux_layout_get_payload(win->layout, pane);
			GsurfView *view;

			if (saved >= n_views || views[saved] == NULL ||
			    gsurf_window_get_view_index(window, views[saved]) < 0 ||
			    g_hash_table_contains(used, views[saved])) {
				g_debug("wmux: the saved layout does not fit the restored tabs");
				return FALSE;
			}
			view = views[saved];
			g_hash_table_add(used, view);
			gsurf_wmux_layout_set_payload(win->layout, pane, gsurf_view_get_id(view));
		}
	}

	/* 3. Swap the windows in; the views no pane shows become background
	 *    tabs, and the current window's active pane is shown. */
	g_clear_pointer(&self->wins, g_ptr_array_unref);
	self->wins = g_steal_pointer(&wins);
	self->current = (guint)CLAMP(state->current, 0, (gint)self->wins->len - 1);
	self->last_win = state->last >= 0 && state->last < (gint)self->wins->len &&
		state->last != (gint)self->current ? state->last : -1;
	gsurf_wmux_show_active(self);
	return TRUE;
}

void
gsurf_wmux_layout_iface_init(GsurfWindowLayoutInterface *iface)
{
	iface->get_root = wmux_get_root;
	iface->attach = wmux_attach;
	iface->detach = wmux_detach;
	iface->add_view = wmux_add_view;
	iface->remove_view = wmux_remove_view;
	iface->activate_view = wmux_activate_view;
	iface->is_view_visible = wmux_is_view_visible;
	iface->serialize = wmux_serialize;
	iface->restore = wmux_restore;
}
