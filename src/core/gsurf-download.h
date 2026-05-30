/*
 * gsurf-download.h - Backend-agnostic download data holder
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GsurfDownload is a plain data-holder GObject describing an in-flight or
 * completed download. It carries no GTK/WebKit state; the backend mirrors
 * the engine's download object onto this type so modules and embedders can
 * observe progress through GObject signals and properties.
 */

#ifndef GSURF_DOWNLOAD_H
#define GSURF_DOWNLOAD_H

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * GsurfDownloadState:
 * @GSURF_DOWNLOAD_ACTIVE: the download is in progress
 * @GSURF_DOWNLOAD_FINISHED: the download completed successfully
 * @GSURF_DOWNLOAD_FAILED: the download failed
 * @GSURF_DOWNLOAD_CANCELLED: the download was cancelled
 *
 * Lifecycle state of a #GsurfDownload.
 */
typedef enum {
	GSURF_DOWNLOAD_ACTIVE,
	GSURF_DOWNLOAD_FINISHED,
	GSURF_DOWNLOAD_FAILED,
	GSURF_DOWNLOAD_CANCELLED
} GsurfDownloadState;

#define GSURF_TYPE_DOWNLOAD (gsurf_download_get_type())

G_DECLARE_FINAL_TYPE(GsurfDownload, gsurf_download, GSURF, DOWNLOAD, GObject)

/**
 * gsurf_download_new:
 * @uri: (nullable): the source URI of the download
 *
 * Creates a new download data holder.
 *
 * Returns: (transfer full): a new #GsurfDownload
 */
GsurfDownload *gsurf_download_new(const gchar *uri);

/**
 * gsurf_download_get_uri:
 * @self: a #GsurfDownload
 *
 * Returns: (transfer none) (nullable): the source URI
 */
const gchar *gsurf_download_get_uri(GsurfDownload *self);

/**
 * gsurf_download_set_uri:
 * @self: a #GsurfDownload
 * @uri: (nullable): the source URI
 */
void gsurf_download_set_uri(GsurfDownload *self, const gchar *uri);

/**
 * gsurf_download_get_destination:
 * @self: a #GsurfDownload
 *
 * Returns: (transfer none) (nullable): the destination path
 */
const gchar *gsurf_download_get_destination(GsurfDownload *self);

/**
 * gsurf_download_set_destination:
 * @self: a #GsurfDownload
 * @destination: (nullable): the destination path
 */
void gsurf_download_set_destination(GsurfDownload *self, const gchar *destination);

/**
 * gsurf_download_get_suggested_filename:
 * @self: a #GsurfDownload
 *
 * Returns: (transfer none) (nullable): the suggested filename
 */
const gchar *gsurf_download_get_suggested_filename(GsurfDownload *self);

/**
 * gsurf_download_set_suggested_filename:
 * @self: a #GsurfDownload
 * @suggested_filename: (nullable): the suggested filename
 */
void gsurf_download_set_suggested_filename(GsurfDownload *self,
	const gchar *suggested_filename);

/**
 * gsurf_download_get_progress:
 * @self: a #GsurfDownload
 *
 * Returns: the estimated progress in the range [0.0, 1.0]
 */
gdouble gsurf_download_get_progress(GsurfDownload *self);

/**
 * gsurf_download_set_progress:
 * @self: a #GsurfDownload
 * @progress: the estimated progress in the range [0.0, 1.0]
 *
 * Sets the progress and emits #GsurfDownload::progress-changed.
 */
void gsurf_download_set_progress(GsurfDownload *self, gdouble progress);

/**
 * gsurf_download_get_received_bytes:
 * @self: a #GsurfDownload
 *
 * Returns: the number of bytes received so far
 */
guint64 gsurf_download_get_received_bytes(GsurfDownload *self);

/**
 * gsurf_download_set_received_bytes:
 * @self: a #GsurfDownload
 * @received_bytes: the number of bytes received so far
 */
void gsurf_download_set_received_bytes(GsurfDownload *self, guint64 received_bytes);

/**
 * gsurf_download_get_total_bytes:
 * @self: a #GsurfDownload
 *
 * Returns: the total expected size in bytes, or 0 if unknown
 */
guint64 gsurf_download_get_total_bytes(GsurfDownload *self);

/**
 * gsurf_download_set_total_bytes:
 * @self: a #GsurfDownload
 * @total_bytes: the total expected size in bytes
 */
void gsurf_download_set_total_bytes(GsurfDownload *self, guint64 total_bytes);

/**
 * gsurf_download_get_state:
 * @self: a #GsurfDownload
 *
 * Returns: the current #GsurfDownloadState
 */
GsurfDownloadState gsurf_download_get_state(GsurfDownload *self);

/**
 * gsurf_download_set_state:
 * @self: a #GsurfDownload
 * @state: the new #GsurfDownloadState
 *
 * Sets the state. Emits #GsurfDownload::finished when @state is
 * %GSURF_DOWNLOAD_FINISHED, and #GsurfDownload::failed when @state is
 * %GSURF_DOWNLOAD_FAILED.
 */
void gsurf_download_set_state(GsurfDownload *self, GsurfDownloadState state);

/**
 * gsurf_download_cancel:
 * @self: a #GsurfDownload
 *
 * Marks the download as cancelled by setting its state to
 * %GSURF_DOWNLOAD_CANCELLED. This performs no backend action; the backend
 * is expected to wire cancellation through elsewhere.
 */
void gsurf_download_cancel(GsurfDownload *self);

G_END_DECLS

#endif /* GSURF_DOWNLOAD_H */
