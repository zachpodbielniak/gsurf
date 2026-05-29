/*
 * gsurf.h - GSURF (GObject Surf) Main Umbrella Header
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Include this header to get access to all public GSURF APIs.
 *
 * GSURF is a GObject-based web browser library (a port of suckless surf)
 * with a modular extension system, YAML + C configuration, and an
 * optional MCP server. The bulk of the browser lives in this library so
 * that other applications can embed it.
 */

#ifndef GSURF_H
#define GSURF_H

#define GSURF_INSIDE

/* Core headers */
#include "gsurf-types.h"
#include "gsurf-enums.h"
#include "gsurf-version.h"

/* Configuration */
#include "config/gsurf-settings.h"
#include "config/gsurf-config.h"
#include "config/gsurf-config-compiler.h"

/* Core classes */
#include "core/gsurf-view.h"
#include "core/gsurf-backend.h"
#include "core/gsurf-application.h"

/* Window */
#include "window/gsurf-window.h"

/* Module system */
#include "module/gsurf-module.h"
#include "module/gsurf-module-manager.h"

/* Hook-point interfaces */
#include "interfaces/gsurf-input-handler.h"
#include "interfaces/gsurf-uri-handler.h"
#include "interfaces/gsurf-navigation-hook.h"
#include "interfaces/gsurf-script-injector.h"
#include "interfaces/gsurf-setting-provider.h"
#include "interfaces/gsurf-permission-handler.h"
#include "interfaces/gsurf-download-handler.h"
#include "interfaces/gsurf-cert-handler.h"

/* Utilities */
#include "util/gsurf-keys.h"

/*
 * Additional subsystem headers are added here as they are implemented:
 *   boxed/   more interfaces/   util/
 * (see the project plan for the full layout)
 */

#undef GSURF_INSIDE

G_BEGIN_DECLS

/**
 * gsurf_init:
 * @argc: (inout) (optional): address of argc from main()
 * @argv: (inout) (array length=argc) (optional): address of argv from main()
 *
 * Initializes the GSURF library. Must be called before using any other
 * GSURF functions. Idempotent.
 */
void gsurf_init(int *argc, char ***argv);

/**
 * gsurf_init_check:
 * @argc: (inout) (optional): address of argc from main()
 * @argv: (inout) (array length=argc) (optional): address of argv from main()
 * @error: (out) (optional): location to store an error
 *
 * Initializes the GSURF library, returning %FALSE on failure instead of
 * aborting.
 *
 * Returns: %TRUE on success
 */
gboolean gsurf_init_check(int *argc, char ***argv, GError **error);

/**
 * gsurf_is_initialized:
 *
 * Returns: %TRUE if GSURF has been initialized.
 */
gboolean gsurf_is_initialized(void);

G_END_DECLS

#endif /* GSURF_H */
