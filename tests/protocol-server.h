/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Test-only loopback server. The committed TLS keys are public fixtures.
 * Every accept/read is bounded; stop cancels accept and joins the worker. */
#ifndef PROTOCOL_SERVER_H
#define PROTOCOL_SERVER_H
#include <gio/gio.h>
#include <glib/gstdio.h>

typedef struct {
	GSocketListener *listener;
	GCancellable *cancel;
	GTlsCertificate *certificate;
	GThread *thread;
	guint16 port;
	const gchar *reply;
	gsize reply_length;
	gchar *request;
	gboolean stall;
	gsize fragment_size;
} ProtocolServer;

/* Handle one connection and capture the exact request line. */
static gpointer
protocol_server_thread(gpointer data)
{
	ProtocolServer *server = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketConnection) socket = NULL;
	g_autoptr(GIOStream) tls = NULL;
	g_autoptr(GString) line = g_string_new(NULL);
	GIOStream *stream;
	gchar byte;
	gsize i;

	socket = g_socket_listener_accept(server->listener, NULL, server->cancel, &error);
	if (socket == NULL)
		return NULL;
	g_socket_set_timeout(g_socket_connection_get_socket(socket), 5);
	stream = G_IO_STREAM(socket);
	if (server->certificate != NULL) {
		tls = g_tls_server_connection_new(stream, server->certificate, &error);
		g_assert_no_error(error);
		if (!g_tls_connection_handshake(G_TLS_CONNECTION(tls), server->cancel, &error))
			return NULL;
		stream = tls;
	}
	while (line->len < 2048 &&
	    g_input_stream_read(g_io_stream_get_input_stream(stream), &byte, 1, server->cancel, &error) == 1) {
		g_string_append_c(line, byte);
		if (byte == '\n')
			break;
	}
	server->request = g_string_free(g_steal_pointer(&line), FALSE);
	if (error != NULL || server->stall) {
		if (server->stall) {
			g_clear_error(&error);
			g_input_stream_read(g_io_stream_get_input_stream(stream), &byte, 1, server->cancel, &error);
		}
		return NULL;
	}
	/* Fragment header/body writes to expose buffering and framing bugs. */
	for (i = 0; i < server->reply_length;) {
		gsize chunk = MIN(server->fragment_size > 0 ? server->fragment_size : 1, server->reply_length - i);
		if (!g_output_stream_write_all(g_io_stream_get_output_stream(stream),
		    server->reply + i, chunk, NULL, server->cancel, &error))
			break;
		i += chunk;
	}
	g_io_stream_close(stream, NULL, NULL);
	return NULL;
}

/* Bind specifically to loopback and use an ephemeral port. */
static void
protocol_server_start(ProtocolServer *server, const gchar *reply, gsize length,
	const gchar *certificate)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GInetAddress) ip = g_inet_address_new_from_string("127.0.0.1");
	g_autoptr(GSocketAddress) address = g_inet_socket_address_new(ip, server->port);
	g_autoptr(GSocketAddress) effective = NULL;
	server->listener = g_socket_listener_new();
	server->cancel = g_cancellable_new();
	server->reply = reply;
	server->reply_length = length;
	g_assert_true(g_socket_listener_add_address(server->listener, address,
		G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, &effective, &error));
	g_assert_no_error(error);
	server->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
	if (certificate != NULL) {
		server->certificate = g_tls_certificate_new_from_file(certificate, &error);
		g_assert_no_error(error);
	}
	server->thread = g_thread_new("protocol-fixture", protocol_server_thread, server);
}

/* Finish without cancelling successful writes; callers await client EOF. */
static void
protocol_server_stop(ProtocolServer *server)
{
	g_cancellable_cancel(server->cancel);
	g_socket_listener_close(server->listener);
	g_thread_join(server->thread);
	g_clear_object(&server->listener);
	g_clear_object(&server->cancel);
	g_clear_object(&server->certificate);
}

/* Delete only the private fixture directory and its directly owned files. */
static void
protocol_remove_directory(const gchar *path)
{
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
	const gchar *name;
	if (dir != NULL) {
		while ((name = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *file = g_build_filename(path, name, NULL);
			g_assert_cmpint(g_unlink(file), ==, 0);
		}
	}
	g_assert_cmpint(g_rmdir(path), ==, 0);
}
#endif
