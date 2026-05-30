/*
 * gsurf-permission-request.c - Backend-agnostic permission request holder
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/gsurf-permission-request.h"

struct _GsurfPermissionRequest
{
	GObject parent_instance;

	GsurfPermissionType type;
	gchar              *origin;
	gboolean            handled;
};

enum {
	SIG_DECIDED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GsurfPermissionRequest, gsurf_permission_request, G_TYPE_OBJECT)

GsurfPermissionType
gsurf_permission_request_get_permission_type(GsurfPermissionRequest *self)
{
	g_return_val_if_fail(GSURF_IS_PERMISSION_REQUEST(self),
		GSURF_PERMISSION_TYPE_OTHER);
	return self->type;
}

const gchar *
gsurf_permission_request_get_origin(GsurfPermissionRequest *self)
{
	g_return_val_if_fail(GSURF_IS_PERMISSION_REQUEST(self), NULL);
	return self->origin;
}

gboolean
gsurf_permission_request_get_handled(GsurfPermissionRequest *self)
{
	g_return_val_if_fail(GSURF_IS_PERMISSION_REQUEST(self), FALSE);
	return self->handled;
}

void
gsurf_permission_request_allow(GsurfPermissionRequest *self)
{
	g_return_if_fail(GSURF_IS_PERMISSION_REQUEST(self));

	self->handled = TRUE;
	g_signal_emit(self, signals[SIG_DECIDED], 0, TRUE);
}

void
gsurf_permission_request_deny(GsurfPermissionRequest *self)
{
	g_return_if_fail(GSURF_IS_PERMISSION_REQUEST(self));

	self->handled = TRUE;
	g_signal_emit(self, signals[SIG_DECIDED], 0, FALSE);
}

static void
gsurf_permission_request_finalize(GObject *object)
{
	GsurfPermissionRequest *self = GSURF_PERMISSION_REQUEST(object);

	g_clear_pointer(&self->origin, g_free);

	G_OBJECT_CLASS(gsurf_permission_request_parent_class)->finalize(object);
}

static void
gsurf_permission_request_class_init(GsurfPermissionRequestClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gsurf_permission_request_finalize;

	/**
	 * GsurfPermissionRequest::decided:
	 * @self: the #GsurfPermissionRequest
	 * @allowed: %TRUE if the request was allowed, %FALSE if denied
	 *
	 * Emitted when the request is allowed or denied.
	 */
	signals[SIG_DECIDED] = g_signal_new(
		"decided", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
gsurf_permission_request_init(GsurfPermissionRequest *self)
{
	self->type = GSURF_PERMISSION_TYPE_OTHER;
	self->origin = NULL;
	self->handled = FALSE;
}

GsurfPermissionRequest *
gsurf_permission_request_new(GsurfPermissionType type, const gchar *origin)
{
	GsurfPermissionRequest *self = g_object_new(GSURF_TYPE_PERMISSION_REQUEST, NULL);

	self->type = type;
	self->origin = g_strdup(origin);
	return self;
}
