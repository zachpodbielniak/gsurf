/*
 * gsurf-download.c - Backend-agnostic download data holder
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/gsurf-download.h"

struct _GsurfDownload
{
	GObject parent_instance;

	gchar             *uri;
	gchar             *destination;
	gchar             *suggested_filename;
	gdouble            progress;
	guint64            received_bytes;
	guint64            total_bytes;
	GsurfDownloadState state;
};

enum {
	SIG_PROGRESS_CHANGED,
	SIG_FINISHED,
	SIG_FAILED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GsurfDownload, gsurf_download, G_TYPE_OBJECT)

const gchar *
gsurf_download_get_uri(GsurfDownload *self)
{
	g_return_val_if_fail(GSURF_IS_DOWNLOAD(self), NULL);
	return self->uri;
}

void
gsurf_download_set_uri(GsurfDownload *self, const gchar *uri)
{
	g_return_if_fail(GSURF_IS_DOWNLOAD(self));

	g_clear_pointer(&self->uri, g_free);
	self->uri = g_strdup(uri);
}

const gchar *
gsurf_download_get_destination(GsurfDownload *self)
{
	g_return_val_if_fail(GSURF_IS_DOWNLOAD(self), NULL);
	return self->destination;
}

void
gsurf_download_set_destination(GsurfDownload *self, const gchar *destination)
{
	g_return_if_fail(GSURF_IS_DOWNLOAD(self));

	g_clear_pointer(&self->destination, g_free);
	self->destination = g_strdup(destination);
}

const gchar *
gsurf_download_get_suggested_filename(GsurfDownload *self)
{
	g_return_val_if_fail(GSURF_IS_DOWNLOAD(self), NULL);
	return self->suggested_filename;
}

void
gsurf_download_set_suggested_filename(GsurfDownload *self,
	const gchar *suggested_filename)
{
	g_return_if_fail(GSURF_IS_DOWNLOAD(self));

	g_clear_pointer(&self->suggested_filename, g_free);
	self->suggested_filename = g_strdup(suggested_filename);
}

gdouble
gsurf_download_get_progress(GsurfDownload *self)
{
	g_return_val_if_fail(GSURF_IS_DOWNLOAD(self), 0.0);
	return self->progress;
}

void
gsurf_download_set_progress(GsurfDownload *self, gdouble progress)
{
	g_return_if_fail(GSURF_IS_DOWNLOAD(self));

	self->progress = progress;
	g_signal_emit(self, signals[SIG_PROGRESS_CHANGED], 0);
}

guint64
gsurf_download_get_received_bytes(GsurfDownload *self)
{
	g_return_val_if_fail(GSURF_IS_DOWNLOAD(self), 0);
	return self->received_bytes;
}

void
gsurf_download_set_received_bytes(GsurfDownload *self, guint64 received_bytes)
{
	g_return_if_fail(GSURF_IS_DOWNLOAD(self));
	self->received_bytes = received_bytes;
}

guint64
gsurf_download_get_total_bytes(GsurfDownload *self)
{
	g_return_val_if_fail(GSURF_IS_DOWNLOAD(self), 0);
	return self->total_bytes;
}

void
gsurf_download_set_total_bytes(GsurfDownload *self, guint64 total_bytes)
{
	g_return_if_fail(GSURF_IS_DOWNLOAD(self));
	self->total_bytes = total_bytes;
}

GsurfDownloadState
gsurf_download_get_state(GsurfDownload *self)
{
	g_return_val_if_fail(GSURF_IS_DOWNLOAD(self), GSURF_DOWNLOAD_ACTIVE);
	return self->state;
}

void
gsurf_download_set_state(GsurfDownload *self, GsurfDownloadState state)
{
	g_return_if_fail(GSURF_IS_DOWNLOAD(self));

	self->state = state;

	if (state == GSURF_DOWNLOAD_FINISHED)
		g_signal_emit(self, signals[SIG_FINISHED], 0);
	else if (state == GSURF_DOWNLOAD_FAILED)
		g_signal_emit(self, signals[SIG_FAILED], 0, NULL);
}

void
gsurf_download_cancel(GsurfDownload *self)
{
	g_return_if_fail(GSURF_IS_DOWNLOAD(self));

	/* State-only stub: the backend wires actual cancellation elsewhere. */
	self->state = GSURF_DOWNLOAD_CANCELLED;
}

static void
gsurf_download_finalize(GObject *object)
{
	GsurfDownload *self = GSURF_DOWNLOAD(object);

	g_clear_pointer(&self->uri, g_free);
	g_clear_pointer(&self->destination, g_free);
	g_clear_pointer(&self->suggested_filename, g_free);

	G_OBJECT_CLASS(gsurf_download_parent_class)->finalize(object);
}

static void
gsurf_download_class_init(GsurfDownloadClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gsurf_download_finalize;

	/**
	 * GsurfDownload::progress-changed:
	 * @self: the #GsurfDownload
	 *
	 * Emitted when the download progress is updated.
	 */
	signals[SIG_PROGRESS_CHANGED] = g_signal_new(
		"progress-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);

	/**
	 * GsurfDownload::finished:
	 * @self: the #GsurfDownload
	 *
	 * Emitted when the download completes successfully.
	 */
	signals[SIG_FINISHED] = g_signal_new(
		"finished", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);

	/**
	 * GsurfDownload::failed:
	 * @self: the #GsurfDownload
	 * @message: (nullable): a human-readable failure message
	 *
	 * Emitted when the download fails.
	 */
	signals[SIG_FAILED] = g_signal_new(
		"failed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gsurf_download_init(GsurfDownload *self)
{
	self->uri = NULL;
	self->destination = NULL;
	self->suggested_filename = NULL;
	self->progress = 0.0;
	self->received_bytes = 0;
	self->total_bytes = 0;
	self->state = GSURF_DOWNLOAD_ACTIVE;
}

GsurfDownload *
gsurf_download_new(const gchar *uri)
{
	GsurfDownload *self = g_object_new(GSURF_TYPE_DOWNLOAD, NULL);

	self->uri = g_strdup(uri);
	return self;
}
