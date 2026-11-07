/*
 * gsurf-modal-module.c - Vim-style modal navigation + link hinting
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler at high priority and tracks its own mode.
 * NORMAL mode: bare keys are navigation commands (hjkl, gg, G, d/u, H/L,
 * r, and gt/gT to switch tabs); the insert keys (`i` and `a` by default,
 * modal.insert_keys) enter INSERT (keys pass to the page) until Escape;
 * `gi` focuses the first text field of the page, whose keys are then the
 * field's for as long as it keeps focus; Escape returns to NORMAL and
 * leaves (blurs) a focused field, so the next key is a command again, as
 * in vim. A focused page field takes its keys in NORMAL, and the
 * published mode (the indicators) reads INSERT while it does: the module
 * manager derives that from the page focus, this module owns the rest.
 * `f` enters FOLLOW mode: vimium-style link hints are overlaid on every
 * clickable element with short chord labels; typing a chord narrows the
 * set and a unique match is clicked. `F` opens the match in a new view.
 * A mouse click, another active view, or the page navigating or crashing
 * ends FOLLOW (and removes the labels), so it never outlives its hints.
 * Ports surf's modal patch and adds vim navigation + hinting.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <stdlib.h>
#include <string.h>

#define GSURF_TYPE_MODAL_MODULE (gsurf_modal_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfModalModule, gsurf_modal_module, GSURF, MODAL_MODULE, GsurfModule)

struct _GsurfModalModule
{
	GsurfModule parent_instance;
	GsurfModePolicy mode;
	gboolean pending_g;       /* saw 'g', waiting for the second */
	gboolean hint_newview;    /* current FOLLOW session opens in a new view */
	gint scroll_step;
	GStrv insert_keys;        /* keys that enter INSERT, default {i, a} */
	gchar *hint_key;          /* default "f" */
	gchar *hint_key_newview;  /* default "F" */
	gchar *hint_chars;        /* alphabet, e.g. "asdfghjkl" */
	gchar *hint_bg;
	gchar *hint_fg;
	gint hint_font_size;     /* CSS pixels, independent of page font settings */
	GsurfView *hint_view;     /* weak; the page showing FOLLOW's labels */
	GsurfWindow *hint_window; /* weak; whose active view FOLLOW watches */
};

static void gsurf_modal_input_init(GsurfInputHandlerInterface *iface);
static void gsurf_modal_keybind_init(GsurfKeybindProviderInterface *iface);
static void gsurf_modal_action_init(GsurfActionHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfModalModule, gsurf_modal_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_modal_input_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_KEYBIND_PROVIDER, gsurf_modal_keybind_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_ACTION_HANDLER, gsurf_modal_action_init))

static void
scroll(GsurfView *view, const gchar *js)
{
	gsurf_view_run_javascript_async(view, js, NULL, NULL, NULL);
}

static const gchar CLEAR_HINTS_JS[] = "window.__gsurf_clear&&window.__gsurf_clear()";

/* Stop watching the FOLLOW session's page and window (see watch_hints()). */
static void unwatch_hints(GsurfModalModule *self);

/* Change mode and publish INSERT passthrough so the host suppresses
 * bare-key dispatch to every module (not just this one) while typing,
 * and the mode itself (gsurf_module_manager_set_mode()). Leaving FOLLOW
 * also stops watching its page. */
static void
set_mode(GsurfModalModule *self, GsurfModePolicy mode)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();

	if (mode != GSURF_MODE_FOLLOW)
		unwatch_hints(self);
	self->mode = mode;
	gsurf_module_manager_set_input_passthrough(mgr, mode == GSURF_MODE_INSERT);
	/* Publish it for mode indicators (chromebar, the wmux status line). */
	gsurf_module_manager_set_mode(mgr, mode);
}

/* End a FOLLOW session that the user left some other way than a hint
 * key (a click, another view, a navigation): drop the labels from the
 * page that shows them, if it is still there, and return to NORMAL. */
static void
cancel_follow(GsurfModalModule *self, gboolean clear_labels)
{
	if (self->mode != GSURF_MODE_FOLLOW)
		return;
	if (clear_labels && self->hint_view != NULL)
		gsurf_view_run_javascript_async(self->hint_view, CLEAR_HINTS_JS, NULL, NULL, NULL);
	set_mode(self, GSURF_MODE_NORMAL);
}

/* The hint page navigated: once the new document commits, the labels
 * are gone, and so is the session. */
static void
on_hint_view_load(GsurfView *view, GsurfLoadEvent event, gpointer user_data)
{
	(void)view;
	if (event == GSURF_LOAD_COMMITTED)
		cancel_follow(GSURF_MODAL_MODULE(user_data), FALSE);
}

/* The hint page's web process died, taking the labels with it. */
static void
on_hint_view_crashed(GsurfView *view, gpointer user_data)
{
	(void)view;
	cancel_follow(GSURF_MODAL_MODULE(user_data), FALSE);
}

/* Another view became active (a tab switch, a pane click, the hint tab
 * closing): the labels belong to the old page, so the session ends and
 * they are removed, or they would greet the user on returning there. */
static void
on_hint_active_view_changed(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(user_data);

	(void)window;
	if (view != self->hint_view)
		cancel_follow(self, TRUE);
}

/* Follow a FOLLOW session's page (@view) and its window, so the session
 * ends with its labels rather than lingering in the indicator and
 * swallowing the next keys. */
static void
watch_hints(GsurfModalModule *self, GsurfView *view)
{
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());

	unwatch_hints(self);
	self->hint_view = view;
	g_object_add_weak_pointer(G_OBJECT(view), (gpointer *)&self->hint_view);
	g_signal_connect_object(view, "load-changed", G_CALLBACK(on_hint_view_load), self, 0);
	g_signal_connect_object(view, "web-process-terminated",
		G_CALLBACK(on_hint_view_crashed), self, 0);
	if (window != NULL) {
		self->hint_window = window;
		g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&self->hint_window);
		g_signal_connect_object(window, "active-view-changed",
			G_CALLBACK(on_hint_active_view_changed), self, 0);
	}
}

static void
unwatch_hints(GsurfModalModule *self)
{
	if (self->hint_view != NULL) {
		g_signal_handlers_disconnect_by_func(self->hint_view, on_hint_view_load, self);
		g_signal_handlers_disconnect_by_func(self->hint_view, on_hint_view_crashed, self);
		g_object_remove_weak_pointer(G_OBJECT(self->hint_view),
			(gpointer *)&self->hint_view);
		self->hint_view = NULL;
	}
	if (self->hint_window != NULL) {
		g_signal_handlers_disconnect_by_func(self->hint_window,
			on_hint_active_view_changed, self);
		g_object_remove_weak_pointer(G_OBJECT(self->hint_window),
			(gpointer *)&self->hint_window);
		self->hint_window = NULL;
	}
}

/* Leave INSERT (or FOLLOW) for NORMAL from a key or action: also leave
 * the focused page field, so the next key is a command (vim's Escape),
 * and give the page the keyboard back from a chrome entry (address bar,
 * find box) that has it, or the window would still be typing there. The
 * blur and the focus are harmless when there is nothing to leave. */
static void
leave_to_normal(GsurfModalModule *self, GsurfView *view)
{
	set_mode(self, GSURF_MODE_NORMAL);
	if (view != NULL) {
		gsurf_view_blur_editable(view);
		gsurf_view_focus(view);
	}
}

/* Whether @name is one of the configured insert keys. */
static gboolean
is_insert_key(GsurfModalModule *self, const gchar *name)
{
	return self->insert_keys != NULL &&
	       g_strv_contains((const gchar * const *)self->insert_keys, name);
}

/* vimium's `gi`: focus the first visible text field of the page (open
 * shadow roots included) and put the caret at its end. Returns "ok" once
 * the field really has the focus (the deepest active element, through
 * shadow roots), or "none". */
static const gchar FOCUS_FIRST_INPUT_JS[] =
	"(function(){var NOT=/^(button|submit|reset|checkbox|radio|file|image|range|color|hidden)$/;"
	"function ed(e){if(e.isContentEditable)return true;var t=e.tagName;"
	"if(t==='TEXTAREA')return true;"
	"if(t==='INPUT')return !NOT.test(String(e.type||'text').toLowerCase());"
	"var r=e.getAttribute&&e.getAttribute('role');return r==='textbox'||r==='searchbox';}"
	"function vis(e){var r=e.getBoundingClientRect();var s=getComputedStyle(e);"
	"return r.width>0&&r.height>0&&s.visibility!=='hidden'&&s.display!=='none'"
	"&&!e.disabled&&!e.readOnly;}"
	"function find(root){var all=root.querySelectorAll('*');"
	"for(var i=0;i<all.length;i++){var e=all[i];if(ed(e)&&vis(e))return e;"
	"if(e.shadowRoot){var f=find(e.shadowRoot);if(f)return f;}}return null;}"
	"var e=find(document);if(!e)return 'none';e.focus();"
	"var a=document.activeElement;"
	"while(a&&a.shadowRoot&&a.shadowRoot.activeElement)a=a.shadowRoot.activeElement;"
	"if(a!==e)return 'none';"
	"try{if(typeof e.value==='string'&&e.setSelectionRange){var n=e.value.length;e.setSelectionRange(n,n);}}catch(x){}"
	"return 'ok';})()";

/* --- synchronous JS (nested main loop) for the hint round-trips --- */
typedef struct { GMainLoop *loop; GsurfView *view; gchar *result; } JsCtx;

static void
js_done(GObject *src, GAsyncResult *res, gpointer data)
{
	JsCtx *c = data;
	c->result = gsurf_view_run_javascript_finish(c->view, res, NULL);
	g_main_loop_quit(c->loop);
}

static gchar *
run_js_sync(GsurfView *view, const gchar *script)
{
	JsCtx c = { 0 };
	c.view = view;
	c.loop = g_main_loop_new(NULL, FALSE);
	gsurf_view_run_javascript_async(view, script, NULL, js_done, &c);
	g_main_loop_run(c.loop);
	g_main_loop_unref(c.loop);
	return c.result;
}

/* Build the JS that renders hints and installs window.__gsurf_press /
 * __gsurf_clear. Returns the number of hints rendered. Fixed-length chord
 * labels (length grows with the link count) keep them prefix-free. */
static gchar *
build_show_js(GsurfModalModule *self, gboolean newview)
{
	g_autofree gchar *chars_q = g_strdup_printf("\"%s\"", self->hint_chars);
	g_autofree gchar *bg_q = g_strdup_printf("\"%s\"", self->hint_bg);
	g_autofree gchar *fg_q = g_strdup_printf("\"%s\"", self->hint_fg);

	return g_strdup_printf(
		"(function(){"
		"var A=%s.split('');var BG=%s;var FG=%s;window.__gsurfNV=%s;"
		"function clr(){if(window.__gh){window.__gh.forEach(function(o){"
		"if(o.t&&o.t.parentNode)o.t.parentNode.removeChild(o.t);});}window.__gh=null;window.__gb='';}"
		"function vis(e){var r=e.getBoundingClientRect();return r.width>0&&r.height>0"
		"&&r.bottom>0&&r.right>0&&r.top<innerHeight&&r.left<innerWidth&&e.offsetParent!==null;}"
		"function gen(n){var L=1,c=A.length;while(c<n){L++;c*=A.length;}var o=[];"
		"for(var i=0;i<n;i++){var s='',x=i,j;for(j=0;j<L;j++){s=A[x%%A.length]+s;x=Math.floor(x/A.length);}o.push(s);}return o;}"
		"window.__gsurf_clear=clr;"
		"window.__gsurf_press=function(ch){var a=window.__gh;if(!a)return 'none';"
		"var b=(ch==='BackSpace')?window.__gb.slice(0,-1):(window.__gb+ch.toLowerCase());window.__gb=b;"
		"var m=a.filter(function(o){return o.l.indexOf(b)===0;});"
		"a.forEach(function(o){o.t.style.opacity=(o.l.indexOf(b)===0)?'1':'0.15';});"
		"if(m.length===0){clr();return 'none';}"
		"if(m.length===1){var e=m[0].e;var h=(e.tagName==='A'&&e.href)?e.href:null;clr();"
		"if(window.__gsurfNV&&h)return 'open:'+h;try{e.focus();}catch(x){}e.click();return 'click';}"
		"return 'more';};"
		"clr();"
		"var sel='a[href],button,input:not([type=hidden]),textarea,select,[onclick],[role=button],[tabindex]';"
		"var els=[].slice.call(document.querySelectorAll(sel)).filter(vis);"
		"var L=gen(els.length);var ar=[];"
		"for(var i=0;i<els.length;i++){var e=els[i],r=e.getBoundingClientRect();"
		"var t=document.createElement('div');t.textContent=L[i];"
		"t.style.cssText='position:fixed;z-index:2147483647;left:'+Math.max(0,r.left)+'px;top:'+Math.max(0,r.top)+'px;"
		"background:'+BG+';color:'+FG+';font:bold %dpx monospace;padding:0 3px;border-radius:3px;box-shadow:0 1px 2px rgba(0,0,0,.5);';"
		"document.documentElement.appendChild(t);ar.push({l:L[i],e:e,t:t});}"
		"window.__gh=ar;window.__gb='';return ar.length;})()",
		chars_q, bg_q, fg_q, newview ? "true" : "false", self->hint_font_size);
}

/* Open @uri in a new active view whose tab-tree parent is @opener. */
static void
open_in_new_view(GsurfView *opener, const gchar *uri)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfWindow *window = gsurf_module_manager_get_active_window(mgr);
	GsurfConfig *config = gsurf_config_get_default();
	GsurfView *view;

	if (window == NULL)
		return;
	view = gsurf_view_new_full(opener, NULL, uri, FALSE);
	if (view == NULL)
		return;
	if (config != NULL && config->settings != NULL)
		gsurf_view_apply_settings(view, config->settings);
	gsurf_window_add_view(window, view);
	gsurf_window_set_active_view(window, view);
	gsurf_view_load_uri(view, uri);
	g_object_unref(view);
}

/* Start a hint session; returns TRUE if hints were rendered. */
static gboolean
start_hints(GsurfModalModule *self, GsurfView *view, gboolean newview)
{
	g_autofree gchar *js = build_show_js(self, newview);
	g_autofree gchar *res = run_js_sync(view, js);

	if (res != NULL && atoi(res) > 0) {
		set_mode(self, GSURF_MODE_FOLLOW);
		self->hint_newview = newview;
		watch_hints(self, view);
		return TRUE;
	}
	return FALSE;
}

/* Forward a keystroke to the page-side hint matcher and act on the result. */
static void
follow_press(GsurfModalModule *self, GsurfView *view, const gchar *name)
{
	g_autofree gchar *press = g_strdup_printf("window.__gsurf_press(\"%s\")", name);
	g_autofree gchar *res = run_js_sync(view, press);

	if (res == NULL) {
		set_mode(self, GSURF_MODE_NORMAL);
		return;
	}
	if (g_str_has_prefix(res, "open:")) {
		open_in_new_view(view, res + 5);
		set_mode(self, GSURF_MODE_NORMAL);
	} else if (g_strcmp0(res, "more") != 0) {
		/* "click" or "none": session is over. */
		set_mode(self, GSURF_MODE_NORMAL);
	}
}

/* A bare modifier keypress (Shift/Ctrl/Alt/Super/...) arrives as its own
 * key event. It is never a modal command and must not disturb a pending
 * chord — e.g. typing `gT` is g, then Shift-down, then t, and the Shift
 * event would otherwise clear the pending `g`. */
static gboolean
is_modifier_name(const gchar *name)
{
	return g_str_has_prefix(name, "Shift") ||
	       g_str_has_prefix(name, "Control") ||
	       g_str_has_prefix(name, "Alt") ||
	       g_str_has_prefix(name, "Meta") ||
	       g_str_has_prefix(name, "Super") ||
	       g_str_has_prefix(name, "Hyper") ||
	       g_strcmp0(name, "Caps_Lock") == 0 ||
	       g_strcmp0(name, "ISO_Level3_Shift") == 0;
}

/* Rotate the active view in the active window by @delta (vim gt/gT). Works
 * directly on the window's view list, so it does not depend on the tabs
 * module being loaded. No-op with fewer than two views. */
static void
modal_switch_view(gint delta)
{
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());
	GsurfView *active;
	gint n, cur = -1, i, next;

	if (window == NULL)
		return;
	n = (gint)gsurf_window_get_n_views(window);
	if (n <= 1)
		return;

	active = gsurf_window_get_active_view(window);
	for (i = 0; i < n; i++)
		if (gsurf_window_get_nth_view(window, i) == active) { cur = i; break; }
	if (cur < 0)
		return;

	next = ((cur + delta) % n + n) % n;
	gsurf_window_set_active_view(window, gsurf_window_get_nth_view(window, next));
}

static gboolean
gsurf_modal_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                             guint keyval, guint keycode, guint state,
                             GsurfModePolicy mode_hint)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(handler);
	/* Bare name with Shift folded into a letter's case: Shift+g is "G",
	 * while a Caps Lock "G" stays "g" (no accidental back/forward). */
	g_autofree gchar *name = gsurf_keys_to_name(keyval, state);
	gboolean has_mod = (state & (GSURF_MOD_CTRL | GSURF_MOD_ALT | GSURF_MOD_SUPER)) != 0;
	g_autofree gchar *js = NULL;

	if (name == NULL || view == NULL)
		return FALSE;

	/* Stay transparent to bare modifier presses so they neither act as
	 * commands nor reset a pending chord (keeps `gT`, `gg`, etc. intact). */
	if (is_modifier_name(name))
		return FALSE;

	/* FOLLOW (link-hint) mode owns all keys until a match or cancel. */
	if (self->mode == GSURF_MODE_FOLLOW) {
		if (g_strcmp0(name, "Escape") == 0) {
			g_free(run_js_sync(view, CLEAR_HINTS_JS));
			set_mode(self, GSURF_MODE_NORMAL);
			return TRUE;
		}
		if (g_strcmp0(name, "BackSpace") == 0 ||
		    (strlen(name) == 1 && g_ascii_isalpha(name[0])))
			follow_press(self, view, name);
		return TRUE; /* consume everything while hinting */
	}

	/* INSERT mode: only Escape is ours; everything else goes to the page.
	 * (With passthrough published, the host already suppresses bare-key
	 * dispatch to all modules; this handles Escape and is a safety net.) */
	if (self->mode == GSURF_MODE_INSERT) {
		if (g_strcmp0(name, "Escape") == 0 && !has_mod) {
			leave_to_normal(self, view);
			return TRUE;
		}
		return FALSE;
	}

	/* NORMAL mode: let modified chords (Ctrl+...) fall through to core. */
	if (has_mod)
		return FALSE;

	/* Focus-aware passthrough: when the page has an editable element
	 * focused (text input, textarea, contenteditable, select), let keys
	 * reach it so the user can type without entering INSERT mode. Escape
	 * blurs the field and returns to command context. This mirrors the
	 * way vimium/qutebrowser behave. */
	if (gsurf_view_get_editing(view)) {
		if (g_strcmp0(name, "Escape") == 0) {
			gsurf_view_blur_editable(view);
			return TRUE;
		}
		return FALSE;
	}

	/* Chords beginning with 'g': gg -> top, gt -> next tab, gT -> prev
	 * tab, gi -> first text field + INSERT, gf -> read the tab full-frame
	 * in --lrg=3d (toggle). */
	if (self->pending_g) {
		self->pending_g = FALSE;
		if (g_strcmp0(name, "g") == 0) {
			scroll(view, "window.scrollTo(0,0)");
			return TRUE;
		}
		if (g_strcmp0(name, "i") == 0) {
			g_autofree gchar *res = run_js_sync(view, FOCUS_FIRST_INPUT_JS);

			/* The field has the focus now (the script checked), but the
			 * page reports it asynchronously: record it at once, so the
			 * very next key is text and the indicator reads INSERT. It
			 * stays the field's until it loses the focus (Escape, a
			 * click elsewhere, a submit that loads the next page), as
			 * any focused field: no sticky INSERT to leave by hand. */
			if (g_strcmp0(res, "ok") == 0)
				gsurf_view_set_editing(view, TRUE);
			return TRUE;
		}
		if (g_strcmp0(name, "t") == 0) {
			modal_switch_view(+1);
			return TRUE;
		}
		if (g_strcmp0(name, "T") == 0) {
			modal_switch_view(-1);
			return TRUE;
		}
		/* gf: the 3D reading view (fly head-on, full-frame, then the flat
		 * pixel-exact page; again to fly back).  Outside a 3D window
		 * nothing performs the action, so 'f' falls through to hints. */
		if (g_strcmp0(name, "f") == 0
		    && gsurf_module_manager_execute_action(
		           gsurf_module_manager_get_default(), NULL, view,
		           GSURF_ACTION_LRG_READING, NULL))
			return TRUE;
		/* fall through: 'g' then other key — treat the new key normally */
	}

	/* Link hinting. */
	if (self->hint_key != NULL && g_strcmp0(name, self->hint_key) == 0) {
		start_hints(self, view, FALSE);
		return TRUE;
	}
	if (self->hint_key_newview != NULL && g_strcmp0(name, self->hint_key_newview) == 0) {
		start_hints(self, view, TRUE);
		return TRUE;
	}

	if (is_insert_key(self, name)) { set_mode(self, GSURF_MODE_INSERT); return TRUE; }
	/* Escape cancels a pending `g` chord; otherwise it falls through to the
	 * core keymap (Escape: stop). */
	if (g_strcmp0(name, "Escape") == 0) {
		gboolean had_chord = self->pending_g;

		self->pending_g = FALSE;
		return had_chord;
	}
	if (g_strcmp0(name, "g") == 0) { self->pending_g = TRUE; return TRUE; }
	if (g_strcmp0(name, "G") == 0) { scroll(view, "window.scrollTo(0,document.body.scrollHeight)"); return TRUE; }
	if (g_strcmp0(name, "H") == 0) { gsurf_view_go_back(view); return TRUE; }
	if (g_strcmp0(name, "L") == 0) { gsurf_view_go_forward(view); return TRUE; }
	if (g_strcmp0(name, "r") == 0) { gsurf_view_reload(view, FALSE); return TRUE; }

	if (g_strcmp0(name, "j") == 0) js = g_strdup_printf("window.scrollBy(0,%d)", self->scroll_step);
	else if (g_strcmp0(name, "k") == 0) js = g_strdup_printf("window.scrollBy(0,-%d)", self->scroll_step);
	else if (g_strcmp0(name, "h") == 0) js = g_strdup_printf("window.scrollBy(-%d,0)", self->scroll_step);
	else if (g_strcmp0(name, "l") == 0) js = g_strdup_printf("window.scrollBy(%d,0)", self->scroll_step);
	else if (g_strcmp0(name, "d") == 0) js = g_strdup("window.scrollBy(0,window.innerHeight/2)");
	else if (g_strcmp0(name, "u") == 0) js = g_strdup("window.scrollBy(0,-window.innerHeight/2)");

	if (js != NULL) {
		scroll(view, js);
		return TRUE;
	}

	return FALSE;
}

/* A mouse button (or wheel notch) ends a FOLLOW session: the user chose
 * to click or scroll instead, and the labels would no longer match the
 * page. The press itself goes on to the page. */
static gboolean
gsurf_modal_handle_mouse_event(GsurfInputHandler *handler, GsurfView *view,
                               guint button, guint state)
{
	(void)view;
	(void)button;
	(void)state;
	cancel_follow(GSURF_MODAL_MODULE(handler), TRUE);
	return FALSE;
}

static void
gsurf_modal_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_modal_handle_key_event;
	iface->handle_mouse_event = gsurf_modal_handle_mouse_event;
}

/* Core keybind actions this module owns: hinting and mode switches. */
static gboolean
gsurf_modal_handle_action(GsurfActionHandler *handler, GsurfView *view,
                          GsurfAction action)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(handler);

	if (view == NULL)
		return FALSE;

	switch (action) {
	case GSURF_ACTION_FOLLOW_HINTS:
		start_hints(self, view, FALSE);
		return TRUE;
	case GSURF_ACTION_FOLLOW_HINTS_NEW_VIEW:
		start_hints(self, view, TRUE);
		return TRUE;
	case GSURF_ACTION_ENTER_INSERT_MODE:
		set_mode(self, GSURF_MODE_INSERT);
		return TRUE;
	case GSURF_ACTION_ENTER_NORMAL_MODE:
		if (self->mode == GSURF_MODE_FOLLOW) {
			g_free(run_js_sync(view, CLEAR_HINTS_JS));
			set_mode(self, GSURF_MODE_NORMAL);
		} else {
			leave_to_normal(self, view);
		}
		return TRUE;
	default:
		return FALSE;
	}
}

static void
gsurf_modal_action_init(GsurfActionHandlerInterface *iface)
{
	iface->handle_action = gsurf_modal_handle_action;
}

static void
gsurf_modal_list_keybinds(GsurfKeybindProvider *provider, GPtrArray *entries)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(provider);

	gsurf_keybind_help_append(entries, self->hint_key,
		"Follow link hints", "modal", "follow-hints");
	gsurf_keybind_help_append(entries, self->hint_key_newview,
		"Follow a hint in a new view", "modal", "follow-hints-new-view");
	if (self->insert_keys != NULL) {
		gchar **k;

		for (k = self->insert_keys; *k != NULL; k++)
			gsurf_keybind_help_append(entries, *k,
				"Enter insert mode", "modal", "enter-insert-mode");
	}
	gsurf_keybind_help_append(entries, "gi",
		"Focus the first text field (type until it loses focus)", "modal", "focus-input");
	gsurf_keybind_help_append(entries, "Escape",
		"Return to normal mode and leave the field", "modal", "enter-normal-mode");
	gsurf_keybind_help_append(entries, "h", "Scroll left", "modal", "scroll-left");
	gsurf_keybind_help_append(entries, "j", "Scroll down", "modal", "scroll-down");
	gsurf_keybind_help_append(entries, "k", "Scroll up", "modal", "scroll-up");
	gsurf_keybind_help_append(entries, "l", "Scroll right", "modal", "scroll-right");
	gsurf_keybind_help_append(entries, "d", "Half-page down", "modal", "half-page-down");
	gsurf_keybind_help_append(entries, "u", "Half-page up", "modal", "half-page-up");
	gsurf_keybind_help_append(entries, "gg", "Scroll to the top", "modal", "scroll-top");
	gsurf_keybind_help_append(entries, "G", "Scroll to the bottom", "modal", "scroll-bottom");
	gsurf_keybind_help_append(entries, "H", "Go back", "modal", "back");
	gsurf_keybind_help_append(entries, "L", "Go forward", "modal", "forward");
	gsurf_keybind_help_append(entries, "r", "Reload the page", "modal", "reload");
	gsurf_keybind_help_append(entries, "gt", "Next tab", "modal", "tab-next");
	gsurf_keybind_help_append(entries, "gT", "Previous tab", "modal", "tab-prev");
	gsurf_keybind_help_append(entries, "gf",
		"3D: read the tab full-frame (again: back out)", "modal", "lrg-reading");
}

static void
gsurf_modal_keybind_init(GsurfKeybindProviderInterface *iface)
{
	iface->list_keybinds = gsurf_modal_list_keybinds;
}

static const gchar *gsurf_modal_get_name(GsurfModule *m) { return "modal"; }
static const gchar *gsurf_modal_get_description(GsurfModule *m)
{
	return "Vim-style modal navigation with link hinting";
}

static gboolean
gsurf_modal_activate(GsurfModule *module)
{
	/* Run before other input handlers so modal commands win. */
	gsurf_module_set_priority(module, GSURF_PRIORITY_HIGHEST);
	return TRUE;
}

/* Disabled: no module owns the modes any more, so the published mode
 * goes back to NORMAL (and passthrough off). */
static void
gsurf_modal_deactivate(GsurfModule *module)
{
	set_mode(GSURF_MODAL_MODULE(module), GSURF_MODE_NORMAL);
}

static void
dup_member(gchar **dst, YamlMapping *m, const gchar *key)
{
	if (yaml_mapping_has_member(m, key)) {
		g_free(*dst);
		*dst = g_strdup(yaml_mapping_get_string_member(m, key));
	}
}

/* modules.modal.insert_keys: a list of bare key names (or one name).
 * An empty list disables them (the enter-insert-mode action remains);
 * anything that is not a string is reported and the default kept. */
static void
configure_insert_keys(GsurfModalModule *self, YamlNode *node)
{
	g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
	YamlSequence *seq;
	guint i;

	if (node == NULL)
		return;
	if (yaml_node_get_node_type(node) == YAML_NODE_SCALAR) {
		const gchar *one = yaml_node_get_string(node);

		if (one != NULL && *one != '\0')
			g_strv_builder_add(builder, one);
	} else if (yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE) {
		seq = yaml_node_get_sequence(node);
		for (i = 0; i < yaml_sequence_get_length(seq); i++) {
			YamlNode *e = yaml_sequence_get_element(seq, i);
			const gchar *key = (e != NULL && yaml_node_get_node_type(e) == YAML_NODE_SCALAR)
				? yaml_node_get_string(e) : NULL;

			if (key == NULL || *key == '\0') {
				g_warning("modal.insert_keys: entry %u is not a key name; keeping %s",
					i, "the previous keys");
				return;
			}
			g_strv_builder_add(builder, key);
		}
	} else {
		g_warning("modal.insert_keys must be a list of key names; keeping the previous keys");
		return;
	}
	g_strfreev(self->insert_keys);
	self->insert_keys = g_strv_builder_end(builder);
}

static void
gsurf_modal_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	const gchar *start;
	const gchar *font_size_text;
	gint64 font_size;

	node = gsurf_config_get_module_node(config, "modal");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "scroll_step"))
		self->scroll_step = (gint)yaml_mapping_get_int_member(m, "scroll_step");
	if (yaml_mapping_has_member(m, "start_mode")) {
		start = yaml_mapping_get_string_member(m, "start_mode");
		set_mode(self, g_strcmp0(start, "insert") == 0
			? GSURF_MODE_INSERT : GSURF_MODE_NORMAL);
	}
	if (yaml_mapping_has_member(m, "insert_keys"))
		configure_insert_keys(self, yaml_mapping_get_member(m, "insert_keys"));
	dup_member(&self->hint_key, m, "hint_key");
	dup_member(&self->hint_key_newview, m, "hint_key_newview");
	dup_member(&self->hint_chars, m, "hint_chars");
	dup_member(&self->hint_bg, m, "hint_bg");
	dup_member(&self->hint_fg, m, "hint_fg");
	/* Reject unusable sizes before narrowing to the CSS integer value. */
	if (yaml_mapping_has_member(m, "hint_font_size")) {
		font_size_text = yaml_mapping_get_string_member(m, "hint_font_size");
		if (font_size_text != NULL &&
		    g_ascii_string_to_signed(font_size_text, 10, 1, G_MAXINT, &font_size, NULL))
			self->hint_font_size = (gint)font_size;
		else
			g_warning("modal.hint_font_size must be a positive integer <= %d; keeping %dpx",
			          G_MAXINT, self->hint_font_size);
	}
}

static void
gsurf_modal_module_finalize(GObject *object)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(object);

	g_clear_pointer(&self->insert_keys, g_strfreev);
	g_clear_pointer(&self->hint_key, g_free);
	g_clear_pointer(&self->hint_key_newview, g_free);
	g_clear_pointer(&self->hint_chars, g_free);
	g_clear_pointer(&self->hint_bg, g_free);
	g_clear_pointer(&self->hint_fg, g_free);
	unwatch_hints(self);

	G_OBJECT_CLASS(gsurf_modal_module_parent_class)->finalize(object);
}

static void
gsurf_modal_module_class_init(GsurfModalModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_modal_module_finalize;
	module_class->activate = gsurf_modal_activate;
	module_class->deactivate = gsurf_modal_deactivate;
	module_class->get_name = gsurf_modal_get_name;
	module_class->get_description = gsurf_modal_get_description;
	module_class->configure = gsurf_modal_configure;
}

static void
gsurf_modal_module_init(GsurfModalModule *self)
{
	self->mode = GSURF_MODE_NORMAL;
	self->pending_g = FALSE;
	self->scroll_step = 40;
	{
		const gchar *defaults[] = { "i", "a", NULL };

		self->insert_keys = g_strdupv((gchar **)defaults);
	}
	self->hint_key = g_strdup("f");
	self->hint_key_newview = g_strdup("F");
	self->hint_chars = g_strdup("asdfghjkl");
	self->hint_bg = g_strdup("#ffd700");
	self->hint_fg = g_strdup("#000000");
	self->hint_font_size = 11;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_MODAL_MODULE;
}
