/*
 * gsurf-window.c - Abstract browser window
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "boxed/gsurf-keybind-help.h"
#include "util/gsurf-kiosk.h"
#include "window/gsurf-window.h"
#include "window/gsurf-window-layout.h"

#include <gdk/gdk.h>

typedef struct {
	GPtrArray *views;     /* owned refs to GsurfView */
	GsurfView *active;    /* borrowed (element of views) */
	gboolean   fullscreen;
	gboolean   help_overlay; /* in-page JS help is visible */
	GsurfWindowLayout *layout; /* owned; NULL = the backend's view stack */
	gboolean   layout_warned;  /* refusal already logged for this window */
	gboolean   key_grab;       /* every key goes to key-press */
} GsurfWindowPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(GsurfWindow, gsurf_window, G_TYPE_OBJECT)

enum {
	SIG_VIEW_ADDED,
	SIG_VIEW_REMOVED,
	SIG_ACTIVE_VIEW_CHANGED,
	SIG_CLOSE_REQUEST,
	SIG_KEY_PRESS,
	SIG_BUTTON_PRESS,
	SIG_VIEW_MOVED,
	SIG_VIEW_REPLACED,
	SIG_VISIBLE_VIEWS_CHANGED,
	SIG_LAYOUT_GROUPS_CHANGED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gsurf_window_constructed(GObject *object)
{
	GsurfWindow *self = GSURF_WINDOW(object);
	GsurfWindowClass *klass = GSURF_WINDOW_GET_CLASS(self);

	G_OBJECT_CLASS(gsurf_window_parent_class)->constructed(object);

	/* Build the native toplevel/container now that the concrete
	 * instance has been initialized. */
	if (klass->realize != NULL)
		klass->realize(self);
}

/* Remove an installed layout before the views and the backend's widgets
 * go: the layout drops its per-window state and every view widget is
 * back in the stack, where the backend's own teardown expects it. The
 * GTK backends do this themselves before destroying their toplevel;
 * this covers every other subclass. */
static void
gsurf_window_dispose(GObject *object)
{
	GsurfWindow *self = GSURF_WINDOW(object);
	GsurfWindowPrivate *priv = gsurf_window_get_instance_private(self);

	if (priv->layout != NULL)
		gsurf_window_set_layout(self, NULL);

	G_OBJECT_CLASS(gsurf_window_parent_class)->dispose(object);
}

static void
gsurf_window_finalize(GObject *object)
{
	GsurfWindow *self = GSURF_WINDOW(object);
	GsurfWindowPrivate *priv = gsurf_window_get_instance_private(self);

	g_clear_pointer(&priv->views, g_ptr_array_unref);

	G_OBJECT_CLASS(gsurf_window_parent_class)->finalize(object);
}

static gchar *
js_quote(const gchar *s)
{
	g_autofree gchar *esc = g_strescape(s != NULL ? s : "", "");

	return g_strdup_printf("\"%s\"", esc);
}

/* Dismiss the in-page overlay injected by the default help vfunc. */
static void
gsurf_window_hide_js_help(GsurfWindow *self)
{
	GsurfWindowPrivate *priv = gsurf_window_get_instance_private(self);
	GsurfView *view = priv->active;

	priv->help_overlay = FALSE;
	if (view == NULL)
		return;
	gsurf_view_run_javascript_async(view,
		"(function(){var e=document.getElementById('gsurf-keybind-help');"
		"if(e&&e.parentNode)e.parentNode.removeChild(e);})()",
		NULL, NULL, NULL);
}

/* Move the in-page overlay cursor (j/k) or scroll the panel (h/l). */
static void
gsurf_window_move_js_help(GsurfWindow *self, GsurfKeybindHelpKey action)
{
	GsurfWindowPrivate *priv = gsurf_window_get_instance_private(self);
	GsurfView *view = priv->active;
	const gchar *dir;
	g_autofree gchar *js = NULL;

	if (view == NULL)
		return;

	switch (action) {
	case GSURF_KEYBIND_HELP_KEY_UP:
		dir = "k";
		break;
	case GSURF_KEYBIND_HELP_KEY_DOWN:
		dir = "j";
		break;
	case GSURF_KEYBIND_HELP_KEY_LEFT:
		dir = "h";
		break;
	case GSURF_KEYBIND_HELP_KEY_RIGHT:
		dir = "l";
		break;
	default:
		return;
	}

	js = g_strdup_printf(
		"(function(){"
		"var wrap=document.getElementById('gsurf-keybind-help');"
		"if(!wrap)return;"
		"var box=wrap.firstChild;"
		"if(!box)return;"
		"var rows=box.querySelectorAll('tr');"
		"var i=parseInt(wrap.getAttribute('data-idx')||'0',10);"
		"var dir='%s';"
		"function paint(n){"
		"if(rows.length===0)return;"
		"if(i>=0&&i<rows.length)rows[i].style.background='';"
		"if(n<0)n=0;"
		"if(n>=rows.length)n=rows.length-1;"
		"i=n;"
		"wrap.setAttribute('data-idx',String(i));"
		"rows[i].style.background='#45475a';"
		"rows[i].scrollIntoView({block:'nearest'});"
		"}"
		"if(dir==='j')paint(i+1);"
		"else if(dir==='k')paint(i-1);"
		"else if(dir==='h')box.scrollLeft-=80;"
		"else if(dir==='l')box.scrollLeft+=80;"
		"})()",
		dir);
	gsurf_view_run_javascript_async(view, js, NULL, NULL, NULL);
}

/*
 * Backend-agnostic overlay: inject a page-level panel. GTK backends
 * override this with a native dialog; LRG and embedders that do not
 * override still get a working help menu.
 */
static void
gsurf_window_show_keybind_help_js(GsurfWindow *self, GPtrArray *entries)
{
	GsurfWindowPrivate *priv = gsurf_window_get_instance_private(self);
	GsurfView *view;
	GString *js;
	guint i;

	view = priv->active;
	if (view == NULL)
		return;

	if (priv->help_overlay) {
		gsurf_window_hide_js_help(self);
		return;
	}

	js = g_string_new(
		"(function(){"
		"var old=document.getElementById('gsurf-keybind-help');"
		"if(old&&old.parentNode)old.parentNode.removeChild(old);"
		"var rows=[");

	for (i = 0; entries != NULL && i < entries->len; i++) {
		const GsurfKeybindHelp *h = g_ptr_array_index(entries, i);
		g_autofree gchar *pretty = gsurf_keybind_help_pretty_key(h->key);
		g_autofree gchar *k = js_quote(pretty);
		g_autofree gchar *d = js_quote(h->description);
		g_autofree gchar *s = js_quote(h->source);

		if (i > 0)
			g_string_append_c(js, ',');
		g_string_append_printf(js, "{key:%s,description:%s,source:%s}", k, d, s);
	}

	g_string_append(js,
		"];"
		"var wrap=document.createElement('div');"
		"wrap.id='gsurf-keybind-help';"
		"wrap.style.cssText='position:fixed;inset:0;z-index:2147483647;"
		"background:rgba(17,17,27,.72);display:flex;align-items:center;"
		"justify-content:center;';"
		"var box=document.createElement('div');"
		"box.style.cssText='background:#1e1e2e;color:#cdd6f4;max-width:920px;"
		"width:90%;max-height:80vh;overflow:auto;padding:16px 20px;"
		"border-radius:8px;font:13px/1.45 monospace;"
		"box-shadow:0 8px 32px rgba(0,0,0,.55);';"
		"var title=document.createElement('div');"
		"title.textContent='Keybindings  (hjkl move, q/?/Escape close)';"
		"title.style.cssText='font-weight:bold;margin-bottom:12px;font-size:16px;';"
		"box.appendChild(title);"
		"var table=document.createElement('table');"
		"table.style.cssText='border-collapse:collapse;width:100%;';"
		"rows.forEach(function(r){"
		"var tr=document.createElement('tr');"
		"function td(text,nowrap){var c=document.createElement('td');"
		"c.textContent=text||'';c.style.cssText='padding:3px 14px 3px 0;"
		"vertical-align:top;'+(nowrap?'white-space:nowrap;':'');return c;}"
		"tr.appendChild(td(r.key,true));"
		"tr.appendChild(td(r.description,false));"
		"tr.appendChild(td(r.source,true));"
		"table.appendChild(tr);});"
		"box.appendChild(table);wrap.appendChild(box);"
		"wrap.setAttribute('data-idx','0');"
		"if(table.firstChild)table.firstChild.style.background='#45475a';"
		"wrap.addEventListener('click',function(e){"
		"if(e.target===wrap&&wrap.parentNode)wrap.parentNode.removeChild(wrap);});"
		"document.documentElement.appendChild(wrap);})()");

	gsurf_view_run_javascript_async(view, js->str, NULL, NULL, NULL);
	g_string_free(js, TRUE);
	priv->help_overlay = TRUE;
}

static void
gsurf_window_class_init(GsurfWindowClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->constructed = gsurf_window_constructed;
	object_class->dispose = gsurf_window_dispose;
	object_class->finalize = gsurf_window_finalize;
	klass->show_keybind_help = gsurf_window_show_keybind_help_js;

	signals[SIG_VIEW_ADDED] = g_signal_new(
		"view-added", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_VIEW);

	signals[SIG_VIEW_REMOVED] = g_signal_new(
		"view-removed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_VIEW);

	/**
	 * GsurfWindow::active-view-changed:
	 * @self: the window
	 * @view: (nullable): the new active view, or %NULL when empty
	 *
	 * Emitted after switching views, including removal of the active view.
	 */
	signals[SIG_ACTIVE_VIEW_CHANGED] = g_signal_new(
		"active-view-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_VIEW);

	/**
	 * GsurfWindow::close-request:
	 *
	 * Emitted when the user requests to close the window. Returning
	 * %TRUE prevents the default close.
	 */
	signals[SIG_CLOSE_REQUEST] = g_signal_new(
		"close-request", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_true_handled, NULL, NULL,
		G_TYPE_BOOLEAN, 0);

	/**
	 * GsurfWindow::key-press:
	 * @window: the window
	 * @keyval: the key value
	 * @keycode: the hardware keycode
	 * @state: the modifier state (#GsurfKeyMod-compatible)
	 *
	 * Returns: %TRUE if the key was consumed.
	 */
	signals[SIG_KEY_PRESS] = g_signal_new(
		"key-press", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_true_handled, NULL, NULL,
		G_TYPE_BOOLEAN, 3, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);

	/**
	 * GsurfWindow::button-press:
	 * @window: the window
	 * @button: the button number
	 * @state: the modifier state
	 *
	 * Returns: %TRUE if the press was consumed.
	 */
	signals[SIG_BUTTON_PRESS] = g_signal_new(
		"button-press", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_true_handled, NULL, NULL,
		G_TYPE_BOOLEAN, 2, G_TYPE_UINT, G_TYPE_UINT);

	/**
	 * GsurfWindow::view-moved:
	 * @window: the window
	 * @view: the view that moved
	 * @from: its previous index
	 * @to: its new index
	 *
	 * Emitted after gsurf_window_move_view() reorders the view list.
	 */
	signals[SIG_VIEW_MOVED] = g_signal_new(
		"view-moved", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 3, GSURF_TYPE_VIEW, G_TYPE_UINT, G_TYPE_UINT);

	/**
	 * GsurfWindow::view-replaced:
	 * @window: the window
	 * @old_view: the view being replaced, still in the window
	 * @new_view: its replacement, already in the window
	 *
	 * Emitted by gsurf_window_reopen_view() after adding @new_view
	 * (#GsurfWindow::view-added) and before activating it
	 * (#GsurfWindow::active-view-changed) and removing @old_view
	 * (#GsurfWindow::view-removed), so chrome can hand @old_view's place
	 * (its children in a tab tree, its pane in a layout) to @new_view
	 * before activation would move it.
	 */
	signals[SIG_VIEW_REPLACED] = g_signal_new(
		"view-replaced", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, GSURF_TYPE_VIEW, GSURF_TYPE_VIEW);

	/**
	 * GsurfWindow::visible-views-changed:
	 * @window: the window
	 *
	 * Emitted when the set of views on screen may have changed: a
	 * #GsurfWindowLayout was installed or removed, or the layout called
	 * gsurf_window_layout_notify_visible(). Ask
	 * gsurf_window_get_view_visible() for each view. Without a layout the
	 * visible view is the active one; follow
	 * #GsurfWindow::active-view-changed for that.
	 */
	signals[SIG_VISIBLE_VIEWS_CHANGED] = g_signal_new(
		"visible-views-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);

	/**
	 * GsurfWindow::layout-groups-changed:
	 * @window: the window
	 *
	 * Emitted when the groups of the installed #GsurfWindowLayout may
	 * have changed (gsurf_window_layout_list_groups()): a layout was
	 * installed or removed, or the layout called
	 * gsurf_window_layout_notify_groups().
	 *
	 * Since: 0.4.2
	 */
	signals[SIG_LAYOUT_GROUPS_CHANGED] = g_signal_new(
		"layout-groups-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

static void
gsurf_window_init(GsurfWindow *self)
{
	GsurfWindowPrivate *priv = gsurf_window_get_instance_private(self);

	priv->views = g_ptr_array_new_with_free_func(g_object_unref);
	priv->active = NULL;
	priv->fullscreen = FALSE;
}

/* --- View management --- */

void
gsurf_window_add_view(GsurfWindow *self, GsurfView *view)
{
	gsurf_window_insert_view(self, view, G_MAXUINT);
}

void
gsurf_window_insert_view(GsurfWindow *self, GsurfView *view, guint index)
{
	GsurfWindowPrivate *priv;
	GsurfWindowClass *klass;
	gboolean first;

	g_return_if_fail(GSURF_IS_WINDOW(self));
	g_return_if_fail(GSURF_IS_VIEW(view));

	priv = gsurf_window_get_instance_private(self);
	klass = GSURF_WINDOW_GET_CLASS(self);

	first = (priv->views->len == 0);
	/* Native containers cannot insert the same widget twice, and one
	 * removal must release the view's entire membership in this window. */
	if (g_ptr_array_find(priv->views, view, NULL))
		return;
	if (!first && gsurf_kiosk_is_enabled())
		return;
	g_ptr_array_insert(priv->views, (gint)MIN(index, priv->views->len), g_object_ref(view));

	/* With a layout the view goes to the layout, not the stack; the
	 * backend still wires its event routing (buttons, focus). */
	if (priv->layout != NULL) {
		if (klass->connect_view != NULL)
			klass->connect_view(self, view);
		gsurf_window_layout_add_view(priv->layout, self, view);
	} else if (klass->insert_view != NULL) {
		klass->insert_view(self, view);
	}

	g_signal_emit(self, signals[SIG_VIEW_ADDED], 0, view);

	if (first)
		gsurf_window_set_active_view(self, view);
}

void
gsurf_window_remove_view(GsurfWindow *self, GsurfView *view)
{
	GsurfWindowPrivate *priv;
	GsurfWindowClass *klass;
	guint idx;

	g_return_if_fail(GSURF_IS_WINDOW(self));
	g_return_if_fail(GSURF_IS_VIEW(view));

	priv = gsurf_window_get_instance_private(self);
	klass = GSURF_WINDOW_GET_CLASS(self);

	if (!g_ptr_array_find(priv->views, view, &idx))
		return;

	/* The layout unparents the widget; the neighbour logic below then
	 * activates the next view through the layout too. */
	if (priv->layout != NULL)
		gsurf_window_layout_remove_view(priv->layout, self, view);
	else if (klass->remove_view != NULL)
		klass->remove_view(self, view);

	/* Hold a ref across the removal so we can still emit the signal. */
	g_object_ref(view);
	g_ptr_array_remove_index(priv->views, idx);

	if (priv->active == view) {
		/* Let the setter update the active pointer: preassigning it makes
		 * its equality guard skip the backend switch and notification. */
		if (priv->views->len > 0) {
			gsurf_window_set_active_view(self,
				g_ptr_array_index(priv->views, MIN(idx, priv->views->len - 1)));
		} else {
			priv->active = NULL;
			g_signal_emit(self, signals[SIG_ACTIVE_VIEW_CHANGED], 0, NULL);
		}
	}

	g_signal_emit(self, signals[SIG_VIEW_REMOVED], 0, view);
	g_object_unref(view);
}

void
gsurf_window_move_view(GsurfWindow *self, GsurfView *view, guint index)
{
	GsurfWindowPrivate *priv;
	guint from;
	guint to;
	gpointer item;

	g_return_if_fail(GSURF_IS_WINDOW(self));
	g_return_if_fail(GSURF_IS_VIEW(view));

	priv = gsurf_window_get_instance_private(self);
	if (!g_ptr_array_find(priv->views, view, &from))
		return;
	to = MIN(index, priv->views->len - 1);
	if (to == from)
		return;

	/* Steal without unref, then reinsert: the list keeps its reference. */
	item = g_ptr_array_steal_index(priv->views, from);
	g_ptr_array_insert(priv->views, (gint)to, item);
	g_signal_emit(self, signals[SIG_VIEW_MOVED], 0, view, from, to);
}

gint
gsurf_window_get_view_index(GsurfWindow *self, GsurfView *view)
{
	GsurfWindowPrivate *priv;
	guint index;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), -1);

	priv = gsurf_window_get_instance_private(self);
	return g_ptr_array_find(priv->views, view, &index) ? (gint)index : -1;
}

void
gsurf_window_set_active_view(GsurfWindow *self, GsurfView *view)
{
	GsurfWindowPrivate *priv;
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));
	g_return_if_fail(GSURF_IS_VIEW(view));

	priv = gsurf_window_get_instance_private(self);
	klass = GSURF_WINDOW_GET_CLASS(self);

	if (priv->active == view)
		return;

	/* The active pointer borrows the reference held by the view list. */
	if (!g_ptr_array_find(priv->views, view, NULL))
		return;

	priv->active = view;

	if (priv->layout != NULL)
		gsurf_window_layout_activate_view(priv->layout, self, view);
	else if (klass->set_active_view != NULL)
		klass->set_active_view(self, view);

	g_signal_emit(self, signals[SIG_ACTIVE_VIEW_CHANGED], 0, view);
}

GsurfView *
gsurf_window_get_active_view(GsurfWindow *self)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), NULL);

	priv = gsurf_window_get_instance_private(self);
	return priv->active;
}

GPtrArray *
gsurf_window_get_views(GsurfWindow *self)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), NULL);

	priv = gsurf_window_get_instance_private(self);
	return priv->views;
}

guint
gsurf_window_get_n_views(GsurfWindow *self)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), 0);

	priv = gsurf_window_get_instance_private(self);
	return priv->views->len;
}

GsurfView *
gsurf_window_get_nth_view(GsurfWindow *self, guint index)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), NULL);

	priv = gsurf_window_get_instance_private(self);
	if (index >= priv->views->len)
		return NULL;
	return g_ptr_array_index(priv->views, index);
}

/* --- Layouts, visibility, key grab --- */

/* Log a refused gsurf_window_set_layout() once per window: a module that
 * retries on every activation must not flood the log. */
static void
gsurf_window_warn_layout(GsurfWindow *self, const gchar *why)
{
	GsurfWindowPrivate *priv = gsurf_window_get_instance_private(self);

	if (priv->layout_warned)
		return;
	priv->layout_warned = TRUE;
	g_warning("gsurf_window_set_layout: %s; the window keeps one view at a time", why);
}

/* Hand every view from the backend stack to @layout (priv->layout is
 * already @layout). The order is part of the contract: the widgets leave
 * the stack before attach, the views arrive in window order, the active
 * one is activated, and only then is the root packed. */
static void
gsurf_window_attach_layout(GsurfWindow *self, GsurfWindowLayout *layout)
{
	GsurfWindowPrivate *priv = gsurf_window_get_instance_private(self);
	GsurfWindowClass *klass = GSURF_WINDOW_GET_CLASS(self);
	guint i;

	/* 1. Out of the stack (the views keep their own widget references). */
	for (i = 0; i < priv->views->len; i++) {
		if (klass->remove_view != NULL)
			klass->remove_view(self, g_ptr_array_index(priv->views, i));
	}

	/* 2. The layout takes them over, in order, then shows the active one. */
	gsurf_window_layout_attach(layout, self);
	for (i = 0; i < priv->views->len; i++)
		gsurf_window_layout_add_view(layout, self, g_ptr_array_index(priv->views, i));
	if (priv->active != NULL)
		gsurf_window_layout_activate_view(layout, self, priv->active);

	/* 3. Its root replaces the stack on screen. */
	klass->set_content(self, gsurf_window_layout_get_root(layout));
}

/* Undo gsurf_window_attach_layout() for the installed layout and drop the
 * window's reference to it. Every view widget ends up in the stack again,
 * in window order, with the active view shown. */
static void
gsurf_window_detach_layout(GsurfWindow *self)
{
	GsurfWindowPrivate *priv = gsurf_window_get_instance_private(self);
	GsurfWindowClass *klass = GSURF_WINDOW_GET_CLASS(self);
	g_autoptr(GsurfWindowLayout) layout = NULL;
	guint i;

	/* 1. Clear the pointer first: from here on the window routes views to
	 *    the stack again, also for anything the layout's detach triggers
	 *    (a focus change must not reach a half-detached layout). */
	layout = g_steal_pointer(&priv->layout);

	/* 2. The layout lets go of every view widget; its root leaves. */
	gsurf_window_layout_detach(layout, self);
	if (klass->set_content != NULL)
		klass->set_content(self, NULL);

	/* 3. Back into the stack (insert_view is safe to repeat: it wires
	 *    each widget once), the active one shown and focused. */
	for (i = 0; i < priv->views->len; i++) {
		if (klass->insert_view != NULL)
			klass->insert_view(self, g_ptr_array_index(priv->views, i));
	}
	if (priv->active != NULL && klass->set_active_view != NULL)
		klass->set_active_view(self, priv->active);
}

gboolean
gsurf_window_set_layout(GsurfWindow *self, GsurfWindowLayout *layout)
{
	GsurfWindowPrivate *priv;
	GsurfWindowClass *klass;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), FALSE);
	g_return_val_if_fail(layout == NULL || GSURF_IS_WINDOW_LAYOUT(layout), FALSE);

	priv = gsurf_window_get_instance_private(self);
	klass = GSURF_WINDOW_GET_CLASS(self);

	/* 1. Nothing to do: the same layout, or no layout to remove. */
	if (layout == priv->layout)
		return TRUE;

	/* 2. Refusals (only when installing; removal always works). Kiosk
	 *    shows exactly one view, and a backend without native content
	 *    packing (libregnum renders to a texture) has nowhere to put a
	 *    layout's root. */
	if (layout != NULL) {
		if (gsurf_kiosk_is_enabled()) {
			gsurf_window_warn_layout(self, "kiosk mode shows a single view");
			return FALSE;
		}
		if (klass->set_content == NULL) {
			gsurf_window_warn_layout(self, "this backend cannot host a layout");
			return FALSE;
		}
	}

	/* 3. Replace: the old layout goes first, then the new one comes. The
	 *    signal is emitted once, after both. */
	if (priv->layout != NULL)
		gsurf_window_detach_layout(self);
	if (layout != NULL) {
		priv->layout = g_object_ref(layout);
		gsurf_window_attach_layout(self, layout);
	}

	g_signal_emit(self, signals[SIG_VISIBLE_VIEWS_CHANGED], 0);
	g_signal_emit(self, signals[SIG_LAYOUT_GROUPS_CHANGED], 0);
	return TRUE;
}

GsurfWindowLayout *
gsurf_window_get_layout(GsurfWindow *self)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), NULL);

	priv = gsurf_window_get_instance_private(self);
	return priv->layout;
}

void
gsurf_window_layout_notify_visible(GsurfWindow *window)
{
	g_return_if_fail(GSURF_IS_WINDOW(window));

	g_signal_emit(window, signals[SIG_VISIBLE_VIEWS_CHANGED], 0);
}

void
gsurf_window_layout_notify_groups(GsurfWindow *window)
{
	g_return_if_fail(GSURF_IS_WINDOW(window));

	g_signal_emit(window, signals[SIG_LAYOUT_GROUPS_CHANGED], 0);
}

gboolean
gsurf_window_get_view_visible(GsurfWindow *self, GsurfView *view)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), FALSE);
	g_return_val_if_fail(GSURF_IS_VIEW(view), FALSE);

	priv = gsurf_window_get_instance_private(self);
	if (!g_ptr_array_find(priv->views, view, NULL))
		return FALSE;
	if (priv->layout != NULL)
		return gsurf_window_layout_is_view_visible(priv->layout, self, view);
	return priv->active == view;
}

void
gsurf_window_set_key_grab(GsurfWindow *self, gboolean grab)
{
	GsurfWindowPrivate *priv;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	priv = gsurf_window_get_instance_private(self);
	priv->key_grab = grab ? TRUE : FALSE;
}

gboolean
gsurf_window_get_key_grab(GsurfWindow *self)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), FALSE);

	priv = gsurf_window_get_instance_private(self);
	return priv->key_grab;
}

/* --- Native operations --- */

void
gsurf_window_present(GsurfWindow *self)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	if (klass->present != NULL)
		klass->present(self);
}

void
gsurf_window_set_title(GsurfWindow *self, const gchar *title)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	if (klass->set_title != NULL)
		klass->set_title(self, title);
}

void
gsurf_window_set_default_size(GsurfWindow *self, gint width, gint height)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	if (klass->set_default_size != NULL)
		klass->set_default_size(self, width, height);
}

void
gsurf_window_set_fullscreen(GsurfWindow *self, gboolean fullscreen)
{
	GsurfWindowPrivate *priv;
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	priv = gsurf_window_get_instance_private(self);
	klass = GSURF_WINDOW_GET_CLASS(self);

	priv->fullscreen = fullscreen;
	if (klass->set_fullscreen != NULL)
		klass->set_fullscreen(self, fullscreen);
}

gboolean
gsurf_window_get_fullscreen(GsurfWindow *self)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), FALSE);

	priv = gsurf_window_get_instance_private(self);
	return priv->fullscreen;
}

gpointer
gsurf_window_get_native_widget(GsurfWindow *self)
{
	GsurfWindowClass *klass;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), NULL);

	klass = GSURF_WINDOW_GET_CLASS(self);
	return klass->get_native_widget != NULL ? klass->get_native_widget(self) : NULL;
}

void
gsurf_window_add_top_widget(GsurfWindow *self, gpointer widget)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	/* Kiosk hides chrome like a PWA; modules stay loaded and dispatch. */
	if (!gsurf_kiosk_is_enabled() && klass->add_chrome_widget != NULL)
		klass->add_chrome_widget(self, widget, TRUE);
}

void
gsurf_window_add_bottom_widget(GsurfWindow *self, gpointer widget)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	if (!gsurf_kiosk_is_enabled() && klass->add_chrome_widget != NULL)
		klass->add_chrome_widget(self, widget, FALSE);
}

void
gsurf_window_add_side_widget(GsurfWindow *self, gpointer widget, GsurfChromeEdge edge)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	if (!gsurf_kiosk_is_enabled() && klass->add_side_widget != NULL)
		klass->add_side_widget(self, widget, edge);
}

void
gsurf_window_show_keybind_help(GsurfWindow *self, GPtrArray *entries)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	if (klass->show_keybind_help != NULL)
		klass->show_keybind_help(self, entries);
}

/* --- Event emission helpers --- */

gboolean
gsurf_window_emit_key_press(GsurfWindow *self, guint keyval, guint keycode, guint state)
{
	GsurfWindowPrivate *priv;
	gboolean handled = FALSE;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), FALSE);

	priv = gsurf_window_get_instance_private(self);
	/* Overlay keys must not leak to modal/core (j/k would scroll the page). */
	if (priv->help_overlay) {
		GsurfKeybindHelpKey action;

		action = gsurf_keybind_help_key_action(keyval, state);
		if (action == GSURF_KEYBIND_HELP_KEY_CLOSE) {
			gsurf_window_hide_js_help(self);
			return TRUE;
		}
		if (action != GSURF_KEYBIND_HELP_KEY_NONE) {
			gsurf_window_move_js_help(self, action);
			return TRUE;
		}
		if ((state & (GSURF_MOD_CTRL | GSURF_MOD_ALT | GSURF_MOD_SUPER)) == 0)
			return TRUE;
	}

	g_signal_emit(self, signals[SIG_KEY_PRESS], 0, keyval, keycode, state, &handled);
	return handled;
}

gboolean
gsurf_window_emit_button_press(GsurfWindow *self, guint button, guint state)
{
	gboolean handled = FALSE;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), FALSE);

	g_signal_emit(self, signals[SIG_BUTTON_PRESS], 0, button, state, &handled);
	return handled;
}

gboolean
gsurf_window_emit_close_request(GsurfWindow *self)
{
	gboolean handled = FALSE;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), FALSE);

	g_signal_emit(self, signals[SIG_CLOSE_REQUEST], 0, &handled);
	return handled;
}
