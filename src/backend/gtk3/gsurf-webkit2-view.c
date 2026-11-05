/*
 * gsurf-webkit2-view.c - WebKit2GTK 4.1 / GTK3 web view backend
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "backend/webkit/gsurf-webkit-protocol.h"
#include "backend/webkit/gsurf-webkit-content.h"
#include "backend/webkit/gsurf-webkit-session.h"
#include "backend/webkit/gsurf-webkit-common.h"

#include "util/gsurf-kiosk.h"
#include "backend/gtk3/gsurf-webkit2-view.h"
#include "module/gsurf-module-manager.h"

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

struct _GsurfWebkit2View
{
	GsurfView parent_instance;

	WebKitWebView *webview; /* the native GtkWidget; we hold a ref */
	GHashTable    *message_handlers; /* "world\x1fname" already registered */
};

G_DEFINE_FINAL_TYPE(GsurfWebkit2View, gsurf_webkit2_view, GSURF_TYPE_VIEW)

/* --- Native signal -> GsurfView signal translation --- */

static void
on_load_changed(WebKitWebView *wv, WebKitLoadEvent event, gpointer user_data)
{
	GsurfWebkit2View *self = user_data;

	/* Apply per-URI setting overrides (uri-params, useragent) at the
	 * start of each navigation. */
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

	/* WebKitLoadEvent and GsurfLoadEvent share the same ordering. */
	gsurf_view_emit_load_changed(GSURF_VIEW(self), (GsurfLoadEvent)event);
}

/* Let permission-handler modules decide (geolocation, notifications). */
static gboolean
on_permission_request(WebKitWebView *wv, WebKitPermissionRequest *req, gpointer user_data)
{
	return gsurf_webkit_common_handle_permission(GSURF_VIEW(user_data), req);
}

/* Let context-menu-provider modules contribute entries to the menu. */
static gboolean
on_context_menu(WebKitWebView *wv, WebKitContextMenu *menu, GdkEvent *event,
                WebKitHitTestResult *hit_result, gpointer user_data)
{
	if (gsurf_kiosk_is_enabled())
		return TRUE;
	gsurf_webkit_common_populate_menu(GSURF_VIEW(user_data), menu, hit_result);
	return FALSE;   /* keep the (augmented) menu */
}

static gboolean
on_load_failed_tls(WebKitWebView *wv, gchar *failing_uri, GTlsCertificate *cert,
                   GTlsCertificateFlags errors, gpointer user_data)
{
	return gsurf_webkit_common_handle_tls_error(wv, failing_uri, cert, errors);
}

static void
on_notify_title(WebKitWebView *wv, GParamSpec *pspec, gpointer user_data)
{
	GsurfWebkit2View *self = user_data;
	gsurf_view_emit_title_changed(GSURF_VIEW(self), webkit_web_view_get_title(wv));
}

static void
on_notify_uri(WebKitWebView *wv, GParamSpec *pspec, gpointer user_data)
{
	GsurfWebkit2View *self = user_data;
	gsurf_view_emit_uri_changed(GSURF_VIEW(self), webkit_web_view_get_uri(wv));
}

static void
on_notify_progress(WebKitWebView *wv, GParamSpec *pspec, gpointer user_data)
{
	GsurfWebkit2View *self = user_data;
	gsurf_view_emit_progress_changed(GSURF_VIEW(self),
		webkit_web_view_get_estimated_load_progress(wv));
}

static void
on_web_process_terminated(WebKitWebView *wv,
                          WebKitWebProcessTerminationReason reason,
                          gpointer user_data)
{
	GsurfWebkit2View *self = user_data;
	gsurf_view_emit_web_process_terminated(GSURF_VIEW(self));
}

/* Consult navigation-hook modules (e.g. adblock, popup policy) before
 * allowing a navigation. Returning TRUE here means we handled the policy
 * decision (by ignoring it). */
static gboolean
on_decide_policy(WebKitWebView *wv, WebKitPolicyDecision *decision,
                 WebKitPolicyDecisionType type, gpointer user_data)
{
	GsurfWebkit2View *self = user_data;
	WebKitNavigationAction *action;
	WebKitURIRequest *request;
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
	request = webkit_navigation_action_get_request(action);
	uri = webkit_uri_request_get_uri(request);

	if (gsurf_module_manager_dispatch_before_navigate(
			gsurf_module_manager_get_default(), GSURF_VIEW(self), uri)
		== GSURF_POLICY_IGNORE) {
		webkit_policy_decision_ignore(decision);
		return TRUE;
	}
	return FALSE;
}

/* --- Vfunc implementations --- */

static void
gsurf_webkit2_view_load_uri(GsurfView *view, const gchar *uri)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	webkit_web_view_load_uri(self->webview, uri);
}

static void
gsurf_webkit2_view_load_html(GsurfView *view, const gchar *html, const gchar *base_uri)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	webkit_web_view_load_html(self->webview, html, base_uri);
}

static void
gsurf_webkit2_view_reload(GsurfView *view, gboolean bypass_cache)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	if (bypass_cache)
		webkit_web_view_reload_bypass_cache(self->webview);
	else
		webkit_web_view_reload(self->webview);
}

static void
gsurf_webkit2_view_stop_loading(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	webkit_web_view_stop_loading(self->webview);
}

static void
gsurf_webkit2_view_go_back(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	webkit_web_view_go_back(self->webview);
}

static void
gsurf_webkit2_view_go_forward(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	webkit_web_view_go_forward(self->webview);
}

static gboolean
gsurf_webkit2_view_can_go_back(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	return webkit_web_view_can_go_back(self->webview);
}

static gboolean
gsurf_webkit2_view_can_go_forward(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	return webkit_web_view_can_go_forward(self->webview);
}

static const gchar *
gsurf_webkit2_view_get_uri(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	return webkit_web_view_get_uri(self->webview);
}

static const gchar *
gsurf_webkit2_view_get_title(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	return webkit_web_view_get_title(self->webview);
}

static gdouble
gsurf_webkit2_view_get_estimated_load_progress(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	return webkit_web_view_get_estimated_load_progress(self->webview);
}

static void
gsurf_webkit2_view_set_zoom_level(GsurfView *view, gdouble level)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	webkit_web_view_set_zoom_level(self->webview, level);
}

static gdouble
gsurf_webkit2_view_get_zoom_level(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	return webkit_web_view_get_zoom_level(self->webview);
}

static void
gsurf_webkit2_view_apply_settings(GsurfView *view, GsurfSettings *s)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitSettings *ws = webkit_web_view_get_settings(self->webview);

	webkit_settings_set_enable_javascript(ws, s->javascript);
	webkit_settings_set_auto_load_images(ws, s->images);
	webkit_settings_set_enable_webgl(ws, s->webgl);
	webkit_settings_set_enable_webaudio(ws, s->webaudio);
	/* WebRTC requires media streams, including on repeated application. */
	webkit_settings_set_enable_media_stream(ws, s->media_stream || s->webrtc);
	webkit_settings_set_enable_webrtc(ws, s->webrtc);
	webkit_settings_set_enable_smooth_scrolling(ws, s->smooth_scrolling);
	webkit_settings_set_enable_caret_browsing(ws, s->caret_browsing);
	/* Deprecated but still functional. (hyperlink-auditing is intentionally
	 * not set here: WebKitGTK >= 2.32 made its setter a no-op that emits a
	 * runtime warning on every call — the s->hyperlink_auditing config field
	 * is retained for documentation and other backends.) */
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	webkit_settings_set_enable_dns_prefetching(ws, s->dns_prefetch);
	G_GNUC_END_IGNORE_DEPRECATIONS
	webkit_settings_set_enable_site_specific_quirks(ws, s->site_quirks);
	webkit_settings_set_enable_developer_extras(ws,
		s->developer_extras && !gsurf_kiosk_is_enabled());
	webkit_settings_set_javascript_can_open_windows_automatically(ws, s->js_can_open_windows);
	webkit_settings_set_javascript_can_access_clipboard(ws, s->js_can_access_clipboard);
	webkit_settings_set_default_font_size(ws, s->default_font_size);
	webkit_settings_set_default_monospace_font_size(ws, s->default_monospace_font_size);

	if (s->default_charset != NULL)
		webkit_settings_set_default_charset(ws, s->default_charset);

	/* Clear a previous per-site override when the default is requested. */
	webkit_settings_set_user_agent(ws,
		(s->user_agent != NULL && *s->user_agent != '\0') ? s->user_agent : NULL);
}

static void
gsurf_webkit2_view_run_javascript_async(GsurfView *view, const gchar *script,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback, gpointer user_data)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);

	webkit_web_view_evaluate_javascript(self->webview, script, -1,
		NULL, NULL, cancellable, callback, user_data);
}

static gchar *
gsurf_webkit2_view_run_javascript_finish(GsurfView *view, GAsyncResult *result, GError **error)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	JSCValue *value;
	gchar *str;

	value = webkit_web_view_evaluate_javascript_finish(self->webview, result, error);
	if (value == NULL)
		return NULL;

	if (jsc_value_is_string(value))
		str = jsc_value_to_string(value);
	else
		str = jsc_value_to_json(value, 0);

	g_object_unref(value);
	return str;
}

static gpointer
gsurf_webkit2_view_get_native_widget(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	return self->webview;
}

/* Move GTK keyboard focus onto the WebKitWebView (off the chrome). */
static void
gsurf_webkit2_view_focus(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);

	if (self->webview != NULL)
		gtk_widget_grab_focus(GTK_WIDGET(self->webview));
}

static void
gsurf_webkit2_view_show_inspector(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitSettings *ws = webkit_web_view_get_settings(self->webview);
	WebKitWebInspector *insp;

	webkit_settings_set_enable_developer_extras(ws, TRUE);
	insp = webkit_web_view_get_inspector(self->webview);
	if (insp != NULL)
		webkit_web_inspector_show(insp);
}

static void
gsurf_webkit2_view_add_user_script_full(GsurfView *view, const gchar *source,
                                        gboolean at_end, const gchar *const *allow)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);
	WebKitUserScript *script;

	script = webkit_user_script_new(source, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		at_end ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END
		       : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
		(const gchar * const *)allow, NULL);
	webkit_user_content_manager_add_script(ucm, script);
	webkit_user_script_unref(script);
}

static void
gsurf_webkit2_view_add_user_style_full(GsurfView *view, const gchar *css,
                                       const gchar *const *allow)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);
	WebKitUserStyleSheet *sheet;

	sheet = webkit_user_style_sheet_new(css, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		WEBKIT_USER_STYLE_LEVEL_USER, (const gchar * const *)allow, NULL);
	webkit_user_content_manager_add_style_sheet(ucm, sheet);
	webkit_user_style_sheet_unref(sheet);
}

/* --- content filter (sub-resource adblock) --- */
static void
gsurf_webkit2_view_add_content_filter(GsurfView *view, const gchar *identifier,
                                      const gchar *json_rules)
{
	gsurf_webkit_content_add_filter(GSURF_WEBKIT2_VIEW(view)->webview, identifier, json_rules);
}

/* --- clipboard --- */
static void
gsurf_webkit2_view_copy_uri(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	const gchar *uri = webkit_web_view_get_uri(self->webview);
	GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);

	if (uri != NULL)
		gtk_clipboard_set_text(cb, uri, -1);
}

static gchar *
gsurf_webkit2_view_read_clipboard_text(GsurfView *view)
{
	GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	return gtk_clipboard_wait_for_text(cb);
}

/* --- snapshot (PNG) --- */
static void
gsurf_webkit2_view_get_snapshot_async(GsurfView *view, GCancellable *cancellable,
                                      GAsyncReadyCallback callback, gpointer user_data)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	webkit_web_view_get_snapshot(self->webview,
		WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
		cancellable, callback, user_data);
}

static cairo_status_t
png_writer(void *closure, const unsigned char *data, unsigned int length)
{
	g_byte_array_append((GByteArray *)closure, data, length);
	return CAIRO_STATUS_SUCCESS;
}

static GBytes *
gsurf_webkit2_view_get_snapshot_finish(GsurfView *view, GAsyncResult *result, GError **error)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	cairo_surface_t *surface;
	GByteArray *png;

	surface = webkit_web_view_get_snapshot_finish(self->webview, result, error);
	if (surface == NULL)
		return NULL;

	png = g_byte_array_new();
	cairo_surface_write_to_png_stream(surface, png_writer, png);
	cairo_surface_destroy(surface);
	return g_byte_array_free_to_bytes(png);
}

static void
gsurf_webkit2_view_clear_user_content(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);

	webkit_user_content_manager_remove_all_scripts(ucm);
	webkit_user_content_manager_remove_all_style_sheets(ucm);
}

static void
gsurf_webkit2_view_find(GsurfView *view, const gchar *text,
                        gboolean case_sensitive, gboolean forward)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitFindController *fc = webkit_web_view_get_find_controller(self->webview);
	guint32 opts = WEBKIT_FIND_OPTIONS_WRAP_AROUND;

	if (!case_sensitive)
		opts |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;
	if (!forward)
		opts |= WEBKIT_FIND_OPTIONS_BACKWARDS;
	webkit_find_controller_search(fc, text, opts, G_MAXUINT);
}

static void
gsurf_webkit2_view_find_next(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	webkit_find_controller_search_next(webkit_web_view_get_find_controller(self->webview));
}

static void
gsurf_webkit2_view_find_previous(GsurfView *view)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	webkit_find_controller_search_previous(webkit_web_view_get_find_controller(self->webview));
}

static void
gsurf_webkit2_view_set_cookie_accept_policy(GsurfView *view, gint policy)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitCookieManager *cm =
		webkit_web_context_get_cookie_manager(webkit_web_view_get_context(self->webview));
	WebKitCookieAcceptPolicy p =
		(policy == 1) ? WEBKIT_COOKIE_POLICY_ACCEPT_NEVER :
		(policy == 2) ? WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY :
		                WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS;

	webkit_cookie_manager_set_accept_policy(cm, p);
}

static void
gsurf_webkit2_view_set_proxy(GsurfView *view, const gchar *uri)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitWebContext *ctx = webkit_web_view_get_context(self->webview);

	gsurf_webkit_protocol_set_proxy(self->webview, uri);
	/* Deprecated in newer WebKitGTK (website-data-manager API), but this
	 * is what WebKit2GTK 4.1 exposes. */
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	if (uri == NULL || *uri == '\0') {
		webkit_web_context_set_network_proxy_settings(ctx,
			WEBKIT_NETWORK_PROXY_MODE_DEFAULT, NULL);
	} else {
		WebKitNetworkProxySettings *settings =
			webkit_network_proxy_settings_new(uri, NULL);
		webkit_web_context_set_network_proxy_settings(ctx,
			WEBKIT_NETWORK_PROXY_MODE_CUSTOM, settings);
		webkit_network_proxy_settings_free(settings);
	}
	G_GNUC_END_IGNORE_DEPRECATIONS
}

static void
on_mouse_target_changed(WebKitWebView *wv, WebKitHitTestResult *hit,
                        guint modifiers, gpointer user_data)
{
	gsurf_webkit_common_mouse_target(GSURF_VIEW(user_data), hit);
}

static void
on_notify_favicon(WebKitWebView *wv, GParamSpec *pspec, gpointer user_data)
{
	gsurf_view_emit_favicon_changed(GSURF_VIEW(user_data));
}

/* WebKit asks us to provide a view for a popup / target=_blank. Let the
 * host create a tab; return its WebKitWebView so WebKit loads into it. */
static GtkWidget *
on_create(WebKitWebView *wv, WebKitNavigationAction *action, gpointer user_data)
{
	return gsurf_webkit_common_create(GSURF_VIEW(user_data), action);
}

/* Content script (document-start, all frames): reports whether an
 * editable element is focused so the modal layer can pass keys through
 * to form fields without a manual "insert" toggle. */
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
	GsurfWebkit2View *self = user_data;
	JSCValue *value = webkit_javascript_result_get_js_value(result);
	gsurf_view_set_editing(GSURF_VIEW(self), jsc_value_to_boolean(value));
}

/* --- Script worlds and messages --- */

/* One registered module message handler: which view to notify and the
 * handler name to report (the signal callback does not receive it). */
typedef struct {
	GsurfWebkit2View *self;
	gchar            *name;
} MessageClosure;

static void
message_closure_free(gpointer data, GClosure *closure)
{
	MessageClosure *mc = data;

	(void)closure;
	g_free(mc->name);
	g_free(mc);
}

/* Deliver a module's script message as GsurfView::script-message. */
static void
on_module_script_message(WebKitUserContentManager *ucm, WebKitJavascriptResult *result,
                         gpointer user_data)
{
	MessageClosure *mc = user_data;
	JSCValue *value = webkit_javascript_result_get_js_value(result);
	g_autofree gchar *json = jsc_value_to_json(value, 0);

	(void)ucm;
	gsurf_view_emit_script_message(GSURF_VIEW(mc->self), mc->name, json);
}

static void
gsurf_webkit2_view_register_script_message(GsurfView *view, const gchar *name,
                                           const gchar *world)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);
	g_autofree gchar *key = g_strdup_printf("%s\x1f%s", world != NULL ? world : "", name);
	g_autofree gchar *detailed = g_strconcat("script-message-received::", name, NULL);
	MessageClosure *mc;
	gboolean ok;

	if (g_hash_table_contains(self->message_handlers, key))
		return;

	ok = (world != NULL)
		? webkit_user_content_manager_register_script_message_handler_in_world(ucm, name, world)
		: webkit_user_content_manager_register_script_message_handler(ucm, name);
	if (!ok) {
		g_warning("gsurf: script message handler '%s' is already taken", name);
		return;
	}
	g_hash_table_add(self->message_handlers, g_steal_pointer(&key));

	mc = g_new0(MessageClosure, 1);
	mc->self = self;
	mc->name = g_strdup(name);
	/* Disconnected in dispose with g_signal_handlers_disconnect_by_func. */
	g_signal_connect_data(ucm, detailed, G_CALLBACK(on_module_script_message), mc,
		message_closure_free, 0);
}

static void
gsurf_webkit2_view_add_user_script_in_world(GsurfView *view, const gchar *source,
                                            gboolean at_end, const gchar *const *allow,
                                            const gchar *world)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);
	WebKitUserScript *script;

	script = webkit_user_script_new_for_world(source, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		at_end ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END
		       : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
		world, (const gchar * const *)allow, NULL);
	webkit_user_content_manager_add_script(ucm, script);
	webkit_user_script_unref(script);
}

static void
gsurf_webkit2_view_run_javascript_in_world_async(GsurfView *view, const gchar *script,
                                                 const gchar *world, GCancellable *cancellable,
                                                 GAsyncReadyCallback callback, gpointer user_data)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);

	/* Same finish as the main-world call: evaluate_javascript_finish. */
	webkit_web_view_evaluate_javascript(self->webview, script, -1, world, NULL,
		cancellable, callback, user_data);
}

static gpointer
gsurf_webkit2_view_get_favicon(GsurfView *view)
{
	return webkit_web_view_get_favicon(GSURF_WEBKIT2_VIEW(view)->webview);
}

/* Build the native web view inside the view's container. A popup is a
 * related view of its opener (same context and web process, so
 * window.opener works) but keeps its own settings and user content: the
 * related-view default would share them, mixing per-site settings and
 * duplicating every injected script between the two views. */
static GtkWidget *
create_native_view(GsurfWebkit2View *self)
{
	GsurfView *view = GSURF_VIEW(self);
	GsurfView *opener = gsurf_view_get_opener(view);
	g_autoptr(WebKitUserContentManager) ucm = webkit_user_content_manager_new();
	g_autoptr(WebKitSettings) settings = webkit_settings_new();
	WebKitWebContext *ctx;

	if (gsurf_view_get_related(view) && opener != NULL &&
	    WEBKIT_IS_WEB_VIEW(gsurf_view_get_native_widget(opener))) {
		return g_object_new(WEBKIT_TYPE_WEB_VIEW,
			"related-view", gsurf_view_get_native_widget(opener),
			"user-content-manager", ucm,
			"settings", settings,
			NULL);
	}

	/* Web-process extensions must be wired before the context launches
	 * its first web process. */
	ctx = gsurf_webkit_session_get_context(gsurf_view_get_session(view));
	gsurf_webkit_content_attach_context(ctx);
	return g_object_new(WEBKIT_TYPE_WEB_VIEW,
		"web-context", ctx,
		"user-content-manager", ucm,
		"settings", settings,
		NULL);
}

/* --- GObject lifecycle --- */

static void
gsurf_webkit2_view_constructed(GObject *object)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(object);
	WebKitUserContentManager *ucm;
	WebKitUserScript *tracker;
	GtkWidget *widget;

	/* The base class resolves the container and acquires its session. */
	G_OBJECT_CLASS(gsurf_webkit2_view_parent_class)->constructed(object);

	gsurf_webkit_session_prepare_default();
	widget = create_native_view(self);
	gsurf_webkit_protocol_attach(WEBKIT_WEB_VIEW(widget));
	self->webview = WEBKIT_WEB_VIEW(g_object_ref_sink(widget));
	if (gsurf_kiosk_is_enabled())
		gtk_drag_dest_unset(widget);

	/* Page focus tracking -> gsurf_view_set_editing(). */
	ucm = webkit_web_view_get_user_content_manager(self->webview);
	webkit_user_content_manager_register_script_message_handler(ucm, "gsurfFocus");
	g_signal_connect(ucm, "script-message-received::gsurfFocus",
		G_CALLBACK(on_focus_message), self);
	tracker = webkit_user_script_new(FOCUS_TRACKER_JS,
		WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
	webkit_user_content_manager_add_script(ucm, tracker);
	webkit_user_script_unref(tracker);

	g_signal_connect(self->webview, "load-changed",
		G_CALLBACK(on_load_changed), self);
	g_signal_connect(self->webview, "notify::title",
		G_CALLBACK(on_notify_title), self);
	g_signal_connect(self->webview, "notify::uri",
		G_CALLBACK(on_notify_uri), self);
	g_signal_connect(self->webview, "notify::estimated-load-progress",
		G_CALLBACK(on_notify_progress), self);
	g_signal_connect(self->webview, "web-process-terminated",
		G_CALLBACK(on_web_process_terminated), self);
	g_signal_connect(self->webview, "decide-policy",
		G_CALLBACK(on_decide_policy), self);
	g_signal_connect(self->webview, "permission-request",
		G_CALLBACK(on_permission_request), self);
	g_signal_connect(self->webview, "load-failed-with-tls-errors",
		G_CALLBACK(on_load_failed_tls), self);
	gsurf_webkit_session_track_downloads(self->webview);
	g_signal_connect(self->webview, "mouse-target-changed",
		G_CALLBACK(on_mouse_target_changed), self);
	g_signal_connect(self->webview, "notify::favicon",
		G_CALLBACK(on_notify_favicon), self);
	g_signal_connect(self->webview, "create",
		G_CALLBACK(on_create), self);
	g_signal_connect(self->webview, "context-menu",
		G_CALLBACK(on_context_menu), self);

	/* Let script-injector modules add user scripts/styles to this view. */
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
unregister_message_handlers(GsurfWebkit2View *self)
{
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);
	GHashTableIter iter;
	gpointer key;

	webkit_user_content_manager_unregister_script_message_handler(ucm, "gsurfFocus");
	if (self->message_handlers == NULL)
		return;
	g_hash_table_iter_init(&iter, self->message_handlers);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		g_auto(GStrv) parts = g_strsplit(key, "\x1f", 2);

		if (parts[0] == NULL || parts[1] == NULL)
			continue;
		if (*parts[0] != '\0')
			webkit_user_content_manager_unregister_script_message_handler_in_world(ucm,
				parts[1], parts[0]);
		else
			webkit_user_content_manager_unregister_script_message_handler(ucm, parts[1]);
	}
}

static void
gsurf_webkit2_view_dispose(GObject *object)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(object);

	if (self->webview != NULL) {
		gsurf_webkit_protocol_cancel(self->webview);
		unregister_message_handlers(self);
		/* The host may retain the native widget, and the context is shared.
		 * Neither may keep callbacks into a disposed library wrapper. */
		g_signal_handlers_disconnect_by_data(
			webkit_web_view_get_user_content_manager(self->webview), self);
		/* Module message closures carry their own data; match the
		 * function only (the content manager belongs to this view). */
		g_signal_handlers_disconnect_matched(
			webkit_web_view_get_user_content_manager(self->webview),
			G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_module_script_message, NULL);
		g_signal_handlers_disconnect_by_data(self->webview, self);
		g_clear_object(&self->webview);
	}
	g_clear_pointer(&self->message_handlers, g_hash_table_unref);

	G_OBJECT_CLASS(gsurf_webkit2_view_parent_class)->dispose(object);
}

static void
gsurf_webkit2_view_class_init(GsurfWebkit2ViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfViewClass *view_class = GSURF_VIEW_CLASS(klass);

	/* The base class acquires a view's container session before this
	 * class's constructed() runs, so the session factory must already be
	 * installed when the first instance is built. */
	gsurf_webkit_session_install();

	object_class->constructed = gsurf_webkit2_view_constructed;
	object_class->dispose = gsurf_webkit2_view_dispose;

	view_class->load_uri = gsurf_webkit2_view_load_uri;
	view_class->load_html = gsurf_webkit2_view_load_html;
	view_class->reload = gsurf_webkit2_view_reload;
	view_class->stop_loading = gsurf_webkit2_view_stop_loading;
	view_class->go_back = gsurf_webkit2_view_go_back;
	view_class->go_forward = gsurf_webkit2_view_go_forward;
	view_class->can_go_back = gsurf_webkit2_view_can_go_back;
	view_class->can_go_forward = gsurf_webkit2_view_can_go_forward;
	view_class->get_uri = gsurf_webkit2_view_get_uri;
	view_class->get_title = gsurf_webkit2_view_get_title;
	view_class->get_estimated_load_progress = gsurf_webkit2_view_get_estimated_load_progress;
	view_class->set_zoom_level = gsurf_webkit2_view_set_zoom_level;
	view_class->get_zoom_level = gsurf_webkit2_view_get_zoom_level;
	view_class->apply_settings = gsurf_webkit2_view_apply_settings;
	view_class->run_javascript_async = gsurf_webkit2_view_run_javascript_async;
	view_class->run_javascript_finish = gsurf_webkit2_view_run_javascript_finish;
	view_class->get_native_widget = gsurf_webkit2_view_get_native_widget;
	view_class->focus = gsurf_webkit2_view_focus;
	view_class->get_snapshot_async = gsurf_webkit2_view_get_snapshot_async;
	view_class->get_snapshot_finish = gsurf_webkit2_view_get_snapshot_finish;
	view_class->show_inspector = gsurf_webkit2_view_show_inspector;
	view_class->add_user_script_full = gsurf_webkit2_view_add_user_script_full;
	view_class->add_user_style_full = gsurf_webkit2_view_add_user_style_full;
	view_class->clear_user_content = gsurf_webkit2_view_clear_user_content;
	view_class->add_content_filter = gsurf_webkit2_view_add_content_filter;
	view_class->copy_uri = gsurf_webkit2_view_copy_uri;
	view_class->read_clipboard_text = gsurf_webkit2_view_read_clipboard_text;
	view_class->find = gsurf_webkit2_view_find;
	view_class->find_next = gsurf_webkit2_view_find_next;
	view_class->find_previous = gsurf_webkit2_view_find_previous;
	view_class->set_cookie_accept_policy = gsurf_webkit2_view_set_cookie_accept_policy;
	view_class->set_proxy = gsurf_webkit2_view_set_proxy;
	view_class->register_script_message = gsurf_webkit2_view_register_script_message;
	view_class->add_user_script_in_world = gsurf_webkit2_view_add_user_script_in_world;
	view_class->run_javascript_in_world_async = gsurf_webkit2_view_run_javascript_in_world_async;
	view_class->get_favicon = gsurf_webkit2_view_get_favicon;
}

static void
gsurf_webkit2_view_init(GsurfWebkit2View *self)
{
	self->message_handlers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

GsurfView *
gsurf_webkit2_view_new(void)
{
	return g_object_new(GSURF_TYPE_WEBKIT2_VIEW, NULL);
}
