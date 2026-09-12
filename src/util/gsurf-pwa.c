/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "util/gsurf-pwa.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

/* Desktop Exec syntax is not shell syntax: quote every argument, escape
 * its four reserved characters, and double literal percent field markers.
 * GKeyFile supplies the second (desktop string) layer of backslash escaping. */
static gchar *
quote_exec_argument(const gchar *argument)
{
	GString *quoted;
	const gchar *p;

	quoted = g_string_new("\"");
	for (p = argument; *p != '\0'; p++) {
		if (strchr("\"`$\\", *p) != NULL)
			g_string_append_c(quoted, '\\');
		if (*p == '%')
			g_string_append_c(quoted, '%');
		g_string_append_c(quoted, *p);
	}
	g_string_append_c(quoted, '"');
	return g_string_free(quoted, FALSE);
}

/* Keep line/control characters out of launcher arguments and labels. */
static gboolean
valid_text(const gchar *text)
{
	const gchar *p;

	if (text == NULL || *text == '\0' || !g_utf8_validate(text, -1, NULL))
		return FALSE;
	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		if (g_unichar_iscntrl(g_utf8_get_char(p)))
			return FALSE;
	}
	return TRUE;
}

gchar *
gsurf_pwa_install(const gchar *uri, const gchar *name,
	const gchar *executable, const gchar *applications_dir, GError **error)
{
	g_autoptr(GUri) parsed = NULL;
	g_autoptr(GKeyFile) desktop = NULL;
	g_autoptr(GFile) file = NULL;
	g_autofree gchar *quoted_executable = NULL;
	g_autofree gchar *quoted_uri = NULL;
	g_autofree gchar *exec = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *filename = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *data = NULL;
	const gchar *scheme;
	const gchar *host;
	gsize length;

	/* Validate before creating directories; malformed input leaves no state. */
	if (!valid_text(uri) || (name != NULL && !valid_text(name)) ||
	    !valid_text(executable) || !g_path_is_absolute(executable) ||
	    applications_dir == NULL || !g_path_is_absolute(applications_dir)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"PWA requires a site URL, nonempty name, and absolute executable/destination paths");
		return NULL;
	}
	/* GLib checks Exec's executable before expanding %% field escapes.
	 * Refuse this rare path instead of installing an unlaunchable entry. */
	if (strchr(executable, '%') != NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"PWA executable path cannot contain '%'; move the executable and retry");
		return NULL;
	}
	if (!g_file_test(executable, G_FILE_TEST_IS_REGULAR) ||
	    !g_file_test(executable, G_FILE_TEST_IS_EXECUTABLE)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"PWA executable '%s' is not an executable file", executable);
		return NULL;
	}
	parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED, error);
	if (parsed == NULL)
		return NULL;
	scheme = g_uri_get_scheme(parsed);
	host = g_uri_get_host(parsed);
	if (scheme == NULL ||
	    (g_ascii_strcasecmp(scheme, "http") != 0 &&
	     g_ascii_strcasecmp(scheme, "https") != 0) ||
	    host == NULL || *host == '\0' || g_uri_get_userinfo(parsed) != NULL ||
	    strpbrk(uri, " \t\r\n") != NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"PWA URL must be an absolute HTTP(S) URL without credentials or whitespace");
		return NULL;
	}

	/* Hash the complete URI so distinct apps on one host do not collide. */
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, uri, -1);
	filename = g_strdup_printf("gsurf-pwa-%s.desktop", digest);
	path = g_build_filename(applications_dir, filename, NULL);
	quoted_executable = quote_exec_argument(executable);
	quoted_uri = quote_exec_argument(uri);
	exec = g_strdup_printf("%s --kiosk -- %s", quoted_executable, quoted_uri);
	desktop = g_key_file_new();
	g_key_file_set_string(desktop, "Desktop Entry", "Type", "Application");
	g_key_file_set_string(desktop, "Desktop Entry", "Name", name != NULL ? name : host);
	g_key_file_set_string(desktop, "Desktop Entry", "Exec", exec);
	g_key_file_set_string(desktop, "Desktop Entry", "Icon", "gsurf");
	g_key_file_set_string(desktop, "Desktop Entry", "Categories", "Network;WebBrowser;");
	g_key_file_set_boolean(desktop, "Desktop Entry", "Terminal", FALSE);
	g_key_file_set_boolean(desktop, "Desktop Entry", "StartupNotify", TRUE);
	data = g_key_file_to_data(desktop, &length, error);
	if (data == NULL)
		return NULL;
	if (g_mkdir_with_parents(applications_dir, 0755) != 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
			"Cannot create applications directory '%s': %s",
			applications_dir, g_strerror(errno));
		return NULL;
	}

	/* Replace atomically, including symlinks, rather than following an old
	 * launcher's target. The caller chooses user versus system policy. */
	file = g_file_new_for_path(path);
	if (!g_file_replace_contents(file, data, length, NULL, FALSE,
		G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, error))
		return NULL;
	/* System launchers must remain readable even with a restrictive umask. */
	if (g_chmod(path, 0644) != 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
			"Cannot make launcher '%s' readable: %s", path, g_strerror(errno));
		return NULL;
	}
	return g_steal_pointer(&path);
}
