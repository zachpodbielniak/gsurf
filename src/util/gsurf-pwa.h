/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef GSURF_PWA_H
#define GSURF_PWA_H

#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * gsurf_pwa_install:
 * @uri: absolute HTTP or HTTPS site URI
 * @name: (nullable): display name, or %NULL to use the hostname
 * @executable: absolute path to the installed gsurf executable or AppImage
 * @applications_dir: absolute destination applications directory
 * @error: (out) (optional): return location for an error
 *
 * Installs a desktop launcher for the exact URI in kiosk mode. Reinstalling
 * the same URI replaces its launcher; names never become filesystem paths.
 * Does not initialize a display, run the browser, or fetch site resources.
 *
 * Returns: (transfer full): installed path, or %NULL on failure
 */
gchar *gsurf_pwa_install(const gchar *uri, const gchar *name,
	const gchar *executable, const gchar *applications_dir, GError **error);

/**
 * GsurfPwaEntry:
 * @id: the indexable id: a slug of @name ("My Mail" -> "my-mail"), with the
 *   first 8 hex digits of the launcher's URL hash appended when two apps
 *   would share a slug (and the scope when one URL is installed in both), so
 *   it is unique within a listing and stable as long as the set of names is
 * @name: the display name
 * @uri: (nullable): the site URL, or %NULL if the launcher could not be read
 * @path: the launcher file
 * @system: %TRUE for a system-wide launcher, %FALSE for a per-user one
 *
 * One installed PWA launcher, as listed by gsurf_pwa_list().
 */
typedef struct {
	gchar    *id;
	gchar    *name;
	gchar    *uri;
	gchar    *path;
	gboolean  system;
} GsurfPwaEntry;

/**
 * gsurf_pwa_entry_free: (skip)
 * @entry: (nullable): an entry
 */
void gsurf_pwa_entry_free(GsurfPwaEntry *entry);

/**
 * gsurf_pwa_list: (skip)
 * @user_dir: (nullable): the per-user applications directory
 * @system_dir: (nullable): the system applications directory
 *
 * Finds the launchers gsurf_pwa_install() wrote (gsurf-pwa-SHA256.desktop)
 * in both directories (either may be missing) and gives each a unique id.
 * The URL comes from the launcher's X-Gsurf-PWA-URI key, or for launchers
 * written before that key existed, from its Exec line.  Sorted by id.
 *
 * Returns: (transfer full) (element-type GsurfPwaEntry): the entries
 */
GPtrArray *gsurf_pwa_list(const gchar *user_dir, const gchar *system_dir);

/**
 * gsurf_pwa_find: (skip)
 * @entries: (element-type GsurfPwaEntry): a listing from gsurf_pwa_list()
 * @query: an id, a display name (case-insensitive), or a URL
 * @error: (out) (optional): return location for an error
 *
 * Looks @query up: an exact id wins; otherwise the display name or URL must
 * match exactly one entry.  Several matches fail with %G_IO_ERROR_EXISTS
 * naming their ids; none with %G_IO_ERROR_NOT_FOUND.
 *
 * Returns: (transfer none) (nullable): the entry, owned by @entries
 */
GsurfPwaEntry *gsurf_pwa_find(GPtrArray *entries, const gchar *query, GError **error);

/**
 * gsurf_pwa_uninstall: (skip)
 * @entry: an entry from gsurf_pwa_list()
 * @out_trashed: (out) (optional): %TRUE if the launcher went to the trash,
 *   %FALSE if it was deleted (no trash on that filesystem)
 * @error: (out) (optional): return location for an error
 *
 * Removes the launcher, to the trash when possible so it can be restored.
 *
 * Returns: %TRUE on success
 */
gboolean gsurf_pwa_uninstall(const GsurfPwaEntry *entry, gboolean *out_trashed,
	GError **error);

G_END_DECLS
#endif
