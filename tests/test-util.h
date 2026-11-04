/*
 * test-util.h - Shared helpers for the gsurf test suites
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Header-only so every auto-discovered tests/test-*.c can include it without
 * a Makefile change.
 */

#ifndef GSURF_TEST_UTIL_H
#define GSURF_TEST_UTIL_H

#include <glib.h>
#include <glib/gstdio.h>

/*
 * gsurf_test_rmtree:
 * @path: (type filename): directory created by the test (e.g. g_dir_make_tmp())
 *
 * Recursively removes @path and everything below it, without following
 * symlinks. Tests that point XDG_* at a temporary directory cannot know
 * every file a tool writes there (gcc wrapped by ccache, for instance,
 * creates $XDG_CACHE_HOME/ccache), so cleanup must not assume an exact
 * layout. Returns %TRUE when @path no longer exists.
 */
static inline gboolean
gsurf_test_rmtree(
	const gchar	*path
){
	GDir *dir;
	const gchar *name;

	if (path == NULL)
		return TRUE;

	/* Symlinks and regular files are unlinked directly; never descend
	 * through a link into something the test does not own. */
	if (g_file_test(path, G_FILE_TEST_IS_SYMLINK) ||
	    !g_file_test(path, G_FILE_TEST_IS_DIR)) {
		if (g_unlink(path) != 0 && g_file_test(path, G_FILE_TEST_EXISTS))
			return FALSE;
		return TRUE;
	}

	/* Directory: empty it depth-first, then remove it. */
	dir = g_dir_open(path, 0, NULL);
	if (dir != NULL) {
		while ((name = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *child = g_build_filename(path, name, NULL);

			gsurf_test_rmtree(child);
		}
		g_dir_close(dir);
	}
	return g_rmdir(path) == 0;
}

#endif /* GSURF_TEST_UTIL_H */
