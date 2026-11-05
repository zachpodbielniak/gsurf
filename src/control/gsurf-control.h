/*
 * gsurf-control.h - The control socket API in one include
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Not part of <gsurf.h>: these headers use json-glib, which a C config
 * (compiled by crispy without json-glib) must not need. Include
 * <gsurf/control/gsurf-control.h> and add json-glib-1.0 to your flags
 * (pkg-config gsurf already requires it).
 *
 * Since: 0.5
 */

#ifndef GSURF_CONTROL_H
#define GSURF_CONTROL_H

#include "gsurf.h"
#include "control/gsurf-control-server.h"
#include "control/gsurf-control-provider.h"
#include "control/gsurf-control-client.h"

#endif /* GSURF_CONTROL_H */
