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

#include "backend/webkit/gsurf-webkit-protocol.h"
#include "backend/webkit/gsurf-webkit-content.h"
#include "backend/webkit/gsurf-webkit-session.h"
#include "backend/webkit/gsurf-webkit-common.h"

#include "util/gsurf-kiosk.h"
#include "backend/gtk4/gsurf-webkit6-view.h"
#include "module/gsurf-module-manager.h"

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <string.h>

struct _GsurfWebkit6View
{
	GsurfView parent_instance;
	WebKitWebView *webview;
	GHashTable    *message_handlers; /* "world\x1fname" already registered */
};

G_DEFINE_FINAL_TYPE(GsurfWebkit6View, gsurf_webkit6_view, GSURF_TYPE_VIEW)

/* Suppress native navigation/inspector menu entries in kiosk sessions;
 * otherwise let context-menu-provider modules add their entries. */
static gboolean
on_context_menu(WebKitWebView *wv, WebKitContextMenu *menu,
                WebKitHitTestResult *hit, gpointer user_data)
{
	(void) wv;
	if (gsurf_kiosk_is_enabled())
		return TRUE;
	gsurf_webkit_common_populate_menu(GSURF_VIEW(user_data), menu, hit);
	return FALSE;
}

static gboolean
on_load_failed_tls(WebKitWebView *wv, gchar *failing_uri, GTlsCertificate *cert,
                   GTlsCertificateFlags errors, gpointer user_data)
{
	(void) user_data;
	return gsurf_webkit_common_handle_tls_error(wv, failing_uri, cert, errors);
}

static void
on_mouse_target_changed(WebKitWebView *wv, WebKitHitTestResult *hit,
                        guint modifiers, gpointer user_data)
{
	(void) wv;
	(void) modifiers;
	gsurf_webkit_common_mouse_target(GSURF_VIEW(user_data), hit);
}

static void
on_notify_favicon(WebKitWebView *wv, GParamSpec *pspec, gpointer user_data)
{
	(void) wv;
	(void) pspec;
	gsurf_view_emit_favicon_changed(GSURF_VIEW(user_data));
}

/* A popup / target=_blank: the host builds a related view for it. */
static GtkWidget *
on_create(WebKitWebView *wv, WebKitNavigationAction *action, gpointer user_data)
{
	(void) wv;
	return gsurf_webkit_common_create(GSURF_VIEW(user_data), action);
}

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

	if (gsurf_kiosk_is_enabled() &&
	    type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
		webkit_policy_decision_ignore(decision);
		return TRUE;
	}

	/* A top-level response: modules may still veto it (host
	 * assignment in the containers module); see
	 * gsurf_navigation_hook_decide_main_response(). */
	if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
		return gsurf_webkit_common_decide_response(GSURF_VIEW(self), decision);

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
	(void) wv;
	return gsurf_webkit_common_handle_permission(GSURF_VIEW(user_data), req);
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
	/* WebRTC requires media streams, including on repeated application. */
	webkit_settings_set_enable_media_stream(ws, s->media_stream || s->webrtc);
	webkit_settings_set_enable_webrtc(ws, s->webrtc);
	webkit_settings_set_enable_smooth_scrolling(ws, s->smooth_scrolling);
	webkit_settings_set_enable_caret_browsing(ws, s->caret_browsing);
	webkit_settings_set_enable_site_specific_quirks(ws, s->site_quirks);
	webkit_settings_set_enable_developer_extras(ws,
		s->developer_extras && !gsurf_kiosk_is_enabled());
	webkit_settings_set_javascript_can_open_windows_automatically(ws, s->js_can_open_windows);
	webkit_settings_set_default_font_size(ws, s->default_font_size);
	webkit_settings_set_default_monospace_font_size(ws, s->default_monospace_font_size);
	if (s->default_charset != NULL)
		webkit_settings_set_default_charset(ws, s->default_charset);
	/* Clear a previous per-site override when the default is requested. */
	webkit_settings_set_user_agent(ws,
		(s->user_agent != NULL && *s->user_agent != '\0') ? s->user_agent : NULL);
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

static void gsurf_webkit6_view_focus(GsurfView *v)
{ gtk_widget_grab_focus(GTK_WIDGET(GSURF_WEBKIT6_VIEW(v)->webview)); }

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

static void
gsurf_webkit6_view_add_content_filter(GsurfView *v, const gchar *identifier,
                                      const gchar *json_rules)
{
	gsurf_webkit_content_add_filter(GSURF_WEBKIT6_VIEW(v)->webview, identifier, json_rules);
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

	gsurf_webkit_protocol_set_proxy(GSURF_WEBKIT6_VIEW(v)->webview, uri);
	if (uri == NULL || *uri == '\0') {
		webkit_network_session_set_proxy_settings(ns, WEBKIT_NETWORK_PROXY_MODE_DEFAULT, NULL);
	} else {
		WebKitNetworkProxySettings *s = webkit_network_proxy_settings_new(uri, NULL);
		webkit_network_session_set_proxy_settings(ns, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, s);
		webkit_network_proxy_settings_free(s);
	}
}

/* --- snapshot / clipboard --- */

static void
gsurf_webkit6_view_get_snapshot_async(GsurfView *v, GCancellable *cancellable,
                                      GAsyncReadyCallback callback, gpointer user_data)
{
	webkit_web_view_get_snapshot(GSURF_WEBKIT6_VIEW(v)->webview,
		WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
		cancellable, callback, user_data);
}

/* WebKitGTK 6.0 returns a GdkTexture; encode it as PNG like GTK3 does. */
static GBytes *
gsurf_webkit6_view_get_snapshot_finish(GsurfView *v, GAsyncResult *result, GError **error)
{
	g_autoptr(GdkTexture) texture = webkit_web_view_get_snapshot_finish(
		GSURF_WEBKIT6_VIEW(v)->webview, result, error);

	if (texture == NULL)
		return NULL;
	return gdk_texture_save_to_png_bytes(texture);
}

static void
gsurf_webkit6_view_copy_uri(GsurfView *v)
{
	GtkWidget *widget = GTK_WIDGET(GSURF_WEBKIT6_VIEW(v)->webview);
	const gchar *uri = webkit_web_view_get_uri(GSURF_WEBKIT6_VIEW(v)->webview);

	if (uri != NULL)
		gdk_clipboard_set_text(gtk_widget_get_clipboard(widget), uri);
}

typedef struct {
	GMainLoop *loop;
	gchar     *text;
} ClipboardRead;

static void
on_clipboard_text(GObject *source, GAsyncResult *result, gpointer user_data)
{
	ClipboardRead *read = user_data;

	read->text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, NULL);
	g_main_loop_quit(read->loop);
}

/* GTK4 clipboard reads are asynchronous only; the GsurfView contract is
 * synchronous (as gtk_clipboard_wait_for_text() was on GTK3), so wait in
 * a nested main loop. */
static gchar *
gsurf_webkit6_view_read_clipboard_text(GsurfView *v)
{
	GtkWidget *widget = GTK_WIDGET(GSURF_WEBKIT6_VIEW(v)->webview);
	ClipboardRead read = { NULL, NULL };

	read.loop = g_main_loop_new(NULL, FALSE);
	gdk_clipboard_read_text_async(gtk_widget_get_clipboard(widget), NULL,
		on_clipboard_text, &read);
	g_main_loop_run(read.loop);
	g_main_loop_unref(read.loop);
	return read.text;
}

/* --- script worlds and messages --- */

/* One registered module message handler (see the GTK3 backend). */
typedef struct {
	GsurfWebkit6View *self;
	gchar            *name;
} MessageClosure;

static void
message_closure_free(gpointer data, GClosure *closure)
{
	MessageClosure *mc = data;

	(void) closure;
	g_free(mc->name);
	g_free(mc);
}

static void
on_module_script_message(WebKitUserContentManager *ucm, JSCValue *value, gpointer user_data)
{
	MessageClosure *mc = user_data;
	g_autofree gchar *json = jsc_value_to_json(value, 0);

	(void) ucm;
	gsurf_view_emit_script_message(GSURF_VIEW(mc->self), mc->name, json);
}

static void
gsurf_webkit6_view_register_script_message(GsurfView *v, const gchar *name,
                                           const gchar *world)
{
	GsurfWebkit6View *self = GSURF_WEBKIT6_VIEW(v);
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);
	g_autofree gchar *key = g_strdup_printf("%s\x1f%s", world != NULL ? world : "", name);
	g_autofree gchar *detailed = g_strconcat("script-message-received::", name, NULL);
	MessageClosure *mc;

	if (g_hash_table_contains(self->message_handlers, key))
		return;
	if (!webkit_user_content_manager_register_script_message_handler(ucm, name, world)) {
		g_warning("gsurf: script message handler '%s' is already taken", name);
		return;
	}
	g_hash_table_add(self->message_handlers, g_steal_pointer(&key));

	mc = g_new0(MessageClosure, 1);
	mc->self = self;
	mc->name = g_strdup(name);
	g_signal_connect_data(ucm, detailed, G_CALLBACK(on_module_script_message), mc,
		message_closure_free, 0);
}

static void
gsurf_webkit6_view_add_user_script_in_world(GsurfView *v, const gchar *source,
                                            gboolean at_end, const gchar *const *allow,
                                            const gchar *world)
{
	WebKitUserContentManager *ucm =
		webkit_web_view_get_user_content_manager(GSURF_WEBKIT6_VIEW(v)->webview);
	WebKitUserScript *script = webkit_user_script_new_for_world(source,
		WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		at_end ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END
		       : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
		world, (const gchar * const *)allow, NULL);

	webkit_user_content_manager_add_script(ucm, script);
	webkit_user_script_unref(script);
}

static void
gsurf_webkit6_view_run_js_in_world_async(GsurfView *v, const gchar *script,
                                         const gchar *world, GCancellable *c,
                                         GAsyncReadyCallback cb, gpointer ud)
{
	webkit_web_view_evaluate_javascript(GSURF_WEBKIT6_VIEW(v)->webview, script, -1,
		world, NULL, c, cb, ud);
}

static gpointer
gsurf_webkit6_view_get_favicon(GsurfView *v)
{
	return webkit_web_view_get_favicon(GSURF_WEBKIT6_VIEW(v)->webview);
}

/* Build the native web view in the view's container: its network
 * session, or the opener's for a related popup. Popups keep their own
 * settings and user content (see the GTK3 backend for why). */
static GtkWidget *
create_native_view(GsurfWebkit6View *self)
{
	GsurfView *view = GSURF_VIEW(self);
	GsurfView *opener = gsurf_view_get_opener(view);
	g_autoptr(WebKitUserContentManager) ucm = webkit_user_content_manager_new();
	g_autoptr(WebKitSettings) settings = webkit_settings_new();
	gpointer session = gsurf_view_get_session(view);

	if (gsurf_view_get_related(view) && opener != NULL &&
	    WEBKIT_IS_WEB_VIEW(gsurf_view_get_native_widget(opener))) {
		return g_object_new(WEBKIT_TYPE_WEB_VIEW,
			"related-view", gsurf_view_get_native_widget(opener),
			"user-content-manager", ucm,
			"settings", settings,
			NULL);
	}
	return g_object_new(WEBKIT_TYPE_WEB_VIEW,
		"web-context", gsurf_webkit_session_get_context(session),
		"network-session", session != NULL ? session : webkit_network_session_get_default(),
		"user-content-manager", ucm,
		"settings", settings,
		NULL);
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

/* WebKitGTK 6.0 hands script-message handlers the JSCValue directly
 * (4.1 wrapped it in a WebKitJavascriptResult). */
static void
on_focus_message(WebKitUserContentManager *ucm, JSCValue *value, gpointer user_data)
{
	GsurfWebkit6View *self = user_data;
	gsurf_view_set_editing(GSURF_VIEW(self), jsc_value_to_boolean(value));
}

static void
gsurf_webkit6_view_constructed(GObject *object)
{
	GsurfWebkit6View *self = GSURF_WEBKIT6_VIEW(object);
	WebKitUserContentManager *ucm;
	WebKitUserScript *tracker;
	GtkWidget *widget;

	/* The base class resolves the container and acquires its session. */
	G_OBJECT_CLASS(gsurf_webkit6_view_parent_class)->constructed(object);

	/* Web-process extensions are configured on the (shared) web context
	 * and must be wired before it launches its first web process. */
	gsurf_webkit_content_attach_context(webkit_web_context_get_default());
	gsurf_webkit_session_prepare_default();
	widget = create_native_view(self);
	gsurf_webkit_protocol_attach(WEBKIT_WEB_VIEW(widget));
	self->webview = WEBKIT_WEB_VIEW(g_object_ref_sink(widget));
	if (gsurf_kiosk_is_enabled()) {
		g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(widget);
		guint i;

		/* GTK4 uses event controllers for external drops. Iterate backwards
		 * because removing a controller changes the observed list. */
		for (i = g_list_model_get_n_items(controllers); i > 0; i--) {
			g_autoptr(GtkEventController) controller = g_list_model_get_item(controllers, i - 1);
			if (GTK_IS_DROP_TARGET(controller) || GTK_IS_DROP_TARGET_ASYNC(controller))
				gtk_widget_remove_controller(widget, controller);
		}
	}

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
	g_signal_connect(self->webview, "context-menu", G_CALLBACK(on_context_menu), self);
	g_signal_connect(self->webview, "decide-policy", G_CALLBACK(on_decide_policy), self);
	g_signal_connect(self->webview, "permission-request",
		G_CALLBACK(on_permission_request), self);
	g_signal_connect(self->webview, "load-failed-with-tls-errors",
		G_CALLBACK(on_load_failed_tls), self);
	g_signal_connect(self->webview, "mouse-target-changed",
		G_CALLBACK(on_mouse_target_changed), self);
	g_signal_connect(self->webview, "notify::favicon",
		G_CALLBACK(on_notify_favicon), self);
	g_signal_connect(self->webview, "create", G_CALLBACK(on_create), self);
	gsurf_webkit_session_track_downloads(self->webview);

	gsurf_module_manager_dispatch_inject_scripts(
		gsurf_module_manager_get_default(), GSURF_VIEW(self), NULL);
}

/* Unregister every script message handler this view registered (the
 * focus tracker's and the modules'), so a message the page posted just
 * before the view went away is dropped by WebKit instead of reaching a
 * handler whose content manager is being destroyed (a WebKit critical:
 * "Script message ... received after the WebKitUserContentManager has
 * been destroyed"). Quick tab closes (wmux kill-pane) hit that race. */
static void
unregister_message_handlers(GsurfWebkit6View *self)
{
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);
	GHashTableIter iter;
	gpointer key;

	webkit_user_content_manager_unregister_script_message_handler(ucm, "gsurfFocus", NULL);
	if (self->message_handlers == NULL)
		return;
	g_hash_table_iter_init(&iter, self->message_handlers);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		g_auto(GStrv) parts = g_strsplit(key, "\x1f", 2);

		if (parts[0] == NULL || parts[1] == NULL)
			continue;
		webkit_user_content_manager_unregister_script_message_handler(ucm, parts[1],
			*parts[0] != '\0' ? parts[0] : NULL);
	}
}

static void
gsurf_webkit6_view_dispose(GObject *object)
{
	GsurfWebkit6View *self = GSURF_WEBKIT6_VIEW(object);

	if (self->webview != NULL) {
		gsurf_webkit_protocol_cancel(self->webview);
		unregister_message_handlers(self);
		/* The content manager can outlive the library wrapper in embedders. */
		g_signal_handlers_disconnect_by_data(
			webkit_web_view_get_user_content_manager(self->webview), self);
		g_signal_handlers_disconnect_matched(
			webkit_web_view_get_user_content_manager(self->webview),
			G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_module_script_message, NULL);
		g_signal_handlers_disconnect_by_data(self->webview, self);
		g_clear_object(&self->webview);
	}
	g_clear_pointer(&self->message_handlers, g_hash_table_unref);
	G_OBJECT_CLASS(gsurf_webkit6_view_parent_class)->dispose(object);
}

static void
gsurf_webkit6_view_class_init(GsurfWebkit6ViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfViewClass *vc = GSURF_VIEW_CLASS(klass);

	/* The base class acquires a view's container session before this
	 * class's constructed() runs, so the session factory must already be
	 * installed when the first instance is built. */
	gsurf_webkit_session_install();

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
	vc->focus = gsurf_webkit6_view_focus;
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
	vc->get_snapshot_async = gsurf_webkit6_view_get_snapshot_async;
	vc->get_snapshot_finish = gsurf_webkit6_view_get_snapshot_finish;
	vc->copy_uri = gsurf_webkit6_view_copy_uri;
	vc->read_clipboard_text = gsurf_webkit6_view_read_clipboard_text;
	vc->register_script_message = gsurf_webkit6_view_register_script_message;
	vc->add_user_script_in_world = gsurf_webkit6_view_add_user_script_in_world;
	vc->run_javascript_in_world_async = gsurf_webkit6_view_run_js_in_world_async;
	vc->get_favicon = gsurf_webkit6_view_get_favicon;
}

static void
gsurf_webkit6_view_init(GsurfWebkit6View *self)
{
	self->message_handlers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

GsurfView *
gsurf_webkit6_view_new(void)
{
	return g_object_new(GSURF_TYPE_WEBKIT6_VIEW, NULL);
}
