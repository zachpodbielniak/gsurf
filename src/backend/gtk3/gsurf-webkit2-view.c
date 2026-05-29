/*
 * gsurf-webkit2-view.c - WebKit2GTK 4.1 / GTK3 web view backend
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "backend/gtk3/gsurf-webkit2-view.h"
#include "module/gsurf-module-manager.h"

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

struct _GsurfWebkit2View
{
	GsurfView parent_instance;

	WebKitWebView *webview; /* the native GtkWidget; we hold a ref */
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

static gboolean
on_permission_request(WebKitWebView *wv, WebKitPermissionRequest *req, gpointer user_data)
{
	GsurfWebkit2View *self = user_data;
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
	return FALSE; /* PROMPT: leave to the engine default */
}

static gboolean
on_download_decide_destination(WebKitDownload *download, const gchar *suggested, gpointer user_data)
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
on_download_started(WebKitWebContext *context, WebKitDownload *download, gpointer user_data)
{
	g_signal_connect(download, "decide-destination",
		G_CALLBACK(on_download_decide_destination), user_data);
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
	webkit_settings_set_enable_media_stream(ws, s->media_stream);
	webkit_settings_set_enable_smooth_scrolling(ws, s->smooth_scrolling);
	webkit_settings_set_enable_caret_browsing(ws, s->caret_browsing);
	/* These two are deprecated in newer WebKitGTK but harmless. */
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	webkit_settings_set_enable_dns_prefetching(ws, s->dns_prefetch);
	webkit_settings_set_enable_hyperlink_auditing(ws, s->hyperlink_auditing);
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
gsurf_webkit2_view_add_user_script(GsurfView *view, const gchar *source, gboolean at_end)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);
	WebKitUserScript *script;

	script = webkit_user_script_new(source, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		at_end ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END
		       : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
		NULL, NULL);
	webkit_user_content_manager_add_script(ucm, script);
	webkit_user_script_unref(script);
}

static void
gsurf_webkit2_view_add_user_style(GsurfView *view, const gchar *css)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(view);
	WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(self->webview);
	WebKitUserStyleSheet *sheet;

	sheet = webkit_user_style_sheet_new(css, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
	webkit_user_content_manager_add_style_sheet(ucm, sheet);
	webkit_user_style_sheet_unref(sheet);
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

/* --- GObject lifecycle --- */

static void
gsurf_webkit2_view_constructed(GObject *object)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(object);
	GtkWidget *widget;

	G_OBJECT_CLASS(gsurf_webkit2_view_parent_class)->constructed(object);

	widget = webkit_web_view_new();
	self->webview = WEBKIT_WEB_VIEW(g_object_ref_sink(widget));

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
	g_signal_connect(webkit_web_view_get_context(self->webview), "download-started",
		G_CALLBACK(on_download_started), self);

	/* Let script-injector modules add user scripts/styles to this view. */
	gsurf_module_manager_dispatch_inject_scripts(
		gsurf_module_manager_get_default(), GSURF_VIEW(self), NULL);
}

static void
gsurf_webkit2_view_dispose(GObject *object)
{
	GsurfWebkit2View *self = GSURF_WEBKIT2_VIEW(object);

	if (self->webview != NULL) {
		g_signal_handlers_disconnect_by_data(self->webview, self);
		g_clear_object(&self->webview);
	}

	G_OBJECT_CLASS(gsurf_webkit2_view_parent_class)->dispose(object);
}

static void
gsurf_webkit2_view_class_init(GsurfWebkit2ViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfViewClass *view_class = GSURF_VIEW_CLASS(klass);

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
	view_class->show_inspector = gsurf_webkit2_view_show_inspector;
	view_class->add_user_script = gsurf_webkit2_view_add_user_script;
	view_class->add_user_style = gsurf_webkit2_view_add_user_style;
	view_class->clear_user_content = gsurf_webkit2_view_clear_user_content;
	view_class->find = gsurf_webkit2_view_find;
	view_class->find_next = gsurf_webkit2_view_find_next;
	view_class->find_previous = gsurf_webkit2_view_find_previous;
	view_class->set_cookie_accept_policy = gsurf_webkit2_view_set_cookie_accept_policy;
	view_class->set_proxy = gsurf_webkit2_view_set_proxy;
}

static void
gsurf_webkit2_view_init(GsurfWebkit2View *self)
{
}

GsurfView *
gsurf_webkit2_view_new(void)
{
	return g_object_new(GSURF_TYPE_WEBKIT2_VIEW, NULL);
}
