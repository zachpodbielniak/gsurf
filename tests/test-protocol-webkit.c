/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gsurf.h>
#include "backend/webkit/gsurf-webkit-protocol.h"
#include "protocol-server.h"
#include <string.h>

/* Run only when explicitly opted into a display/web-process integration. */
static gboolean
have_display(void)
{
	if (g_getenv("GSURF_TEST_GUI") == NULL) {
		g_test_skip("set GSURF_TEST_GUI=1 to exercise WebKit protocols");
		return FALSE;
	}
#ifdef GSURF_BACKEND_GTK3
	g_assert_true(gtk_init_check(NULL, NULL));
#else
	g_assert_true(gtk_init_check());
#endif
	return TRUE;
}

/* A bound applies to both loading and embedding-side script evaluation. */
static gboolean
deadline(gpointer data)
{
	g_error("WebKit protocol integration timed out");
	return G_SOURCE_REMOVE;
}

typedef struct {
	GMainLoop *loop;
	const gchar *uri;
	gboolean started;
} LoadWait;

/* A cancelled older load can emit FINISHED while a new one is pending. */
static void
loaded(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
	LoadWait *wait = data;
	if (event == WEBKIT_LOAD_STARTED)
		wait->started = TRUE;
	if (event == WEBKIT_LOAD_FINISHED && wait->started &&
	    (wait->uri == NULL || g_strcmp0(wait->uri, webkit_web_view_get_uri(view)) == 0))
		g_main_loop_quit(wait->loop);
}

/* Report actual navigation failures instead of mistaking an error document
 * for successful native protocol integration. */
static gboolean
load_failed(WebKitWebView *view, WebKitLoadEvent event, const gchar *uri,
	GError *error, gpointer data)
{
	if (!g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) &&
	    !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		g_error("WebKit failed loading %s: %s", uri, error->message);
	return FALSE;
}

/* Wait for document loading, bounded by a source we always remove. */
static void
wait_load(WebKitWebView *view, const gchar *uri)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	LoadWait wait = { loop, uri, uri == NULL };
	gulong handler = g_signal_connect(view, "load-changed", G_CALLBACK(loaded), &wait);
	guint timeout = g_timeout_add_seconds(15, deadline, NULL);
	if (uri != NULL)
		webkit_web_view_load_uri(view, uri);
	g_main_loop_run(loop);
	g_source_remove(timeout);
	g_signal_handler_disconnect(view, handler);
}

/* Return the result of a test-side DOM inspection. No page script is trusted. */
typedef struct {
	GMainLoop *loop;
	gchar *text;
} ScriptResult;

static void
script_done(GObject *source, GAsyncResult *result, gpointer data)
{
	ScriptResult *script = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JSCValue) value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
	g_assert_no_error(error);
	script->text = jsc_value_to_string(value);
	g_main_loop_quit(script->loop);
}

/* Embedding-side evaluation remains available with page JavaScript disabled. */
static gchar *
evaluate(WebKitWebView *view, const gchar *expression)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	ScriptResult result = { loop, NULL };
	guint timeout = g_timeout_add_seconds(10, deadline, NULL);
	webkit_web_view_evaluate_javascript(view, expression, -1, NULL, NULL, NULL, script_done, &result);
	g_main_loop_run(loop);
	g_source_remove(timeout);
	return result.text;
}

/* Load a real Gopher menu through the public view and inspect its DOM. */
static void
test_menu(void)
{
	ProtocolServer server = { 0 };
	g_autoptr(GsurfView) view = NULL;
	WebKitWebView *native;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *href = NULL;
	const gchar *reply = "iLocal menu <safe>\tfake\tfake\t0\r\n1Next\t/next\tlocalhost\t70\r\n.\r\n";
	if (!have_display())
		return;
	protocol_server_start(&server, reply, strlen(reply), NULL);
	view = gsurf_view_new();
	native = WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
	g_signal_connect(native, "load-failed", G_CALLBACK(load_failed), NULL);
	uri = g_strdup_printf("gopher://127.0.0.1:%u/", server.port);
	wait_load(native, uri);
	protocol_server_stop(&server);
	text = evaluate(native, "document.body.textContent");
	href = evaluate(native, "document.querySelector('a').getAttribute('href')");
	g_assert_nonnull(strstr(text, "Local menu <safe>"));
	g_assert_cmpstr(href, ==, "gopher://localhost/1/next");
	g_assert_cmpstr(gsurf_view_get_uri(view), ==, uri);
	g_free(server.request);
}

/* A type-7 prompt submits a standard HTML GET form;
 * the adapter converts that navigation into a Gopher selector TAB query. */
static void
test_search(void)
{
	ProtocolServer server = { 0 };
	g_autoptr(GsurfView) view = NULL;
	WebKitWebView *native;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *ignored = NULL;
	g_autofree gchar *text = NULL;
	const gchar *reply = "iSearch result\tfake\tfake\t0\r\n.\r\n";
	if (!have_display())
		return;
	protocol_server_start(&server, reply, strlen(reply), NULL);
	view = gsurf_view_new();
	native = WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
	g_signal_connect(native, "load-failed", G_CALLBACK(load_failed), NULL);
	uri = g_strdup_printf("gopher://127.0.0.1:%u/7find", server.port);
	wait_load(native, uri);
	ignored = evaluate(native, "document.querySelector('input').value='a b+c'; document.querySelector('form').requestSubmit(); 'submitted'");
	wait_load(native, NULL);
	protocol_server_stop(&server);
	g_assert_cmpstr(server.request, ==, "find\ta b+c\r\n");
	text = evaluate(native, "document.body.textContent");
	g_assert_nonnull(strstr(text, "Search result"));
	g_assert_nonnull(strstr(webkit_web_view_get_uri(native), "/7find%09a%20b%2Bc"));
	g_clear_pointer(&server.request, g_free);
	/* Restore the prompt through browser history and submit it again. */
	webkit_web_view_go_back(native);
	wait_load(native, NULL);
	g_assert_cmpstr(webkit_web_view_get_uri(native), ==, uri);
	protocol_server_start(&server, reply, strlen(reply), NULL);
	g_clear_pointer(&ignored, g_free);
	ignored = evaluate(native, "document.querySelector('input').value='again'; document.querySelector('form').requestSubmit(); 'submitted'");
	wait_load(native, NULL);
	protocol_server_stop(&server);
	g_assert_cmpstr(server.request, ==, "find\tagain\r\n");
	g_free(server.request);
}

#ifdef GSURF_BACKEND_GTK3
/* Inject real key events, as the offscreen engine does, without evaluating
 * JavaScript in a document whose scripting capability is disabled. */
static void
send_key(GtkWidget *widget, guint keyval)
{
	GdkDisplay *display = gtk_widget_get_display(widget);
	GdkKeymapKey *keys = NULL;
	gint count = 0;
	guint i;
	g_assert_true(gdk_keymap_get_entries_for_keyval(gdk_keymap_get_for_display(display),
		keyval, &keys, &count));
	for (i = 0; i < 2; i++) {
		GdkEvent *event = gdk_event_new(i == 0 ? GDK_KEY_PRESS : GDK_KEY_RELEASE);
		event->key.window = g_object_ref(gtk_widget_get_window(widget));
		event->key.send_event = TRUE;
		event->key.time = GDK_CURRENT_TIME;
		event->key.keyval = keyval;
		event->key.hardware_keycode = (guint16)keys[0].keycode;
		event->key.group = (guint8)keys[0].group;
		gdk_event_set_device(event, gdk_seat_get_keyboard(gdk_display_get_default_seat(display)));
		gtk_main_do_event(event);
		gdk_event_free(event);
	}
	g_free(keys);
}

/* A native Enter key submits the prompt while page JavaScript is disabled. */
/* A real Enter event submits a Gopher+ ASK form with scripting disabled. */
static void
test_scriptless_ask(void)
{
	ProtocolServer server = { 0 };
	g_autoptr(GsurfView) view = NULL;
	GtkWidget *window;
	WebKitWebView *native;
	g_autofree gchar *uri = NULL;
	const gchar *prompt = "+-1\r\n+ASK:\r\n Ask: Name\r\n.\r\n";
	const gchar *expected = "ask\t+\t1\r\n+-1\r\na\r\n.\r\n";
	if (!have_display())
		return;
	protocol_server_start(&server, prompt, strlen(prompt), NULL);
	view = gsurf_view_new();
	native = WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
	webkit_settings_set_hardware_acceleration_policy(webkit_web_view_get_settings(native),
		WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
	window = gtk_offscreen_window_new();
	gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
	gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(native));
	gtk_widget_show_all(window);
	webkit_settings_set_enable_javascript(webkit_web_view_get_settings(native), FALSE);
	uri = g_strdup_printf("gopher://127.0.0.1:%u/0ask%%09%%09%%3F", server.port);
	wait_load(native, uri);
	protocol_server_stop(&server);
	g_clear_pointer(&server.request, g_free);
	server.request_length = strlen(expected);
	protocol_server_start(&server, "+2\r\nOK", 6, NULL);
	gtk_widget_grab_focus(GTK_WIDGET(native));
	send_key(GTK_WIDGET(native), GDK_KEY_a);
	send_key(GTK_WIDGET(native), GDK_KEY_Return);
	wait_load(native, NULL);
	protocol_server_stop(&server);
	g_assert_cmpstr(server.request, ==, expected);
	g_free(server.request);
	gtk_widget_destroy(window);
}
static void
test_scriptless_search(void)
{
	ProtocolServer server = { 0 };
	g_autoptr(GsurfView) view = NULL;
	GtkWidget *window;
	WebKitWebView *native;
	g_autofree gchar *uri = NULL;
	const gchar *reply = "iKeyboard result\tfake\tfake\t0\r\n.\r\n";
	if (!have_display())
		return;
	protocol_server_start(&server, reply, strlen(reply), NULL);
	view = gsurf_view_new();
	native = WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
	window = gtk_offscreen_window_new();
	webkit_settings_set_hardware_acceleration_policy(webkit_web_view_get_settings(native),
		WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
	gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
	gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(native));
	gtk_widget_show_all(window);
	webkit_settings_set_enable_javascript(webkit_web_view_get_settings(native), FALSE);
	g_signal_connect(native, "load-failed", G_CALLBACK(load_failed), NULL);
	uri = g_strdup_printf("gopher://127.0.0.1:%u/7find", server.port);
	wait_load(native, uri);
	gtk_widget_grab_focus(GTK_WIDGET(native));
	send_key(GTK_WIDGET(native), GDK_KEY_a);
	send_key(GTK_WIDGET(native), GDK_KEY_Return);
	wait_load(native, NULL);
	protocol_server_stop(&server);
	g_assert_cmpstr(server.request, ==, "find\ta\r\n");
	g_free(server.request);
	gtk_widget_destroy(window);
}
#endif

/* Same adapter on a WebView created without the gsurf GTK view subclass,
 * matching the offscreen engine's attachment path. */
static void
test_gemini(void)
{
	ProtocolServer server = { 0 };
	g_autoptr(WebKitWebView) native = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *text = NULL;
	const gchar *reply = "20 text/gemini\r\n# Native capsule\n=> /next Next\n";
	if (!have_display())
		return;
	protocol_server_start(&server, reply, strlen(reply), "tests/fixtures/gemini-server.pem");
	native = WEBKIT_WEB_VIEW(g_object_ref_sink(webkit_web_view_new()));
	gsurf_webkit_protocol_attach(native);
	g_signal_connect(native, "load-failed", G_CALLBACK(load_failed), NULL);
	uri = g_strdup_printf("gemini://127.0.0.1:%u/", server.port);
	wait_load(native, uri);
	protocol_server_stop(&server);
	text = evaluate(native, "document.querySelector('h1').textContent");
	g_assert_cmpstr(text, ==, " Native capsule");
	g_free(server.request);
}

/* Sensitive input replaces the existing query and reuses the pinned TLS
 * identity when the form navigates to its encoded response URI. */
static void
test_gemini_input(void)
{
	ProtocolServer server = { 0 };
	g_autoptr(GsurfView) view = NULL;
	WebKitWebView *native;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *input_type = NULL;
	g_autofree gchar *ignored = NULL;
	g_autofree gchar *expected = NULL;
	const gchar *prompt = "11 Enter a value\r\n";
	const gchar *reply = "20 text/gemini\r\n# Input accepted\n";
	if (!have_display())
		return;
	protocol_server_start(&server, prompt, strlen(prompt), "tests/fixtures/gemini-server.pem");
	view = gsurf_view_new();
	native = WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
	g_signal_connect(native, "load-failed", G_CALLBACK(load_failed), NULL);
	uri = g_strdup_printf("gemini://127.0.0.1:%u/input?old-query", server.port);
	wait_load(native, uri);
	protocol_server_stop(&server);
	g_clear_pointer(&server.request, g_free);
	input_type = evaluate(native, "document.querySelector('input').type");
	g_assert_cmpstr(input_type, ==, "password");
	protocol_server_start(&server, reply, strlen(reply), "tests/fixtures/gemini-server.pem");
	expected = g_strdup_printf("gemini://127.0.0.1:%u/input?a%sb%sc\r\n", server.port, "%20", "%2B");
	ignored = evaluate(native, "document.querySelector('input').value='a b+c'; document.querySelector('form').requestSubmit(); 'submitted'");
	wait_load(native, NULL);
	protocol_server_stop(&server);
	g_assert_cmpstr(server.request, ==, expected);
	g_free(server.request);
}

/* Switching pages or stopping a pending scheme load cancels its socket work,
 * and a later completion must not replace the new document. */
static void
test_cancel_navigation(void)
{
	guint i;
	if (!have_display())
		return;
	for (i = 0; i < 3; i++) {
		ProtocolServer server = { 0 };
		g_autoptr(GsurfView) view = gsurf_view_new();
		WebKitWebView *native = WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
		g_autoptr(WebKitWebView) retained_native = g_object_ref(native);
		g_autoptr(GCancellable) cancel = NULL;
		g_autofree gchar *uri = NULL;
		guint timeout = g_timeout_add_seconds(10, deadline, NULL);
		server.stall = TRUE;
		protocol_server_start(&server, "", 0, NULL);
		uri = g_strdup_printf("gopher://127.0.0.1:%u/0pending", server.port);
		g_signal_connect(native, "load-failed", G_CALLBACK(load_failed), NULL);
		webkit_web_view_load_uri(native, uri);
		while (g_object_get_data(G_OBJECT(native), "gsurf-protocol-cancellable") == NULL)
			g_main_context_iteration(NULL, TRUE);
		cancel = g_object_ref(g_object_get_data(G_OBJECT(native), "gsurf-protocol-cancellable"));
		if (i == 0) {
			webkit_web_view_stop_loading(native);
			while (!g_cancellable_is_cancelled(cancel))
				g_main_context_iteration(NULL, TRUE);
		}
		if (i == 2) {
			g_clear_object(&view);
			g_assert_true(g_cancellable_is_cancelled(cancel));
		}
		wait_load(native, "about:blank");
		g_assert_true(g_cancellable_is_cancelled(cancel));
		protocol_server_stop(&server);
		g_assert_cmpstr(webkit_web_view_get_uri(native), ==, "about:blank");
		g_source_remove(timeout);
		g_free(server.request);
	}
}

/* An explicit browser proxy must receive the connection attempt. A denied
 * CONNECT fails closed instead of falling back to direct destination access. */
static void
test_proxy(gconstpointer data)
{
	ProtocolServer server = { 0 };
	g_autoptr(GsurfView) view = NULL;
	WebKitWebView *native;
	g_autofree gchar *proxy = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GsurfConfig) config = NULL;
	g_autoptr(GsurfConfig) saved_config = NULL;
	const gchar *reply = "HTTP/1.0 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
	if (!have_display())
		return;
	protocol_server_start(&server, reply, strlen(reply), NULL);
	proxy = g_strdup_printf("http://127.0.0.1:%u", server.port);
	if (GPOINTER_TO_INT(data)) {
		if (gsurf_config_get_default() != NULL)
			saved_config = g_object_ref(gsurf_config_get_default());
		config = gsurf_config_new();
		g_free(config->proxy_mode);
		g_free(config->proxy_uri);
		config->proxy_mode = g_strdup("custom");
		config->proxy_uri = g_strdup(proxy);
		gsurf_config_set_default(config);
	}
	view = gsurf_view_new();
	native = WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
	if (GPOINTER_TO_INT(data)) {
		gsurf_config_set_default(saved_config);
	} else {
		gsurf_view_set_proxy(view, proxy);
	}
	wait_load(native, "gopher://gsurf-proxy-test.invalid/0text");
	protocol_server_stop(&server);
	gsurf_view_set_proxy(view, NULL);
	g_assert_nonnull(server.request);
	g_assert_true(g_str_has_prefix(server.request, "CONNECT gsurf-proxy-test.invalid:70 HTTP/1."));
	text = evaluate(native, "document.body.textContent");
	g_assert_nonnull(strstr(text, "Unable to load page"));
	g_free(server.request);
}

/* Keep certificate persistence away from the user's real browser profile. */
/* A '?' menu item retrieves attributes and submits all ASK answers through
 * WebKit's form policy, including multiline framing and masked input. */
static void
test_gopher_plus_ask(void)
{
	ProtocolServer server = { 0 };
	g_autoptr(GsurfView) view = NULL;
	WebKitWebView *native;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *ignored = NULL;
	g_autofree gchar *text = NULL;
	const gchar *prompt = "+-1\r\n+ASK:\r\n Ask: Name\r\n AskP: Secret\r\n AskL: Notes\r\n.\r\n";
	const gchar *reply = "+8\r\nAccepted";
	const gchar *expected = "ask\t+\t1\r\n+-1\r\nAlice\r\nsecret\r\n2\r\n..\r\nnext\r\n.\r\n";
	guint i;
	if (!have_display())
		return;
	protocol_server_start(&server, prompt, strlen(prompt), NULL);
	view = gsurf_view_new();
	native = WEBKIT_WEB_VIEW(gsurf_view_get_native_widget(view));
	uri = g_strdup_printf("gopher://127.0.0.1:%u/0ask%%09%%09%%3F", server.port);
	wait_load(native, uri);
	protocol_server_stop(&server);
	g_assert_cmpstr(server.request, ==, "ask\t!+ASK\r\n");
	g_clear_pointer(&server.request, g_free);
	for (i = 0; i < 2; i++) {
		server.request_length = strlen(expected);
		server.hold_open = TRUE;
		protocol_server_start(&server, reply, strlen(reply), NULL);
		g_clear_pointer(&ignored, g_free);
		ignored = evaluate(native, "document.querySelectorAll('input')[0].value='Alice'; "
			"document.querySelector('input[type=password]').value='secret'; "
			"document.querySelector('textarea').value='.\\nnext'; document.querySelector('form').requestSubmit(); 'submitted'");
		wait_load(native, NULL);
		protocol_server_stop(&server);
		g_assert_cmpstr(server.request, ==, expected);
		g_clear_pointer(&server.request, g_free);
		g_clear_pointer(&text, g_free);
		text = evaluate(native, "document.body.textContent");
		g_assert_nonnull(strstr(text, "Accepted"));
		if (i == 0) {
			webkit_web_view_go_back(native);
			wait_load(native, NULL);
			g_assert_cmpstr(webkit_web_view_get_uri(native), ==, uri);
		}
	}
}
int
main(int argc, char **argv)
{
	g_autofree gchar *state = g_dir_make_tmp("gsurf-protocol-webkit-XXXXXX", NULL);
	g_autofree gchar *pins = g_build_filename(state, "gsurf", "gemini", "certificates", NULL);
	g_autofree gchar *gemini = g_build_filename(state, "gsurf", "gemini", NULL);
	g_autofree gchar *gsurf = g_build_filename(state, "gsurf", NULL);
	gint result;
	g_setenv("XDG_DATA_HOME", state, TRUE);
	g_setenv("GIO_USE_PROXY_RESOLVER", "dummy", TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/protocol-webkit/gopher-plus-ask", test_gopher_plus_ask);
	g_test_add_func("/protocol-webkit/menu", test_menu);
	g_test_add_func("/protocol-webkit/search", test_search);
#ifdef GSURF_BACKEND_GTK3
	g_test_add_func("/protocol-webkit/scriptless-search", test_scriptless_search);
	g_test_add_func("/protocol-webkit/scriptless-ask", test_scriptless_ask);
#endif
	g_test_add_func("/protocol-webkit/gemini", test_gemini);
	g_test_add_func("/protocol-webkit/gemini-input", test_gemini_input);
	g_test_add_func("/protocol-webkit/cancel-navigation", test_cancel_navigation);
	g_test_add_data_func("/protocol-webkit/proxy", GINT_TO_POINTER(0), test_proxy);
	g_test_add_data_func("/protocol-webkit/config-proxy", GINT_TO_POINTER(1), test_proxy);
	result = g_test_run();
	if (g_file_test(pins, G_FILE_TEST_IS_DIR)) {
		protocol_remove_directory(pins);
		g_rmdir(gemini);
		g_rmdir(gsurf);
	}
	/* WebKit may create other owned cache files; preserve those on failure. */
	g_rmdir(state);
	return result;
}
