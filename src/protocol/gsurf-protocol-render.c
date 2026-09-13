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

	if (*line == '\0')
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
	return g_uri_join(G_URI_FLAGS_ENCODED, g_uri_get_scheme(parsed), NULL,
		g_uri_get_host(parsed), g_uri_get_port(parsed), path,
		gemini ? escaped : NULL, NULL);
}
