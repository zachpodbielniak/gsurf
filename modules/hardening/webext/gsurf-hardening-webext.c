/*
 * gsurf-hardening-webext.c - hardening web-process extension
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loaded by WebKit into every web process (the hardening module registers
 * it through gsurf_web_process_extension_register()). It enforces the
 * per-request protections, all decided by the shared
 * gsurf-hardening-rules.c:
 *   - "send-request": https-only upgrade, tracking-parameter stripping,
 *     cross-origin Referer trimming, `Sec-GPC` / `DNT` headers, and
 *     cancelling speculative requests (link prefetch, `<a ping>`);
 *   - "window-object-cleared" (main world): `navigator.globalPrivacyControl`
 *     before the document's own scripts run.
 * Exemptions are looked up by the TOP-LEVEL page host, so one site
 * exception covers every request the page makes, whatever host it goes to.
 * The policy arrives once per web process as the "hardening" entry of the
 * initialization data (gsurf_hardening_policy_to_variant()).
 */

#ifdef GSURF_BACKEND_GTK4
#include <webkit/webkit-web-process-extension.h>
typedef WebKitWebProcessExtension GsurfWebExtension;
#else
#include <webkit2/webkit-web-extension.h>
typedef WebKitWebExtension GsurfWebExtension;
#endif
#include <libsoup/soup.h>
#include <string.h>
#include "../gsurf-hardening-rules.h"

/* The policy of this web process; set once in initialize(). */
static GsurfHardeningPolicy *policy;

/* The host the top-level page lives on: the request's own host when the
 * request is the top-level document (or a redirect of it), else the
 * host of the page that issued it. */
static gchar *
top_level_host(WebKitWebPage *page, const gchar *uri, WebKitURIResponse *redirected_response)
{
	const gchar *page_uri = webkit_web_page_get_uri(page);
	const gchar *source = page_uri;
	g_autoptr(GUri) parsed = NULL;
	const gchar *host;

	/* WebKit updates the page URI when the main frame starts its
	 * provisional load, before the document request is sent, so the
	 * document request (and each of its redirects) is recognized by
	 * URI. */
	if (g_strcmp0(uri, page_uri) == 0)
		source = uri;
	else if (redirected_response != NULL &&
	         g_strcmp0(webkit_uri_response_get_uri(redirected_response), page_uri) == 0)
		source = uri;
	if (source == NULL)
		return NULL;
	parsed = g_uri_parse(source, G_URI_FLAGS_ENCODED | G_URI_FLAGS_NON_DNS, NULL);
	if (parsed == NULL || (host = g_uri_get_host(parsed)) == NULL || *host == '\0')
		return NULL;
	return g_ascii_strdown(host, -1);
}

/* Prefetches and hyperlink-auditing pings are never asked for by the
 * user; recognize them from their headers. */
static gboolean
request_is_speculative(SoupMessageHeaders *headers)
{
	const gchar *purpose;

	if (headers == NULL)
		return FALSE;
	purpose = soup_message_headers_get_one(headers, "Purpose");
	if (purpose == NULL)
		purpose = soup_message_headers_get_one(headers, "X-Purpose");
	return gsurf_hardening_is_speculative(purpose,
		soup_message_headers_get_one(headers, "Sec-Purpose"),
		soup_message_headers_get_one(headers, "Ping-To"),
		soup_message_headers_get_one(headers, "Content-Type"));
}

/* Apply the Referer policy for a request going to @target. */
static void
rewrite_referer(SoupMessageHeaders *headers, const gchar *target)
{
	g_autofree gchar *replacement = NULL;
	const gchar *referer = soup_message_headers_get_one(headers, "Referer");

	switch (gsurf_hardening_referer_rewrite(policy->referer, referer, target, &replacement)) {
	case GSURF_HARDENING_REFERER_REPLACE:
		soup_message_headers_replace(headers, "Referer", replacement);
		break;
	case GSURF_HARDENING_REFERER_REMOVE:
		soup_message_headers_remove(headers, "Referer");
		break;
	case GSURF_HARDENING_REFERER_KEEP:
	default:
		break;
	}
}

static gboolean
on_send_request(WebKitWebPage *page, WebKitURIRequest *request,
                WebKitURIResponse *redirected_response, gpointer user_data)
{
	const gchar *uri = webkit_uri_request_get_uri(request);
	SoupMessageHeaders *headers = webkit_uri_request_get_http_headers(request);
	g_autofree gchar *top_host = NULL;
	g_autofree gchar *rewritten = NULL;
	guint features;

	if (uri == NULL || (!g_str_has_prefix(uri, "http:") && !g_str_has_prefix(uri, "https:")))
		return FALSE;
	top_host = top_level_host(page, uri, redirected_response);
	features = gsurf_hardening_policy_features_for_host(policy, top_host);
	if (features == 0)
		return FALSE;

	/* 1. Speculative requests are cancelled outright. */
	if ((features & GSURF_HARDENING_NO_PREFETCH) && request_is_speculative(headers))
		return TRUE;

	/* 2. https-only. A same-host https -> http redirect would be upgraded
	 *    straight back and loop until WebKit gives up; fail it now, like
	 *    Firefox's HTTPS-Only mode refuses the downgrade. */
	if (features & GSURF_HARDENING_HTTPS_ONLY) {
		if (redirected_response != NULL &&
		    gsurf_hardening_is_downgrade(webkit_uri_response_get_uri(redirected_response), uri))
			return TRUE;
		rewritten = gsurf_hardening_upgrade_uri(uri, policy->https_only_local);
	}

	/* 3. Tracking parameters, on the (possibly upgraded) URI. */
	if (features & GSURF_HARDENING_STRIP_PARAMS) {
		gchar *stripped = gsurf_hardening_strip_params(rewritten != NULL ? rewritten : uri,
			(const gchar * const *)policy->strip_params);

		if (stripped != NULL) {
			g_free(rewritten);
			rewritten = stripped;
		}
	}
	if (rewritten != NULL)
		webkit_uri_request_set_uri(request, rewritten);

	/* 4. Headers. Some internal requests carry none. */
	if (headers == NULL)
		return FALSE;
	if (features & GSURF_HARDENING_REFERER)
		rewrite_referer(headers, rewritten != NULL ? rewritten : uri);
	if (features & GSURF_HARDENING_GPC)
		soup_message_headers_replace(headers, "Sec-GPC", "1");
	if (features & GSURF_HARDENING_DNT)
		soup_message_headers_replace(headers, "DNT", "1");
	return FALSE;
}

/* A frame's window object was (re)created for a new document: expose
 * Global Privacy Control before any of the document's scripts run,
 * unless the top-level page's host is exempt from gpc. */
static void
on_window_object_cleared(WebKitScriptWorld *world, WebKitWebPage *page, WebKitFrame *frame,
                         gpointer user_data)
{
	/* The page URI is the top-level document's, for subframes too (and
	 * already the new document's while the main frame commits). */
	const gchar *top_uri = webkit_web_page_get_uri(page);
	g_autoptr(GUri) parsed = NULL;
	g_autofree gchar *host = NULL;
	g_autoptr(JSCContext) context = NULL;
	g_autoptr(JSCValue) result = NULL;

	if (top_uri == NULL)
		return;
	parsed = g_uri_parse(top_uri, G_URI_FLAGS_ENCODED | G_URI_FLAGS_NON_DNS, NULL);
	if (parsed != NULL && g_uri_get_host(parsed) != NULL)
		host = g_ascii_strdown(g_uri_get_host(parsed), -1);
	if (!(gsurf_hardening_policy_features_for_host(policy, host) & GSURF_HARDENING_GPC))
		return;

	context = webkit_frame_get_js_context_for_script_world(frame, world);
	result = jsc_context_evaluate(context, GSURF_HARDENING_GPC_JS, -1);
	if (jsc_context_get_exception(context) != NULL)
		jsc_context_clear_exception(context);
}

static void
on_page_created(GsurfWebExtension *extension, WebKitWebPage *page, gpointer user_data)
{
	g_signal_connect(page, "send-request", G_CALLBACK(on_send_request), NULL);
}

static void
initialize(GsurfWebExtension *extension, GVariant *user_data)
{
	g_autoptr(GVariant) mine = NULL;
	guint features;

	if (user_data == NULL || !g_variant_is_of_type(user_data, G_VARIANT_TYPE_VARDICT))
		return;
	mine = g_variant_lookup_value(user_data, "hardening", G_VARIANT_TYPE_VARDICT);
	if (mine == NULL)
		return;

	/* The policy lives as long as the web process. */
	policy = gsurf_hardening_policy_new_from_variant(mine);
	features = gsurf_hardening_policy_get_features(policy);
	if (features & (GSURF_HARDENING_HTTPS_ONLY | GSURF_HARDENING_STRIP_PARAMS |
	                GSURF_HARDENING_REFERER | GSURF_HARDENING_GPC | GSURF_HARDENING_DNT |
	                GSURF_HARDENING_NO_PREFETCH))
		g_signal_connect(extension, "page-created", G_CALLBACK(on_page_created), NULL);
	if (features & GSURF_HARDENING_GPC)
		g_signal_connect(webkit_script_world_get_default(), "window-object-cleared",
			G_CALLBACK(on_window_object_cleared), NULL);
}

#ifdef GSURF_BACKEND_GTK4
G_MODULE_EXPORT void
webkit_web_process_extension_initialize_with_user_data(WebKitWebProcessExtension *extension,
                                                       const GVariant *user_data)
{
	initialize(extension, (GVariant *)user_data);
}
#else
G_MODULE_EXPORT void
webkit_web_extension_initialize_with_user_data(WebKitWebExtension *extension,
                                               const GVariant *user_data)
{
	initialize(extension, (GVariant *)user_data);
}
#endif
