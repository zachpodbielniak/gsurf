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
	return gsurf_pwa_install_full(uri, name, NULL, executable, applications_dir,
		error);
}

gchar *
gsurf_pwa_install_full(const gchar *uri, const gchar *name,
	const gchar *container, const gchar *executable,
	const gchar *applications_dir, GError **error)
{
	g_autoptr(GUri) parsed = NULL;
	g_autoptr(GKeyFile) desktop = NULL;
	g_autoptr(GFile) file = NULL;
	g_autofree gchar *quoted_executable = NULL;
	g_autofree gchar *quoted_uri = NULL;
	g_autofree gchar *quoted_container = NULL;
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
	    (container != NULL && !valid_text(container)) ||
	    !valid_text(executable) || !g_path_is_absolute(executable) ||
	    applications_dir == NULL || !g_path_is_absolute(applications_dir)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"PWA requires a site URL, nonempty name/container, and absolute "
			"executable/destination paths");
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
	/* A pinned container is a startup request (gsurf --container), which
	 * outranks the container rules; without one the rules place the app. */
	if (container != NULL) {
		quoted_container = quote_exec_argument(container);
		exec = g_strdup_printf("%s --kiosk --container %s -- %s", quoted_executable,
			quoted_container, quoted_uri);
	} else {
		exec = g_strdup_printf("%s --kiosk -- %s", quoted_executable, quoted_uri);
	}
	desktop = g_key_file_new();
	g_key_file_set_string(desktop, "Desktop Entry", "Type", "Application");
	g_key_file_set_string(desktop, "Desktop Entry", "Name", name != NULL ? name : host);
	g_key_file_set_string(desktop, "Desktop Entry", "Exec", exec);
	/* What --list-pwa shows, without having to parse Exec back. */
	g_key_file_set_string(desktop, "Desktop Entry", "X-Gsurf-PWA-URI", uri);
	if (container != NULL)
		g_key_file_set_string(desktop, "Desktop Entry", "X-Gsurf-PWA-Container",
			container);
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

/* ---------------------------------------------------------------- list -- */

#define PWA_PREFIX "gsurf-pwa-"
#define PWA_SUFFIX ".desktop"
#define PWA_DIGEST_LEN 64
#define PWA_SLUG_MAX 40

void
gsurf_pwa_entry_free(GsurfPwaEntry *entry)
{
	if (entry == NULL)
		return;
	g_free(entry->id);
	g_free(entry->name);
	g_free(entry->uri);
	g_free(entry->path);
	g_free(entry->container);
	g_free(entry);
}

/* The launcher filename's URL hash, or NULL if @filename is not one of ours
 * (gsurf-pwa-<64 hex>.desktop). */
static gchar *
launcher_digest(const gchar *filename)
{
	gsize prefix = strlen(PWA_PREFIX);
	gsize i;

	if (!g_str_has_prefix(filename, PWA_PREFIX) || !g_str_has_suffix(filename, PWA_SUFFIX)
	    || strlen(filename) != prefix + PWA_DIGEST_LEN + strlen(PWA_SUFFIX))
		return NULL;
	for (i = 0; i < PWA_DIGEST_LEN; i++)
		if (!g_ascii_isxdigit(filename[prefix + i]))
			return NULL;
	return g_strndup(filename + prefix, PWA_DIGEST_LEN);
}

/* A typable id from a display name: ASCII letters and digits lowercased,
 * every other run of characters one '-', trimmed, capped; "pwa" if nothing
 * is left (a name in another script). */
static gchar *
slugify(const gchar *name)
{
	GString *slug = g_string_new(NULL);
	const gchar *p;

	for (p = name; p != NULL && *p != '\0' && slug->len < PWA_SLUG_MAX; p++) {
		if (g_ascii_isalnum(*p))
			g_string_append_c(slug, g_ascii_tolower(*p));
		else if (slug->len > 0 && slug->str[slug->len - 1] != '-')
			g_string_append_c(slug, '-');
	}
	while (slug->len > 0 && slug->str[slug->len - 1] == '-')
		g_string_truncate(slug, slug->len - 1);
	if (slug->len == 0)
		g_string_assign(slug, "pwa");
	return g_string_free(slug, FALSE);
}

/* The argument after @flag in a launcher's Exec ("--" gives the site URL,
 * "--container" the pinned container), for launchers written before the
 * X-Gsurf-PWA-* keys.  Exec's quoting is a subset of the shell's (double
 * quotes, backslash before " ` $ \), and GKeyFile already undid the
 * desktop-string layer; "%%" is the escaped '%'. */
static gchar *
arg_from_exec(const gchar *exec, const gchar *flag)
{
	g_auto(GStrv) argv = NULL;
	gint argc = 0;
	gint i;

	if (exec == NULL || !g_shell_parse_argv(exec, &argc, &argv, NULL))
		return NULL;
	for (i = 0; i + 1 < argc; i++) {
		if (g_strcmp0(argv[i], flag) == 0) {
			g_auto(GStrv) parts = g_strsplit(argv[i + 1], "%%", -1);

			return g_strjoinv("%", parts);
		}
		/* Nothing after "--" is an option. */
		if (g_strcmp0(argv[i], "--") == 0)
			break;
	}
	return NULL;
}

/* Read one directory's launchers into @out, with their slug as a provisional
 * id; @digests receives each entry's URL hash (same index). */
static void
scan_directory(const gchar *directory, gboolean system, GPtrArray *out,
               GPtrArray *digests)
{
	g_autoptr(GDir) dir = NULL;
	const gchar *filename;

	if (directory == NULL)
		return;
	dir = g_dir_open(directory, 0, NULL);
	if (dir == NULL)
		return;
	while ((filename = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *digest = launcher_digest(filename);
		g_autoptr(GKeyFile) desktop = NULL;
		g_autofree gchar *path = NULL;
		GsurfPwaEntry *entry;

		if (digest == NULL)
			continue;
		path = g_build_filename(directory, filename, NULL);
		desktop = g_key_file_new();
		entry = g_new0(GsurfPwaEntry, 1);
		entry->path = g_steal_pointer(&path);
		entry->system = system;
		if (g_key_file_load_from_file(desktop, entry->path, G_KEY_FILE_NONE, NULL)) {
			entry->name = g_key_file_get_string(desktop, "Desktop Entry", "Name", NULL);
			g_autofree gchar *exec = g_key_file_get_string(desktop,
				"Desktop Entry", "Exec", NULL);

			entry->uri = g_key_file_get_string(desktop, "Desktop Entry",
				"X-Gsurf-PWA-URI", NULL);
			if (entry->uri == NULL)
				entry->uri = arg_from_exec(exec, "--");
			entry->container = g_key_file_get_string(desktop, "Desktop Entry",
				"X-Gsurf-PWA-Container", NULL);
			if (entry->container == NULL)
				entry->container = arg_from_exec(exec, "--container");
		}
		/* An unreadable launcher still lists (and uninstalls) by its hash. */
		if (entry->name == NULL)
			entry->name = g_strdup(filename);
		entry->id = slugify(entry->name);
		g_ptr_array_add(out, entry);
		g_ptr_array_add(digests, g_steal_pointer(&digest));
	}
}

/* How many entries carry @id. */
static guint
count_id(GPtrArray *entries, const gchar *id)
{
	guint i, n = 0;

	for (i = 0; i < entries->len; i++)
		if (g_strcmp0(((GsurfPwaEntry *)g_ptr_array_index(entries, i))->id, id) == 0)
			n++;
	return n;
}

static gint
compare_entries(gconstpointer a, gconstpointer b)
{
	const GsurfPwaEntry *ea = *(const GsurfPwaEntry *const *)a;
	const GsurfPwaEntry *eb = *(const GsurfPwaEntry *const *)b;

	return g_strcmp0(ea->id, eb->id);
}

GPtrArray *
gsurf_pwa_list(const gchar *user_dir, const gchar *system_dir)
{
	GPtrArray *entries = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gsurf_pwa_entry_free);
	g_autoptr(GPtrArray) digests = g_ptr_array_new_with_free_func(g_free);
	g_autofree gboolean *clash = NULL;
	guint i;

	scan_directory(user_dir, FALSE, entries, digests);
	scan_directory(system_dir, TRUE, entries, digests);

	/* 1. Slugs shared by several apps get the URL hash's first 8 digits. */
	clash = g_new0(gboolean, MAX(entries->len, 1));
	for (i = 0; i < entries->len; i++)
		clash[i] = count_id(entries,
			((GsurfPwaEntry *)g_ptr_array_index(entries, i))->id) > 1;
	for (i = 0; i < entries->len; i++) {
		GsurfPwaEntry *e = g_ptr_array_index(entries, i);

		if (clash[i]) {
			gchar *id = g_strdup_printf("%s-%.8s", e->id,
				(const gchar *)g_ptr_array_index(digests, i));

			g_free(e->id);
			e->id = id;
		}
	}
	/* 2. The same URL installed per user and system-wide still collides:
	 *    the scope tells them apart. */
	for (i = 0; i < entries->len; i++)
		clash[i] = count_id(entries,
			((GsurfPwaEntry *)g_ptr_array_index(entries, i))->id) > 1;
	for (i = 0; i < entries->len; i++) {
		GsurfPwaEntry *e = g_ptr_array_index(entries, i);

		if (clash[i]) {
			gchar *id = g_strdup_printf("%s-%s", e->id, e->system ? "system" : "user");

			g_free(e->id);
			e->id = id;
		}
	}

	g_ptr_array_sort(entries, compare_entries);
	return entries;
}

GsurfPwaEntry *
gsurf_pwa_find(GPtrArray *entries, const gchar *query, GError **error)
{
	g_autoptr(GPtrArray) matches = g_ptr_array_new();
	g_autofree gchar *folded = NULL;
	guint i;

	g_return_val_if_fail(entries != NULL, NULL);

	if (query == NULL || *query == '\0') {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"no PWA named (give an id from --list-pwa)");
		return NULL;
	}

	/* 1. An id is unique by construction. */
	for (i = 0; i < entries->len; i++) {
		GsurfPwaEntry *e = g_ptr_array_index(entries, i);

		if (g_strcmp0(e->id, query) == 0)
			return e;
	}

	/* 2. A display name (any case) or the exact URL. */
	folded = g_utf8_casefold(query, -1);
	for (i = 0; i < entries->len; i++) {
		GsurfPwaEntry *e = g_ptr_array_index(entries, i);
		g_autofree gchar *name = g_utf8_casefold(e->name, -1);

		if (g_strcmp0(name, folded) == 0 || g_strcmp0(e->uri, query) == 0)
			g_ptr_array_add(matches, e);
	}
	if (matches->len == 1)
		return g_ptr_array_index(matches, 0);
	if (matches->len == 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"no installed PWA is called '%s' (--list-pwa shows the ids)", query);
		return NULL;
	}
	{
		g_autoptr(GString) ids = g_string_new(NULL);

		for (i = 0; i < matches->len; i++)
			g_string_append_printf(ids, "%s%s", i > 0 ? ", " : "",
				((GsurfPwaEntry *)g_ptr_array_index(matches, i))->id);
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
			"'%s' matches %u PWAs; uninstall one by id: %s", query, matches->len,
			ids->str);
	}
	return NULL;
}

gboolean
gsurf_pwa_uninstall(const GsurfPwaEntry *entry, gboolean *out_trashed, GError **error)
{
	g_autoptr(GFile) file = NULL;
	g_autoptr(GError) trash_error = NULL;

	g_return_val_if_fail(entry != NULL && entry->path != NULL, FALSE);

	if (out_trashed != NULL)
		*out_trashed = FALSE;
	file = g_file_new_for_path(entry->path);
	/* Reversible first: the trash, so a mistaken uninstall can be restored. */
	if (g_file_trash(file, NULL, &trash_error)) {
		if (out_trashed != NULL)
			*out_trashed = TRUE;
		return TRUE;
	}
	if (!g_error_matches(trash_error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
		g_propagate_prefixed_error(error, g_steal_pointer(&trash_error),
			"Cannot remove '%s': ", entry->path);
		return FALSE;
	}
	/* No trash on that filesystem (often /usr/local as root): delete. */
	if (!g_file_delete(file, NULL, error)) {
		g_prefix_error(error, "Cannot remove '%s': ", entry->path);
		return FALSE;
	}
	return TRUE;
}
