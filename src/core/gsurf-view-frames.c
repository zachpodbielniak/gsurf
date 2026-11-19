/*
 * gsurf-view-frames.c - A view's frames: list them, run a script in one
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The UI-process end of the frames module's web-process extension
 * (modules/frames/webext/), over gsurf_view_send_page_message_async().
 * The messages are in gsurf-frames-protocol.h.
 */

#include <gio/gio.h>
#include "core/gsurf-view-frames.h"
#include "core/gsurf-frames-protocol.h"

/* What a caller without the module is told. */
#define NEEDS_MODULE \
	"page frames need the frames module (modules.frames.enabled: true, at startup)"

/* A value from one frame, and where that frame is in the view. */
typedef struct {
	gchar   *text;
	gdouble  x;
	gdouble  y;
	guint64  id;
} FrameValue;

static void
frame_value_free(gpointer data)
{
	FrameValue *value = data;

	g_free(value->text);
	g_free(value);
}

/* NOT_SUPPORTED from the page means nothing answered: say what to enable. */
static void
return_page_error(GTask *task, GError *error)
{
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s",
			NEEDS_MODULE);
		g_error_free(error);
		return;
	}
	g_task_return_error(task, error);
}

/* --- the list --- */

static void
listed(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GTask) task = user_data;
	g_autoptr(GVariant) reply = NULL;
	GError *error = NULL;
	const gchar *json = NULL;

	reply = gsurf_view_send_page_message_finish(GSURF_VIEW(source), result, &error);
	if (reply == NULL) {
		return_page_error(task, error);
		return;
	}
	if (!g_variant_is_of_type(reply, G_VARIANT_TYPE("(s)"))) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			"the frames extension answered '%s'", g_variant_get_type_string(reply));
		return;
	}
	g_variant_get(reply, "(&s)", &json);
	g_task_return_pointer(task, g_strdup(json), g_free);
}

void
gsurf_view_list_frames_async(GsurfView *self, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task;

	g_return_if_fail(GSURF_IS_VIEW(self));

	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, gsurf_view_list_frames_async);
	gsurf_view_send_page_message_async(self, GSURF_FRAMES_MSG_LIST, NULL, cancellable,
		listed, task);
}

gchar *
gsurf_view_list_frames_finish(GsurfView *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* --- a script in one frame --- */

static void
main_frame_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GTask) task = user_data;
	GsurfView *view = g_task_get_source_object(task);
	GError *error = NULL;
	FrameValue *value;
	gchar *text;

	(void)source;
	text = gsurf_view_run_javascript_finish(view, result, &error);
	if (error != NULL) {
		g_task_return_error(task, error);
		return;
	}
	/* a value with no JSON form (undefined) is null, as in a frame */
	if (text == NULL)
		text = g_strdup("null");
	value = g_new0(FrameValue, 1);
	value->text = text;
	g_task_return_pointer(task, value, frame_value_free);
}

static void
frame_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GTask) task = user_data;
	g_autoptr(GVariant) reply = NULL;
	GError *error = NULL;
	FrameValue *value;
	const gchar *text = NULL;
	guint status = 0;
	guint64 id = 0;
	gdouble x = 0;
	gdouble y = 0;

	reply = gsurf_view_send_page_message_finish(GSURF_VIEW(source), result, &error);
	if (reply == NULL) {
		return_page_error(task, error);
		return;
	}
	if (!g_variant_is_of_type(reply, G_VARIANT_TYPE(GSURF_FRAMES_EVAL_REPLY))) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			"the frames extension answered '%s'", g_variant_get_type_string(reply));
		return;
	}
	g_variant_get(reply, "(ut&sdd)", &status, &id, &text, &x, &y);
	if (status == GSURF_FRAMES_STATUS_NO_FRAME) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s", text);
		return;
	}
	if (status != GSURF_FRAMES_STATUS_OK) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", text);
		return;
	}
	value = g_new0(FrameValue, 1);
	value->text = g_strdup(text);
	value->x = x;
	value->y = y;
	value->id = id;
	g_task_return_pointer(task, value, frame_value_free);
}

void
gsurf_view_run_javascript_in_frame_async(GsurfView *self, const gchar *frame,
                                         const gchar *require, const gchar *script,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task;

	g_return_if_fail(GSURF_IS_VIEW(self));
	g_return_if_fail(script != NULL);

	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, gsurf_view_run_javascript_in_frame_async);
	if (frame == NULL) {
		gsurf_view_run_javascript_async(self, script, cancellable, main_frame_done, task);
		return;
	}
	gsurf_view_send_page_message_async(self, GSURF_FRAMES_MSG_EVAL,
		g_variant_new(GSURF_FRAMES_EVAL_PARAMS, frame, script, require != NULL ? require : ""),
		cancellable, frame_done, task);
}

gchar *
gsurf_view_run_javascript_in_frame_finish(GsurfView *self, GAsyncResult *result,
                                          gdouble *x, gdouble *y, guint64 *frame_id,
                                          GError **error)
{
	FrameValue *value;
	gchar *text;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);

	value = g_task_propagate_pointer(G_TASK(result), error);
	if (value == NULL)
		return NULL;
	if (x != NULL)
		*x = value->x;
	if (y != NULL)
		*y = value->y;
	if (frame_id != NULL)
		*frame_id = value->id;
	text = g_steal_pointer(&value->text);
	frame_value_free(value);
	return text;
}
