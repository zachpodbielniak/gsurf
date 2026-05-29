/*
 * gsurf-config-compiler.h - C configuration compiler (via crispy)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Compiles ~/.config/gsurf/config.c into a shared object (with content
 * hash caching) using deps/crispy, loads it, and calls its exported
 * `gsurf_config_init()`. Mirrors gst's GstConfigCompiler.
 */

#ifndef GSURF_CONFIG_COMPILER_H
#define GSURF_CONFIG_COMPILER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GSURF_TYPE_CONFIG_COMPILER (gsurf_config_compiler_get_type())

G_DECLARE_FINAL_TYPE(GsurfConfigCompiler, gsurf_config_compiler, GSURF, CONFIG_COMPILER, GObject)

/**
 * gsurf_config_compiler_new:
 * @error: (out) (optional): location for an error
 *
 * Creates a C-config compiler backed by crispy's gcc compiler.
 *
 * Returns: (transfer full) (nullable): a new compiler, or %NULL on error
 */
GsurfConfigCompiler *gsurf_config_compiler_new(GError **error);

/**
 * gsurf_config_compiler_find_default_path:
 *
 * Searches the standard locations for a config.c file
 * ($XDG_CONFIG_HOME/gsurf/config.c, then the system config dir).
 *
 * Returns: (transfer full) (nullable): the path, or %NULL if none found
 */
gchar *gsurf_config_compiler_find_default_path(void);

/**
 * gsurf_config_compiler_compile_and_load:
 * @self: a #GsurfConfigCompiler
 * @source_path: path to the config .c file
 * @error: (out) (optional): location for an error
 *
 * Compiles @source_path to a cached .so (if not already cached), loads
 * it, resolves `gsurf_config_init`, and calls it. The init function
 * operates on gsurf_config_get_default().
 *
 * Returns: %TRUE on success
 */
gboolean gsurf_config_compiler_compile_and_load(GsurfConfigCompiler *self,
                                                const gchar *source_path,
                                                GError **error);

G_END_DECLS

#endif /* GSURF_CONFIG_COMPILER_H */
