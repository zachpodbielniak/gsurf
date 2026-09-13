/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "protocol/gsurf-protocol.h"
#include "protocol-server.h"
#include <string.h>

/* Every async test iterates only a private thread-default context. */
typedef struct {
	GMainLoop *loop;
	GsurfProtocolResponse *response;
	GError *error;
} FetchResult;

/* Capture ownership on the initiating context, exactly once. */
static void
fetched(GObject *source, GAsyncResult *result, gpointer data)
{
	FetchResult *fetch = data;
	fetch->response = gsurf_protocol_fetch_finish(result, &fetch->error);
	g_main_loop_quit(fetch->loop);
}

/* A stalled fixture cancels the operation while its socket read is pending. */
static gboolean
cancel_fetch(gpointer data)
{
	g_cancellable_cancel(G_CANCELLABLE(data));
	return G_SOURCE_REMOVE;
}

/* Timeouts fail the test rather than quietly skipping incomplete results. */
static gboolean
fetch_timeout(gpointer data)
{
	g_error("Protocol fixture did not finish within ten seconds");
	return G_SOURCE_REMOVE;
}

/* Fetch with an isolated context and an optional cancellation deadline. */
static GsurfProtocolResponse *
fetch_uri(const gchar *uri, const gchar *pins, gboolean cancel, GError **error)
{
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GMainLoop) loop = g_main_loop_new(context, FALSE);
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	g_autoptr(GSource) timeout = g_timeout_source_new_seconds(10);
	g_autoptr(GSource) cancellation = g_timeout_source_new(100);
	FetchResult result = { loop, NULL, NULL };
	g_source_set_callback(timeout, fetch_timeout, NULL, NULL);
	g_source_attach(timeout, context);
	if (cancel) {
		g_source_set_callback(cancellation, cancel_fetch, cancellable, NULL);
		g_source_attach(cancellation, context);
	}
	g_main_context_push_thread_default(context);
	gsurf_protocol_fetch_async(uri, pins, "direct://", cancellable, fetched, &result);
	g_main_loop_run(loop);
	g_main_context_pop_thread_default(context);
	g_source_destroy(timeout);
	g_source_destroy(cancellation);
	if (result.error != NULL)
		g_propagate_error(error, result.error);
	return result.response;
}

/* URI parsing preserves selector bytes and refuses request injection. */
static void
test_requests(void)
{
	const gchar *bad[] = { "https://example.org/", "gopher:///1", "gopher://u@host/",
		"gopher://host/0a%0Db", "gopher://host/0a%0Ab", "gopher://host/0a%00b",
		"gopher://host/1?q", "gemini://u@host/", "gemini://host:0/", "gemini://host/\r\nx", NULL };
	g_autofree gchar *host = NULL;
	g_autofree gchar *wire = NULL;
	gboolean gemini;
	gchar type;
	guint16 port;
	guint i;
	wire = gsurf_protocol_request("gopher://[::1]:7070/7search%09a%20b#local", &host, &port, &type, &gemini, NULL);
	g_assert_cmpstr(wire, ==, "search\ta b\r\n");
	g_assert_cmpstr(host, ==, "::1");
	g_assert_cmpuint(port, ==, 7070);
	g_assert_cmpint(type, ==, '7');
	g_assert_false(gemini);
	g_clear_pointer(&wire, g_free);
	g_clear_pointer(&host, g_free);
	wire = gsurf_protocol_request("GEMINI://example.org?q=a%20b#fragment", &host, &port, &type, &gemini, NULL);
	g_assert_cmpstr(wire, ==, "gemini://example.org/?q=a%20b\r\n");
	g_assert_cmpuint(port, ==, 1965);
	g_assert_true(gemini);
	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *bad_host = NULL;
		g_autofree gchar *result = gsurf_protocol_request(bad[i], &bad_host, &port, &type, &gemini, &error);
		g_assert_null(result);
		g_assert_nonnull(error);
	}
	{
		g_autofree gchar *path = g_strnfill(1024, 'x');
		g_autofree gchar *long_uri = g_strconcat("gemini://host/", path, NULL);
		g_clear_pointer(&wire, g_free);
		g_clear_pointer(&host, g_free);
		wire = gsurf_protocol_request(long_uri, &host, &port, &type, &gemini, NULL);
		g_assert_null(wire);
	}
}

/* Limit and status parsing tests include unknown-but-valid class members. */
static void
test_headers(void)
{
	const gchar *bad[] = { "", "2", "20", "20\ttext/gemini", "00 nope", "70 nope", "20 a\rb", "2x bad", "20 \xff", "20 \xc2\x85", NULL };
	gint status;
	g_autofree gchar *meta = NULL;
	g_autofree gchar *large = g_strnfill(1028, 'a');
	guint i;
	g_assert_true(gsurf_protocol_parse_header("29 text/gemini; lang=en", &status, &meta, NULL));
	g_assert_cmpint(status, ==, 29);
	g_assert_cmpstr(meta, ==, "text/gemini; lang=en");
	g_clear_pointer(&meta, g_free);
	g_assert_true(gsurf_protocol_parse_header("51", &status, &meta, NULL));
	g_assert_cmpstr(meta, ==, "");
	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;
		g_assert_false(gsurf_protocol_parse_header(bad[i], &status, &meta, &error));
		g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	}
	memcpy(large, "20 ", 3);
	g_assert_false(gsurf_protocol_parse_header(large, &status, &meta, NULL));
}

/* Escaping, link resolution, literal preformatted markup and menu targets. */
static void
test_render(void)
{
	g_autofree gchar *html = gsurf_protocol_render(
		"#Title\n* one\n* two\n>quote\n```alt\n=>not a link\n<script>\n```\n"
		"=>../next?q=a&b Label <x>\n=>javascript:alert(1) Unsafe\n", "gemini://host/dir/page", FALSE);
	g_autofree gchar *menu = gsurf_protocol_render(
		"iWelcome <reader>\tfake\tfake\t0\n1Submenu\t/a ?b#c\tlocalhost\t70\n"
		"hWeb\tURL:https://example.org/?a&b\tunused\t70\n.\n", "gopher://host/", TRUE);
	g_assert_nonnull(strstr(html, "<h1>Title</h1>"));
	g_assert_nonnull(strstr(html, "<ul><li>one</li><li>two</li></ul>"));
	g_assert_nonnull(strstr(html, "<blockquote>quote</blockquote>"));
	g_assert_nonnull(strstr(html, "<pre>=&gt;not a link\n&lt;script&gt;"));
	g_assert_nonnull(strstr(html, "href=\"gemini://host/next?q=a&amp;b\""));
	g_assert_null(strstr(html, "href=\"javascript:"));
	g_assert_nonnull(strstr(menu, "Welcome &lt;reader&gt;"));
	g_assert_nonnull(strstr(menu, "gopher://localhost/1/a%20%3Fb%23c"));
	g_assert_nonnull(strstr(menu, "https://example.org/?a&amp;b"));
}

/* HTML form encoding must not leak field names or '+' into Gemini requests. */
static void
test_input(void)
{
	g_autofree gchar *gemini = gsurf_protocol_input_uri("gemini://host/search?gsurf-query=a+b%2Bc%26d", NULL);
	g_autofree gchar *gopher = gsurf_protocol_input_uri("gopher://host/7find?gsurf-query=one+two", NULL);
	g_autofree gchar *html = gsurf_protocol_input_page("gemini://host/", "Secret <value>", TRUE);
	g_assert_cmpstr(gemini, ==, "gemini://host/search?a%20b%2Bc%26d");
	g_assert_cmpstr(gopher, ==, "gopher://host/7find%09one%20two");
	g_assert_nonnull(strstr(html, "type='password'"));
	g_assert_nonnull(strstr(html, "Secret &lt;value&gt;"));
}

/* Loopback TCP verifies exact selector framing, binary data and dot-stuffing. */
static void
test_gopher(void)
{
	ProtocolServer server = { 0 };
	g_autofree gchar *uri = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfProtocolResponse) response = NULL;
	const gchar *reply = "Hello\r\n..dot\r\n.\r\n";
	g_autofree gchar *body = NULL;
	protocol_server_start(&server, reply, strlen(reply), NULL);
	uri = g_strdup_printf("gopher://127.0.0.1:%u/0text#local", server.port);
	response = fetch_uri(uri, NULL, FALSE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);
	protocol_server_stop(&server);
	g_assert_cmpstr(server.request, ==, "text\r\n");
	body = g_strndup(g_bytes_get_data(response->body, NULL), g_bytes_get_size(response->body));
	g_assert_cmpstr(body, ==, "Hello\n.dot\n");
	g_free(server.request);
}

/* Binary NULs and terminator-looking octets must survive unchanged. */
static void
test_binary(void)
{
	ProtocolServer server = { 0 };
	const gchar reply[] = { '\0', '\r', '\n', '.', '\r', '\n', '\xff' };
	g_autofree gchar *uri = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfProtocolResponse) response = NULL;
	protocol_server_start(&server, reply, sizeof(reply), NULL);
	uri = g_strdup_printf("gopher://127.0.0.1:%u/9binary", server.port);
	response = fetch_uri(uri, NULL, FALSE, &error);
	g_assert_no_error(error);
	protocol_server_stop(&server);
	g_assert_cmpmem(g_bytes_get_data(response->body, NULL), g_bytes_get_size(response->body), reply, sizeof(reply));
	g_free(server.request);
}

/* TLS fixtures exercise first use, pin reuse, and changed-cert rejection. */
static void
test_gemini(void)
{
	g_autofree gchar *pins = g_dir_make_tmp("gsurf-protocol-pins-XXXXXX", NULL);
	guint16 port = 0;
	guint i;
	for (i = 0; i < 3; i++) {
		ProtocolServer server = { 0 };
		g_autofree gchar *uri = NULL;
		g_autoptr(GError) error = NULL;
		g_autoptr(GsurfProtocolResponse) response = NULL;
		const gchar *reply = "20 text/gemini\r\n# Capsule\n=> /next Next\n";
		server.port = port;
		protocol_server_start(&server, reply, strlen(reply), i == 2 ?
			"tests/fixtures/gemini-changed.pem" : "tests/fixtures/gemini-server.pem");
		port = server.port;
		uri = g_strdup_printf("gemini://127.0.0.1:%u/start#fragment", port);
		response = fetch_uri(uri, pins, FALSE, &error);
		protocol_server_stop(&server);
		if (i == 2) {
			g_assert_null(response);
			g_assert_error(error, G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE);
			g_assert_nonnull(strstr(error->message, "certificate changed"));
		} else {
			g_autofree gchar *body = NULL;
			g_autofree gchar *expected = g_strdup_printf("gemini://127.0.0.1:%u/start\r\n", port);
			g_assert_no_error(error);
			g_assert_cmpstr(server.request, ==, expected);
			body = g_strndup(g_bytes_get_data(response->body, NULL), g_bytes_get_size(response->body));
			g_assert_nonnull(strstr(body, "<h1> Capsule</h1>"));
		}
		g_free(server.request);
	}
	protocol_remove_directory(pins);
}

/* Headers may terminate without a body; failures must be distinguishable
 * from valid input/redirect/error statuses. No external network is used. */
static void
test_gemini_statuses(void)
{
	const gchar *replies[] = { "10 Search\r\n", "11 Password\r\n", "30 ../next\r\n",
		"51 Missing\r\n", "60 Certificate\r\n", "20 text/gemini\n", "20 partial", NULL };
	const gint statuses[] = { 10, 11, 30, 51, 60 };
	g_autofree gchar *pins = g_dir_make_tmp("gsurf-protocol-status-XXXXXX", NULL);
	guint i;
	for (i = 0; replies[i] != NULL; i++) {
		ProtocolServer server = { 0 };
		g_autofree gchar *uri = NULL;
		g_autoptr(GError) error = NULL;
		g_autoptr(GsurfProtocolResponse) response = NULL;
		protocol_server_start(&server, replies[i], strlen(replies[i]), "tests/fixtures/gemini-server.pem");
		uri = g_strdup_printf("gemini://127.0.0.1:%u/", server.port);
		response = fetch_uri(uri, pins, FALSE, &error);
		protocol_server_stop(&server);
		if (i < G_N_ELEMENTS(statuses)) {
			g_assert_no_error(error);
			g_assert_cmpint(response->status, ==, statuses[i]);
		} else {
			g_assert_null(response);
			g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
		}
		g_free(server.request);
	}
	protocol_remove_directory(pins);
}

/* Cancelling a pending read must complete on the private context promptly. */
static void
test_cancellation(void)
{
	ProtocolServer server = { 0 };
	g_autofree gchar *uri = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfProtocolResponse) response = NULL;
	server.stall = TRUE;
	protocol_server_start(&server, "", 0, NULL);
	uri = g_strdup_printf("gopher://127.0.0.1:%u/0wait", server.port);
	response = fetch_uri(uri, NULL, TRUE, &error);
	g_assert_null(response);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	protocol_server_stop(&server);
	g_free(server.request);
}

/* Empty pages and non-UTF-8 charsets must not crash or silently mojibake. */
static void
test_gemtext_charset(void)
{
	const gchar *replies[] = { "20 text/gemini\r\n", "20 text/gemini; charset=\"ISO-8859-1\"\r\n# caf\xe9\n",
		"20 text/gemini; charset=not-a-charset\r\n# bad\n", "20 text/plain\r\ncaf\xc3\xa9\n", NULL };
	g_autofree gchar *pins = g_dir_make_tmp("gsurf-protocol-charset-XXXXXX", NULL);
	guint i;
	for (i = 0; replies[i] != NULL; i++) {
		ProtocolServer server = { 0 };
		g_autofree gchar *uri = NULL;
		g_autoptr(GError) error = NULL;
		g_autoptr(GsurfProtocolResponse) response = NULL;
		protocol_server_start(&server, replies[i], strlen(replies[i]), "tests/fixtures/gemini-server.pem");
		uri = g_strdup_printf("gemini://127.0.0.1:%u/", server.port);
		response = fetch_uri(uri, pins, FALSE, &error);
		protocol_server_stop(&server);
		if (i == 2) {
			g_assert_null(response);
			g_assert_error(error, G_CONVERT_ERROR, G_CONVERT_ERROR_NO_CONVERSION);
		} else {
			g_autofree gchar *body = NULL;
			g_assert_no_error(error);
			g_assert_nonnull(response);
			body = g_strndup(g_bytes_get_data(response->body, NULL), g_bytes_get_size(response->body));
			g_assert_nonnull(strstr(body, i == 0 ? "</body></html>" : "caf\xc3\xa9"));
			if (i == 3)
				g_assert_cmpstr(response->mime, ==, "text/plain; charset=utf-8");
		}
		g_free(server.request);
	}
	protocol_remove_directory(pins);
}

/* The bounded transport must reject oversize data, not return partial bytes. */
static void
test_response_limit(void)
{
	ProtocolServer server = { 0 };
	g_autofree gchar *reply = g_malloc0(32 * 1024 * 1024 + 1);
	g_autofree gchar *uri = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfProtocolResponse) response = NULL;
	server.fragment_size = 8192;
	protocol_server_start(&server, reply, 32 * 1024 * 1024 + 1, NULL);
	uri = g_strdup_printf("gopher://127.0.0.1:%u/9large", server.port);
	response = fetch_uri(uri, NULL, FALSE, &error);
	g_assert_null(response);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
	protocol_server_stop(&server);
	g_free(server.request);
}

/* Register the hermetic protocol contract independently of display tests. */
int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/protocol/requests", test_requests);
	g_test_add_func("/protocol/headers", test_headers);
	g_test_add_func("/protocol/render", test_render);
	g_test_add_func("/protocol/input", test_input);
	g_test_add_func("/protocol/gopher", test_gopher);
	g_test_add_func("/protocol/binary", test_binary);
	g_test_add_func("/protocol/gemini", test_gemini);
	g_test_add_func("/protocol/gemini-statuses", test_gemini_statuses);
	g_test_add_func("/protocol/cancellation", test_cancellation);
	g_test_add_func("/protocol/gemtext-charset", test_gemtext_charset);
	g_test_add_func("/protocol/response-limit", test_response_limit);
	return g_test_run();
}
