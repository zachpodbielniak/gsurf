/*
 * gsurf-control-private.h - Shared internals of the control server parts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Not installed and not scanned for the GIR: the server
 * (gsurf-control-server.c) and the core method set
 * (gsurf-control-methods.c) share these.
 */

#ifndef GSURF_CONTROL_PRIVATE_H
#define GSURF_CONTROL_PRIVATE_H

#include "gsurf.h"
#include "control/gsurf-control-server.h"

G_BEGIN_DECLS

/* Registers the core methods (instance.info, open, views.*, ...). */
G_GNUC_INTERNAL void         gsurf_control_methods_register(GsurfControlServer *server);

/* The window the methods act on: the application's active window. */
G_GNUC_INTERNAL GsurfWindow *gsurf_control_server_get_window(GsurfControlServer *self);

/* A JSON value for a YAML node (mappings, sequences and scalars; scalars
 * become booleans, integers or numbers when they read as such). */
G_GNUC_INTERNAL JsonNode    *gsurf_control_yaml_to_json(gpointer yaml_node);

G_END_DECLS

#endif /* GSURF_CONTROL_PRIVATE_H */
