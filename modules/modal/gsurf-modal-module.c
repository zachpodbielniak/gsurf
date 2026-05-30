/*
 * gsurf-modal-module.c - Vim-style modal navigation + link hinting
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler at high priority and tracks its own mode.
 * NORMAL mode: bare keys are navigation commands (hjkl, gg, G, d/u, H/L,
 * r); `i` enters INSERT (keys pass to the page); Escape returns to NORMAL.
 * `f` enters FOLLOW mode: vimium-style link hints are overlaid on every
 * clickable element with short chord labels; typing a chord narrows the
 * set and a unique match is clicked. `F` opens the match in a new view.
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
	gchar *key_insert;
	gchar *key_normal;
	gchar *hint_key;          /* default "f" */
	gchar *hint_key_newview;  /* default "F" */
	gchar *hint_chars;        /* alphabet, e.g. "asdfghjkl" */
	gchar *hint_bg;
	gchar *hint_fg;
};

static void gsurf_modal_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfModalModule, gsurf_modal_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_modal_input_init))

static void
scroll(GsurfView *view, const gchar *js)
{
	gsurf_view_run_javascript_async(view, js, NULL, NULL, NULL);
}

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
		"background:'+BG+';color:'+FG+';font:bold 11px monospace;padding:0 3px;border-radius:3px;box-shadow:0 1px 2px rgba(0,0,0,.5);';"
		"document.documentElement.appendChild(t);ar.push({l:L[i],e:e,t:t});}"
		"window.__gh=ar;window.__gb='';return ar.length;})()",
		chars_q, bg_q, fg_q, newview ? "true" : "false");
}

static void
open_in_new_view(const gchar *uri)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfWindow *window = gsurf_module_manager_get_active_window(mgr);
	GsurfConfig *config = gsurf_config_get_default();
	GsurfView *view;

	if (window == NULL)
		return;
	view = gsurf_view_new();
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
		self->mode = GSURF_MODE_FOLLOW;
		self->hint_newview = newview;
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
		self->mode = GSURF_MODE_NORMAL;
		return;
	}
	if (g_str_has_prefix(res, "open:")) {
		open_in_new_view(res + 5);
		self->mode = GSURF_MODE_NORMAL;
	} else if (g_strcmp0(res, "more") != 0) {
		/* "click" or "none": session is over. */
		self->mode = GSURF_MODE_NORMAL;
	}
}

static gboolean
gsurf_modal_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                             guint keyval, guint keycode, guint state,
                             GsurfModePolicy mode_hint)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(handler);
	g_autofree gchar *name = gsurf_keys_to_string(keyval, 0); /* bare name */
	gboolean has_mod = (state & (GSURF_MOD_CTRL | GSURF_MOD_ALT | GSURF_MOD_SUPER)) != 0;
	g_autofree gchar *js = NULL;

	if (name == NULL || view == NULL)
		return FALSE;

	/* FOLLOW (link-hint) mode owns all keys until a match or cancel. */
	if (self->mode == GSURF_MODE_FOLLOW) {
		if (g_strcmp0(name, "Escape") == 0) {
			run_js_sync(view, "window.__gsurf_clear&&window.__gsurf_clear()");
			self->mode = GSURF_MODE_NORMAL;
			return TRUE;
		}
		if (g_strcmp0(name, "BackSpace") == 0 ||
		    (strlen(name) == 1 && g_ascii_isalpha(name[0])))
			follow_press(self, view, name);
		return TRUE; /* consume everything while hinting */
	}

	/* INSERT mode: only Escape is ours; everything else goes to the page. */
	if (self->mode == GSURF_MODE_INSERT) {
		if (g_strcmp0(name, "Escape") == 0) {
			self->mode = GSURF_MODE_NORMAL;
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
			gsurf_view_run_javascript_async(view,
				"document.activeElement&&document.activeElement.blur&&document.activeElement.blur()",
				NULL, NULL, NULL);
			return TRUE;
		}
		return FALSE;
	}

	/* Chord: gg -> top. */
	if (self->pending_g) {
		self->pending_g = FALSE;
		if (g_strcmp0(name, "g") == 0) {
			scroll(view, "window.scrollTo(0,0)");
			return TRUE;
		}
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

	if (g_strcmp0(name, "i") == 0) { self->mode = GSURF_MODE_INSERT; return TRUE; }
	if (g_strcmp0(name, "Escape") == 0) { self->pending_g = FALSE; return TRUE; }
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

static void
gsurf_modal_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_modal_handle_key_event;
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

static void
dup_member(gchar **dst, YamlMapping *m, const gchar *key)
{
	if (yaml_mapping_has_member(m, key)) {
		g_free(*dst);
		*dst = g_strdup(yaml_mapping_get_string_member(m, key));
	}
}

static void
gsurf_modal_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	const gchar *start;

	node = gsurf_config_get_module_node(config, "modal");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "scroll_step"))
		self->scroll_step = (gint)yaml_mapping_get_int_member(m, "scroll_step");
	if (yaml_mapping_has_member(m, "start_mode")) {
		start = yaml_mapping_get_string_member(m, "start_mode");
		self->mode = g_strcmp0(start, "insert") == 0
			? GSURF_MODE_INSERT : GSURF_MODE_NORMAL;
	}
	dup_member(&self->hint_key, m, "hint_key");
	dup_member(&self->hint_key_newview, m, "hint_key_newview");
	dup_member(&self->hint_chars, m, "hint_chars");
	dup_member(&self->hint_bg, m, "hint_bg");
	dup_member(&self->hint_fg, m, "hint_fg");
}

static void
gsurf_modal_module_finalize(GObject *object)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(object);

	g_clear_pointer(&self->key_insert, g_free);
	g_clear_pointer(&self->key_normal, g_free);
	g_clear_pointer(&self->hint_key, g_free);
	g_clear_pointer(&self->hint_key_newview, g_free);
	g_clear_pointer(&self->hint_chars, g_free);
	g_clear_pointer(&self->hint_bg, g_free);
	g_clear_pointer(&self->hint_fg, g_free);

	G_OBJECT_CLASS(gsurf_modal_module_parent_class)->finalize(object);
}

static void
gsurf_modal_module_class_init(GsurfModalModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_modal_module_finalize;
	module_class->activate = gsurf_modal_activate;
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
	self->key_insert = g_strdup("i");
	self->key_normal = g_strdup("Escape");
	self->hint_key = g_strdup("f");
	self->hint_key_newview = g_strdup("F");
	self->hint_chars = g_strdup("asdfghjkl");
	self->hint_bg = g_strdup("#ffd700");
	self->hint_fg = g_strdup("#000000");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_MODAL_MODULE;
}
