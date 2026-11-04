/*
 * gsurf-tabs-module.c - Tab strip, tree sidebar and keyboard tab management
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The module core. It drives the library's multi-view GsurfWindow with
 * keyboard binds and actions (new/close/switch/reopen, plus the tree
 * actions: sidebar toggle, collapse, move up/down), and keeps a
 * GsurfTabTree in step with the window's view order:
 *
 *   - view-added: the tree places a view opened from another tab right
 *     after its opener's last descendant and the module moves the view
 *     there (gsurf_window_move_view());
 *   - view-removed: the tree promotes or closes the children
 *     (on_close_parent);
 *   - view-moved by anyone else: the tree re-syncs from the window order;
 *   - active-view-changed: collapsed ancestors of the new tab expand.
 *
 * The strip and sidebar widgets live in gsurf-tabs-ui.c. Implements
 * #GsurfInputHandler, #GsurfKeybindProvider and #GsurfActionHandler.
 * Builds on GTK3 and GTK4.
 */

#include <gmodule.h>
#include <yaml-glib.h>

#include "gsurf-tabs-private.h"

static void gsurf_tabs_input_init(GsurfInputHandlerInterface *iface);
static void gsurf_tabs_keybind_init(GsurfKeybindProviderInterface *iface);
static void gsurf_tabs_action_init(GsurfActionHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfTabsModule, gsurf_tabs_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_tabs_input_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_KEYBIND_PROVIDER, gsurf_tabs_keybind_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_ACTION_HANDLER, gsurf_tabs_action_init))

/* Default keys (docs/tabs.org lists them with their conflict check). */
#define DEFAULT_KEY_NEW       "Ctrl+t"
#define DEFAULT_KEY_CLOSE     "Ctrl+w"
#define DEFAULT_KEY_NEXT      "Ctrl+Tab"
#define DEFAULT_KEY_PREV      "Ctrl+Shift+Tab"
#define DEFAULT_KEY_REOPEN    "Ctrl+Shift+t"
#define DEFAULT_KEY_SIDEBAR   "Ctrl+Shift+greater"
#define DEFAULT_KEY_COLLAPSE  "Ctrl+Shift+bracketleft"
#define DEFAULT_KEY_MOVE_UP   "Ctrl+Shift+Page_Up"
#define DEFAULT_KEY_MOVE_DOWN "Ctrl+Shift+Page_Down"

/* Bounds for the numeric options. */
#define SIDEBAR_WIDTH_MIN (60)
#define SIDEBAR_WIDTH_MAX (2000)
#define INDENT_MAX        (64)
#define MAX_DEPTH_MAX     (64)

static void on_view_title_changed(GsurfView *view, const gchar *title, gpointer user_data);
static void on_view_favicon_changed(GsurfView *view, gpointer user_data);

/* ------------------------------------------------------------------ */
/* Widget ids                                                          */
/* ------------------------------------------------------------------ */

void
gsurf_tabs_set_widget_id(GtkWidget *widget, guint64 id)
{
	g_object_set_data_full(G_OBJECT(widget), GSURF_TABS_ID_DATA,
		g_memdup2(&id, sizeof(id)), g_free);
}

guint64
gsurf_tabs_get_widget_id(GtkWidget *widget)
{
	guint64 *id = g_object_get_data(G_OBJECT(widget), GSURF_TABS_ID_DATA);

	return id != NULL ? *id : 0;
}

/* ------------------------------------------------------------------ */
/* Window helpers                                                      */
/* ------------------------------------------------------------------ */

/* The window's view with @id, or NULL when it is not (or no longer) in
 * the tracked window. */
static GsurfView *
window_view(GsurfTabsModule *self, guint64 id)
{
	GsurfView *view;

	if (self->window == NULL || id == 0)
		return NULL;
	view = gsurf_view_lookup(id);
	if (view == NULL || gsurf_window_get_view_index(self->window, view) < 0)
		return NULL;
	return view;
}

static guint64
active_id(GsurfTabsModule *self)
{
	GsurfView *active;

	if (self->window == NULL)
		return 0;
	active = gsurf_window_get_active_view(self->window);
	return active != NULL ? gsurf_view_get_id(active) : 0;
}

/* Re-sync the tree from the window's view order and openers. */
static void
tree_sync_from_window(GsurfTabsModule *self)
{
	g_autofree guint64 *ids = NULL;
	g_autofree guint64 *openers = NULL;
	guint i;
	guint n;

	if (self->window == NULL || self->tree == NULL)
		return;
	n = gsurf_window_get_n_views(self->window);
	ids = g_new0(guint64, MAX(n, 1));
	openers = g_new0(guint64, MAX(n, 1));
	for (i = 0; i < n; i++) {
		GsurfView *view = gsurf_window_get_nth_view(self->window, i);

		ids[i] = gsurf_view_get_id(view);
		openers[i] = gsurf_view_get_opener_id(view);
	}
	gsurf_tab_tree_sync(self->tree, ids, openers, n);
}

/* Reorder the window's views to the tree's order. Our own view-moved
 * emissions are ignored while this runs. */
static void
apply_tree_order(GsurfTabsModule *self)
{
	guint i;
	guint n;

	if (self->window == NULL || self->tree == NULL)
		return;
	self->syncing = TRUE;
	n = gsurf_tab_tree_get_n_tabs(self->tree);
	for (i = 0; i < n; i++) {
		GsurfView *view = window_view(self, gsurf_tab_tree_get_nth(self->tree, i));

		if (view != NULL)
			gsurf_window_move_view(self->window, view, i);
	}
	self->syncing = FALSE;
}

/* Watch a view's title and favicon for its row. */
static void
watch_view(GsurfTabsModule *self, GsurfView *view)
{
	if (g_signal_handler_find(view, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self) != 0)
		return;
	g_signal_connect(view, "title-changed", G_CALLBACK(on_view_title_changed), self);
	g_signal_connect(view, "favicon-changed", G_CALLBACK(on_view_favicon_changed), self);
}

static void
unwatch_view(GsurfTabsModule *self, GsurfView *view)
{
	g_signal_handlers_disconnect_by_data(view, self);
}

/* Remember a closing view's URI for tab-reopen. */
static void
remember_closed(GsurfTabsModule *self, GsurfView *view)
{
	const gchar *uri = gsurf_view_get_uri(view);

	if (uri != NULL && *uri != '\0' && g_strcmp0(uri, "about:blank") != 0)
		g_queue_push_head(self->closed_uris, g_strdup(uri));
}

/* ------------------------------------------------------------------ */
/* Tab operations (shared by keys, actions and the UI)                  */
/* ------------------------------------------------------------------ */

static GsurfView *
open_tab_uri(GsurfWindow *window, const gchar *uri)
{
	GsurfConfig *config = gsurf_config_get_default();
	const gchar *target = (uri != NULL && *uri != '\0') ? uri
		: ((config && config->new_view_uri) ? config->new_view_uri : "about:blank");
	/* The target doubles as the container placement hint, so a new or
	 * reopened tab for a host with a containers rule starts in that
	 * container instead of being reassigned after its first request. */
	GsurfView *view = gsurf_view_new_full(NULL, NULL, target, FALSE);

	if (view == NULL)
		return NULL;
	if (config != NULL && config->settings != NULL)
		gsurf_view_apply_settings(view, config->settings);
	gsurf_window_add_view(window, view);
	gsurf_window_set_active_view(window, view);
	gsurf_view_load_uri(view, target);
	g_object_unref(view);
	return view;
}

void
gsurf_tabs_activate_tab(GsurfTabsModule *self, guint64 id)
{
	GsurfView *view = window_view(self, id);

	if (view != NULL)
		gsurf_window_set_active_view(self->window, view);
}

/* Closing keeps at least one view: closing the last is the host's call. */
void
gsurf_tabs_close_tab(GsurfTabsModule *self, guint64 id)
{
	GsurfView *view = window_view(self, id);

	if (view == NULL || gsurf_window_get_n_views(self->window) <= 1)
		return;
	remember_closed(self, view);
	gsurf_window_remove_view(self->window, view);
}

/* Collapse or expand @id's subtree. Collapsing a subtree that holds the
 * active tab activates @id first, so the active tab stays visible (and
 * activation does not immediately expand it again). */
void
gsurf_tabs_toggle_collapse(GsurfTabsModule *self, guint64 id)
{
	gboolean collapse;
	gint index;
	gint active_index;
	gint last;

	if (self->tree == NULL || gsurf_tab_tree_get_n_children(self->tree, id) == 0)
		return;
	collapse = !gsurf_tab_tree_get_collapsed(self->tree, id);
	if (collapse) {
		index = gsurf_tab_tree_get_index(self->tree, id);
		last = gsurf_tab_tree_get_last_descendant_index(self->tree, id);
		active_index = gsurf_tab_tree_get_index(self->tree, active_id(self));
		if (active_index > index && active_index <= last)
			gsurf_tabs_activate_tab(self, id);
	}
	gsurf_tab_tree_set_collapsed(self->tree, id, collapse);
	gsurf_tabs_ui_sync(self);
}

/* Reload the tab with @id from the network cache rules (not bypassed). */
void
gsurf_tabs_reload_tab(GsurfTabsModule *self, guint64 id)
{
	GsurfView *view = window_view(self, id);

	if (view != NULL)
		gsurf_view_reload(view, FALSE);
}

/* Open a copy of the tab with @id as its child (so the tree shows it
 * right under the original), in the same container, and activate it. */
void
gsurf_tabs_duplicate_tab(GsurfTabsModule *self, guint64 id)
{
	GsurfConfig *config = gsurf_config_get_default();
	GsurfView *view = window_view(self, id);
	g_autoptr(GsurfView) copy = NULL;
	const gchar *container;
	const gchar *uri;

	if (view == NULL)
		return;
	uri = gsurf_view_get_uri(view);
	if (uri == NULL || *uri == '\0')
		uri = gsurf_view_get_initial_uri(view);
	if (uri == NULL || *uri == '\0')
		uri = (config != NULL && config->new_view_uri != NULL)
			? config->new_view_uri : "about:blank";
	/* An explicit container (default included), so the containers
	 * module's new-tab policy does not pick another one. */
	container = gsurf_view_get_container_id(view);
	copy = gsurf_view_new_full(view, container != NULL ? container : GSURF_CONTAINER_DEFAULT,
		uri, FALSE);
	if (copy == NULL)
		return;
	if (config != NULL && config->settings != NULL)
		gsurf_view_apply_settings(copy, config->settings);
	gsurf_window_add_view(self->window, copy);
	gsurf_window_set_active_view(self->window, copy);
	gsurf_view_load_uri(copy, uri);
}

/* Close every descendant of the tab with @id, keeping the tab itself
 * (and, as always, at least one view in the window). */
void
gsurf_tabs_close_children(GsurfTabsModule *self, guint64 id)
{
	g_autoptr(GArray) doomed = NULL;
	gint index;
	gint last;
	gint i;
	guint j;

	if (self->tree == NULL || window_view(self, id) == NULL)
		return;
	index = gsurf_tab_tree_get_index(self->tree, id);
	last = gsurf_tab_tree_get_last_descendant_index(self->tree, id);
	if (index < 0 || last <= index)
		return;

	/* Collect first: every removal re-derives the tree. */
	doomed = g_array_new(FALSE, FALSE, sizeof(guint64));
	for (i = index + 1; i <= last; i++) {
		guint64 child = gsurf_tab_tree_get_nth(self->tree, (guint)i);

		g_array_append_val(doomed, child);
	}
	for (j = 0; j < doomed->len; j++)
		gsurf_tabs_close_tab(self, g_array_index(doomed, guint64, j));
}

/* Reopen the tab with @id in @container, in its place in the tree (the
 * view-replaced handler hands the new view its children). */
void
gsurf_tabs_reopen_tab(GsurfTabsModule *self, guint64 id, const gchar *container)
{
	GsurfView *view = window_view(self, id);

	if (view == NULL || gsurf_kiosk_is_enabled())
		return;
	gsurf_window_reopen_view(self->window, view, container);
}

/* tab-tree-collapse on the active tab: toggle its subtree; on a leaf,
 * collapse its parent (activating the parent). */
static void
collapse_active(GsurfTabsModule *self)
{
	guint64 id = active_id(self);
	guint64 parent;

	if (self->tree == NULL || id == 0)
		return;
	if (gsurf_tab_tree_get_n_children(self->tree, id) > 0) {
		gsurf_tabs_toggle_collapse(self, id);
		return;
	}
	parent = gsurf_tab_tree_get_parent(self->tree, id);
	if (parent != 0 && !gsurf_tab_tree_get_collapsed(self->tree, parent))
		gsurf_tabs_toggle_collapse(self, parent);
}

/* tab-move-up / tab-move-down: move the active tab and its subtree
 * among its siblings, then apply the new order to the window. */
static void
move_active(GsurfTabsModule *self, gint direction)
{
	guint64 id = active_id(self);

	if (self->tree == NULL || id == 0)
		return;
	if (gsurf_tab_tree_move_subtree(self->tree, id, direction)) {
		apply_tree_order(self);
		gsurf_tabs_ui_sync(self);
	}
}

static void
remove_active_tab(GsurfTabsModule *self, GsurfWindow *window)
{
	GsurfView *active = gsurf_window_get_active_view(window);

	if (active != NULL)
		gsurf_tabs_close_tab(self, gsurf_view_get_id(active));
}

static void
switch_tab(GsurfWindow *window, gint delta)
{
	gint n = (gint)gsurf_window_get_n_views(window);
	gint cur;
	gint next;

	cur = gsurf_window_get_view_index(window, gsurf_window_get_active_view(window));
	if (n <= 1 || cur < 0)
		return;
	next = ((cur + delta) % n + n) % n;
	gsurf_window_set_active_view(window, gsurf_window_get_nth_view(window, (guint)next));
}

/* ------------------------------------------------------------------ */
/* Window, view and container signals                                  */
/* ------------------------------------------------------------------ */

static void
on_view_title_changed(GsurfView *view, const gchar *title, gpointer user_data)
{
	gsurf_tabs_ui_update_tab(GSURF_TABS_MODULE(user_data), gsurf_view_get_id(view));
}

static void
on_view_favicon_changed(GsurfView *view, gpointer user_data)
{
	gsurf_tabs_ui_update_tab(GSURF_TABS_MODULE(user_data), gsurf_view_get_id(view));
}

/* A new view: place it in the tree and move it to its tree position. */
static void
on_view_added(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(user_data);
	gint at;
	guint target;

	watch_view(self, view);
	if (self->tree == NULL)
		return;
	at = gsurf_window_get_view_index(window, view);
	target = gsurf_tab_tree_add(self->tree, gsurf_view_get_id(view),
		gsurf_view_get_opener_id(view), at >= 0 ? (guint)at : G_MAXUINT);
	if (at >= 0 && (guint)at != target) {
		self->syncing = TRUE;
		gsurf_window_move_view(window, view, target);
		self->syncing = FALSE;
	}
	/* Anything we could not mirror exactly (a stray reorder) is fixed by
	 * re-syncing from the window, the source of truth for the order. */
	if (gsurf_tab_tree_get_n_tabs(self->tree) != gsurf_window_get_n_views(window))
		tree_sync_from_window(self);
	gsurf_tabs_ui_sync(self);
}

/* A view left: promote or close its children per on_close_parent. */
static void
on_view_removed(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(user_data);
	g_autoptr(GArray) closed = NULL;
	guint i;

	unwatch_view(self, view);
	if (self->tree == NULL)
		return;

	closed = g_array_new(FALSE, FALSE, sizeof(guint64));
	gsurf_tab_tree_remove(self->tree, gsurf_view_get_id(view), self->on_close_parent, closed);

	/* close_children: close the descendants too. Their own view-removed
	 * finds them gone from the tree already. The last view is never
	 * closed; it stays open as a root. */
	for (i = 0; i < closed->len; i++) {
		GsurfView *child = window_view(self, g_array_index(closed, guint64, i));

		if (child == NULL || gsurf_window_get_n_views(window) <= 1)
			continue;
		remember_closed(self, child);
		gsurf_window_remove_view(window, child);
	}
	if (closed->len > 0 ||
	    gsurf_tab_tree_get_n_tabs(self->tree) != gsurf_window_get_n_views(window))
		tree_sync_from_window(self);
	gsurf_tabs_ui_sync(self);
}

/* gsurf_window_reopen_view() (this module's menu, the containers
 * module's container-reopen-next, anyone): the new view takes the old
 * one's place in the tree, children and collapse state included, so the
 * old view's removal right after finds nothing to promote or close. */
static void
on_view_replaced(GsurfWindow *window, GsurfView *old_view, GsurfView *new_view,
                 gpointer user_data)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(user_data);

	(void)window;
	if (self->tree == NULL)
		return;
	if (gsurf_tab_tree_replace(self->tree, gsurf_view_get_id(old_view),
	                           gsurf_view_get_id(new_view))) {
		apply_tree_order(self);
		gsurf_tabs_ui_sync(self);
	}
}

/* Someone else reordered the window: the window order wins. */
static void
on_view_moved(GsurfWindow *window, GsurfView *view, guint from, guint to, gpointer user_data)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(user_data);

	if (self->syncing)
		return;
	tree_sync_from_window(self);
	gsurf_tabs_ui_sync(self);
}

/* Activating a hidden tab (tab-next, a click in the strip, the host)
 * expands its collapsed ancestors. */
static void
on_active_view_changed(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(user_data);

	if (self->tree != NULL && view != NULL)
		gsurf_tab_tree_expand_ancestors(self->tree, gsurf_view_get_id(view));
	gsurf_tabs_ui_sync(self);
}

static void
on_container_notify(GObject *container, GParamSpec *pspec, gpointer user_data)
{
	gsurf_tabs_ui_update_containers(GSURF_TABS_MODULE(user_data));
}

static void
watch_container(GsurfTabsModule *self, GsurfContainer *container)
{
	if (g_signal_handler_find(container, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self) != 0)
		return;
	g_signal_connect(container, "notify::color", G_CALLBACK(on_container_notify), self);
	g_signal_connect(container, "notify::name", G_CALLBACK(on_container_notify), self);
}

static void
on_container_added(GsurfContainerRegistry *registry, GsurfContainer *container,
                   gpointer user_data)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(user_data);

	watch_container(self, container);
	gsurf_tabs_ui_update_containers(self);
}

static void
on_container_removed(GsurfContainerRegistry *registry, GsurfContainer *container,
                     gpointer user_data)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(user_data);

	g_signal_handlers_disconnect_by_data(container, self);
	gsurf_tabs_ui_update_containers(self);
}

/* Follow the container registry for stripe colours and names. */
static void
connect_registry(GsurfTabsModule *self)
{
	g_autoptr(GPtrArray) containers = NULL;
	guint i;

	self->registry = g_object_ref(gsurf_container_registry_get_default());
	g_signal_connect(self->registry, "container-added", G_CALLBACK(on_container_added), self);
	g_signal_connect(self->registry, "container-removed", G_CALLBACK(on_container_removed), self);
	containers = gsurf_container_registry_list(self->registry);
	for (i = 0; i < containers->len; i++)
		watch_container(self, g_ptr_array_index(containers, i));
}

static void
disconnect_registry(GsurfTabsModule *self)
{
	g_autoptr(GPtrArray) containers = NULL;
	guint i;

	if (self->registry == NULL)
		return;
	g_signal_handlers_disconnect_by_data(self->registry, self);
	containers = gsurf_container_registry_list(self->registry);
	for (i = 0; i < containers->len; i++)
		g_signal_handlers_disconnect_by_data(g_ptr_array_index(containers, i), self);
	g_clear_object(&self->registry);
}

/* ------------------------------------------------------------------ */
/* Keys and actions                                                    */
/* ------------------------------------------------------------------ */

/* Perform a tab action; shared by the module's own keys and the core
 * keybind actions forwarded through GsurfActionHandler. */
static gboolean
tabs_perform(GsurfTabsModule *self, GsurfWindow *window, GsurfAction action)
{
	/* Kiosk keeps the module loaded but never opens extra views. */
	if (gsurf_kiosk_is_enabled() &&
	    (action == GSURF_ACTION_TAB_NEW || action == GSURF_ACTION_TAB_REOPEN))
		return TRUE;

	switch (action) {
	case GSURF_ACTION_TAB_NEW:
		open_tab_uri(window, NULL);
		return TRUE;
	case GSURF_ACTION_TAB_CLOSE:
		remove_active_tab(self, window);
		return TRUE;
	case GSURF_ACTION_TAB_REOPEN:
		if (!g_queue_is_empty(self->closed_uris)) {
			g_autofree gchar *uri = g_queue_pop_head(self->closed_uris);

			open_tab_uri(window, uri);
		}
		return TRUE;
	case GSURF_ACTION_TAB_NEXT:
		switch_tab(window, +1);
		return TRUE;
	case GSURF_ACTION_TAB_PREV:
		switch_tab(window, -1);
		return TRUE;
	case GSURF_ACTION_TAB_SIDEBAR_TOGGLE:
		self->sidebar_shown = !self->sidebar_shown;
		gsurf_tabs_ui_update_visibility(self);
		return TRUE;
	case GSURF_ACTION_TAB_TREE_COLLAPSE:
		collapse_active(self);
		return TRUE;
	case GSURF_ACTION_TAB_MOVE_UP:
		move_active(self, -1);
		return TRUE;
	case GSURF_ACTION_TAB_MOVE_DOWN:
		move_active(self, +1);
		return TRUE;
	default:
		return FALSE;
	}
}

static gboolean
key_is(const gchar *key, guint keyval, guint state)
{
	return key != NULL && *key != '\0' && gsurf_keys_match(keyval, state, key);
}

static gboolean
gsurf_tabs_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                            guint keyval, guint keycode, guint state,
                            GsurfModePolicy mode)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(handler);
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());
	GsurfAction action = GSURF_ACTION_NONE;

	if (window == NULL)
		return FALSE;

	if (key_is(self->key_new, keyval, state))
		action = GSURF_ACTION_TAB_NEW;
	else if (key_is(self->key_close, keyval, state))
		action = GSURF_ACTION_TAB_CLOSE;
	else if (key_is(self->key_reopen, keyval, state))
		action = GSURF_ACTION_TAB_REOPEN;
	else if (key_is(self->key_next, keyval, state))
		action = GSURF_ACTION_TAB_NEXT;
	else if (key_is(self->key_prev, keyval, state))
		action = GSURF_ACTION_TAB_PREV;
	else if (key_is(self->key_sidebar, keyval, state))
		action = GSURF_ACTION_TAB_SIDEBAR_TOGGLE;
	else if (key_is(self->key_collapse, keyval, state))
		action = GSURF_ACTION_TAB_TREE_COLLAPSE;
	else if (key_is(self->key_move_up, keyval, state))
		action = GSURF_ACTION_TAB_MOVE_UP;
	else if (key_is(self->key_move_down, keyval, state))
		action = GSURF_ACTION_TAB_MOVE_DOWN;

	return tabs_perform(self, window, action);
}

static gboolean
gsurf_tabs_handle_action(GsurfActionHandler *handler, GsurfView *view,
                         GsurfAction action)
{
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());

	if (window == NULL)
		return FALSE;
	return tabs_perform(GSURF_TABS_MODULE(handler), window, action);
}

static void
gsurf_tabs_action_init(GsurfActionHandlerInterface *iface)
{
	iface->handle_action = gsurf_tabs_handle_action;
}

static void
gsurf_tabs_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_tabs_handle_key_event;
}

static void
gsurf_tabs_list_keybinds(GsurfKeybindProvider *provider, GPtrArray *entries)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(provider);

	gsurf_keybind_help_append(entries, self->key_new,
		"Open a new tab", "tabs", "tab-new");
	gsurf_keybind_help_append(entries, self->key_close,
		"Close the current tab", "tabs", "tab-close");
	gsurf_keybind_help_append(entries, self->key_next,
		"Switch to the next tab", "tabs", "tab-next");
	gsurf_keybind_help_append(entries, self->key_prev,
		"Switch to the previous tab", "tabs", "tab-prev");
	gsurf_keybind_help_append(entries, self->key_reopen,
		"Reopen the last closed tab", "tabs", "tab-reopen");
	gsurf_keybind_help_append(entries, self->key_sidebar,
		"Show or hide the tree tab sidebar", "tabs", "tab-sidebar-toggle");
	gsurf_keybind_help_append(entries, self->key_collapse,
		"Collapse or expand the current tab's subtree", "tabs", "tab-tree-collapse");
	gsurf_keybind_help_append(entries, self->key_move_up,
		"Move the current tab (and its subtree) up", "tabs", "tab-move-up");
	gsurf_keybind_help_append(entries, self->key_move_down,
		"Move the current tab (and its subtree) down", "tabs", "tab-move-down");
}

static void
gsurf_tabs_keybind_init(GsurfKeybindProviderInterface *iface)
{
	iface->list_keybinds = gsurf_tabs_list_keybinds;
}

/* ------------------------------------------------------------------ */
/* Module lifecycle                                                    */
/* ------------------------------------------------------------------ */

static const gchar *gsurf_tabs_get_name(GsurfModule *m) { return "tabs"; }
static const gchar *gsurf_tabs_get_description(GsurfModule *m)
{
	return "Tab strip, tree-style tab sidebar and keyboard tab management";
}

static void gsurf_tabs_deactivate(GsurfModule *module);

/* Release the views' handlers, the registry, the UI and the tree; the
 * window's own handlers are the caller's business. */
static void
release_activation(GsurfTabsModule *self)
{
	guint i;

	gsurf_tabs_ui_uninstall(self);
	disconnect_registry(self);
	if (self->tree != NULL) {
		for (i = 0; i < gsurf_tab_tree_get_n_tabs(self->tree); i++) {
			GsurfView *view = gsurf_view_lookup(gsurf_tab_tree_get_nth(self->tree, i));

			if (view != NULL)
				unwatch_view(self, view);
		}
	}
	g_clear_pointer(&self->tree, gsurf_tab_tree_free);
	self->syncing = FALSE;
}

/* The host destroyed the window first: drop everything that referred to
 * it (its views stay alive until the window finalizes, so their handlers
 * can still be removed). */
static void
on_window_finalized(gpointer user_data, GObject *where_the_window_was)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(user_data);

	self->window = NULL;
	release_activation(self);
}

static gboolean
gsurf_tabs_activate(GsurfModule *module)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(module);
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());
	guint i;

	if (window == NULL)
		return TRUE;
	/* A second activate() starts from a clean slate. */
	gsurf_tabs_deactivate(module);

	self->window = window;
	g_object_weak_ref(G_OBJECT(window), on_window_finalized, self);
	self->tree = gsurf_tab_tree_new();
	self->sidebar_shown = self->sidebar;

	/* 1. Mirror the views the window already has (startup, session). */
	tree_sync_from_window(self);
	for (i = 0; i < gsurf_window_get_n_views(window); i++)
		watch_view(self, gsurf_window_get_nth_view(window, i));

	/* 2. Follow the window from now on. */
	g_signal_connect(window, "view-added", G_CALLBACK(on_view_added), self);
	g_signal_connect(window, "view-removed", G_CALLBACK(on_view_removed), self);
	g_signal_connect(window, "view-moved", G_CALLBACK(on_view_moved), self);
	g_signal_connect(window, "view-replaced", G_CALLBACK(on_view_replaced), self);
	g_signal_connect(window, "active-view-changed", G_CALLBACK(on_active_view_changed), self);

	/* 3. Kiosk shows no strip and no sidebar; the keys still work. */
	if (gsurf_kiosk_is_enabled())
		return TRUE;
	connect_registry(self);
	gsurf_tabs_ui_install(self);
	return TRUE;
}

/* Release every activation resource; safe to call any number of times. */
static void
gsurf_tabs_deactivate(GsurfModule *module)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(module);
	guint i;

	if (self->window != NULL) {
		for (i = 0; i < gsurf_window_get_n_views(self->window); i++)
			unwatch_view(self, gsurf_window_get_nth_view(self->window, i));
		g_signal_handlers_disconnect_by_data(self->window, self);
		g_object_weak_unref(G_OBJECT(self->window), on_window_finalized, self);
		self->window = NULL;
	}
	release_activation(self);
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static void
set_string(gchar **field, const gchar *value)
{
	g_free(*field);
	*field = g_strdup(value);
}

/* A key string; a present but empty value unbinds the key. */
static void
read_key(YamlMapping *m, const gchar *member, gchar **field)
{
	if (yaml_mapping_has_member(m, member))
		set_string(field, yaml_mapping_get_string_member(m, member));
}

/* A CSS colour, validated with GdkRGBA and stored normalized. Invalid
 * colours warn and keep the default. */
static void
read_color(YamlMapping *m, const gchar *member, gchar **field)
{
	const gchar *text;
	GdkRGBA rgba;

	if (!yaml_mapping_has_member(m, member))
		return;
	text = yaml_mapping_get_string_member(m, member);
	if (text == NULL || !gdk_rgba_parse(&rgba, text)) {
		g_warning("tabs.%s: '%s' is not a CSS colour; keeping %s",
		          member, text != NULL ? text : "", *field);
		return;
	}
	g_free(*field);
	*field = gdk_rgba_to_string(&rgba);
}

/* An integer in [min, max]; anything else warns and keeps the default. */
static void
read_int(YamlMapping *m, const gchar *member, gint min, gint max, gint *field)
{
	const gchar *text;
	gint64 value;

	if (!yaml_mapping_has_member(m, member))
		return;
	text = yaml_mapping_get_string_member(m, member);
	if (text == NULL || !g_ascii_string_to_signed(text, 10, min, max, &value, NULL)) {
		g_warning("tabs.%s must be an integer from %d to %d; keeping %d",
		          member, min, max, *field);
		return;
	}
	*field = (gint)value;
}

static void
read_bool(YamlMapping *m, const gchar *member, gboolean *field)
{
	if (yaml_mapping_has_member(m, member))
		*field = yaml_mapping_get_boolean_member(m, member);
}

/* One of two words; anything else warns and keeps the default. */
static void
read_choice(YamlMapping *m, const gchar *member, const gchar *first,
            const gchar *second, gboolean *is_second)
{
	const gchar *text;

	if (!yaml_mapping_has_member(m, member))
		return;
	text = yaml_mapping_get_string_member(m, member);
	if (g_strcmp0(text, first) == 0)
		*is_second = FALSE;
	else if (g_strcmp0(text, second) == 0)
		*is_second = TRUE;
	else
		g_warning("tabs.%s must be '%s' or '%s', not '%s'; keeping '%s'",
		          member, first, second, text != NULL ? text : "",
		          *is_second ? second : first);
}

/* Every option back to its default (also the state with no tabs: block). */
static void
reset_config(GsurfTabsModule *self)
{
	set_string(&self->key_new, DEFAULT_KEY_NEW);
	set_string(&self->key_close, DEFAULT_KEY_CLOSE);
	set_string(&self->key_next, DEFAULT_KEY_NEXT);
	set_string(&self->key_prev, DEFAULT_KEY_PREV);
	set_string(&self->key_reopen, DEFAULT_KEY_REOPEN);
	set_string(&self->key_sidebar, DEFAULT_KEY_SIDEBAR);
	set_string(&self->key_collapse, DEFAULT_KEY_COLLAPSE);
	set_string(&self->key_move_up, DEFAULT_KEY_MOVE_UP);
	set_string(&self->key_move_down, DEFAULT_KEY_MOVE_DOWN);
	set_string(&self->bg_color, GSURF_TABS_DEFAULT_BG_COLOR);
	set_string(&self->active_color, GSURF_TABS_DEFAULT_ACTIVE_COLOR);
	set_string(&self->fg_color, GSURF_TABS_DEFAULT_FG_COLOR);
	self->position_top = TRUE;
	self->strip = TRUE;
	self->show_single = FALSE;
	self->sidebar = FALSE;
	self->sidebar_right = FALSE;
	self->sidebar_width = GSURF_TABS_DEFAULT_SIDEBAR_WIDTH;
	self->sidebar_hides_strip = TRUE;
	self->indent = GSURF_TABS_DEFAULT_INDENT;
	self->max_depth = GSURF_TABS_DEFAULT_MAX_DEPTH;
	self->on_close_parent = GSURF_TAB_TREE_CLOSE_PROMOTE;
}

/* Each configure() starts from the defaults, so a second call replaces
 * the previous configuration entirely (dropping a key restores its
 * default). It takes effect at the next activate(). */
static void
gsurf_tabs_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	gboolean bottom;
	gboolean close_children;

	reset_config(self);
	node = gsurf_config_get_module_node(config, "tabs");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	read_key(m, "key_new", &self->key_new);
	read_key(m, "key_close", &self->key_close);
	read_key(m, "key_next", &self->key_next);
	read_key(m, "key_prev", &self->key_prev);
	read_key(m, "key_reopen", &self->key_reopen);
	read_key(m, "key_sidebar", &self->key_sidebar);
	read_key(m, "key_collapse", &self->key_collapse);
	read_key(m, "key_move_up", &self->key_move_up);
	read_key(m, "key_move_down", &self->key_move_down);

	bottom = !self->position_top;
	read_choice(m, "position", "top", "bottom", &bottom);
	self->position_top = !bottom;
	read_bool(m, "strip", &self->strip);
	read_bool(m, "show_single", &self->show_single);
	read_color(m, "bg_color", &self->bg_color);
	read_color(m, "active_color", &self->active_color);
	read_color(m, "fg_color", &self->fg_color);

	read_bool(m, "sidebar", &self->sidebar);
	read_choice(m, "sidebar_position", "left", "right", &self->sidebar_right);
	read_int(m, "sidebar_width", SIDEBAR_WIDTH_MIN, SIDEBAR_WIDTH_MAX, &self->sidebar_width);
	read_bool(m, "sidebar_hides_strip", &self->sidebar_hides_strip);
	read_int(m, "indent", 0, INDENT_MAX, &self->indent);
	read_int(m, "max_depth", 0, MAX_DEPTH_MAX, &self->max_depth);

	close_children = (self->on_close_parent == GSURF_TAB_TREE_CLOSE_CHILDREN);
	read_choice(m, "on_close_parent", "promote", "close_children", &close_children);
	self->on_close_parent = close_children ? GSURF_TAB_TREE_CLOSE_CHILDREN
		: GSURF_TAB_TREE_CLOSE_PROMOTE;
}

static void
gsurf_tabs_module_dispose(GObject *object)
{
	gsurf_tabs_deactivate(GSURF_MODULE(object));
	G_OBJECT_CLASS(gsurf_tabs_module_parent_class)->dispose(object);
}

static void
gsurf_tabs_module_finalize(GObject *object)
{
	GsurfTabsModule *self = GSURF_TABS_MODULE(object);

	g_clear_pointer(&self->key_new, g_free);
	g_clear_pointer(&self->key_close, g_free);
	g_clear_pointer(&self->key_next, g_free);
	g_clear_pointer(&self->key_prev, g_free);
	g_clear_pointer(&self->key_reopen, g_free);
	g_clear_pointer(&self->key_sidebar, g_free);
	g_clear_pointer(&self->key_collapse, g_free);
	g_clear_pointer(&self->key_move_up, g_free);
	g_clear_pointer(&self->key_move_down, g_free);
	g_clear_pointer(&self->bg_color, g_free);
	g_clear_pointer(&self->active_color, g_free);
	g_clear_pointer(&self->fg_color, g_free);
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

	object_class->dispose = gsurf_tabs_module_dispose;
	object_class->finalize = gsurf_tabs_module_finalize;
	module_class->activate = gsurf_tabs_activate;
	module_class->deactivate = gsurf_tabs_deactivate;
	module_class->get_name = gsurf_tabs_get_name;
	module_class->get_description = gsurf_tabs_get_description;
	module_class->configure = gsurf_tabs_configure;
}

static void
gsurf_tabs_module_init(GsurfTabsModule *self)
{
	reset_config(self);
	self->closed_uris = g_queue_new();
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_TABS_MODULE;
}
