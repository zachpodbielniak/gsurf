/*
 * gsurf-web-process.h - Web-process extension registry
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Some features have to run inside the engine's web process, next to the
 * page (for example rewriting a sub-resource request before it is sent).
 * Modules register such a shared object here, together with the data it
 * needs, and the WebKit backends hand both to every web process they
 * launch. The registry itself is backend-agnostic: it only records paths
 * and #GVariant payloads.
 */

#ifndef GSURF_WEB_PROCESS_H
#define GSURF_WEB_PROCESS_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gsurf_web_process_extension_register:
 * @name: a unique, stable name (e.g. the owning module's name)
 * @path: absolute path of the web-process extension shared object
 * @data: (nullable) (transfer none): initialization data; a floating
 *   reference is sunk
 *
 * Registers (or replaces) a web-process extension. Web processes launched
 * afterwards load @path and receive @data under @name in their
 * initialization dictionary. Already-running web processes keep the data
 * they started with, so register before the first view is created.
 */
void gsurf_web_process_extension_register(const gchar *name, const gchar *path,
                                          GVariant *data);

/**
 * gsurf_web_process_extension_unregister:
 * @name: the name passed to gsurf_web_process_extension_register()
 *
 * Removes a registered extension. Web processes launched afterwards no
 * longer load it.
 */
void gsurf_web_process_extension_unregister(const gchar *name);

/**
 * gsurf_web_process_extension_get_directory:
 *
 * Returns the directory that holds every registered extension, for the
 * engine's "load all extensions from this directory" setting. With one
 * extension this is the directory it already lives in; with several, a
 * per-process runtime directory of symbolic links is (re)built.
 *
 * Returns: (transfer full) (nullable): the directory, or %NULL when no
 *   extension is registered or the link directory cannot be created
 */
gchar *gsurf_web_process_extension_get_directory(void);

/**
 * gsurf_web_process_extension_get_user_data:
 *
 * Returns: (transfer full): an `a{sv}` dictionary mapping each registered
 *   extension's name to its initialization data (extensions registered
 *   without data are omitted)
 */
GVariant *gsurf_web_process_extension_get_user_data(void);

G_END_DECLS

#endif /* GSURF_WEB_PROCESS_H */
