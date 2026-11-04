/*
 * gsurf-private-file.h - Owner-only directories and files
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Helpers for per-user state that must not be readable by other users:
 * container profiles, autofill data, per-site overrides. Directories are
 * created 0700 and files are written atomically with mode 0600.
 */

#ifndef GSURF_PRIVATE_FILE_H
#define GSURF_PRIVATE_FILE_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gsurf_private_mkdir:
 * @path: (type filename): a directory to create
 * @error: return location for a #GError
 *
 * Creates @path and any missing parents with mode 0700. An existing
 * directory is accepted as is (its mode is not changed).
 *
 * Returns: %TRUE when @path exists as a directory afterwards
 */
gboolean gsurf_private_mkdir(const gchar *path, GError **error);

/**
 * gsurf_private_write:
 * @path: (type filename): the file to (re)write
 * @contents: (array length=length) (element-type guint8): the data
 * @length: the length of @contents, or -1 for a NUL-terminated string
 * @error: return location for a #GError
 *
 * Atomically replaces @path with @contents at mode 0600, creating the
 * parent directory 0700 first.
 *
 * Returns: %TRUE on success
 */
gboolean gsurf_private_write(const gchar *path, const gchar *contents,
                             gssize length, GError **error);

/**
 * gsurf_private_file_is_private:
 * @path: (type filename): an existing file
 * @error: return location for a #GError
 *
 * Checks that @path is a regular file owned by the current user and not
 * readable or writable by group or others (mode & 077 == 0). Used before
 * trusting a file that holds sensitive data.
 *
 * Returns: %TRUE when the file is private; %FALSE with @error set
 *   otherwise (missing file, other owner, or loose permissions)
 */
gboolean gsurf_private_file_is_private(const gchar *path, GError **error);

/**
 * gsurf_expand_home:
 * @path: (type filename) (nullable): a path, possibly starting with "~/"
 *
 * Returns: (transfer full) (type filename) (nullable): @path with a
 *   leading "~" or "~/" replaced by the home directory
 */
gchar *gsurf_expand_home(const gchar *path);

G_END_DECLS

#endif /* GSURF_PRIVATE_FILE_H */
