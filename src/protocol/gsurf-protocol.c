/*
 * Native Gopher and Gemini transport, independent of GTK and WebKit.
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "gsurf-protocol.h"
#include <errno.h>
#include <string.h>

#define RESPONSE_LIMIT (32 * 1024 * 1024)
#define HEADER_LIMIT (1027)
#define REQUEST_TIMEOUT (30)

/* Each worker owns a snapshot, never a view or other GTK object. */
typedef struct {
	gchar *uri;
	gchar *trust_directory;
	gchar *proxy_uri;
	GCancellable *caller_cancel;
	gulong cancel_handler;
	GSource *deadline;
} ProtocolRequest;

/* Release a completed response, including binary content. */
void
gsurf_protocol_response_free(GsurfProtocolResponse *response)
{
	if (response == NULL)
		return;
	g_clear_pointer(&response->body, g_bytes_unref);
	g_free(response->mime);
	g_free(response->meta);
	g_free(response->uri);
	g_free(response);
}

/* Release the worker's immutable request snapshot. */
static void
request_free(gpointer data)
{
	ProtocolRequest *request = data;
	if (request->caller_cancel != NULL) {
		g_cancellable_disconnect(request->caller_cancel, request->cancel_handler);
		g_object_unref(request->caller_cancel);
	}
	g_source_destroy(request->deadline);
	g_source_unref(request->deadline);
	g_free(request->uri);
	g_free(request->trust_directory);
	g_free(request->proxy_uri);
	g_free(request);
}

/* Reject decoded control characters before constructing a line protocol. */
static gboolean
valid_selector(const gchar *text)
{
	const guchar *p;
	for (p = (const guchar *)text; *p != '\0'; p++) {
		if (*p < 32 && *p != '\t')
			return FALSE;
		if (*p == 127)
			return FALSE;
	}
	return TRUE;
}

/* Parse without decoding the path until the Gopher type has been separated.
 * Fragments never travel on the wire; encoded CR/LF cannot inject requests. */
gchar *
gsurf_protocol_request(const gchar *uri, gchar **host, guint16 *port,
	gchar *item_type, gboolean *gemini, GError **error)
{
	g_autoptr(GUri) parsed = NULL;
	g_autofree gchar *selector = NULL;
	g_autofree gchar *wire_uri = NULL;
	const gchar *scheme, *path;
	gint parsed_port;

	parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED, error);
	if (parsed == NULL)
		return NULL;
	scheme = g_uri_get_scheme(parsed);
	if (scheme == NULL || (g_ascii_strcasecmp(scheme, "gopher") != 0 &&
	    g_ascii_strcasecmp(scheme, "gemini") != 0) ||
	    g_uri_get_host(parsed) == NULL || *g_uri_get_host(parsed) == '\0' ||
	    g_uri_get_userinfo(parsed) != NULL || strpbrk(uri, "\r\n\t ") != NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"Expected a gopher:// or gemini:// URI with a host and no userinfo or whitespace");
		return NULL;
	}
	*gemini = g_ascii_strcasecmp(scheme, "gemini") == 0;
	parsed_port = g_uri_get_port(parsed);
	if (parsed_port == 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Port zero is invalid");
		return NULL;
	}
	*port = (guint16)(parsed_port < 0 ? (*gemini ? 1965 : 70) : parsed_port);
	path = g_uri_get_path(parsed);
	*item_type = '1';
	if (*gemini) {
		wire_uri = g_uri_join(G_URI_FLAGS_ENCODED, "gemini", NULL,
			g_uri_get_host(parsed), parsed_port, *path != '\0' ? path : "/",
			g_uri_get_query(parsed), NULL);
		if (strlen(wire_uri) > 1024) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				"Gemini request URI exceeds 1024 bytes");
			return NULL;
		}
	} else {
		if (*path == '/')
			path++;
		if (*path != '\0')
			*item_type = *path++;
		selector = g_uri_unescape_string(path, NULL);
		if (selector == NULL || !valid_selector(selector) || g_uri_get_query(parsed) != NULL) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				"Invalid Gopher selector (use %09 before search terms; no CR, LF or NUL)");
			return NULL;
		}
		wire_uri = g_steal_pointer(&selector);
	}
	*host = g_strdup(g_uri_get_host(parsed));
	return g_strconcat(wire_uri, "\r\n", NULL);
}

/* Accept the six status classes, including unknown class members. Header
 * input excludes CRLF; callers must enforce framing before reaching here. */
gboolean
gsurf_protocol_parse_header(const gchar *header, gint *status, gchar **meta,
	GError **error)
{
	gsize length;
	const gchar *p;

	length = strlen(header);
	if (length < 2 || length > HEADER_LIMIT || header[0] < '1' || header[0] > '6' ||
	    !g_ascii_isdigit(header[1]) ||
	    (length > 2 && header[2] != ' ') ||
	    (length == 2 && header[0] < '4') || !g_utf8_validate(header, -1, NULL))
		goto invalid;
	for (p = header; *p != '\0'; p = g_utf8_next_char(p)) {
		if (g_unichar_iscntrl(g_utf8_get_char(p)))
			goto invalid;
	}
	*status = (header[0] - '0') * 10 + header[1] - '0';
	*meta = g_strdup(length > 2 ? header + 3 : "");
	return TRUE;
invalid:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		"Malformed Gemini response header (expected status and UTF-8 META, at most 1024 bytes)");
	return FALSE;
}

/* Self-signed certificates are normal on Gemini. Keep all other TLS checks,
 * including hostname and validity, then require a persistent certificate pin. */
static gboolean
accept_certificate(GTlsConnection *connection, GTlsCertificate *certificate,
	GTlsCertificateFlags errors, gpointer data)
{
	return (errors & ~G_TLS_CERTIFICATE_UNKNOWN_CA) == 0;
}

/* One exclusive file per canonical host/port avoids read-modify-write races.
 * A lock protects readers from seeing a partially written first-use pin in
 * this process; exclusive creation fails closed across processes. */
gboolean
gsurf_protocol_check_pin(GTlsCertificate *certificate, const gchar *host,
	guint16 port, const gchar *directory, GError **error)
{
	static GMutex pin_lock;
	g_autoptr(GByteArray) der = NULL;
	g_autofree gchar *identity = NULL;
	g_autofree gchar *lower_host = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *fingerprint = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *saved = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFileOutputStream) output = NULL;
	g_autoptr(GError) local_error = NULL;
	gboolean success = FALSE;

	if (certificate != NULL)
		g_object_get(certificate, "certificate", &der, NULL);
	if (der == NULL) {
		g_set_error_literal(error, G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE,
			"TLS backend did not provide a server certificate");
		return FALSE;
	}
	lower_host = g_ascii_strdown(host, -1);
	identity = g_strdup_printf("%s:%u", lower_host, port);
	key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
	fingerprint = g_compute_checksum_for_data(G_CHECKSUM_SHA256, der->data, der->len);
	path = g_build_filename(directory, key, NULL);
	file = g_file_new_for_path(path);
	g_mutex_lock(&pin_lock);
	if (g_mkdir_with_parents(directory, 0700) != 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			"Cannot create Gemini certificate directory: %s", g_strerror(errno));
		goto out;
	}
	output = g_file_create(file, G_FILE_CREATE_PRIVATE, NULL, &local_error);
	if (output != NULL) {
		success = g_output_stream_write_all(G_OUTPUT_STREAM(output), fingerprint,
			strlen(fingerprint), NULL, NULL, error) &&
			g_output_stream_close(G_OUTPUT_STREAM(output), NULL, error);
	} else if (g_error_matches(local_error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
		if (!g_file_get_contents(path, &saved, NULL, error))
			goto out;
		success = g_strcmp0(saved, fingerprint) == 0;
		if (!success)
			g_set_error(error, G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE,
				"Gemini certificate changed for %s. Verify the change before removing pin %s",
				identity, path);
	} else {
		g_propagate_error(error, g_steal_pointer(&local_error));
	}
out:
	g_mutex_unlock(&pin_lock);
	return success;
}

/* Read a bounded CRLF-terminated header without buffering body bytes away. */
static gchar *
read_header(GInputStream *input, GCancellable *cancellable, GError **error)
{
	g_autoptr(GString) line = g_string_new(NULL);
	gchar byte;
	gssize size;

	while (line->len <= HEADER_LIMIT + 1) {
		size = g_input_stream_read(input, &byte, 1, cancellable, error);
		if (size < 0)
			return NULL;
		if (size == 0 || byte == '\0')
			break;
		if (byte == '\n') {
			if (line->len == 0 || line->str[line->len - 1] != '\r')
				break;
			g_string_truncate(line, line->len - 1);
			return g_string_free(g_steal_pointer(&line), FALSE);
		}
		g_string_append_c(line, byte);
	}
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		"Truncated, oversized or non-CRLF Gemini header");
	return NULL;
}

/* Preserve binary bodies, but remove dot framing and dot-stuffing from the
 * line-oriented Gopher text and menu formats. */
static gchar *
gopher_text(const gchar *data)
{
	g_auto(GStrv) lines = g_strsplit(data, "\n", -1);
	g_autoptr(GString) result = g_string_new(NULL);
	guint i;

	for (i = 0; lines[i] != NULL; i++) {
		gchar *line = lines[i];
		gsize length = strlen(line);
		if (length > 0 && line[length - 1] == '\r')
			line[--length] = '\0';
		if (g_str_equal(line, "."))
			break;
		if (g_str_has_prefix(line, ".."))
			line++;
		g_string_append(result, line);
		if (lines[i + 1] != NULL)
			g_string_append_c(result, '\n');
	}
	return g_string_free(g_steal_pointer(&result), FALSE);
}

/* Extract a quoted or unquoted charset parameter from a media type. */
static gchar *
content_charset(const gchar *mime)
{
	g_auto(GStrv) parameters = g_strsplit(mime, ";", -1);
	guint i;
	for (i = 1; parameters[i] != NULL; i++) {
		gchar *parameter = g_strstrip(parameters[i]);
		gchar *equal = strchr(parameter, '=');
		if (equal != NULL) {
			*equal++ = '\0';
			if (g_ascii_strcasecmp(g_strstrip(parameter), "charset") == 0) {
				gsize length;
				gchar *charset = g_strstrip(equal);
				length = strlen(charset);
				if (length >= 2 && *charset == '"' && charset[length - 1] == '"') {
					charset[length - 1] = '\0';
					charset++;
				}
				return g_strdup(charset);
			}
		}
	}
	return NULL;
}

/* Convert before applying Gemtext syntax, preserving the encoding contract. */
static gchar *
gemtext_decode(GByteArray *bytes, const gchar *mime, GError **error)
{
	g_autofree gchar *charset = content_charset(mime);
	g_autofree gchar *converted = NULL;
	gsize written = 0;
	converted = g_convert(bytes->len > 0 ? (const gchar *)bytes->data : "", bytes->len,
		"UTF-8", charset != NULL ? charset : "UTF-8", NULL, &written, error);
	/* Replace embedded NULs so C-string parsing cannot silently lose a tail. */
	return converted != NULL ? g_utf8_make_valid(converted, written) : NULL;
}

/* MIME parameters and optional whitespace do not change the media type. */
static gboolean
is_gemtext(const gchar *mime)
{
	g_autofree gchar *type = g_strndup(mime, strcspn(mime, ";"));
	return g_ascii_strcasecmp(g_strstrip(type), "text/gemini") == 0;
}

/* Fetch one response. Redirects remain visible links: no invisible
 * cross-protocol requests and no redirect loop can consume worker threads. */
static void
fetch_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
	ProtocolRequest *request = task_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *host = NULL;
	g_autofree gchar *wire = NULL;
	g_autofree gchar *header = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *rendered = NULL;
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketConnection) socket = NULL;
	g_autoptr(GIOStream) tls = NULL;
	g_autoptr(GSocketConnectable) identity = NULL;
	g_autoptr(GByteArray) bytes = g_byte_array_new();
	g_autoptr(GsurfProtocolResponse) response = g_new0(GsurfProtocolResponse, 1);
	GIOStream *stream;
	GInputStream *input;
	guint16 port;
	gchar item_type;
	gboolean gemini;
	guchar buffer[8192];
	gssize count;

	wire = gsurf_protocol_request(request->uri, &host, &port, &item_type, &gemini, &error);
	if (wire == NULL)
		goto fail;
	response->uri = g_strdup(request->uri);
	response->status = 20;
	/* Type 7 without search terms is a local input page, not a request. */
	if (!gemini && item_type == '7' && strchr(wire, '\t') == NULL) {
		response->status = 10;
		response->meta = g_strdup("Search this Gopher index");
		goto done;
	}
	g_socket_client_set_timeout(client, REQUEST_TIMEOUT);
	if (request->proxy_uri != NULL && *request->proxy_uri != '\0') {
		g_autoptr(GProxyResolver) proxy = g_simple_proxy_resolver_new(request->proxy_uri, NULL);
		g_socket_client_set_proxy_resolver(client, proxy);
	}
	identity = g_network_address_new(host, port);
	socket = g_socket_client_connect(client, identity, cancellable, &error);
	if (socket == NULL)
		goto fail;
	stream = G_IO_STREAM(socket);
	if (gemini) {
		tls = g_tls_client_connection_new(stream, identity, &error);
		if (tls == NULL)
			goto fail;
		g_signal_connect(tls, "accept-certificate", G_CALLBACK(accept_certificate), NULL);
		if (!g_tls_connection_handshake(G_TLS_CONNECTION(tls), cancellable, &error))
			goto fail;
		if (g_tls_connection_get_protocol_version(G_TLS_CONNECTION(tls)) < G_TLS_PROTOCOL_VERSION_TLS_1_2) {
			g_set_error_literal(&error, G_TLS_ERROR, G_TLS_ERROR_HANDSHAKE,
				"Gemini requires TLS 1.2 or newer");
			goto fail;
		}
		if (!gsurf_protocol_check_pin(g_tls_connection_get_peer_certificate(G_TLS_CONNECTION(tls)),
			    host, port, request->trust_directory, &error))
			goto fail;
		stream = tls;
	}
	if (!g_output_stream_write_all(g_io_stream_get_output_stream(stream), wire,
	    strlen(wire), NULL, cancellable, &error))
		goto fail;
	input = g_io_stream_get_input_stream(stream);
	if (gemini) {
		header = read_header(input, cancellable, &error);
		if (header == NULL || !gsurf_protocol_parse_header(header,
		    &response->status, &response->meta, &error))
			goto fail;
		if (response->status / 10 != 2)
			goto done;
		response->mime = g_strdup(*response->meta != '\0' ? response->meta : "text/gemini; charset=utf-8");
		if (g_ascii_strncasecmp(response->mime, "text/", 5) == 0) {
			g_autofree gchar *charset = content_charset(response->mime);
			if (charset == NULL) {
				gchar *with_charset = g_strconcat(response->mime, "; charset=utf-8", NULL);
				g_free(response->mime);
				response->mime = with_charset;
			}
		}
	} else {
		switch (item_type) {
		case '0': response->mime = g_strdup("text/plain; charset=utf-8"); break;
		case '1': case '7': response->mime = g_strdup("text/html; charset=utf-8"); break;
		case 'g': response->mime = g_strdup("image/gif"); break;
		case 'h': response->mime = g_strdup("text/html; charset=utf-8"); break;
		default: response->mime = g_strdup("application/octet-stream"); break;
		}
	}
	while ((count = g_input_stream_read(input, buffer, sizeof(buffer), cancellable, &error)) > 0) {
		if (bytes->len + count > RESPONSE_LIMIT) {
			g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
				"Protocol response exceeds the 32 MiB limit");
			goto fail;
		}
		g_byte_array_append(bytes, buffer, (guint)count);
	}
	if (count < 0)
		goto fail;
	if ((!gemini && (item_type == '0' || item_type == '1' || item_type == '7')) ||
	    (gemini && is_gemtext(response->mime))) {
		g_autofree gchar *valid = gemini ? gemtext_decode(bytes, response->mime, &error) :
			g_utf8_make_valid(bytes->len > 0 ? (const gchar *)bytes->data : "", bytes->len);
		if (valid == NULL)
			goto fail;
		text = gemini ? g_strdup(valid) : gopher_text(valid);
		if (gemini || item_type != '0') {
			rendered = gsurf_protocol_render(text, request->uri, !gemini);
			g_free(response->mime);
			response->mime = g_strdup("text/html; charset=utf-8");
		} else {
			rendered = g_strdup(text);
		}
		response->body = g_bytes_new(rendered, strlen(rendered));
	} else {
		if (!gemini && item_type == 'I') {
			g_autofree gchar *content_type = g_content_type_guess(NULL, bytes->data, bytes->len, NULL);
			g_free(response->mime);
			response->mime = g_content_type_get_mime_type(content_type);
		}
		response->body = g_byte_array_free_to_bytes(g_steal_pointer(&bytes));
	}
done:
	if (g_cancellable_set_error_if_cancelled(cancellable, &error))
		goto fail;
	g_source_destroy(request->deadline);
	g_task_return_pointer(task, g_steal_pointer(&response), (GDestroyNotify)gsurf_protocol_response_free);
	return;
fail:
	g_source_destroy(request->deadline);
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
	    (request->caller_cancel == NULL || !g_cancellable_is_cancelled(request->caller_cancel))) {
		g_clear_error(&error);
		g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
			"Protocol request exceeded its 30-second deadline");
	}
	g_task_return_error(task, g_steal_pointer(&error));
}

/* Forward caller cancellation without ever cancelling caller-owned objects
 * when our independent total deadline expires. */
static void
forward_cancel(GCancellable *caller, gpointer data)
{
	g_cancellable_cancel(G_CANCELLABLE(data));
}

/* Bound DNS, slow-drip responses and TLS handshakes as one transaction. */
static gboolean
request_expired(gpointer data)
{
	g_cancellable_cancel(G_CANCELLABLE(data));
	return G_SOURCE_REMOVE;
}

/* Start isolated worker I/O and deliver completion on the caller's context. */
void
gsurf_protocol_fetch_async(const gchar *uri, const gchar *trust_directory, const gchar *proxy_uri,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GCancellable) internal_cancel = g_cancellable_new();
	g_autoptr(GTask) task = g_task_new(NULL, internal_cancel, callback, user_data);
	ProtocolRequest *request = g_new0(ProtocolRequest, 1);
	request->uri = g_strdup(uri);
	request->proxy_uri = g_strdup(proxy_uri);
	request->trust_directory = trust_directory != NULL ? g_strdup(trust_directory) :
		g_build_filename(g_get_user_data_dir(), "gsurf", "gemini", "certificates", NULL);
	if (cancellable != NULL) {
		request->caller_cancel = g_object_ref(cancellable);
		request->cancel_handler = g_cancellable_connect(cancellable, G_CALLBACK(forward_cancel),
			g_object_ref(internal_cancel), g_object_unref);
	}
	request->deadline = g_timeout_source_new_seconds(REQUEST_TIMEOUT);
	g_source_set_callback(request->deadline, request_expired, g_object_ref(internal_cancel), g_object_unref);
	g_source_attach(request->deadline, g_task_get_context(task));
	/* Worker classifies timeout separately from caller cancellation. */
	g_task_set_check_cancellable(task, FALSE);
	g_task_set_task_data(task, request, request_free);
	g_task_run_in_thread(task, fetch_worker);
}

/* Transfer the response to the owner; GTask also propagates cancellation. */
GsurfProtocolResponse *
gsurf_protocol_fetch_finish(GAsyncResult *result, GError **error)
{
	return g_task_propagate_pointer(G_TASK(result), error);
}
