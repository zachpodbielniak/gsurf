/*
 * gsurf-init.c - GSURF library initialization
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gsurf.h"

static gboolean gsurf_initialized = FALSE;

/**
 * gsurf_init_check:
 * @argc: (inout) (optional): address of argc from main()
 * @argv: (inout) (array length=argc) (optional): address of argv from main()
 * @error: (out) (optional): location to store an error
 *
 * Initializes the GSURF library. Idempotent.
 *
 * Returns: %TRUE on success
 */
gboolean
gsurf_init_check(int *argc, char ***argv, GError **error)
{
	(void)argc;
	(void)argv;
	(void)error;

	if (gsurf_initialized)
		return TRUE;

	/* GLib type system is initialized lazily on first use; nothing
	 * else is required at this stage. Backend (GTK) initialization is
	 * performed by GsurfApplication when a window is created so that
	 * the library can be embedded without forcing a GTK main loop. */

	gsurf_initialized = TRUE;
	return TRUE;
}

/**
 * gsurf_init:
 * @argc: (inout) (optional): address of argc from main()
 * @argv: (inout) (array length=argc) (optional): address of argv from main()
 *
 * Initializes the GSURF library, aborting on failure. Idempotent.
 */
void
gsurf_init(int *argc, char ***argv)
{
	GError *error = NULL;

	if (!gsurf_init_check(argc, argv, &error)) {
		g_critical("gsurf_init failed: %s",
			error ? error->message : "unknown error");
		g_clear_error(&error);
	}
}

/**
 * gsurf_is_initialized:
 *
 * Returns: %TRUE if GSURF has been initialized.
 */
gboolean
gsurf_is_initialized(void)
{
	return gsurf_initialized;
}
