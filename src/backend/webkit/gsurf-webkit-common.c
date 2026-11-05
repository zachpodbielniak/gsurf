/*
 * gsurf-webkit-common.c - WebKit signal handling shared by GTK3 and GTK4
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "backend/webkit/gsurf-webkit-common.h"
#include "boxed/gsurf-hit-test.h"
#include "boxed/gsurf-menu-item.h"
#include "module/gsurf-module-manager.h"

/* --- focus tracking --- */

/*
 * Per frame: the deep active element (through open shadow roots, since
 * document.activeElement is only the shadow host); a frame whose deep
 * active element is an <iframe>/<frame> says nothing (that subframe
 * reports). Editable: contenteditable (inherited), <textarea>, <select>,
 * text-like <input> (text, search, email, url, tel, password, number,
 * date, ...; not buttons, checkboxes, files, ranges, colours) and
 * anything inside role=textbox/searchbox/combobox. Reports on focusin,
 * on focusout (once the focus has settled), at start (top frame always,
 * resetting the state for a new page; a subframe only when it starts
 * with a focused field) and when a frame goes away while focused.
 */
const gchar gsurf_webkit_focus_tracker_js[] =
	"(function(){if(window.__gsurfFT)return;window.__gsurfFT=1;"
	"var id=Math.random().toString(36).slice(2)+Date.now().toString(36);"
	"var top=false;try{top=(window.top===window);}catch(x){}"
	"var NOT=/^(button|submit|reset|checkbox|radio|file|image|range|color|hidden)$/;"
	"function deep(){var a=document.activeElement;"
	"while(a&&a.shadowRoot&&a.shadowRoot.activeElement)a=a.shadowRoot.activeElement;return a;}"
	"function ed(e){if(!e||e.nodeType!==1)return false;if(e.isContentEditable)return true;"
	"var t=e.tagName;if(t==='TEXTAREA'||t==='SELECT')return true;"
	"if(t==='INPUT')return !NOT.test(String(e.type||'text').toLowerCase());"
	"try{if(e.closest('[role=textbox],[role=searchbox],[role=combobox]'))return true;}catch(x){}"
	"return false;}"
	"function post(v){try{window.webkit.messageHandlers.gsurfFocus.postMessage({f:id,t:top,e:v});}catch(x){}}"
	"function snd(){var a=deep();if(a&&(a.tagName==='IFRAME'||a.tagName==='FRAME'))return;post(ed(a));}"
	"document.addEventListener('focusin',snd,true);"
	"document.addEventListener('focusout',function(){setTimeout(snd,0);},true);"
	"window.addEventListener('pagehide',function(){if(!top)post(false);},true);"
	"if(top)snd();else if(ed(deep()))post(true);})()";

#define EDITING_FRAME_KEY "gsurf-editing-frame"

void
gsurf_webkit_common_focus_message(GsurfView *view, JSCValue *value)
{
	g_autoptr(JSCValue) frame_value = NULL;
	g_autoptr(JSCValue) top_value = NULL;
	g_autoptr(JSCValue) editing_value = NULL;
	g_autofree gchar *frame = NULL;
	const gchar *current;
	gboolean editing;
	gboolean top;

	g_return_if_fail(GSURF_IS_VIEW(view));

	/* A bare boolean (an older tracker): take it as is. */
	if (jsc_value_is_boolean(value)) {
		gsurf_view_set_editing(view, jsc_value_to_boolean(value));
		return;
	}
	if (!jsc_value_is_object(value))
		return;
	frame_value = jsc_value_object_get_property(value, "f");
	top_value = jsc_value_object_get_property(value, "t");
	editing_value = jsc_value_object_get_property(value, "e");
	frame = jsc_value_is_string(frame_value) ? jsc_value_to_string(frame_value) : NULL;
	editing = jsc_value_to_boolean(editing_value);
	top = jsc_value_to_boolean(top_value);
	current = g_object_get_data(G_OBJECT(view), EDITING_FRAME_KEY);

	if (editing) {
		/* Focus is in this frame's field: whatever any other frame said
		 * before is stale (only one element has focus). */
		g_object_set_data_full(G_OBJECT(view), EDITING_FRAME_KEY,
			g_steal_pointer(&frame), g_free);
		gsurf_view_set_editing(view, TRUE);
	} else if (top || (frame != NULL && g_strcmp0(frame, current) == 0)) {
		g_object_set_data(G_OBJECT(view), EDITING_FRAME_KEY, NULL);
		gsurf_view_set_editing(view, FALSE);
	}
}

/* --- context menu --- */

/* A context-menu item contributed by a module: run its shell command. */
static void
on_menu_action_activate(GSimpleAction *action, GVariant *param, gpointer user_data)
{
	const gchar *command = g_object_get_data(G_OBJECT(action), "gsurf-command");
	g_autoptr(GError) error = NULL;

	(void)param;
	(void)user_data;
	if (command != NULL && !g_spawn_command_line_async(command, &error))
		g_warning("gsurf context-menu: %s", error ? error->message : "spawn failed");
}

/* Build a GsurfHitTest from a WebKit hit-test result. */
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

void
gsurf_webkit_common_populate_menu(WebKitContextMenu *menu, WebKitHitTestResult *hit_result)
{
	GsurfHitTest *hit = hit_test_from_webkit(hit_result);
	GPtrArray *items = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_menu_item_free);
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
}

/* --- downloads --- */

static gboolean
on_download_decide_destination(WebKitDownload *download, const gchar *suggested,
                               gpointer user_data)
{
	WebKitURIRequest *req = webkit_download_get_request(download);
	const gchar *uri = req ? webkit_uri_request_get_uri(req) : NULL;
	g_autofree gchar *path = gsurf_module_manager_dispatch_decide_destination(
		gsurf_module_manager_get_default(), uri, suggested);

	(void)user_data;
	if (path == NULL)
		return FALSE;
#ifdef GSURF_BACKEND_GTK4
	/* WebKitGTK 6.0 takes a filesystem path. */
	webkit_download_set_destination(download, path);
	return TRUE;
#else
	{
		/* WebKitGTK 4.1 takes a file:// URI. */
		g_autofree gchar *file_uri = g_filename_to_uri(path, NULL, NULL);

		if (file_uri != NULL)
			webkit_download_set_destination(download, file_uri);
		return file_uri != NULL;
	}
#endif
}

void
gsurf_webkit_common_track_download(WebKitDownload *download)
{
	g_signal_connect(download, "decide-destination",
		G_CALLBACK(on_download_decide_destination), NULL);
}

/* --- TLS, permissions, hover --- */

gboolean
gsurf_webkit_common_handle_tls_error(WebKitWebView *wv, const gchar *failing_uri,
                                     GTlsCertificate *cert, GTlsCertificateFlags errors)
{
	g_autoptr(GUri) parsed = g_uri_parse(failing_uri, G_URI_FLAGS_NONE, NULL);
	const gchar *host = parsed ? g_uri_get_host(parsed) : NULL;

	if (host == NULL)
		return FALSE;
	if (gsurf_module_manager_dispatch_verify_cert(gsurf_module_manager_get_default(),
			host, errors) != GSURF_TLS_PROCEED)
		return FALSE;

	/* The exception belongs to the session the view browses in, so a
	 * container's exception never leaks into another container. */
#ifdef GSURF_BACKEND_GTK4
	webkit_network_session_allow_tls_certificate_for_host(
		webkit_web_view_get_network_session(wv), cert, host);
#else
	webkit_web_context_allow_tls_certificate_for_host(
		webkit_web_view_get_context(wv), cert, host);
#endif
	webkit_web_view_load_uri(wv, failing_uri);
	return TRUE;
}

gboolean
gsurf_webkit_common_handle_permission(GsurfView *view, WebKitPermissionRequest *req)
{
	const gchar *type = "other";
	GsurfPermissionVerdict v;

	if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req))
		type = "geolocation";
	else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req))
		type = "notification";

	v = gsurf_module_manager_dispatch_permission(gsurf_module_manager_get_default(),
		view, type, NULL);
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

void
gsurf_webkit_common_mouse_target(GsurfView *view, WebKitHitTestResult *hit)
{
	const gchar *uri = webkit_hit_test_result_context_is_link(hit)
		? webkit_hit_test_result_get_link_uri(hit) : NULL;

	gsurf_view_set_hovered_uri(view, uri);
}

/* --- popups --- */

GtkWidget *
gsurf_webkit_common_create(GsurfView *view, WebKitNavigationAction *action)
{
	WebKitURIRequest *req = webkit_navigation_action_get_request(action);
	const gchar *uri = req ? webkit_uri_request_get_uri(req) : NULL;
	GsurfView *newview = gsurf_view_emit_create_view(view, uri);

	/* The host must have built @newview as a related view of @view
	 * (gsurf_view_new_full(view, NULL, uri, TRUE)); WebKit requires a
	 * popup to share its opener's session. */
	if (newview != NULL)
		return GTK_WIDGET(gsurf_view_get_native_widget(newview));
	return NULL;
}

/* --- main-frame response policy --- */

gboolean
gsurf_webkit_common_decide_response(GsurfView *view, WebKitPolicyDecision *decision)
{
	WebKitResponsePolicyDecision *response = WEBKIT_RESPONSE_POLICY_DECISION(decision);
	const gchar *uri;

	/* Only top-level documents the engine will show: sub-resources,
	 * iframe documents and downloads are none of the hooks' business. */
	if (!webkit_response_policy_decision_is_main_frame_main_resource(response) ||
	    !webkit_response_policy_decision_is_mime_type_supported(response))
		return FALSE;

	uri = webkit_uri_request_get_uri(webkit_response_policy_decision_get_request(response));
	if (gsurf_module_manager_dispatch_main_response(gsurf_module_manager_get_default(),
			view, uri) != GSURF_POLICY_IGNORE)
		return FALSE;

	webkit_policy_decision_ignore(decision);
	return TRUE;
}
