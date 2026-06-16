/*
 * gsurf-lrg-backend.c - libregnum (LRG) backend init + identity
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "backend/lrg/gsurf-lrg-backend.h"
#include "backend/lrg/gsurf-lrg-engine.h"

gboolean
gsurf_lrg_backend_init(int *argc, char ***argv, GError **error)
{
	return gsurf_lrg_engine_backend_init(argc, argv, error);
}

const gchar *
gsurf_lrg_backend_engine_name(void)
{
	return gsurf_lrg_engine_get_engine_name();
}
