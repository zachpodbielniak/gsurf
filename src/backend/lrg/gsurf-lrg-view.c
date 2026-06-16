/*
 * gsurf-lrg-view.c - libregnum (LRG) web view
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GsurfView subclass for the libregnum backend. The web engine (offscreen
 * WebKitGTK today) is owned by a #GsurfLrgEngine; this file wires the native
 * WebKitWebView signals into #GsurfView signals, implements the #GsurfView
 * vfuncs (a near-mirror of the GTK3 backend, since the WebKit object API is
 * shared), and implements the LRG-only API: producing the page as a #GrlTexture
 * and forwarding input to the engine.
 */

#include "backend/lrg/gsurf-lrg-view.h"
#include "backend/lrg/gsurf-lrg-engine.h"
#include "module/gsurf-module-manager.h"
#include "boxed/gsurf-hit-test.h"
#include "boxed/gsurf-menu-item.h"

#include <string.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <graylib.h>

struct _GsurfLrgView
{
	GsurfView parent_instance;

	GsurfLrgEngine *engine;
	WebKitWebView  *webview;   /* borrowed from @engine */

	GrlTexture     *texture;   /* owned; lazily created on the render thread */
	int             tex_w;
	int             tex_h;
	gboolean        frame_dirty;
	gboolean        focused;
};

G_DEFINE_FINAL_TYPE(GsurfLrgView, gsurf_lrg_view, GSURF_TYPE_VIEW)

/* ============================================================= *
 *  Native WebKit signal -> GsurfView signal translation
 *  (ported from the GTK3 backend; the WebKit API is shared)
 * ============================================================= */

static void
on_load_changed(WebKitWebView *wv, WebKitLoadEvent event, gpointer user_data)
{
	GsurfLrgView *self = user_data;

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

static gboolean
on_permission_request(WebKitWebView *wv, WebKitPermissionRequest *req,
                      gpointer user_data)
{
	GsurfLrgView *self = user_data;
	const gchar *type = "other";
	GsurfPermissionVerdict v;

	if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req))
		type = "geolocation";
	else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req))
		type = "notification";

	v = gsurf_module_manager_dispatch_permission(gsurf_module_manager_get_default(),
		GSURF_VIEW(self), type, NULL);
	if (v == GSURF_PERMISSION_ALLOW) {
		webkit_permission_request_allow(req);
		return TRUE;
	}
	if (v == GSURF_PERMISSION_DENY) {
		webkit_permission_request_deny(req);
		return TRUE;
	}
	return FALSE;
}

static gboolean
on_download_decide_destination(WebKitDownload *download, const gchar *suggested,
                               gpointer user_data)
{
	WebKitURIRequest *req = webkit_download_get_request(download);
	const gchar *uri = req ? webkit_uri_request_get_uri(req) : NULL;
	g_autofree gchar *path = gsurf_module_manager_dispatch_decide_destination(
		gsurf_module_manager_get_default(), uri, suggested);
	g_autofree gchar *file_uri = NULL;

	if (path == NULL)
		return FALSE;

	file_uri = g_filename_to_uri(path, NULL, NULL);
	if (file_uri != NULL)
		webkit_download_set_destination(download, file_uri);
	return TRUE;
}

static void
on_download_started(WebKitWebContext *context, WebKitDownload *download,
                    gpointer user_data)
{
	g_signal_connect(download, "decide-destination",
		G_CALLBACK(on_download_decide_destination), user_data);
}

static void
on_menu_action_activate(GSimpleAction *action, GVariant *param, gpointer user_data)
{
	const gchar *command = g_object_get_data(G_OBJECT(action), "gsurf-command");
	g_autoptr(GError) error = NULL;

	if (command != NULL && !g_spawn_command_line_async(command, &error))
		g_warning("gsurf context-menu: %s", error ? error->message : "spawn failed");
}

static GsurfHitTest *
hit_test_from_webkit(WebKitHitTestResult *r)
{
	GsurfHitTest *ht = gsurf_hit_test_new();
	guint ctx = 0;

	if (webkit_hit_test_result_context_is_link(r)) {
		ctx |= GSURF_HIT_TEST_LINK;
		gsurf_hit_test_set_link_uri(ht, webkit_hit_test_result_get_link_uri(r));
		gsurf_hit_test_set_link_label(ht, webkit_hit_test_result_get_link_label(r));
	}
	if (webkit_hit_test_result_context_is_image(r)) {
		ctx |= GSURF_HIT_TEST_IMAGE;
		gsurf_hit_test_set_image_uri(ht, webkit_hit_test_result_get_image_uri(r));
	}
	if (webkit_hit_test_result_context_is_media(r)) {
		ctx |= GSURF_HIT_TEST_MEDIA;
		gsurf_hit_test_set_media_uri(ht, webkit_hit_test_result_get_media_uri(r));
	}
	if (webkit_hit_test_result_context_is_editable(r))
		ctx |= GSURF_HIT_TEST_EDITABLE;
	if (webkit_hit_test_result_context_is_scrollbar(r))
		ctx |= GSURF_HIT_TEST_SCROLLBAR;
	gsurf_hit_test_set_context(ht, ctx);
	return ht;
}

static gboolean
on_context_menu(WebKitWebView *wv, WebKitContextMenu *menu, GdkEvent *event,
                WebKitHitTestResult *hit_result, gpointer user_data)
{
	GsurfHitTest *hit = hit_test_from_webkit(hit_result);
	GPtrArray *items = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gsurf_menu_item_free);
	guint i;

	gsurf_module_manager_dispatch_populate_menu(gsurf_module_manager_get_default(),
		hit, items);

	if (items->len > 0)
		webkit_context_menu_append(menu, webkit_context_menu_item_new_separator());

	for (i = 0; i < items->len; i++) {
		GsurfMenuItem *mi = g_ptr_array_index(items, i);
		g_autofree gchar *action_name = NULL;
		GSimpleAction *act;

		if (gsurf_menu_item_get_is_separator(mi)) {
			webkit_context_menu_append(menu, webkit_context_menu_item_new_separator());
			continue;
		}
		action_name = g_strdup_printf("gsurf-menu-%u", i);
		act = g_simple_action_new(action_name, NULL);
		g_object_set_data_full(G_OBJECT(act), "gsurf-command",
			g_strdup(gsurf_menu_item_get_arg(mi)), g_free);
		g_signal_connect(act, "activate", G_CALLBACK(on_menu_action_activate), NULL);
		webkit_context_menu_append(menu,
			webkit_context_menu_item_new_from_gaction(G_ACTION(act),
				gsurf_menu_item_get_label(mi), NULL));
		g_object_unref(act);
	}

	g_ptr_array_unref(items);
	gsurf_hit_test_free(hit);
	return FALSE;
}

static gboolean
on_load_failed_tls(WebKitWebView *wv, gchar *failing_uri, GTlsCertificate *cert,
                   GTlsCertificateFlags errors, gpointer user_data)
{
	g_autoptr(GUri) parsed = g_uri_parse(failing_uri, G_URI_FLAGS_NONE, NULL);
	const gchar *host = parsed ? g_uri_get_host(parsed) : NULL;

	if (gsurf_module_manager_dispatch_verify_cert(gsurf_module_manager_get_default(),
			host, errors) == GSURF_TLS_PROCEED) {
		webkit_web_context_allow_tls_certificate_for_host(
			webkit_web_view_get_context(wv), cert, host);
		webkit_web_view_load_uri(wv, failing_uri);
		return TRUE;
	}
	return FALSE;
}

static void
on_notify_title(WebKitWebView *wv, GParamSpec *pspec, gpointer user_data)
{
	gsurf_view_emit_title_changed(GSURF_VIEW(user_data), webkit_web_view_get_title(wv));
}

static void
on_notify_uri(WebKitWebView *wv, GParamSpec *pspec, gpointer user_data)
{
	gsurf_view_emit_uri_changed(GSURF_VIEW(user_data), webkit_web_view_get_uri(wv));
}

static void
on_notify_progress(WebKitWebView *wv, GParamSpec *pspec, gpointer user_data)
{
	gsurf_view_emit_progress_changed(GSURF_VIEW(user_data),
		webkit_web_view_get_estimated_load_progress(wv));
}

static void
on_web_process_terminated(WebKitWebView *wv,
                          WebKitWebProcessTerminationReason reason,
                          gpointer user_data)
{
	gsurf_view_emit_web_process_terminated(GSURF_VIEW(user_data));
}

static gboolean
on_decide_policy(WebKitWebView *wv, WebKitPolicyDecision *decision,
                 WebKitPolicyDecisionType type, gpointer user_data)
{
	GsurfLrgView *self = user_data;
	WebKitNavigationAction *action;
	WebKitURIRequest *request;
	const gchar *uri;

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

static void
on_mouse_target_changed(WebKitWebView *wv, WebKitHitTestResult *hit,
                        guint modifiers, gpointer user_data)
{
	const gchar *uri = webkit_hit_test_result_context_is_link(hit)
		? webkit_hit_test_result_get_link_uri(hit) : NULL;
	gsurf_view_set_hovered_uri(GSURF_VIEW(user_data), uri);
}

static void
on_notify_favicon(WebKitWebView *wv, GParamSpec *pspec, gpointer user_data)
{
	gsurf_view_emit_favicon_changed(GSURF_VIEW(user_data));
}

static GtkWidget *
on_create(WebKitWebView *wv, WebKitNavigationAction *action, gpointer user_data)
{
	GsurfLrgView *self = user_data;
	WebKitURIRequest *req = webkit_navigation_action_get_request(action);
	const gchar *uri = req ? webkit_uri_request_get_uri(req) : NULL;
	GsurfView *newview = gsurf_view_emit_create_view(GSURF_VIEW(self), uri);

	/* A new LRG view's native WebKitWebView is what WebKit loads into. */
	if (newview != NULL && GSURF_IS_LRG_VIEW(newview))
		return GTK_WIDGET(GSURF_LRG_VIEW(newview)->webview);
	return NULL;
}

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
on_focus_message(WebKitUserContentManager *ucm, WebKitJavascriptResult *result,
                 gpointer user_data)
{
	JSCValue *value = webkit_javascript_result_get_js_value(result);
	gsurf_view_set_editing(GSURF_VIEW(user_data), jsc_value_to_boolean(value));
}

static void
lrg_view_apply_context_config(GsurfLrgView *self)
{
	GsurfConfig *cfg = gsurf_config_get_default();
	WebKitWebContext *ctx = webkit_web_view_get_context(self->webview);

	if (cfg == NULL)
		return;

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	webkit_web_context_set_tls_errors_policy(ctx,
		cfg->tls_strict ? WEBKIT_TLS_ERRORS_POLICY_FAIL : WEBKIT_TLS_ERRORS_POLICY_IGNORE);
	webkit_web_context_set_cache_model(ctx,
		cfg->enable_disk_cache ? WEBKIT_CACHE_MODEL_WEB_BROWSER
		                       : WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);

	if (!cfg->ephemeral && cfg->cookie_jar != NULL && *cfg->cookie_jar != '\0') {
		WebKitCookieManager *cm = webkit_web_context_get_cookie_manager(ctx);
		g_autofree gchar *dir = g_path_get_dirname(cfg->cookie_jar);
		g_mkdir_with_parents(dir, 0755);
		webkit_cookie_manager_set_persistent_storage(cm, cfg->cookie_jar,
			WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
	}

	if (g_strcmp0(cfg->proxy_mode, "none") == 0) {
		webkit_web_context_set_network_proxy_settings(ctx,
			WEBKIT_NETWORK_PROXY_MODE_NO_PROXY, NULL);
	} else if (g_strcmp0(cfg->proxy_mode, "custom") == 0 &&
	           cfg->proxy_uri != NULL && *cfg->proxy_uri != '\0') {
		WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new(cfg->proxy_uri, NULL);
		webkit_web_context_set_network_proxy_settings(ctx, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
		webkit_network_proxy_settings_free(ps);
	}
	G_GNUC_END_IGNORE_DEPRECATIONS
}

static void
lrg_view_wire_signals(GsurfLrgView *self)
{
	WebKitUserContentManager *ucm;
	WebKitUserScript *tracker;

	ucm = webkit_web_view_get_user_content_manager(self->webview);
	webkit_user_content_manager_register_script_message_handler(ucm, "gsurfFocus");
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
	g_signal_connect(self->webview, "load-failed-with-tls-errors",
		G_CALLBACK(on_load_failed_tls), self);
	g_signal_connect(webkit_web_view_get_context(self->webview), "download-started",
		G_CALLBACK(on_download_started), self);
	g_signal_connect(self->webview, "mouse-target-changed",
		G_CALLBACK(on_mouse_target_changed), self);
	g_signal_connect(self->webview, "notify::favicon",
		G_CALLBACK(on_notify_favicon), self);
	g_signal_connect(self->webview, "create", G_CALLBACK(on_create), self);
	g_signal_connect(self->webview, "context-menu", G_CALLBACK(on_context_menu), self);
}

/* ============================================================= *
 *  GsurfView vfuncs (operate on @webview; mirror the GTK3 backend)
 * ============================================================= */

static void
lrg_view_load_uri(GsurfView *view, const gchar *uri)
{
	webkit_web_view_load_uri(GSURF_LRG_VIEW(view)->webview, uri);
}

static void
lrg_view_load_html(GsurfView *view, const gchar *html, const gchar *base_uri)
{
	webkit_web_view_load_html(GSURF_LRG_VIEW(view)->webview, html, base_uri);
}

static void
lrg_view_reload(GsurfView *view, gboolean bypass_cache)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
	if (bypass_cache)
		webkit_web_view_reload_bypass_cache(self->webview);
	else
		webkit_web_view_reload(self->webview);
}

static void
lrg_view_stop_loading(GsurfView *view)
{
	webkit_web_view_stop_loading(GSURF_LRG_VIEW(view)->webview);
}

static void
lrg_view_go_back(GsurfView *view)
{
	webkit_web_view_go_back(GSURF_LRG_VIEW(view)->webview);
}

static void
lrg_view_go_forward(GsurfView *view)
{
	webkit_web_view_go_forward(GSURF_LRG_VIEW(view)->webview);
}

static gboolean
lrg_view_can_go_back(GsurfView *view)
{
	return webkit_web_view_can_go_back(GSURF_LRG_VIEW(view)->webview);
}

static gboolean
lrg_view_can_go_forward(GsurfView *view)
{
	return webkit_web_view_can_go_forward(GSURF_LRG_VIEW(view)->webview);
}

static const gchar *
lrg_view_get_uri(GsurfView *view)
{
	return webkit_web_view_get_uri(GSURF_LRG_VIEW(view)->webview);
}

static const gchar *
lrg_view_get_title(GsurfView *view)
{
	return webkit_web_view_get_title(GSURF_LRG_VIEW(view)->webview);
}

static gdouble
lrg_view_get_estimated_load_progress(GsurfView *view)
{
	return webkit_web_view_get_estimated_load_progress(GSURF_LRG_VIEW(view)->webview);
}

static void
lrg_view_set_zoom_level(GsurfView *view, gdouble level)
{
	webkit_web_view_set_zoom_level(GSURF_LRG_VIEW(view)->webview, level);
}

static gdouble
lrg_view_get_zoom_level(GsurfView *view)
{
	return webkit_web_view_get_zoom_level(GSURF_LRG_VIEW(view)->webview);
}

static void
lrg_view_apply_settings(GsurfView *view, GsurfSettings *s)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
	WebKitSettings *ws = webkit_web_view_get_settings(self->webview);

	webkit_settings_set_enable_javascript(ws, s->javascript);
	webkit_settings_set_auto_load_images(ws, s->images);
	webkit_settings_set_enable_webgl(ws, s->webgl);
	webkit_settings_set_enable_webaudio(ws, s->webaudio);
	webkit_settings_set_enable_media_stream(ws, s->media_stream);
	webkit_settings_set_enable_smooth_scrolling(ws, s->smooth_scrolling);
	webkit_settings_set_enable_caret_browsing(ws, s->caret_browsing);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	webkit_settings_set_enable_dns_prefetching(ws, s->dns_prefetch);
	G_GNUC_END_IGNORE_DEPRECATIONS
	webkit_settings_set_enable_site_specific_quirks(ws, s->site_quirks);
	webkit_settings_set_enable_developer_extras(ws, s->developer_extras);
	webkit_settings_set_javascript_can_open_windows_automatically(ws, s->js_can_open_windows);
	webkit_settings_set_javascript_can_access_clipboard(ws, s->js_can_access_clipboard);
	webkit_settings_set_default_font_size(ws, s->default_font_size);
	webkit_settings_set_default_monospace_font_size(ws, s->default_monospace_font_size);

	if (s->default_charset != NULL)
		webkit_settings_set_default_charset(ws, s->default_charset);
	if (s->user_agent != NULL && *s->user_agent != '\0')
		webkit_settings_set_user_agent(ws, s->user_agent);

	/* The LRG backend always renders without GPU compositing so the page can
	 * be read back from the offscreen surface. */
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	webkit_settings_set_hardware_acceleration_policy(ws,
		WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
	G_GNUC_END_IGNORE_DEPRECATIONS
}

static void
lrg_view_run_javascript_async(GsurfView *view, const gchar *script,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{
	webkit_web_view_evaluate_javascript(GSURF_LRG_VIEW(view)->webview, script, -1,
		NULL, NULL, cancellable, callback, user_data);
}

static gchar *
lrg_view_run_javascript_finish(GsurfView *view, GAsyncResult *result, GError **error)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
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
lrg_view_get_native_widget(GsurfView *view)
{
	/* No GTK embedding for the LRG backend; the host composites the texture. */
	(void)view;
	return NULL;
}

static void
lrg_view_show_inspector(GsurfView *view)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
	WebKitSettings *ws = webkit_web_view_get_settings(self->webview);
	WebKitWebInspector *insp;

	webkit_settings_set_enable_developer_extras(ws, TRUE);
	insp = webkit_web_view_get_inspector(self->webview);
	if (insp != NULL)
		webkit_web_inspector_show(insp);
}

static void
lrg_view_add_user_script_full(GsurfView *view, const gchar *source,
                              gboolean at_end, const gchar *const *allow)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
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
lrg_view_add_user_style_full(GsurfView *view, const gchar *css,
                             const gchar *const *allow)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);
	WebKitUserStyleSheet *sheet;

	sheet = webkit_user_style_sheet_new(css, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		WEBKIT_USER_STYLE_LEVEL_USER, (const gchar * const *)allow, NULL);
	webkit_user_content_manager_add_style_sheet(ucm, sheet);
	webkit_user_style_sheet_unref(sheet);
}

typedef struct { WebKitWebView *webview; gchar *id; } FilterCtx;

static void
on_filter_saved(GObject *source, GAsyncResult *res, gpointer data)
{
	FilterCtx *ctx = data;
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
lrg_view_add_content_filter(GsurfView *view, const gchar *identifier,
                            const gchar *json_rules)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
	g_autofree gchar *dir = g_build_filename(g_get_user_cache_dir(), "gsurf", "filters", NULL);
	WebKitUserContentFilterStore *store;
	GBytes *bytes;
	FilterCtx *ctx;

	g_mkdir_with_parents(dir, 0755);
	store = webkit_user_content_filter_store_new(dir);
	bytes = g_bytes_new(json_rules, strlen(json_rules));

	ctx = g_new0(FilterCtx, 1);
	ctx->webview = self->webview;
	ctx->id = g_strdup(identifier);
	webkit_user_content_filter_store_save(store, identifier, bytes, NULL,
		on_filter_saved, ctx);

	g_bytes_unref(bytes);
	g_object_unref(store);
}

static void
lrg_view_copy_uri(GsurfView *view)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
	const gchar *uri = webkit_web_view_get_uri(self->webview);
	GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);

	if (uri != NULL)
		gtk_clipboard_set_text(cb, uri, -1);
}

static gchar *
lrg_view_read_clipboard_text(GsurfView *view)
{
	GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	(void)view;
	return gtk_clipboard_wait_for_text(cb);
}

static void
lrg_view_get_snapshot_async(GsurfView *view, GCancellable *cancellable,
                            GAsyncReadyCallback callback, gpointer user_data)
{
	webkit_web_view_get_snapshot(GSURF_LRG_VIEW(view)->webview,
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
lrg_view_get_snapshot_finish(GsurfView *view, GAsyncResult *result, GError **error)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
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
lrg_view_clear_user_content(GsurfView *view)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);

	webkit_user_content_manager_remove_all_scripts(ucm);
	webkit_user_content_manager_remove_all_style_sheets(ucm);
}

static void
lrg_view_find(GsurfView *view, const gchar *text, gboolean case_sensitive,
              gboolean forward)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
	WebKitFindController *fc = webkit_web_view_get_find_controller(self->webview);
	guint32 opts = WEBKIT_FIND_OPTIONS_WRAP_AROUND;

	if (!case_sensitive)
		opts |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;
	if (!forward)
		opts |= WEBKIT_FIND_OPTIONS_BACKWARDS;
	webkit_find_controller_search(fc, text, opts, G_MAXUINT);
}

static void
lrg_view_find_next(GsurfView *view)
{
	webkit_find_controller_search_next(
		webkit_web_view_get_find_controller(GSURF_LRG_VIEW(view)->webview));
}

static void
lrg_view_find_previous(GsurfView *view)
{
	webkit_find_controller_search_previous(
		webkit_web_view_get_find_controller(GSURF_LRG_VIEW(view)->webview));
}

static void
lrg_view_set_cookie_accept_policy(GsurfView *view, gint policy)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
	WebKitCookieManager *cm =
		webkit_web_context_get_cookie_manager(webkit_web_view_get_context(self->webview));
	WebKitCookieAcceptPolicy p =
		(policy == 1) ? WEBKIT_COOKIE_POLICY_ACCEPT_NEVER :
		(policy == 2) ? WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY :
		                WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS;

	webkit_cookie_manager_set_accept_policy(cm, p);
}

static void
lrg_view_set_proxy(GsurfView *view, const gchar *uri)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(view);
	WebKitWebContext *ctx = webkit_web_view_get_context(self->webview);

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

/* ============================================================= *
 *  LRG-only API: texture output + input
 * ============================================================= */

gboolean
gsurf_lrg_view_capture(GsurfLrgView *self)
{
	gboolean fresh;

	g_return_val_if_fail(GSURF_IS_LRG_VIEW(self), FALSE);

	fresh = gsurf_lrg_engine_capture(self->engine);
	if (fresh)
		self->frame_dirty = TRUE;
	return fresh;
}

gpointer
gsurf_lrg_view_get_texture(GsurfLrgView *self, int *out_width, int *out_height)
{
	const guint8 *px;
	int w = 0, h = 0;

	g_return_val_if_fail(GSURF_IS_LRG_VIEW(self), NULL);

	px = gsurf_lrg_engine_get_pixels(self->engine, &w, &h);
	if (px == NULL || w <= 0 || h <= 0)
		return NULL;

	if (self->texture == NULL || self->tex_w != w || self->tex_h != h) {
		g_autoptr(GrlColor) clear = grl_color_new(0, 0, 0, 0);
		g_autoptr(GrlImage) img = grl_image_new_color(w, h, clear);

		g_clear_object(&self->texture);
		grl_image_set_format(img, GRL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
		self->texture = grl_texture_new_from_image(img);
		self->tex_w = w;
		self->tex_h = h;
		self->frame_dirty = TRUE;
	}

	if (self->texture != NULL && self->frame_dirty) {
		g_autoptr(GrlRectangle) rect = grl_rectangle_new(0.0f, 0.0f,
			(gfloat)w, (gfloat)h);
		grl_texture_update_rec(self->texture, rect, px);
		self->frame_dirty = FALSE;
	}

	if (out_width != NULL)
		*out_width = w;
	if (out_height != NULL)
		*out_height = h;
	return self->texture;
}

void
gsurf_lrg_view_resize(GsurfLrgView *self, int width, int height, double scale)
{
	g_return_if_fail(GSURF_IS_LRG_VIEW(self));
	gsurf_lrg_engine_set_size(self->engine, width, height, scale);
}

void
gsurf_lrg_view_send_key(GsurfLrgView *self, guint keysym, guint keycode,
                        guint mods, gboolean pressed)
{
	g_return_if_fail(GSURF_IS_LRG_VIEW(self));
	gsurf_lrg_engine_send_key(self->engine, keysym, keycode, mods, pressed);
}

void
gsurf_lrg_view_send_motion(GsurfLrgView *self, int x, int y, guint mods)
{
	g_return_if_fail(GSURF_IS_LRG_VIEW(self));
	gsurf_lrg_engine_send_motion(self->engine, x, y, mods);
}

void
gsurf_lrg_view_send_button(GsurfLrgView *self, int x, int y, guint button,
                           gboolean pressed, guint mods)
{
	g_return_if_fail(GSURF_IS_LRG_VIEW(self));
	gsurf_lrg_engine_send_button(self->engine, x, y, button, pressed, mods);
}

void
gsurf_lrg_view_send_axis(GsurfLrgView *self, int x, int y, double dx, double dy,
                         guint mods)
{
	g_return_if_fail(GSURF_IS_LRG_VIEW(self));
	gsurf_lrg_engine_send_axis(self->engine, x, y, dx, dy, mods);
}

void
gsurf_lrg_view_set_focus(GsurfLrgView *self, gboolean focused)
{
	g_return_if_fail(GSURF_IS_LRG_VIEW(self));
	if (self->focused == focused)
		return;
	self->focused = focused;
	gsurf_lrg_engine_set_focus(self->engine, focused);
}

gboolean
gsurf_lrg_view_get_focus(GsurfLrgView *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_VIEW(self), FALSE);
	return self->focused;
}

/* ============================================================= *
 *  GObject lifecycle
 * ============================================================= */

static void
gsurf_lrg_view_constructed(GObject *object)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(object);
	g_autoptr(GError) error = NULL;

	G_OBJECT_CLASS(gsurf_lrg_view_parent_class)->constructed(object);

	self->engine = gsurf_lrg_engine_new(GSURF_VIEW(self), &error);
	if (self->engine == NULL) {
		g_warning("gsurf LRG: engine init failed: %s",
		          error ? error->message : "unknown");
		return;
	}
	self->webview = gsurf_lrg_engine_get_webview(self->engine);

	lrg_view_wire_signals(self);
	lrg_view_apply_context_config(self);

	gsurf_module_manager_dispatch_inject_scripts(
		gsurf_module_manager_get_default(), GSURF_VIEW(self), NULL);
}

static void
gsurf_lrg_view_dispose(GObject *object)
{
	GsurfLrgView *self = GSURF_LRG_VIEW(object);

	g_clear_object(&self->texture);
	if (self->engine != NULL) {
		if (self->webview != NULL)
			g_signal_handlers_disconnect_by_data(self->webview, self);
		self->webview = NULL;
		g_clear_pointer(&self->engine, gsurf_lrg_engine_free);
	}

	G_OBJECT_CLASS(gsurf_lrg_view_parent_class)->dispose(object);
}

static void
gsurf_lrg_view_class_init(GsurfLrgViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfViewClass *view_class = GSURF_VIEW_CLASS(klass);

	object_class->constructed = gsurf_lrg_view_constructed;
	object_class->dispose = gsurf_lrg_view_dispose;

	view_class->load_uri = lrg_view_load_uri;
	view_class->load_html = lrg_view_load_html;
	view_class->reload = lrg_view_reload;
	view_class->stop_loading = lrg_view_stop_loading;
	view_class->go_back = lrg_view_go_back;
	view_class->go_forward = lrg_view_go_forward;
	view_class->can_go_back = lrg_view_can_go_back;
	view_class->can_go_forward = lrg_view_can_go_forward;
	view_class->get_uri = lrg_view_get_uri;
	view_class->get_title = lrg_view_get_title;
	view_class->get_estimated_load_progress = lrg_view_get_estimated_load_progress;
	view_class->set_zoom_level = lrg_view_set_zoom_level;
	view_class->get_zoom_level = lrg_view_get_zoom_level;
	view_class->apply_settings = lrg_view_apply_settings;
	view_class->run_javascript_async = lrg_view_run_javascript_async;
	view_class->run_javascript_finish = lrg_view_run_javascript_finish;
	view_class->get_native_widget = lrg_view_get_native_widget;
	view_class->get_snapshot_async = lrg_view_get_snapshot_async;
	view_class->get_snapshot_finish = lrg_view_get_snapshot_finish;
	view_class->show_inspector = lrg_view_show_inspector;
	view_class->add_user_script_full = lrg_view_add_user_script_full;
	view_class->add_user_style_full = lrg_view_add_user_style_full;
	view_class->clear_user_content = lrg_view_clear_user_content;
	view_class->add_content_filter = lrg_view_add_content_filter;
	view_class->copy_uri = lrg_view_copy_uri;
	view_class->read_clipboard_text = lrg_view_read_clipboard_text;
	view_class->find = lrg_view_find;
	view_class->find_next = lrg_view_find_next;
	view_class->find_previous = lrg_view_find_previous;
	view_class->set_cookie_accept_policy = lrg_view_set_cookie_accept_policy;
	view_class->set_proxy = lrg_view_set_proxy;
}

static void
gsurf_lrg_view_init(GsurfLrgView *self)
{
	/* Instance fields are zero-initialized; the engine is created in
	 * constructed(). */
}

GsurfView *
gsurf_lrg_view_new(void)
{
	return g_object_new(GSURF_TYPE_LRG_VIEW, NULL);
}
