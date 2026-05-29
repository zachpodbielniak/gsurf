/*
 * gsurf-version.c - GSURF runtime version accessors
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gsurf-version.h"

/**
 * gsurf_get_version:
 * @major: (out) (optional): location for major version
 * @minor: (out) (optional): location for minor version
 * @micro: (out) (optional): location for micro version
 *
 * Retrieves the runtime version of the GSURF library.
 */
void
gsurf_get_version(guint *major, guint *minor, guint *micro)
{
	if (major != NULL)
		*major = GSURF_VERSION_MAJOR;
	if (minor != NULL)
		*minor = GSURF_VERSION_MINOR;
	if (micro != NULL)
		*micro = GSURF_VERSION_MICRO;
}

/**
 * gsurf_get_version_string:
 *
 * Retrieves the runtime version string of the GSURF library.
 *
 * Returns: (transfer none): the version string
 */
const gchar *
gsurf_get_version_string(void)
{
	return GSURF_VERSION_STRING;
}
