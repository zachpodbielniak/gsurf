/*
 * Shared scheme adapter for GTK3, GTK4, and the offscreen LRG engine.
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "gsurf-webkit-protocol.h"
#include "protocol/gsurf-protocol.h"
#include "config/gsurf-config.h"
#include <libsoup/soup.h>

/* Keep a weak view reference: closing a tab cancels I/O and never makes a
 * late completion call into a destroyed view. Requests are finished once. */
typedef struct {
	WebKitURISchemeRequest *request;
	GWeakRef view;
	GCancellable *cancellable;
} SchemeRequest;

/* GTK3 proxy policy belongs to the context; GTK4 uses a network session. */
static GObject *
proxy_owner(WebKitWebView *view)
{
#ifdef GSURF_BACKEND_GTK4
	return G_OBJECT(webkit_web_view_get_network_session(view));
#else
	return G_OBJECT(webkit_web_view_get_context(view));
#endif
}

/* Keep explicit gsurf proxy changes effective for non-HTTP protocols too. */
void
gsurf_webkit_protocol_set_proxy(WebKitWebView *view, const gchar *uri)
{
	g_object_set_data_full(proxy_owner(view), "gsurf-protocol-proxy", g_strdup(uri), g_free);
}

/* Called when the view navigates or releases its current cancellable. */
static void
cancel_request(gpointer data)
{
	g_cancellable_cancel(G_CANCELLABLE(data));
	g_object_unref(data);
}

/* Explicit owner teardown must cancel even when an embedder or WebKit
 * retains the native view after the library wrapper has been disposed. */
void
gsurf_webkit_protocol_cancel(WebKitWebView *view)
{
	g_object_set_data(G_OBJECT(view), "gsurf-protocol-cancellable", NULL);
}

/* Finish with an explicit content policy, including server-supplied HTML.
 * Small-web pages can display passive content, but cannot execute scripts
 * or make automatic HTTP fetches. Downloads retain the exact body bytes. */
static void
finish_content(WebKitURISchemeRequest *request, GBytes *bytes, const gchar *mime)
{
	g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes(bytes);
	g_autoptr(WebKitURISchemeResponse) response = webkit_uri_scheme_response_new(stream, g_bytes_get_size(bytes));
	SoupMessageHeaders *headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
	webkit_uri_scheme_response_set_content_type(response, mime);
	soup_message_headers_append(headers, "Content-Security-Policy",
		"default-src 'none'; style-src 'unsafe-inline'; img-src data: gopher: gemini:; "
		"form-action gopher: gemini:; base-uri 'none'; frame-ancestors 'none'");
	soup_message_headers_append(headers, "X-Content-Type-Options", "nosniff");
	webkit_uri_scheme_response_set_http_headers(response, headers);
	/* set_http_headers takes full ownership of headers. */
	webkit_uri_scheme_request_finish_with_response(request, response);
}

/* Apply only to the request that started this worker; an obsolete completion
 * is cancelled without replacing the current page or its form state. */
static void
fetch_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	SchemeRequest *pending = user_data;
	g_autoptr(WebKitWebView) view = g_weak_ref_get(&pending->view);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfProtocolResponse) response = gsurf_protocol_fetch_finish(result, &error);
	g_autofree gchar *html = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *target = NULL;
	const gchar *uri = webkit_uri_scheme_request_get_uri(pending->request);

	if (view == NULL || g_cancellable_is_cancelled(pending->cancellable)) {
		g_clear_error(&error);
		error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Navigation cancelled");
		webkit_uri_scheme_request_finish_error(pending->request, error);
	} else {
		if (response == NULL) {
			html = gsurf_protocol_message_page("Unable to load page", error->message, NULL);
		} else if (response->status / 10 == 1) {
			html = gsurf_protocol_input_page(uri, response->meta, response->status == 11);
		} else if (response->status / 10 == 3) {
			target = g_uri_resolve_relative(uri, response->meta, G_URI_FLAGS_ENCODED, NULL);
			html = gsurf_protocol_message_page("Gemini redirect", response->meta, target);
		} else if (response->status / 10 != 2) {
			title = g_strdup_printf("Gemini status %d%s", response->status,
				response->status / 10 == 6 ? " — client certificate required (unsupported)" : "");
			html = gsurf_protocol_message_page(title, response->meta, NULL);
		}
		if (html != NULL) {
			bytes = g_bytes_new(html, strlen(html));
			finish_content(pending->request, bytes, "text/html; charset=utf-8");
		} else {
			finish_content(pending->request, response->body, response->mime);
		}
	}
	g_weak_ref_clear(&pending->view);
	g_object_unref(pending->cancellable);
	g_object_unref(pending->request);
	g_free(pending);
}

/* The context may outlive every gsurf view. The callback owns no host state;
 * any view using that context can retrieve the registered schemes. */
static void
scheme_requested(WebKitURISchemeRequest *request, gpointer data)
{
	WebKitWebView *view = webkit_uri_scheme_request_get_web_view(request);
	SchemeRequest *pending = g_new0(SchemeRequest, 1);
	GCancellable *group = view != NULL ?
		g_object_get_data(G_OBJECT(view), "gsurf-protocol-cancellable") : NULL;
	pending->request = g_object_ref(request);
	pending->cancellable = group != NULL ? g_object_ref(group) : g_cancellable_new();
	g_weak_ref_init(&pending->view, view);
	if (view != NULL && group == NULL)
		g_object_set_data_full(G_OBJECT(view), "gsurf-protocol-cancellable",
			g_object_ref(pending->cancellable), cancel_request);
	gsurf_protocol_fetch_async(webkit_uri_scheme_request_get_uri(request), NULL,
		view != NULL ? g_object_get_data(proxy_owner(view), "gsurf-protocol-proxy") : NULL,
		pending->cancellable, fetch_done, pending);
}

/* The stop action changes is-loading without starting another navigation. */
static void
loading_changed(WebKitWebView *view, GParamSpec *pspec, gpointer data)
{
	if (!webkit_web_view_is_loading(view))
		g_object_set_data(G_OBJECT(view), "gsurf-protocol-cancellable", NULL);
}

/* Navigation interrupts blocked socket I/O from the previous document. */
static void
load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
	if (event == WEBKIT_LOAD_STARTED) {
		gsurf_webkit_protocol_cancel(view);
	}
}

/* WebKit handles form encoding; the protocol wants a raw encoded query (or
 * selector TAB query), not a named HTML field. Recognize same-resource forms
 * by their field and URI, including prompts restored from the back/forward
 * cache without a new scheme request. No transient prompt flag can do that. */
static gboolean
decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
	WebKitPolicyDecisionType type, gpointer data)
{
	WebKitNavigationAction *action;
	g_autoptr(GUri) current = NULL;
	g_autoptr(GUri) target = NULL;
	g_autofree gchar *uri = NULL;

	if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
		return FALSE;
	action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
	if (webkit_navigation_action_get_navigation_type(action) != WEBKIT_NAVIGATION_TYPE_FORM_SUBMITTED)
		return FALSE;
	if (webkit_web_view_get_uri(view) == NULL)
		return FALSE;
	current = g_uri_parse(webkit_web_view_get_uri(view), G_URI_FLAGS_ENCODED, NULL);
	target = g_uri_parse(webkit_uri_request_get_uri(webkit_navigation_action_get_request(action)), G_URI_FLAGS_ENCODED, NULL);
	if (current == NULL || target == NULL ||
	    g_strcmp0(g_uri_get_scheme(current), g_uri_get_scheme(target)) != 0 ||
	    g_strcmp0(g_uri_get_host(current), g_uri_get_host(target)) != 0 ||
	    g_uri_get_port(current) != g_uri_get_port(target) ||
	    g_strcmp0(g_uri_get_path(current), g_uri_get_path(target)) != 0)
		return FALSE;
	uri = gsurf_protocol_input_uri(webkit_uri_request_get_uri(webkit_navigation_action_get_request(action)), NULL);
	if (uri == NULL)
		return FALSE;
	webkit_policy_decision_ignore(decision);
	webkit_web_view_load_uri(view, uri);
	return TRUE;
}

/* Called before the first load on each native view, including LRG's hidden
 * WebView. Registration belongs to the context, while state belongs to views. */
void
gsurf_webkit_protocol_attach(WebKitWebView *view)
{
	WebKitWebContext *context = webkit_web_view_get_context(view);
	GsurfConfig *config = gsurf_config_get_default();
	if (g_object_get_data(G_OBJECT(context), "gsurf-protocol-registered") == NULL) {
		webkit_web_context_register_uri_scheme(context, "gopher", scheme_requested, NULL, NULL);
		webkit_web_context_register_uri_scheme(context, "gemini", scheme_requested, NULL, NULL);
		g_object_set_data(G_OBJECT(context), "gsurf-protocol-registered", GINT_TO_POINTER(1));
	}
	if (g_object_get_data(G_OBJECT(view), "gsurf-protocol-attached") != NULL)
		return;
	if (config != NULL) {
		if (g_strcmp0(config->proxy_mode, "none") == 0)
			gsurf_webkit_protocol_set_proxy(view, "direct://");
		else if (g_strcmp0(config->proxy_mode, "custom") == 0 &&
		    config->proxy_uri != NULL && *config->proxy_uri != '\0')
			gsurf_webkit_protocol_set_proxy(view, config->proxy_uri);
	}
	g_object_set_data(G_OBJECT(view), "gsurf-protocol-attached", GINT_TO_POINTER(1));
	g_signal_connect(view, "load-changed", G_CALLBACK(load_changed), NULL);
	g_signal_connect(view, "notify::is-loading", G_CALLBACK(loading_changed), NULL);
	g_signal_connect(view, "decide-policy", G_CALLBACK(decide_policy), NULL);
}
