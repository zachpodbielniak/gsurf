/*
 * gsurf-webkit6-view.c - WebKitGTK 6.0 / GTK4 web view backend
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GTK4 port of the WebKit2GTK view. WebKitGTK 6.0 replaces
 * WebKitWebContext with WebKitNetworkSession for cookies, downloads,
 * TLS, and proxy; this backend uses that API. Built only when
 * GTK_BACKEND=gtk4 (requires webkitgtk-6.0 to compile/verify).
 */

#include "backend/gtk4/gsurf-webkit6-view.h"
#include "module/gsurf-module-manager.h"

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <string.h>

struct _GsurfWebkit6View
{
	GsurfView parent_instance;
	WebKitWebView *webview;
};

G_DEFINE_FINAL_TYPE(GsurfWebkit6View, gsurf_webkit6_view, GSURF_TYPE_VIEW)

/* --- native signal translation --- */

static void
on_load_changed(WebKitWebView *wv, WebKitLoadEvent event, gpointer user_data)
{
	GsurfWebkit6View *self = user_data;

	if (event == WEBKIT_LOAD_STARTED) {
		GsurfConfig *cfg = gsurf_config_get_default();
		if (cfg != NULL && cfg->settings != NULL) {
			GsurfSettings *s = gsurf_settings_copy(cfg->settings);
			gsurf_module_manager_dispatch_apply_settings(
				gsurf_module_manager_get_default(), GSURF_VIEW(self),
				s, webkit_web_view_get_uri(wv));
			gsurf_view_apply_settings(GSURF_VIEW(self), s);
			g_object_unref(s);
		}
	}
	gsurf_view_emit_load_changed(GSURF_VIEW(self), (GsurfLoadEvent)event);
}

static void
on_notify_title(WebKitWebView *wv, GParamSpec *p, gpointer user_data)
{
	gsurf_view_emit_title_changed(GSURF_VIEW(user_data), webkit_web_view_get_title(wv));
}

static void
on_notify_uri(WebKitWebView *wv, GParamSpec *p, gpointer user_data)
{
	gsurf_view_emit_uri_changed(GSURF_VIEW(user_data), webkit_web_view_get_uri(wv));
}

static void
on_notify_progress(WebKitWebView *wv, GParamSpec *p, gpointer user_data)
{
	gsurf_view_emit_progress_changed(GSURF_VIEW(user_data),
		webkit_web_view_get_estimated_load_progress(wv));
}

static void
on_web_process_terminated(WebKitWebView *wv, WebKitWebProcessTerminationReason r, gpointer user_data)
{
	gsurf_view_emit_web_process_terminated(GSURF_VIEW(user_data));
}

static gboolean
on_decide_policy(WebKitWebView *wv, WebKitPolicyDecision *decision,
                 WebKitPolicyDecisionType type, gpointer user_data)
{
	GsurfWebkit6View *self = user_data;
	WebKitNavigationAction *action;
	const gchar *uri;

	if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
	    type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
		return FALSE;

	action = webkit_navigation_policy_decision_get_navigation_action(
		WEBKIT_NAVIGATION_POLICY_DECISION(decision));
	uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));

	if (gsurf_module_manager_dispatch_before_navigate(
			gsurf_module_manager_get_default(), GSURF_VIEW(self), uri)
		== GSURF_POLICY_IGNORE) {
		webkit_policy_decision_ignore(decision);
		return TRUE;
	}
	return FALSE;
}

static gboolean
on_permission_request(WebKitWebView *wv, WebKitPermissionRequest *req, gpointer user_data)
{
	GsurfWebkit6View *self = user_data;
	const gchar *type = "other";
	GsurfPermissionVerdict v;

	if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req))
		type = "geolocation";
	else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req))
		type = "notification";

	v = gsurf_module_manager_dispatch_permission(gsurf_module_manager_get_default(),
		GSURF_VIEW(self), type, NULL);
	if (v == GSURF_PERMISSION_ALLOW) { webkit_permission_request_allow(req); return TRUE; }
	if (v == GSURF_PERMISSION_DENY) { webkit_permission_request_deny(req); return TRUE; }
	return FALSE;
}

/* --- vfuncs --- */

static void gsurf_webkit6_view_load_uri(GsurfView *v, const gchar *uri)
{ webkit_web_view_load_uri(GSURF_WEBKIT6_VIEW(v)->webview, uri); }

static void gsurf_webkit6_view_load_html(GsurfView *v, const gchar *html, const gchar *base)
{ webkit_web_view_load_html(GSURF_WEBKIT6_VIEW(v)->webview, html, base); }

static void gsurf_webkit6_view_reload(GsurfView *v, gboolean bypass)
{
	WebKitWebView *wv = GSURF_WEBKIT6_VIEW(v)->webview;
	if (bypass) webkit_web_view_reload_bypass_cache(wv); else webkit_web_view_reload(wv);
}

static void gsurf_webkit6_view_stop_loading(GsurfView *v)
{ webkit_web_view_stop_loading(GSURF_WEBKIT6_VIEW(v)->webview); }

static void gsurf_webkit6_view_go_back(GsurfView *v)
{ webkit_web_view_go_back(GSURF_WEBKIT6_VIEW(v)->webview); }

static void gsurf_webkit6_view_go_forward(GsurfView *v)
{ webkit_web_view_go_forward(GSURF_WEBKIT6_VIEW(v)->webview); }

static gboolean gsurf_webkit6_view_can_go_back(GsurfView *v)
{ return webkit_web_view_can_go_back(GSURF_WEBKIT6_VIEW(v)->webview); }

static gboolean gsurf_webkit6_view_can_go_forward(GsurfView *v)
{ return webkit_web_view_can_go_forward(GSURF_WEBKIT6_VIEW(v)->webview); }

static const gchar *gsurf_webkit6_view_get_uri(GsurfView *v)
{ return webkit_web_view_get_uri(GSURF_WEBKIT6_VIEW(v)->webview); }

static const gchar *gsurf_webkit6_view_get_title(GsurfView *v)
{ return webkit_web_view_get_title(GSURF_WEBKIT6_VIEW(v)->webview); }

static gdouble gsurf_webkit6_view_get_progress(GsurfView *v)
{ return webkit_web_view_get_estimated_load_progress(GSURF_WEBKIT6_VIEW(v)->webview); }

static void gsurf_webkit6_view_set_zoom(GsurfView *v, gdouble z)
{ webkit_web_view_set_zoom_level(GSURF_WEBKIT6_VIEW(v)->webview, z); }

static gdouble gsurf_webkit6_view_get_zoom(GsurfView *v)
{ return webkit_web_view_get_zoom_level(GSURF_WEBKIT6_VIEW(v)->webview); }

static void
gsurf_webkit6_view_apply_settings(GsurfView *v, GsurfSettings *s)
{
	WebKitSettings *ws = webkit_web_view_get_settings(GSURF_WEBKIT6_VIEW(v)->webview);

	webkit_settings_set_enable_javascript(ws, s->javascript);
	webkit_settings_set_auto_load_images(ws, s->images);
	webkit_settings_set_enable_webgl(ws, s->webgl);
	webkit_settings_set_enable_webaudio(ws, s->webaudio);
	webkit_settings_set_enable_media_stream(ws, s->media_stream);
	webkit_settings_set_enable_smooth_scrolling(ws, s->smooth_scrolling);
	webkit_settings_set_enable_caret_browsing(ws, s->caret_browsing);
	webkit_settings_set_enable_site_specific_quirks(ws, s->site_quirks);
	webkit_settings_set_enable_developer_extras(ws, s->developer_extras);
	webkit_settings_set_javascript_can_open_windows_automatically(ws, s->js_can_open_windows);
	webkit_settings_set_default_font_size(ws, s->default_font_size);
	webkit_settings_set_default_monospace_font_size(ws, s->default_monospace_font_size);
	if (s->default_charset != NULL)
		webkit_settings_set_default_charset(ws, s->default_charset);
	if (s->user_agent != NULL && *s->user_agent != '\0')
		webkit_settings_set_user_agent(ws, s->user_agent);
}

static void
gsurf_webkit6_view_run_js_async(GsurfView *v, const gchar *script, GCancellable *c,
                                GAsyncReadyCallback cb, gpointer ud)
{ webkit_web_view_evaluate_javascript(GSURF_WEBKIT6_VIEW(v)->webview, script, -1, NULL, NULL, c, cb, ud); }

static gchar *
gsurf_webkit6_view_run_js_finish(GsurfView *v, GAsyncResult *res, GError **error)
{
	JSCValue *value = webkit_web_view_evaluate_javascript_finish(
		GSURF_WEBKIT6_VIEW(v)->webview, res, error);
	gchar *str;

	if (value == NULL)
		return NULL;
	str = jsc_value_is_string(value) ? jsc_value_to_string(value) : jsc_value_to_json(value, 0);
	g_object_unref(value);
	return str;
}

static gpointer gsurf_webkit6_view_get_native_widget(GsurfView *v)
{ return GSURF_WEBKIT6_VIEW(v)->webview; }

static void gsurf_webkit6_view_show_inspector(GsurfView *v)
{
	WebKitWebView *wv = GSURF_WEBKIT6_VIEW(v)->webview;
	WebKitWebInspector *insp;
	webkit_settings_set_enable_developer_extras(webkit_web_view_get_settings(wv), TRUE);
	insp = webkit_web_view_get_inspector(wv);
	if (insp != NULL)
		webkit_web_inspector_show(insp);
}

static void
gsurf_webkit6_view_add_user_script(GsurfView *v, const gchar *src, gboolean at_end)
{
	WebKitUserContentManager *ucm =
		webkit_web_view_get_user_content_manager(GSURF_WEBKIT6_VIEW(v)->webview);
	WebKitUserScript *s = webkit_user_script_new(src, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		at_end ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END
		       : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
	webkit_user_content_manager_add_script(ucm, s);
	webkit_user_script_unref(s);
}

static void
gsurf_webkit6_view_add_user_style(GsurfView *v, const gchar *css)
{
	WebKitUserContentManager *ucm =
		webkit_web_view_get_user_content_manager(GSURF_WEBKIT6_VIEW(v)->webview);
	WebKitUserStyleSheet *ss = webkit_user_style_sheet_new(css,
		WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
	webkit_user_content_manager_add_style_sheet(ucm, ss);
	webkit_user_style_sheet_unref(ss);
}

static void
gsurf_webkit6_view_clear_user_content(GsurfView *v)
{
	WebKitUserContentManager *ucm =
		webkit_web_view_get_user_content_manager(GSURF_WEBKIT6_VIEW(v)->webview);
	webkit_user_content_manager_remove_all_scripts(ucm);
	webkit_user_content_manager_remove_all_style_sheets(ucm);
}

static void
gsurf_webkit6_view_add_user_script_full(GsurfView *v, const gchar *source,
                                        gboolean at_end, const gchar *const *allow)
{
	WebKitUserContentManager *ucm =
		webkit_web_view_get_user_content_manager(GSURF_WEBKIT6_VIEW(v)->webview);
	WebKitUserScript *s = webkit_user_script_new(source,
		WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		at_end ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END
		       : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
		(const gchar * const *)allow, NULL);
	webkit_user_content_manager_add_script(ucm, s);
	webkit_user_script_unref(s);
}

static void
gsurf_webkit6_view_add_user_style_full(GsurfView *v, const gchar *css,
                                       const gchar *const *allow)
{
	WebKitUserContentManager *ucm =
		webkit_web_view_get_user_content_manager(GSURF_WEBKIT6_VIEW(v)->webview);
	WebKitUserStyleSheet *ss = webkit_user_style_sheet_new(css,
		WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER,
		(const gchar * const *)allow, NULL);
	webkit_user_content_manager_add_style_sheet(ucm, ss);
	webkit_user_style_sheet_unref(ss);
}

typedef struct { WebKitWebView *webview; gchar *id; } Gtk6FilterCtx;

static void
gtk6_on_filter_saved(GObject *source, GAsyncResult *res, gpointer data)
{
	Gtk6FilterCtx *ctx = data;
	WebKitUserContentFilter *filter;
	g_autoptr(GError) error = NULL;

	filter = webkit_user_content_filter_store_save_finish(
		WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &error);
	if (filter != NULL) {
		webkit_user_content_manager_add_filter(
			webkit_web_view_get_user_content_manager(ctx->webview), filter);
		webkit_user_content_filter_unref(filter);
	} else if (error != NULL) {
		g_warning("gsurf: content filter '%s': %s", ctx->id, error->message);
	}
	g_free(ctx->id);
	g_free(ctx);
}

static void
gsurf_webkit6_view_add_content_filter(GsurfView *v, const gchar *identifier,
                                      const gchar *json_rules)
{
	GsurfWebkit6View *self = GSURF_WEBKIT6_VIEW(v);
	g_autofree gchar *dir = g_build_filename(g_get_user_cache_dir(), "gsurf", "filters", NULL);
	WebKitUserContentFilterStore *store;
	GBytes *bytes;
	Gtk6FilterCtx *ctx;

	g_mkdir_with_parents(dir, 0755);
	store = webkit_user_content_filter_store_new(dir);
	bytes = g_bytes_new(json_rules, strlen(json_rules));

	ctx = g_new0(Gtk6FilterCtx, 1);
	ctx->webview = self->webview;
	ctx->id = g_strdup(identifier);
	webkit_user_content_filter_store_save(store, identifier, bytes, NULL,
		gtk6_on_filter_saved, ctx);

	g_bytes_unref(bytes);
	g_object_unref(store);
}

static void
gsurf_webkit6_view_find(GsurfView *v, const gchar *text, gboolean cs, gboolean fwd)
{
	WebKitFindController *fc = webkit_web_view_get_find_controller(GSURF_WEBKIT6_VIEW(v)->webview);
	guint32 opts = WEBKIT_FIND_OPTIONS_WRAP_AROUND;
	if (!cs) opts |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;
	if (!fwd) opts |= WEBKIT_FIND_OPTIONS_BACKWARDS;
	webkit_find_controller_search(fc, text, opts, G_MAXUINT);
}

static void gsurf_webkit6_view_find_next(GsurfView *v)
{ webkit_find_controller_search_next(webkit_web_view_get_find_controller(GSURF_WEBKIT6_VIEW(v)->webview)); }

static void gsurf_webkit6_view_find_previous(GsurfView *v)
{ webkit_find_controller_search_previous(webkit_web_view_get_find_controller(GSURF_WEBKIT6_VIEW(v)->webview)); }

static void
gsurf_webkit6_view_set_cookie_policy(GsurfView *v, gint policy)
{
	WebKitNetworkSession *ns = webkit_web_view_get_network_session(GSURF_WEBKIT6_VIEW(v)->webview);
	WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(ns);
	WebKitCookieAcceptPolicy p =
		(policy == 1) ? WEBKIT_COOKIE_POLICY_ACCEPT_NEVER :
		(policy == 2) ? WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY :
		                WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS;
	webkit_cookie_manager_set_accept_policy(cm, p);
}

static void
gsurf_webkit6_view_set_proxy(GsurfView *v, const gchar *uri)
{
	WebKitNetworkSession *ns = webkit_web_view_get_network_session(GSURF_WEBKIT6_VIEW(v)->webview);

	if (uri == NULL || *uri == '\0') {
		webkit_network_session_set_proxy_settings(ns, WEBKIT_NETWORK_PROXY_MODE_DEFAULT, NULL);
	} else {
		WebKitNetworkProxySettings *s = webkit_network_proxy_settings_new(uri, NULL);
		webkit_network_session_set_proxy_settings(ns, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, s);
		webkit_network_proxy_settings_free(s);
	}
}

/* --- lifecycle --- */

/* See the GTK3 backend: reports DOM edit-focus to gsurf_view_set_editing. */
static const gchar *FOCUS_TRACKER_JS =
	"(function(){if(window.__gsurfFT)return;window.__gsurfFT=1;"
	"function ed(e){if(!e)return false;if(e.isContentEditable)return true;"
	"var t=e.tagName;if(t==='TEXTAREA'||t==='SELECT')return true;"
	"if(t==='INPUT'){var ty=(e.getAttribute('type')||'text').toLowerCase();"
	"return !/^(button|submit|reset|checkbox|radio|file|image|range|color|hidden)$/.test(ty);}return false;}"
	"function snd(){try{window.webkit.messageHandlers.gsurfFocus.postMessage(ed(document.activeElement));}catch(x){}}"
	"document.addEventListener('focusin',snd,true);"
	"document.addEventListener('focusout',function(){setTimeout(snd,0);},true);snd();})()";

static void
on_focus_message(WebKitUserContentManager *ucm, WebKitJavascriptResult *result, gpointer user_data)
{
	GsurfWebkit6View *self = user_data;
	JSCValue *value = webkit_javascript_result_get_js_value(result);
	gsurf_view_set_editing(GSURF_VIEW(self), jsc_value_to_boolean(value));
}

static void
gsurf_webkit6_view_constructed(GObject *object)
{
	GsurfWebkit6View *self = GSURF_WEBKIT6_VIEW(object);
	WebKitUserContentManager *ucm;
	WebKitUserScript *tracker;
	GtkWidget *widget;

	G_OBJECT_CLASS(gsurf_webkit6_view_parent_class)->constructed(object);

	widget = webkit_web_view_new();
	self->webview = WEBKIT_WEB_VIEW(g_object_ref_sink(widget));

	ucm = webkit_web_view_get_user_content_manager(self->webview);
	webkit_user_content_manager_register_script_message_handler(ucm, "gsurfFocus", NULL);
	g_signal_connect(ucm, "script-message-received::gsurfFocus",
		G_CALLBACK(on_focus_message), self);
	tracker = webkit_user_script_new(FOCUS_TRACKER_JS,
		WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
	webkit_user_content_manager_add_script(ucm, tracker);
	webkit_user_script_unref(tracker);

	g_signal_connect(self->webview, "load-changed", G_CALLBACK(on_load_changed), self);
	g_signal_connect(self->webview, "notify::title", G_CALLBACK(on_notify_title), self);
	g_signal_connect(self->webview, "notify::uri", G_CALLBACK(on_notify_uri), self);
	g_signal_connect(self->webview, "notify::estimated-load-progress",
		G_CALLBACK(on_notify_progress), self);
	g_signal_connect(self->webview, "web-process-terminated",
		G_CALLBACK(on_web_process_terminated), self);
	g_signal_connect(self->webview, "decide-policy", G_CALLBACK(on_decide_policy), self);
	g_signal_connect(self->webview, "permission-request",
		G_CALLBACK(on_permission_request), self);

	gsurf_module_manager_dispatch_inject_scripts(
		gsurf_module_manager_get_default(), GSURF_VIEW(self), NULL);
}

static void
gsurf_webkit6_view_dispose(GObject *object)
{
	GsurfWebkit6View *self = GSURF_WEBKIT6_VIEW(object);

	if (self->webview != NULL) {
		g_signal_handlers_disconnect_by_data(self->webview, self);
		g_clear_object(&self->webview);
	}
	G_OBJECT_CLASS(gsurf_webkit6_view_parent_class)->dispose(object);
}

static void
gsurf_webkit6_view_class_init(GsurfWebkit6ViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfViewClass *vc = GSURF_VIEW_CLASS(klass);

	object_class->constructed = gsurf_webkit6_view_constructed;
	object_class->dispose = gsurf_webkit6_view_dispose;

	vc->load_uri = gsurf_webkit6_view_load_uri;
	vc->load_html = gsurf_webkit6_view_load_html;
	vc->reload = gsurf_webkit6_view_reload;
	vc->stop_loading = gsurf_webkit6_view_stop_loading;
	vc->go_back = gsurf_webkit6_view_go_back;
	vc->go_forward = gsurf_webkit6_view_go_forward;
	vc->can_go_back = gsurf_webkit6_view_can_go_back;
	vc->can_go_forward = gsurf_webkit6_view_can_go_forward;
	vc->get_uri = gsurf_webkit6_view_get_uri;
	vc->get_title = gsurf_webkit6_view_get_title;
	vc->get_estimated_load_progress = gsurf_webkit6_view_get_progress;
	vc->set_zoom_level = gsurf_webkit6_view_set_zoom;
	vc->get_zoom_level = gsurf_webkit6_view_get_zoom;
	vc->apply_settings = gsurf_webkit6_view_apply_settings;
	vc->run_javascript_async = gsurf_webkit6_view_run_js_async;
	vc->run_javascript_finish = gsurf_webkit6_view_run_js_finish;
	vc->get_native_widget = gsurf_webkit6_view_get_native_widget;
	vc->show_inspector = gsurf_webkit6_view_show_inspector;
	vc->add_user_script = gsurf_webkit6_view_add_user_script;
	vc->add_user_style = gsurf_webkit6_view_add_user_style;
	vc->add_user_script_full = gsurf_webkit6_view_add_user_script_full;
	vc->add_user_style_full = gsurf_webkit6_view_add_user_style_full;
	vc->add_content_filter = gsurf_webkit6_view_add_content_filter;
	vc->clear_user_content = gsurf_webkit6_view_clear_user_content;
	vc->find = gsurf_webkit6_view_find;
	vc->find_next = gsurf_webkit6_view_find_next;
	vc->find_previous = gsurf_webkit6_view_find_previous;
	vc->set_cookie_accept_policy = gsurf_webkit6_view_set_cookie_policy;
	vc->set_proxy = gsurf_webkit6_view_set_proxy;
}

static void
gsurf_webkit6_view_init(GsurfWebkit6View *self)
{
}

GsurfView *
gsurf_webkit6_view_new(void)
{
	return g_object_new(GSURF_TYPE_WEBKIT6_VIEW, NULL);
}
