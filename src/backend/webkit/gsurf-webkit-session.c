/*
 * gsurf-webkit-session.c - Per-container WebKit sessions
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "backend/webkit/gsurf-webkit-session.h"
#include "backend/webkit/gsurf-webkit-content.h"
#include "backend/webkit/gsurf-webkit-common.h"
#include "config/gsurf-config.h"
#include "core/gsurf-container-registry.h"
#include "util/gsurf-private-file.h"

/* Map gsurf's cookie policy number onto WebKit's enum. */
static WebKitCookieAcceptPolicy
cookie_policy_from_int(gint policy)
{
	switch (policy) {
	case 1:  return WEBKIT_COOKIE_POLICY_ACCEPT_NEVER;
	case 2:  return WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY;
	default: return WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS;
	}
}

/* Point a cookie manager at an SQLite store, creating its directory
 * owner-only (cookies are credentials). */
static void
set_cookie_storage(WebKitCookieManager *cm, const gchar *path)
{
	g_autofree gchar *dir = NULL;
	g_autoptr(GError) error = NULL;

	if (path == NULL || *path == '\0')
		return;
	dir = g_path_get_dirname(path);
	if (!gsurf_private_mkdir(dir, &error)) {
		g_warning("gsurf: cookie storage: %s", error->message);
		return;
	}
	webkit_cookie_manager_set_persistent_storage(cm, path,
		WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
}

#ifdef GSURF_BACKEND_GTK4
/* ---------------- WebKitGTK 6.0: WebKitNetworkSession ---------------- */

/* Configure TLS, cookies and proxy on one network session. The cache
 * model lives on the (shared) web context in 6.0. */
static void
prepare_session(WebKitNetworkSession *ns, const gchar *cookie_path)
{
	GsurfConfig *cfg = gsurf_config_get_default();

	if (cfg == NULL)
		return;
	webkit_network_session_set_tls_errors_policy(ns,
		cfg->tls_strict ? WEBKIT_TLS_ERRORS_POLICY_FAIL : WEBKIT_TLS_ERRORS_POLICY_IGNORE);
	set_cookie_storage(webkit_network_session_get_cookie_manager(ns), cookie_path);

	if (g_strcmp0(cfg->proxy_mode, "none") == 0) {
		webkit_network_session_set_proxy_settings(ns, WEBKIT_NETWORK_PROXY_MODE_NO_PROXY, NULL);
	} else if (g_strcmp0(cfg->proxy_mode, "custom") == 0 &&
	           cfg->proxy_uri != NULL && *cfg->proxy_uri != '\0') {
		WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new(cfg->proxy_uri, NULL);

		webkit_network_session_set_proxy_settings(ns, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
		webkit_network_proxy_settings_free(ps);
	}
}

static gpointer
session_create(const gchar *id, const gchar *data_dir, const gchar *cache_dir)
{
	WebKitNetworkSession *ns;
	g_autofree gchar *cookies = NULL;

	if (data_dir == NULL) {
		ns = webkit_network_session_new_ephemeral();
	} else {
		ns = webkit_network_session_new(data_dir, cache_dir);
		cookies = g_build_filename(data_dir, "cookies.sqlite", NULL);
	}
	prepare_session(ns, cookies);
	return ns;
}

static void
session_apply_policy(gpointer native, gboolean itp, gint cookie_policy)
{
	WebKitNetworkSession *ns = native != NULL ? native : webkit_network_session_get_default();

	webkit_network_session_set_itp_enabled(ns, itp);
	if (cookie_policy >= 0)
		webkit_cookie_manager_set_accept_policy(
			webkit_network_session_get_cookie_manager(ns),
			cookie_policy_from_int(cookie_policy));
}

void
gsurf_webkit_session_prepare_default(void)
{
	GsurfConfig *cfg = gsurf_config_get_default();
	WebKitWebContext *ctx = webkit_web_context_get_default();

	if (cfg == NULL)
		return;
	webkit_web_context_set_cache_model(ctx,
		cfg->enable_disk_cache ? WEBKIT_CACHE_MODEL_WEB_BROWSER
		                       : WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
	prepare_session(webkit_network_session_get_default(),
		cfg->ephemeral ? NULL : cfg->cookie_jar);
}

WebKitWebContext *
gsurf_webkit_session_get_context(gpointer native)
{
	(void)native;
	return webkit_web_context_get_default();
}

#else
/* ---------------- WebKitGTK 4.1: WebKitWebContext ---------------- */

/* Configure TLS, cache, cookies and proxy on one web context. These
 * context-level setters are deprecated in favour of the website data
 * manager API in newer WebKitGTK, but they are what 4.1 exposes. */
static void
prepare_context(WebKitWebContext *ctx, const gchar *cookie_path)
{
	GsurfConfig *cfg = gsurf_config_get_default();

	if (cfg == NULL)
		return;

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	webkit_web_context_set_tls_errors_policy(ctx,
		cfg->tls_strict ? WEBKIT_TLS_ERRORS_POLICY_FAIL : WEBKIT_TLS_ERRORS_POLICY_IGNORE);
	webkit_web_context_set_cache_model(ctx,
		cfg->enable_disk_cache ? WEBKIT_CACHE_MODEL_WEB_BROWSER
		                       : WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
	set_cookie_storage(webkit_web_context_get_cookie_manager(ctx), cookie_path);

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

static gpointer
session_create(const gchar *id, const gchar *data_dir, const gchar *cache_dir)
{
	WebKitWebContext *ctx;
	g_autofree gchar *cookies = NULL;

	if (data_dir == NULL) {
		ctx = webkit_web_context_new_ephemeral();
	} else {
		g_autoptr(WebKitWebsiteDataManager) mgr = webkit_website_data_manager_new(
			"base-data-directory", data_dir,
			"base-cache-directory", cache_dir,
			NULL);

		ctx = webkit_web_context_new_with_website_data_manager(mgr);
		cookies = g_build_filename(data_dir, "cookies.sqlite", NULL);
	}
	/* Web-process extensions (adblock, hardening) must reach this
	 * context's processes too. */
	gsurf_webkit_content_attach_context(ctx);
	prepare_context(ctx, cookies);
	return ctx;
}

static void
session_apply_policy(gpointer native, gboolean itp, gint cookie_policy)
{
	WebKitWebContext *ctx = native != NULL ? native : webkit_web_context_get_default();

	webkit_website_data_manager_set_itp_enabled(
		webkit_web_context_get_website_data_manager(ctx), itp);
	if (cookie_policy >= 0)
		webkit_cookie_manager_set_accept_policy(
			webkit_web_context_get_cookie_manager(ctx),
			cookie_policy_from_int(cookie_policy));
}

void
gsurf_webkit_session_prepare_default(void)
{
	GsurfConfig *cfg = gsurf_config_get_default();

	if (cfg == NULL)
		return;
	prepare_context(webkit_web_context_get_default(),
		cfg->ephemeral ? NULL : cfg->cookie_jar);
}

WebKitWebContext *
gsurf_webkit_session_get_context(gpointer native)
{
	return native != NULL ? WEBKIT_WEB_CONTEXT(native) : webkit_web_context_get_default();
}
#endif

static void
on_download_started(GObject *session, WebKitDownload *download, gpointer user_data)
{
	(void)session;
	(void)user_data;
	gsurf_webkit_common_track_download(download);
}

void
gsurf_webkit_session_track_downloads(WebKitWebView *webview)
{
#ifdef GSURF_BACKEND_GTK4
	GObject *session = G_OBJECT(webkit_web_view_get_network_session(webview));
#else
	GObject *session = G_OBJECT(webkit_web_view_get_context(webview));
#endif

	if (session == NULL || g_object_get_data(session, "gsurf-downloads-tracked") != NULL)
		return;
	g_object_set_data(session, "gsurf-downloads-tracked", GINT_TO_POINTER(1));
	g_signal_connect(session, "download-started", G_CALLBACK(on_download_started), NULL);
}

static void
session_destroy(gpointer native)
{
	g_object_unref(native);
}

static const GsurfSessionBackend webkit_session_backend = {
	session_create,
	session_destroy,
	session_apply_policy,
};

void
gsurf_webkit_session_install(void)
{
	gsurf_container_registry_set_session_backend(
		gsurf_container_registry_get_default(), &webkit_session_backend);
}
