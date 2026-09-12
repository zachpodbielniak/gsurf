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

G_END_DECLS
#endif
