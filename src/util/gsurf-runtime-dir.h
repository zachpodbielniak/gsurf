/*
 * gsurf-runtime-dir.h - Per-process runtime directory and the launch dir
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every gsurf process (and every host embedding libgsurf) that needs
 * scratch storage gets a private directory
 * `$XDG_RUNTIME_DIR/gsurf/<pid>-XXXXXX` (mode 0700, normally a tmpfs).
 * Temporary containers keep their whole profile below it, in
 * `temp/<container-id>`, so nothing they store outlives the process: the
 * container's directory is removed when the container is destroyed and
 * the process directory at exit. A process directory holds a `lock` file
 * that its owner keeps flock()ed; a later process removes directories
 * whose lock it can take (their owner is gone), see
 * gsurf_runtime_dir_sweep().
 *
 * The gsurf binary also makes the runtime directory its working
 * directory (gsurf_runtime_dir_enter()), so a relative write by any
 * library (WebKit's network process inherits the working directory) can
 * never land in the directory gsurf was started from. Paths the user
 * gives relative to that directory keep resolving against it through
 * gsurf_get_launch_dir() and gsurf_resolve_path(). Embedders are not
 * moved unless they call gsurf_runtime_dir_enter() themselves.
 */

#ifndef GSURF_RUNTIME_DIR_H
#define GSURF_RUNTIME_DIR_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gsurf_runtime_dir_get:
 * @error: return location for a #GError
 *
 * Returns this process's private runtime directory, creating it (mode
 * 0700, with its held `lock` file) on first use. The first call also
 * sweeps the directories of gsurf processes that are gone and registers
 * an exit handler that removes the directory (see
 * gsurf_runtime_dir_cleanup()).
 *
 * Returns: (transfer none) (type filename) (nullable): the directory, or
 *   %NULL with @error set when it cannot be created
 */
const gchar *gsurf_runtime_dir_get(GError **error);

/**
 * gsurf_runtime_dir_get_temp_container_dir:
 * @id: a temporary container id
 *
 * Returns: (transfer full) (type filename) (nullable): the directory
 *   reserved for temporary container @id (`<runtime>/temp/@id`, not yet
 *   created), or %NULL when there is no runtime directory
 */
gchar *gsurf_runtime_dir_get_temp_container_dir(const gchar *id);

/**
 * gsurf_runtime_dir_remove_tree:
 * @path: (type filename): a file or directory
 * @error: return location for a #GError
 *
 * Removes @path and everything below it. Symbolic links are removed,
 * never followed. A missing @path is not an error.
 *
 * Returns: %TRUE when @path no longer exists
 */
gboolean gsurf_runtime_dir_remove_tree(const gchar *path, GError **error);

/**
 * gsurf_runtime_dir_sweep:
 * @base: (type filename): the directory holding process directories,
 *   normally `$XDG_RUNTIME_DIR/gsurf`
 *
 * Removes the process directories in @base whose owner is gone: their
 * `lock` file is not locked, or (an owner that died while creating it)
 * they have no `lock` file and are older than a minute. Entries that do
 * not look like process directories (`<digits>-XXXXXX`) and this
 * process's own directory are left alone.
 *
 * Returns: the number of directories removed
 */
guint gsurf_runtime_dir_sweep(const gchar *base);

/**
 * gsurf_runtime_dir_cleanup:
 *
 * Removes this process's runtime directory and everything in it (the
 * data of every temporary container) and releases its lock. Runs
 * automatically at exit(); a later gsurf_runtime_dir_get() creates a
 * fresh directory.
 */
void gsurf_runtime_dir_cleanup(void);

/**
 * gsurf_runtime_dir_enter:
 * @error: return location for a #GError
 *
 * Records the current working directory as the launch directory (see
 * gsurf_get_launch_dir()) and changes the working directory to the
 * runtime directory. The gsurf binary calls this right after parsing its
 * command line, before any web process starts; embedders may opt in the
 * same way. Calling it again is a no-op.
 *
 * Returns: %TRUE when the working directory is the runtime directory
 */
gboolean gsurf_runtime_dir_enter(GError **error);

/**
 * gsurf_get_launch_dir:
 *
 * The directory relative paths given by the user resolve against: the
 * working directory recorded by gsurf_runtime_dir_enter(), or the
 * current working directory when it was never called (embedders).
 *
 * Returns: (transfer full) (type filename): the launch directory
 */
gchar *gsurf_get_launch_dir(void);

/**
 * gsurf_resolve_path:
 * @path: (type filename) (nullable): a path from the user or a config file
 *
 * Expands a leading "~" or "~/" to the home directory and makes a
 * relative path absolute against gsurf_get_launch_dir(). Absolute paths
 * are returned unchanged (not canonicalized).
 *
 * Returns: (transfer full) (type filename) (nullable): the resolved path,
 *   %NULL for %NULL, an empty string for an empty string
 */
gchar *gsurf_resolve_path(const gchar *path);

G_END_DECLS

#endif /* GSURF_RUNTIME_DIR_H */
