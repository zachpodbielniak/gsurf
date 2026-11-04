/*
 * gsurf-private-file.c - Owner-only directories and files
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "util/gsurf-private-file.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

gboolean
gsurf_private_mkdir(const gchar *path, GError **error)
{
	g_return_val_if_fail(path != NULL, FALSE);

	if (g_mkdir_with_parents(path, 0700) != 0) {
		gint saved = errno;

		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved),
			"cannot create directory '%s': %s", path, g_strerror(saved));
		return FALSE;
	}
	if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOTDIR,
			"'%s' exists and is not a directory", path);
		return FALSE;
	}
	return TRUE;
}

gboolean
gsurf_private_write(const gchar *path, const gchar *contents,
                    gssize length, GError **error)
{
	g_autofree gchar *dir = NULL;

	g_return_val_if_fail(path != NULL, FALSE);
	g_return_val_if_fail(contents != NULL || length == 0, FALSE);

	dir = g_path_get_dirname(path);
	if (!gsurf_private_mkdir(dir, error))
		return FALSE;

	/* g_file_set_contents_full keeps the mode of a file it replaces, so
	 * tighten an existing (looser) regular file first; the new contents
	 * are then never readable by others either. */
	{
		GStatBuf st;

		if (g_lstat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) != 0600 &&
		    g_chmod(path, 0600) != 0) {
			gint saved = errno;

			g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved),
				"cannot make '%s' private: %s", path, g_strerror(saved));
			return FALSE;
		}
	}

	/* g_file_set_contents_full writes a temporary file with @mode and
	 * renames it into place, so readers never see a partial file and the
	 * data is never briefly world-readable. */
	return g_file_set_contents_full(path, contents != NULL ? contents : "", length,
		G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

gboolean
gsurf_private_file_is_private(const gchar *path, GError **error)
{
	GStatBuf st;

	g_return_val_if_fail(path != NULL, FALSE);

	if (g_lstat(path, &st) != 0) {
		gint saved = errno;

		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved),
			"cannot stat '%s': %s", path, g_strerror(saved));
		return FALSE;
	}
	/* A symlink could point anywhere; require the real file. */
	if (!S_ISREG(st.st_mode)) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
			"'%s' is not a regular file", path);
		return FALSE;
	}
	if (st.st_uid != getuid()) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_PERM,
			"'%s' is not owned by the current user", path);
		return FALSE;
	}
	if ((st.st_mode & 077) != 0) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_PERM,
			"'%s' has mode %04o; it must not be accessible to group or others "
			"(chmod 600 '%s')", path, (guint)(st.st_mode & 07777), path);
		return FALSE;
	}
	return TRUE;
}

gchar *
gsurf_expand_home(const gchar *path)
{
	if (path == NULL)
		return NULL;
	if (g_strcmp0(path, "~") == 0)
		return g_strdup(g_get_home_dir());
	if (g_str_has_prefix(path, "~/"))
		return g_build_filename(g_get_home_dir(), path + 2, NULL);
	return g_strdup(path);
}
