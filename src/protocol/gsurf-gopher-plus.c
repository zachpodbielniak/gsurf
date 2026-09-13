/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Gopher+ wire framing and URI commands. No UI or process-global state. */
#include "gsurf-protocol.h"
#include <string.h>
#include <errno.h>

#define PLUS_LIMIT (32 * 1024 * 1024)
#define ASK_LIMIT (64 * 1024)

/* Validate line-oriented values before allowing them onto a socket. */
static gboolean
line_value(const gchar *value)
{
	const guchar *p;
	for (p = (const guchar *)value; *p != '\0'; p++)
		if (*p < 32 || *p == 127)
			return FALSE;
	return TRUE;
}

/* RFC 4266 reserves a search slot even for non-index items. On the wire
 * only type 7 carries that slot (the original Gopher+ specification). */
gchar *
gsurf_gopher_request(const gchar *selector, gchar type, GError **error)
{
	g_auto(GStrv) fields = g_strsplit(selector, "\t", 3);
	guint n = g_strv_length(fields);
	const gchar *command = NULL;
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *operation = NULL;
	const gchar *payload = NULL;
	gchar *tab;

	if (n == 0)
		return g_strdup("\r\n");
	if (strlen(selector) > ASK_LIMIT || !line_value(fields[0]))
		goto invalid;
	if (n == 3) {
		if (!line_value(fields[1]) || (type != '7' && *fields[1] != '\0'))
			goto invalid;
		command = fields[2];
	} else if (n == 2 && type != '7') {
		command = fields[1];
	}
	prefix = type == '7' && n > 1 ? g_strconcat(fields[0], "\t", fields[1], NULL) : g_strdup(fields[0]);
	if (command == NULL) {
		if (n > 1 && !line_value(fields[1]))
			goto invalid;
		return g_strconcat(prefix, "\r\n", NULL);
	}
	operation = g_strdup(command);
	tab = strchr(operation, '\t');
	if (tab != NULL) {
		*tab++ = '\0';
		if (g_str_equal(tab, "0")) {
			/* Explicit no-data flag. */
		} else if (g_str_has_prefix(tab, "1\r\n+-1\r\n")) {
			g_auto(GStrv) lines = NULL;
			guint i, count;
			payload = tab + strlen("1\r\n+-1\r\n");
			lines = g_strsplit(payload, "\r\n", -1);
			count = g_strv_length(lines);
			if (count < 2 || !g_str_equal(lines[count - 2], ".") || *lines[count - 1] != '\0')
				goto invalid;
			for (i = 0; i < count - 2; i++)
				if (!line_value(lines[i]) || (*lines[i] == '.' && lines[i][1] != '.'))
					goto invalid;
		} else {
			goto invalid;
		}
	}
	if (!line_value(operation) || *operation == '\0' ||
	    strchr("+!$?", *operation) == NULL ||
	    (*operation == '?' && !g_str_equal(operation, "?")) ||
	    (payload != NULL && *operation != '+'))
		goto invalid;
	if (payload != NULL)
		return g_strdup_printf("%s\t%s\t1\r\n+-1\r\n%s", prefix, operation, payload);
	return g_strdup_printf("%s\t%s\r\n", prefix,
		g_str_equal(operation, "?") ? "!+ASK" : operation);
invalid:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		"Invalid Gopher+ request or ASK data (64 KiB limit; no injected request lines)");
	return NULL;
}

/* The normalized wire request has a command after selector/search, before
 * any optional data flag. Never inspect the response to guess the protocol. */
gchar *
gsurf_gopher_command(const gchar *wire, gchar type)
{
	const gchar *p = strchr(wire, '\t');
	if (p != NULL && type == '7')
		p = strchr(p + 1, '\t');
	return p != NULL ? g_strndup(p + 1, strcspn(p + 1, "\t\r\n")) : NULL;
}

/* Replace only the command slot, retaining the selector and any search. */
gchar *
gsurf_gopher_uri(const gchar *uri, const gchar *command)
{
	g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED, NULL);
	g_autofree gchar *decoded = NULL;
	g_auto(GStrv) fields = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *encoded = NULL;
	if (parsed == NULL)
		return NULL;
	decoded = g_uri_unescape_string(g_uri_get_path(parsed), NULL);
	if (decoded == NULL)
		return NULL;
	fields = g_strsplit(*decoded != '\0' && !g_str_equal(decoded, "/") ? decoded : "/1", "\t", 3);
	path = g_strdup_printf("%s\t%s\t%s", fields[0],
		g_str_has_prefix(fields[0], "/7") && fields[1] != NULL ? fields[1] : "", command);
	encoded = g_uri_escape_string(path, "/", FALSE);
	return g_uri_join(G_URI_FLAGS_ENCODED, "gopher", NULL, g_uri_get_host(parsed),
		g_uri_get_port(parsed), encoded, NULL, NULL);
}

/* Read exactly one framed block. Known lengths and dot termination finish
 * immediately, even if the peer keeps its connection open. */
gboolean
gsurf_gopher_read(GInputStream *input, GByteArray *bytes, GCancellable *cancel, GError **error)
{
	g_autoptr(GDataInputStream) data = g_data_input_stream_new(input);
	g_autoptr(GString) header = g_string_new(NULL);
	gchar c, *end;
	gint64 length;
	gssize count;
	guchar buffer[8192];
	gboolean failure;

	g_filter_input_stream_set_close_base_stream(G_FILTER_INPUT_STREAM(data), FALSE);
	while (header->len < 64) {
		count = g_input_stream_read(G_INPUT_STREAM(data), &c, 1, cancel, error);
		if (count < 0)
			return FALSE;
		if (count == 0 || c == '\0')
			goto invalid;
		g_string_append_c(header, c);
		if (c == '\n')
			break;
	}
	if (!g_str_has_suffix(header->str, "\r\n") || header->len < 4 ||
	    (header->str[0] != '+' && header->str[0] != '-'))
		goto invalid;
	g_string_truncate(header, header->len - 2);
	failure = header->str[0] == '-';
	errno = 0;
	length = g_ascii_strtoll(header->str + 1, &end, 10);
	if (errno != 0 || *end != '\0' ||
	    (!g_ascii_isdigit(header->str[1]) && !g_str_equal(header->str + 1, "-1") &&
	     !g_str_equal(header->str + 1, "-2")))
		goto invalid;
	if (length > PLUS_LIMIT)
		goto oversized;
	if (length == -1) {
		/* Bound each line as well as the complete transfer. GDataInputStream's
		 * unbounded read_line API would let a malicious peer bypass the cap. */
		gsize start = 0;
		for (;;) {
			count = g_input_stream_read(G_INPUT_STREAM(data), &c, 1, cancel, error);
			if (count < 0)
				return FALSE;
			if (count == 0)
				goto invalid;
			g_byte_array_append(bytes, (const guint8 *)&c, 1);
			if (bytes->len > PLUS_LIMIT)
				goto oversized;
			if (c == '\n') {
				gsize size = bytes->len - start;
				if (size < 2 || bytes->data[bytes->len - 2] != '\r')
					goto invalid;
				if (size == 3 && bytes->data[start] == '.') {
					g_byte_array_set_size(bytes, start);
					break;
				}
				if (size >= 4 && bytes->data[start] == '.' && bytes->data[start + 1] == '.')
					g_byte_array_remove_index(bytes, start);
				start = bytes->len;
			}
		}
	} else {
		while (length == -2 || bytes->len < (guint64)length) {
			gsize wanted = length == -2 ? sizeof(buffer) : MIN(sizeof(buffer), (gsize)length - bytes->len);
			count = g_input_stream_read(G_INPUT_STREAM(data), buffer, wanted, cancel, error);
			if (count < 0)
				return FALSE;
			if (count == 0) {
				if (length != -2)
					goto invalid;
				break;
			}
			if (bytes->len + count > PLUS_LIMIT)
				goto oversized;
			g_byte_array_append(bytes, buffer, count);
		}
	}
	if (failure) {
		g_autofree gchar *message = g_utf8_make_valid(bytes->len ? (const gchar *)bytes->data : "", bytes->len);
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Gopher+ server error: %s", message);
		return FALSE;
	}
	return TRUE;
oversized:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Gopher+ response exceeds the 32 MiB limit");
	return FALSE;
invalid:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Malformed or truncated Gopher+ transfer");
	return FALSE;
}
