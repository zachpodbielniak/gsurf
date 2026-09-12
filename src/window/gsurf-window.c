/*
 * gsurf-window.c - Abstract browser window
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "boxed/gsurf-keybind-help.h"
#include "util/gsurf-kiosk.h"
#include "window/gsurf-window.h"

#include <gdk/gdk.h>

typedef struct {
	GPtrArray *views;     /* owned refs to GsurfView */
	GsurfView *active;    /* borrowed (element of views) */
	gboolean   fullscreen;
	gboolean   help_overlay; /* in-page JS help is visible */
} GsurfWindowPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(GsurfWindow, gsurf_window, G_TYPE_OBJECT)

enum {
	SIG_VIEW_ADDED,
	SIG_VIEW_REMOVED,
	SIG_ACTIVE_VIEW_CHANGED,
	SIG_CLOSE_REQUEST,
	SIG_KEY_PRESS,
	SIG_BUTTON_PRESS,
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
	g_ptr_array_add(priv->views, g_object_ref(view));

	if (klass->insert_view != NULL)
		klass->insert_view(self, view);

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

	if (klass->remove_view != NULL)
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

	if (klass->set_active_view != NULL)
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
