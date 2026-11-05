/*
 * gsurf-runtime-dir.c - Per-process runtime directory and the launch dir
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "util/gsurf-runtime-dir.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <glib/gstdio.h>

#define LOCK_NAME "lock"
#define TEMP_SUBDIR "temp"
/* A lock-less process directory younger than this may still be in the
 * middle of its creation (mkdtemp, then the lock): leave it alone. */
#define STALE_UNLOCKED_SECONDS (60)

static GMutex   runtime_lock;
static gchar   *runtime_dir = NULL;   /* this process's directory */
static gint     runtime_fd = -1;      /* the held lock file */
static gboolean exit_hook = FALSE;
static gchar   *launch_dir = NULL;    /* recorded by _enter() */

/* The parent of all process directories: $XDG_RUNTIME_DIR/gsurf. */
static gchar *
runtime_base(void)
{
	return g_build_filename(g_get_user_runtime_dir(), "gsurf", NULL);
}

/* "<digits>-XXXXXX": the names g_mkdtemp() gives our process dirs. */
static gboolean
is_process_dir_name(const gchar *name)
{
	const gchar *p = name;
	gsize n = 0;

	while (g_ascii_isdigit(*p))
		p++;
	if (p == name || *p != '-')
		return FALSE;
	for (p++; *p != '\0'; p++, n++)
		if (!g_ascii_isalnum(*p) && *p != '_')
			return FALSE;
	return n == 6;
}

gboolean
gsurf_runtime_dir_remove_tree(const gchar *path, GError **error)
{
	GStatBuf st;
	GDir *dir;
	const gchar *name;
	gboolean ok = TRUE;

	g_return_val_if_fail(path != NULL, FALSE);

	if (g_lstat(path, &st) != 0) {
		if (errno == ENOENT)
			return TRUE;
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
			"cannot stat %s: %s", path, g_strerror(errno));
		return FALSE;
	}

	/* A link or a file goes directly; never descend through a link. */
	if (!S_ISDIR(st.st_mode)) {
		if (g_unlink(path) != 0 && errno != ENOENT) {
			g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
				"cannot remove %s: %s", path, g_strerror(errno));
			return FALSE;
		}
		return TRUE;
	}

	/* A directory: empty it depth-first, then remove it. Keep going past
	 * a failure so as much as possible is gone; report the first one. */
	dir = g_dir_open(path, 0, NULL);
	if (dir != NULL) {
		while ((name = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *child = g_build_filename(path, name, NULL);

			if (!gsurf_runtime_dir_remove_tree(child, ok ? error : NULL))
				ok = FALSE;
		}
		g_dir_close(dir);
	}
	if (g_rmdir(path) != 0 && errno != ENOENT) {
		if (ok)
			g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
				"cannot remove %s: %s", path, g_strerror(errno));
		return FALSE;
	}
	return ok;
}

/* Whether the owner of process directory @path is gone. */
static gboolean
process_dir_is_stale(const gchar *path)
{
	g_autofree gchar *lock = g_build_filename(path, LOCK_NAME, NULL);
	GStatBuf st;
	gboolean stale;
	gint fd;

	fd = g_open(lock, O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0);
	if (fd < 0) {
		/* No lock file: stale only once it is clearly not being made. */
		if (errno != ENOENT || g_lstat(path, &st) != 0)
			return FALSE;
		return (time(NULL) - st.st_mtime) > STALE_UNLOCKED_SECONDS;
	}
	/* The owner holds an exclusive flock for its whole life: taking the
	 * lock proves it is gone, whatever pid namespace it ran in. */
	stale = flock(fd, LOCK_EX | LOCK_NB) == 0;
	close(fd);
	return stale;
}

guint
gsurf_runtime_dir_sweep(const gchar *base)
{
	g_autoptr(GDir) dir = NULL;
	const gchar *name;
	guint removed = 0;

	g_return_val_if_fail(base != NULL, 0);

	dir = g_dir_open(base, 0, NULL);
	if (dir == NULL)
		return 0;
	while ((name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autoptr(GError) error = NULL;

		if (!is_process_dir_name(name))
			continue;
		path = g_build_filename(base, name, NULL);
		if (g_strcmp0(path, runtime_dir) == 0 || !process_dir_is_stale(path))
			continue;
		if (gsurf_runtime_dir_remove_tree(path, &error))
			removed++;
		else
			g_warning("gsurf: stale runtime directory: %s", error->message);
	}
	return removed;
}

static void
on_exit_cleanup(void)
{
	gsurf_runtime_dir_cleanup();
}

const gchar *
gsurf_runtime_dir_get(GError **error)
{
	g_autofree gchar *base = NULL;
	g_autofree gchar *template = NULL;
	g_autofree gchar *lock = NULL;
	const gchar *result;
	gint fd;

	g_mutex_lock(&runtime_lock);
	if (runtime_dir != NULL) {
		result = runtime_dir;
		g_mutex_unlock(&runtime_lock);
		return result;
	}

	/* 1. The parent, owner-only like everything else of ours there. */
	base = runtime_base();
	if (g_mkdir_with_parents(base, 0700) != 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
			"cannot create %s: %s", base, g_strerror(errno));
		g_mutex_unlock(&runtime_lock);
		return NULL;
	}

	/* 2. A fresh, unique directory with a lock file we hold until exit.
	 *    The descriptor is close-on-exec: child processes must not keep
	 *    a dead gsurf's directory alive. */
	template = g_strdup_printf("%s/%ld-XXXXXX", base, (long)getpid());
	if (g_mkdtemp_full(template, 0700) == NULL) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
			"cannot create a runtime directory in %s: %s", base, g_strerror(errno));
		g_mutex_unlock(&runtime_lock);
		return NULL;
	}
	lock = g_build_filename(template, LOCK_NAME, NULL);
	fd = g_open(lock, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
			"cannot lock %s: %s", lock, g_strerror(errno));
		if (fd >= 0)
			close(fd);
		gsurf_runtime_dir_remove_tree(template, NULL);
		g_mutex_unlock(&runtime_lock);
		return NULL;
	}
	runtime_fd = fd;
	runtime_dir = g_steal_pointer(&template);

	/* 3. Leftovers of processes that crashed or were killed. */
	gsurf_runtime_dir_sweep(base);

	if (!exit_hook) {
		exit_hook = TRUE;
		atexit(on_exit_cleanup);
	}
	result = runtime_dir;
	g_mutex_unlock(&runtime_lock);
	return result;
}

gchar *
gsurf_runtime_dir_get_temp_container_dir(const gchar *id)
{
	const gchar *dir;

	g_return_val_if_fail(id != NULL && *id != '\0', NULL);
	/* Ids are validated path-safe names; refuse anything else anyway. */
	g_return_val_if_fail(strchr(id, '/') == NULL && g_strcmp0(id, "..") != 0 &&
	                     g_strcmp0(id, ".") != 0, NULL);

	dir = gsurf_runtime_dir_get(NULL);
	return dir != NULL ? g_build_filename(dir, TEMP_SUBDIR, id, NULL) : NULL;
}

void
gsurf_runtime_dir_cleanup(void)
{
	g_autoptr(GError) error = NULL;

	g_mutex_lock(&runtime_lock);
	if (runtime_dir != NULL) {
		/* Out of the directory first, so removing it cannot leave the
		 * process working in a deleted directory. */
		if (launch_dir != NULL) {
			g_autofree gchar *cwd = g_get_current_dir();

			if (g_str_has_prefix(cwd, runtime_dir) && g_chdir(launch_dir) != 0)
				(void)g_chdir("/");
		}
		if (!gsurf_runtime_dir_remove_tree(runtime_dir, &error))
			g_warning("gsurf: runtime directory: %s", error->message);
		g_clear_pointer(&runtime_dir, g_free);
	}
	if (runtime_fd >= 0) {
		close(runtime_fd);
		runtime_fd = -1;
	}
	g_mutex_unlock(&runtime_lock);
}

gboolean
gsurf_runtime_dir_enter(GError **error)
{
	g_autofree gchar *cwd = NULL;
	const gchar *dir;

	if (launch_dir != NULL)
		return TRUE;
	dir = gsurf_runtime_dir_get(error);
	if (dir == NULL)
		return FALSE;
	cwd = g_get_current_dir();
	if (g_chdir(dir) != 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
			"cannot change to %s: %s", dir, g_strerror(errno));
		return FALSE;
	}
	launch_dir = g_steal_pointer(&cwd);
	return TRUE;
}

gchar *
gsurf_get_launch_dir(void)
{
	return launch_dir != NULL ? g_strdup(launch_dir) : g_get_current_dir();
}

gchar *
gsurf_resolve_path(const gchar *path)
{
	g_autofree gchar *base = NULL;

	if (path == NULL)
		return NULL;
	if (*path == '\0')
		return g_strdup(path);
	if (path[0] == '~' && path[1] == '\0')
		return g_strdup(g_get_home_dir());
	if (path[0] == '~' && path[1] == '/')
		return g_build_filename(g_get_home_dir(), path + 2, NULL);
	if (g_path_is_absolute(path))
		return g_strdup(path);
	base = gsurf_get_launch_dir();
	return g_build_filename(base, path, NULL);
}
