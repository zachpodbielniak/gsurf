/*
 * gsurf-permission-request.h - Backend-agnostic permission request holder
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GsurfPermissionRequest is a plain data-holder GObject describing a pending
 * permission request (geolocation, notifications, media, etc.). It carries no
 * GTK/WebKit state; the backend mirrors the engine's permission request onto
 * this type so modules and embedders can allow or deny it through GObject.
 */

#ifndef GSURF_PERMISSION_REQUEST_H
#define GSURF_PERMISSION_REQUEST_H

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * GsurfPermissionType:
 * @GSURF_PERMISSION_TYPE_GEOLOCATION: geolocation access
 * @GSURF_PERMISSION_TYPE_NOTIFICATION: desktop notifications
 * @GSURF_PERMISSION_TYPE_MEDIA: camera/microphone access
 * @GSURF_PERMISSION_TYPE_CLIPBOARD: clipboard access
 * @GSURF_PERMISSION_TYPE_OTHER: any other permission kind
 *
 * The kind of capability a #GsurfPermissionRequest asks for.
 */
typedef enum {
	GSURF_PERMISSION_TYPE_GEOLOCATION,
	GSURF_PERMISSION_TYPE_NOTIFICATION,
	GSURF_PERMISSION_TYPE_MEDIA,
	GSURF_PERMISSION_TYPE_CLIPBOARD,
	GSURF_PERMISSION_TYPE_OTHER
} GsurfPermissionType;

#define GSURF_TYPE_PERMISSION_REQUEST (gsurf_permission_request_get_type())

G_DECLARE_FINAL_TYPE(GsurfPermissionRequest, gsurf_permission_request, GSURF, PERMISSION_REQUEST, GObject)

/**
 * gsurf_permission_request_new:
 * @type: the #GsurfPermissionType being requested
 * @origin: (nullable): the requesting origin (e.g. "https://example.com")
 *
 * Creates a new permission request data holder.
 *
 * Returns: (transfer full): a new #GsurfPermissionRequest
 */
GsurfPermissionRequest *gsurf_permission_request_new(GsurfPermissionType type,
	const gchar *origin);

/**
 * gsurf_permission_request_get_permission_type:
 * @self: a #GsurfPermissionRequest
 *
 * Returns: the requested #GsurfPermissionType
 */
GsurfPermissionType gsurf_permission_request_get_permission_type(
	GsurfPermissionRequest *self);

/**
 * gsurf_permission_request_get_origin:
 * @self: a #GsurfPermissionRequest
 *
 * Returns: (transfer none) (nullable): the requesting origin
 */
const gchar *gsurf_permission_request_get_origin(GsurfPermissionRequest *self);

/**
 * gsurf_permission_request_get_handled:
 * @self: a #GsurfPermissionRequest
 *
 * Returns: %TRUE if the request has already been allowed or denied
 */
gboolean gsurf_permission_request_get_handled(GsurfPermissionRequest *self);

/**
 * gsurf_permission_request_allow:
 * @self: a #GsurfPermissionRequest
 *
 * Marks the request as handled and emits #GsurfPermissionRequest::decided
 * with %TRUE.
 */
void gsurf_permission_request_allow(GsurfPermissionRequest *self);

/**
 * gsurf_permission_request_deny:
 * @self: a #GsurfPermissionRequest
 *
 * Marks the request as handled and emits #GsurfPermissionRequest::decided
 * with %FALSE.
 */
void gsurf_permission_request_deny(GsurfPermissionRequest *self);

G_END_DECLS

#endif /* GSURF_PERMISSION_REQUEST_H */
