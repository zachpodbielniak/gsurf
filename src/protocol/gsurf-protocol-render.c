/*
 * Safe, script-free presentation of Gopher menus and Gemini documents.
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "gsurf-protocol.h"
#include <string.h>

/* A restrictive policy also applies to untrusted HTML returned by a server
 * through the WebKit adapter. Native navigation remains available. */
static const gchar page_start[] =
	"<!doctype html><html><head><meta charset='utf-8'>"
	"<meta name='viewport' content='width=device-width,initial-scale=1'>"
	"<meta http-equiv='Content-Security-Policy' content=\"default-src 'none'; "
	"style-src 'unsafe-inline'; form-action gopher: gemini:\">"
	"<title>gsurf — small web</title><style>"
	"body{max-width:52em;margin:3em auto;padding:0 1.5em;"
	"font:18px/1.6 sans-serif;color:#242424;background:#faf9f6}"
	"a{color:#235788}pre{overflow:auto;white-space:pre-wrap}"
	"blockquote{border-left:3px solid #aaa;margin-left:0;padding-left:1em}"
	".menu{white-space:pre-wrap;font-family:monospace}input,button{font:inherit}"
	"</style></head><body>";

/* Only actual browser navigation schemes may become active links. Relative
 * references are resolved now, so custom-scheme URL quirks cannot alter them. */
static gchar *
safe_link(const gchar *base, const gchar *target)
{
	g_autofree gchar *resolved = NULL;
	const gchar *scheme;

	resolved = base != NULL ? g_uri_resolve_relative(base, target, G_URI_FLAGS_ENCODED, NULL) :
		g_strdup(target);
	if (resolved == NULL)
		return NULL;
	scheme = g_uri_peek_scheme(resolved);
	if (scheme == NULL || (g_ascii_strcasecmp(scheme, "gemini") != 0 &&
	    g_ascii_strcasecmp(scheme, "gopher") != 0 &&
	    g_ascii_strcasecmp(scheme, "http") != 0 &&
	    g_ascii_strcasecmp(scheme, "https") != 0 &&
	    g_ascii_strcasecmp(scheme, "mailto") != 0))
		return NULL;
	return g_markup_escape_text(resolved, -1);
}

/* Render a link or inert label when the target is invalid/unsupported. */
static void
append_link(GString *html, const gchar *base, const gchar *target, const gchar *label)
{
	g_autofree gchar *href = safe_link(base, target);
	g_autofree gchar *escaped = g_markup_escape_text(label, -1);
	if (href != NULL)
		g_string_append_printf(html, "<a href=\"%s\">%s</a>", href, escaped);
	else
		g_string_append(html, escaped);
}

/* Parse menu fields without interpreting selector bytes as URI syntax. */
static void
append_menu_line(GString *html, const gchar *line)
{
	g_auto(GStrv) fields = NULL;
	g_autofree gchar *selector = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *target = NULL;
	g_autofree gchar *label = NULL;
	gchar *end = NULL;
	guint64 port;

	if (*line == '\0' || line[1] == '\0')
		return;
	fields = g_strsplit(line + 1, "\t", -1);
	if (*line == 'i' || *line == '3' || g_strv_length(fields) < 4) {
		label = g_markup_escape_text(fields[0], -1);
		g_string_append_printf(html, "%s\n", label);
		return;
	}
	if (*line == 'h' && g_str_has_prefix(fields[1], "URL:")) {
		append_link(html, NULL, fields[1] + 4, fields[0]);
	} else {
		port = g_ascii_strtoull(fields[3], &end, 10);
		if (*fields[2] == '\0' || *fields[3] == '\0' || *end != '\0' || port == 0 || port > 65535 ||
		    strpbrk(fields[2], "/?#@ \r\n\t") != NULL) {
			label = g_markup_escape_text(fields[0], -1);
			g_string_append_printf(html, "%s\n", label);
			return;
		}
		/* Unknown and binary types retain their Gopher type for downloads. */
		selector = g_uri_escape_string(fields[1], "/", FALSE);
		path = g_strdup_printf("/%c%s", *line, selector);
		target = g_uri_join(G_URI_FLAGS_ENCODED, "gopher", NULL, fields[2],
			port == 70 ? -1 : (gint)port, path, NULL, NULL);
		if (g_strv_length(fields) >= 5 &&
		    (g_str_equal(fields[4], "+") || g_str_equal(fields[4], "?"))) {
			g_autofree gchar *plus = gsurf_gopher_uri(target,
				strchr(":;<", *line) != NULL ? "!" : fields[4]);
			g_autofree gchar *info = gsurf_gopher_uri(target, "!");
			append_link(html, NULL, plus, fields[0]);
			g_string_append(html, "  [");
			append_link(html, NULL, info, "attributes / views");
			g_string_append(html, "]\n");
			return;
		}
		append_link(html, NULL, target, fields[0]);
	}
	g_string_append_c(html, '\n');
}

/* Gemtext has line-level markup only. Preformatted mode takes precedence
 * over every other marker, and list groups are closed at each transition. */
gchar *
gsurf_protocol_render(const gchar *text, const gchar *uri, gboolean menu)
{
	g_auto(GStrv) lines = g_strsplit(g_str_has_prefix(text, "\357\273\277") ? text + 3 : text, "\n", -1);
	g_autoptr(GString) html = g_string_new(page_start);
	gboolean pre = FALSE, list = FALSE;
	guint i;

	if (menu)
		g_string_append(html, "<div class='menu'>");
	for (i = 0; lines[i] != NULL; i++) {
		gchar *line = lines[i];
		gsize length = strlen(line);
		g_autofree gchar *escaped = NULL;
		if (length > 0 && line[length - 1] == '\r')
			line[length - 1] = '\0';
		if (menu) {
			if (g_str_equal(line, "."))
				break;
			append_menu_line(html, line);
			continue;
		}
		if (list && !g_str_has_prefix(line, "* ")) {
			g_string_append(html, "</ul>");
			list = FALSE;
		}
		if (g_str_has_prefix(line, "```")) {
			g_string_append(html, pre ? "</pre>" : "<pre>");
			pre = !pre;
			continue;
		}
		escaped = g_markup_escape_text(line, -1);
		if (pre) {
			g_string_append_printf(html, "%s\n", escaped);
		} else if (g_str_has_prefix(line, "=>")) {
			gchar *target = line + 2, *label;
			while (*target == ' ' || *target == '\t')
				target++;
			label = target;
			while (*label != '\0' && *label != ' ' && *label != '\t')
				label++;
			if (*label != '\0') {
				*label++ = '\0';
				while (*label == ' ' || *label == '\t')
					label++;
			}
			g_string_append(html, "<p>");
			if (*target != '\0')
				append_link(html, uri, target, *label != '\0' ? label : target);
			g_string_append(html, "</p>");
		} else if (g_str_has_prefix(line, "* ")) {
			if (!list)
				g_string_append(html, "<ul>");
			list = TRUE;
			g_string_append_printf(html, "<li>%s</li>", escaped + 2);
		} else if (*line == '>') {
			g_string_append_printf(html, "<blockquote>%s</blockquote>", escaped + 4);
		} else if (*line == '#') {
			guint level = 1;
			while (level < 3 && line[level] == '#')
				level++;
			g_string_append_printf(html, "<h%u>%s</h%u>", level, escaped + level, level);
		} else {
			g_string_append_printf(html, "<p>%s</p>", escaped);
		}
	}
	if (list)
		g_string_append(html, "</ul>");
	if (pre)
		g_string_append(html, "</pre>");
	if (menu)
		g_string_append(html, "</div>");
	g_string_append(html, "</body></html>");
	return g_string_free(g_steal_pointer(&html), FALSE);
}

/* Forms work with JavaScript disabled. The backend translates the browser's
 * application/x-www-form-urlencoded submission into the protocol query. */
gchar *
gsurf_protocol_input_page(const gchar *uri, const gchar *prompt, gboolean sensitive)
{
	g_autofree gchar *action = g_markup_escape_text(uri, -1);
	g_autofree gchar *label = g_markup_escape_text(prompt, -1);
	return g_strdup_printf("%s<form method='get' action=\"%s\">"
		"<label for='query'>%s</label><p><input id='query' name='gsurf-query' "
		"type='%s' autocomplete='off' autofocus><button>Submit</button></p>"
		"</form></body></html>", page_start, action, label, sensitive ? "password" : "text");
}

/* Protocol errors and redirects are documents at the requested URI. */
gchar *
gsurf_protocol_message_page(const gchar *title, const gchar *message, const gchar *link)
{
	g_autoptr(GString) html = g_string_new(page_start);
	g_autofree gchar *heading = g_markup_escape_text(title, -1);
	g_autofree gchar *body = g_markup_escape_text(message, -1);
	g_string_append_printf(html, "<h1>%s</h1><p>%s</p>", heading, body);
	if (link != NULL)
		append_link(html, NULL, link, "Continue to destination");
	g_string_append(html, "</body></html>");
	return g_string_free(g_steal_pointer(&html), FALSE);
}

/* Only call this for a form submission from our generated input document.
 * Existing Gemini queries are replaced; Gopher search uses an encoded tab. */
gchar *
gsurf_protocol_input_uri(const gchar *uri, GError **error)
{
	g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED, error);
	g_autofree gchar *form = NULL;
	g_autofree gchar *decoded = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *path = NULL;
	const gchar *query;
	gchar *p;
	gboolean gemini;

	if (parsed == NULL)
		return NULL;
	query = g_uri_get_query(parsed);
	if (query != NULL && g_str_has_prefix(query, "gsurf-ask-"))
		return gsurf_gopher_ask_uri(uri, error);
	if (query == NULL || !g_str_has_prefix(query, "gsurf-query=") || strchr(query, '&') != NULL)
		return NULL;
	gemini = g_ascii_strcasecmp(g_uri_get_scheme(parsed), "gemini") == 0;
	if (!gemini && g_ascii_strcasecmp(g_uri_get_scheme(parsed), "gopher") != 0)
		return NULL;
	form = g_strdup(query + strlen("gsurf-query="));
	for (p = form; *p != '\0'; p++) {
		if (*p == '+')
			*p = ' ';
	}
	decoded = g_uri_unescape_string(form, NULL);
	if (decoded == NULL)
		return NULL;
	escaped = g_uri_escape_string(decoded, NULL, FALSE);
	path = gemini ? g_strdup(g_uri_get_path(parsed)) :
		g_strconcat(g_uri_get_path(parsed), "%09", escaped, NULL);
	if (!gemini) {
		g_autofree gchar *raw = g_uri_unescape_string(g_uri_get_path(parsed), NULL);
		g_auto(GStrv) fields = raw != NULL ? g_strsplit(raw, "\t", 3) : NULL;
		if (fields != NULL && g_strv_length(fields) == 3) {
			g_autofree gchar *selector = g_uri_escape_string(fields[0], "/", FALSE);
			g_autofree gchar *command = g_uri_escape_string(fields[2], NULL, FALSE);
			g_free(path);
			path = g_strconcat(selector, "%09", escaped, "%09", command, NULL);
		}
	}
	return g_uri_join(G_URI_FLAGS_ENCODED, g_uri_get_scheme(parsed), NULL,
		g_uri_get_host(parsed), g_uri_get_port(parsed), path,
		gemini ? escaped : NULL, NULL);
}

/* A Gopher+ ASK block is a static form. Unknown or file-transfer fields
 * disable submission rather than sending a misaligned answer sequence. */
static void
append_ask(GString *html, gchar **lines, guint start, const gchar *uri)
{
	g_autofree gchar *action = g_markup_escape_text(uri, -1);
	gboolean supported = TRUE;
	guint i, index = 0;
	g_string_append_printf(html, "<form method='get' action=\"%s\">", action);
	for (i = start; lines[i] != NULL && lines[i][0] != '+'; i++) {
		gchar *line = lines[i], *colon;
		g_auto(GStrv) fields = NULL;
		g_autofree gchar *label = NULL;
		g_autofree gchar *value = NULL;
		const gchar *kind;
		if (*line == '\0')
			continue;
		if (g_str_has_suffix(line, "\r"))
			line[strlen(line) - 1] = '\0';
		line = g_strchug(line);
		colon = strchr(line, ':');
		if (colon == NULL) {
			supported = FALSE;
			continue;
		}
		*colon++ = '\0';
		kind = line;
		while (*colon == ' ')
			colon++;
		fields = g_strsplit(colon, "\t", -1);
		if (fields[0] == NULL) {
			g_strfreev(fields);
			fields = g_new0(gchar *, 2);
			fields[0] = g_strdup("");
		}
		label = g_markup_escape_text(fields[0], -1);
		value = g_markup_escape_text(fields[1] != NULL ? fields[1] : "", -1);
		if (g_ascii_strcasecmp(kind, "Note") == 0) {
			g_string_append_printf(html, "<p>%s</p>", label);
			continue;
		}
		g_string_append_printf(html, "<p><label>%s ", label);
		if (g_ascii_strcasecmp(kind, "Ask") == 0 || g_ascii_strcasecmp(kind, "AskP") == 0) {
			g_string_append_printf(html, "<input name='gsurf-ask-%u-a' type='%s' value=\"%s\" autocomplete='off'%s>",
				index, g_ascii_strcasecmp(kind, "AskP") == 0 ? "password" : "text", value,
				index == 0 ? " autofocus" : "");
		} else if (g_ascii_strcasecmp(kind, "AskL") == 0) {
			g_string_append_printf(html, "<textarea name='gsurf-ask-%u-l'>%s</textarea>", index, value);
		} else if (g_ascii_strcasecmp(kind, "Choose") == 0) {
			guint j;
			if (fields[1] == NULL)
				supported = FALSE;
			g_string_append_printf(html, "<select name='gsurf-ask-%u-a'>", index);
			for (j = 1; fields[j] != NULL; j++) {
				g_autofree gchar *option = g_markup_escape_text(fields[j], -1);
				g_string_append_printf(html, "<option value=\"%s\">%s</option>", option, option);
			}
			g_string_append(html, "</select>");
		} else if (g_ascii_strcasecmp(kind, "Select") == 0) {
			gboolean selected = g_str_has_suffix(fields[0], ":1");
			g_string_append_printf(html, "<select name='gsurf-ask-%u-a'><option value='0'%s>No</option>"
				"<option value='1'%s>Yes</option></select>", index,
				selected ? "" : " selected", selected ? " selected" : "");
		} else {
			supported = FALSE;
			g_string_append(html, "(unsupported field)");
		}
		g_string_append(html, "</label></p>");
		index++;
	}
	if (supported && index > 0)
		g_string_append(html, "<button>Submit</button>");
	else
		g_string_append(html, "<p>This form contains no supported answers or requires unsupported fields. "
			"File upload and server-selected download filenames are unavailable.</p>");
	g_string_append(html, "</form>");
}

/* Show unknown attributes as escaped text, and turn declared views into
 * explicit choices. Each +INFO resets the base for directory-wide metadata. */
gchar *
gsurf_gopher_attributes(const gchar *text, const gchar *uri)
{
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	g_autoptr(GString) html = g_string_new(page_start);
	g_autofree gchar *base = g_strdup(uri);
	g_autofree gchar *host = NULL;
	g_autofree gchar *wire = NULL;
	g_autofree gchar *command = NULL;
	guint16 port;
	gchar type;
	gboolean gemini;
	gboolean views = FALSE;
	guint i;
	wire = gsurf_protocol_request(uri, &host, &port, &type, &gemini, NULL);
	if (wire != NULL)
		command = gsurf_gopher_command(wire, type);
	g_string_append(html, "<h1>Gopher+ attributes</h1>");
	for (i = 0; lines[i] != NULL; i++) {
		gchar *line = lines[i];
		gsize length = strlen(line);
		g_autofree gchar *escaped = NULL;
		if (length > 0 && line[length - 1] == '\r')
			line[length - 1] = '\0';
		if (g_str_has_prefix(line, "+INFO: ") && line[7] != '\0') {
			g_auto(GStrv) fields = g_strsplit(line + 8, "\t", -1);
			g_string_append(html, "<div class='menu'>");
			append_menu_line(html, line + 7);
			g_string_append(html, "</div>");
			if (g_strv_length(fields) >= 4) {
				g_autofree gchar *selector = g_uri_escape_string(fields[1], "/", FALSE);
				g_autofree gchar *path = g_strdup_printf("/%c%s", line[7], selector);
				gchar *end;
				guint64 port = g_ascii_strtoull(fields[3], &end, 10);
				if (*fields[2] != '\0' && strpbrk(fields[2], "/?#@ \t\r\n") == NULL &&
				    *end == '\0' && port > 0 && port <= 65535) {
					g_free(base);
					base = g_uri_join(G_URI_FLAGS_ENCODED, "gopher", NULL, fields[2], port, path, NULL, NULL);
				}
			}
			views = FALSE;
			continue;
		}
		if (g_str_equal(line, "+ASK:")) {
			/* Submit to the document actually fetched, never an untrusted
			 * +INFO endpoint. Keep the same path for WebKit form validation. */
			if (command != NULL && *command == '$') {
				g_autofree gchar *target = gsurf_gopher_uri(base, "?");
				append_link(html, NULL, target, "Open this item's form");
			} else {
				append_ask(html, lines, i + 1, uri);
			}
			while (lines[i + 1] != NULL && lines[i + 1][0] != '+')
				i++;
			views = FALSE;
			continue;
		}
		if (*line == '+')
			views = g_str_equal(line, "+VIEWS:");
		else if (views && *line == ' ') {
			gchar *colon = strchr(line + 1, ':');
			g_autofree gchar *view = colon != NULL ? g_strndup(line + 1, colon - line - 1) : g_strdup(line + 1);
			g_autofree gchar *command = g_strconcat("+", g_strstrip(view), NULL);
			g_autofree gchar *target = gsurf_gopher_uri(base, command);
			g_string_append(html, "<p>");
			append_link(html, NULL, target, line + 1);
			g_string_append(html, "</p>");
			continue;
		}
		escaped = g_markup_escape_text(line, -1);
		g_string_append_printf(html, "<pre>%s</pre>", escaped);
	}
	g_string_append(html, "</body></html>");
	return g_string_free(g_steal_pointer(&html), FALSE);
}

/* Serialize one ordered answer per field into a dot-stuffed ASK block.
 * Multiline answers carry a line count; single-line answers reject controls. */
gchar *
gsurf_gopher_ask_uri(const gchar *uri, GError **error)
{
	g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED, error);
	g_auto(GStrv) pairs = NULL;
	g_autoptr(GString) block = g_string_new("+\t1\r\n+-1\r\n");
	guint i;
	if (parsed == NULL || g_strcmp0(g_uri_get_scheme(parsed), "gopher") != 0 ||
	    g_uri_get_query(parsed) == NULL || strlen(uri) > 65536)
		return NULL;
	pairs = g_strsplit(g_uri_get_query(parsed), "&", -1);
	for (i = 0; pairs[i] != NULL; i++) {
		gchar *equal = strchr(pairs[i], '='), *p;
		g_autofree gchar *key = NULL;
		g_autofree gchar *value = NULL;
		g_auto(GStrv) lines = NULL;
		guint j;
		gboolean multiline;
		if (equal == NULL)
			goto invalid;
		*equal++ = '\0';
		key = g_strdup_printf("gsurf-ask-%u-", i);
		if (!g_str_has_prefix(pairs[i], key))
			goto invalid;
		p = pairs[i] + strlen(key);
		if (!g_str_equal(p, "a") && !g_str_equal(p, "l"))
			goto invalid;
		multiline = *p == 'l';
		for (p = equal; *p != '\0'; p++)
			if (*p == '+')
				*p = ' ';
		value = g_uri_unescape_string(equal, NULL);
		if (value == NULL)
			goto invalid;
		for (p = value; *p != '\0'; p++) {
			if (((guchar)*p < 32 || *p == 127) &&
			    !(multiline && (*p == '\r' || *p == '\n')))
				goto invalid;
			if (*p == '\r' && p[1] != '\n')
				goto invalid;
		}
		lines = g_strsplit(value, "\n", -1);
		if (multiline)
			g_string_append_printf(block, "%u\r\n", g_strv_length(lines));
		else if (*value == '\0')
			g_string_append(block, "\r\n");
		for (j = 0; lines[j] != NULL; j++) {
			gsize length = strlen(lines[j]);
			if (length > 0 && lines[j][length - 1] == '\r')
				lines[j][length - 1] = '\0';
			g_string_append_printf(block, "%s%s\r\n", *lines[j] == '.' ? "." : "", lines[j]);
		}
	}
	g_string_append(block, ".\r\n");
	if (block->len > 65536)
		goto invalid;
	return gsurf_gopher_uri(uri, block->str);
invalid:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid Gopher+ form answers");
	return NULL;
}
