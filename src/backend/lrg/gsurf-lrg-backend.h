/*
 * gsurf-lrg-backend.h - libregnum (LRG) backend init + identity
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Thin shim the core factory (gsurf-backend.c) calls for the LRG backend; it
 * forwards to the build-selected web engine (gsurf-lrg-engine.h).
 */

#ifndef GSURF_LRG_BACKEND_H
#define GSURF_LRG_BACKEND_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gsurf_lrg_backend_init:
 * @argc: (inout) (optional): address of argc from main()
 * @argv: (inout) (array length=argc) (optional): address of argv
 * @error: (out) (optional): location for an error
 *
 * Process-wide initialization for the LRG backend (forwards to the engine).
 *
 * Returns: %TRUE on success.
 */
gboolean gsurf_lrg_backend_init(int *argc, char ***argv, GError **error);

/**
 * gsurf_lrg_backend_engine_name:
 *
 * Returns: (transfer none): the compiled-in engine name ("lrg-webkitgtk" /
 *   "lrg-wpe").
 */
const gchar *gsurf_lrg_backend_engine_name(void);

G_END_DECLS

#endif /* GSURF_LRG_BACKEND_H */
