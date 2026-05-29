/*
 * gsurf-application.c - Top-level browser session
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/gsurf-application.h"

struct _GsurfApplication
{
	GObject parent_instance;

	GsurfConfig *config;
	GPtrArray   *windows;   /* owned refs to GsurfWindow */
	GMainLoop   *loop;
};

enum {
	SIG_WINDOW_ADDED,
	SIG_WINDOW_REMOVED,
	SIG_SHUTDOWN,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GsurfApplication, gsurf_application, G_TYPE_OBJECT)

static gboolean
on_window_close_request(GsurfWindow *window, gpointer user_data)
{
	GsurfApplication *self = user_data;

	/* Allow the close; drop our reference and quit if it was the last. */
	gsurf_application_remove_window(self, window);
	return FALSE;
}

GsurfConfig *
gsurf_application_get_config(GsurfApplication *self)
{
	g_return_val_if_fail(GSURF_IS_APPLICATION(self), NULL);
	return self->config;
}

void
gsurf_application_add_window(GsurfApplication *self, GsurfWindow *window)
{
	g_return_if_fail(GSURF_IS_APPLICATION(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));

	g_ptr_array_add(self->windows, g_object_ref(window));
	g_signal_connect(window, "close-request",
		G_CALLBACK(on_window_close_request), self);

	g_signal_emit(self, signals[SIG_WINDOW_ADDED], 0, window);
}

void
gsurf_application_remove_window(GsurfApplication *self, GsurfWindow *window)
{
	guint idx;

	g_return_if_fail(GSURF_IS_APPLICATION(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));

	if (!g_ptr_array_find(self->windows, window, &idx))
		return;

	g_object_ref(window);
	g_signal_handlers_disconnect_by_data(window, self);
	g_ptr_array_remove_index(self->windows, idx);
	g_signal_emit(self, signals[SIG_WINDOW_REMOVED], 0, window);
	g_object_unref(window);

	if (self->windows->len == 0)
		gsurf_application_quit(self);
}

GPtrArray *
gsurf_application_get_windows(GsurfApplication *self)
{
	g_return_val_if_fail(GSURF_IS_APPLICATION(self), NULL);
	return self->windows;
}

GsurfWindow *
gsurf_application_get_active_window(GsurfApplication *self)
{
	g_return_val_if_fail(GSURF_IS_APPLICATION(self), NULL);

	if (self->windows->len == 0)
		return NULL;
	return g_ptr_array_index(self->windows, self->windows->len - 1);
}

void
gsurf_application_run(GsurfApplication *self)
{
	g_return_if_fail(GSURF_IS_APPLICATION(self));

	if (self->loop != NULL)
		return;

	self->loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(self->loop);
	g_clear_pointer(&self->loop, g_main_loop_unref);

	g_signal_emit(self, signals[SIG_SHUTDOWN], 0);
}

void
gsurf_application_quit(GsurfApplication *self)
{
	g_return_if_fail(GSURF_IS_APPLICATION(self));

	if (self->loop != NULL && g_main_loop_is_running(self->loop))
		g_main_loop_quit(self->loop);
}

static void
gsurf_application_finalize(GObject *object)
{
	GsurfApplication *self = GSURF_APPLICATION(object);

	g_clear_pointer(&self->windows, g_ptr_array_unref);
	g_clear_object(&self->config);
	g_clear_pointer(&self->loop, g_main_loop_unref);

	G_OBJECT_CLASS(gsurf_application_parent_class)->finalize(object);
}

static void
gsurf_application_class_init(GsurfApplicationClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gsurf_application_finalize;

	signals[SIG_WINDOW_ADDED] = g_signal_new(
		"window-added", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_WINDOW);

	signals[SIG_WINDOW_REMOVED] = g_signal_new(
		"window-removed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_WINDOW);

	signals[SIG_SHUTDOWN] = g_signal_new(
		"shutdown", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

static void
gsurf_application_init(GsurfApplication *self)
{
	self->windows = g_ptr_array_new_with_free_func(g_object_unref);
	self->config = NULL;
	self->loop = NULL;
}

GsurfApplication *
gsurf_application_new(GsurfConfig *config)
{
	GsurfApplication *self = g_object_new(GSURF_TYPE_APPLICATION, NULL);

	self->config = config != NULL ? g_object_ref(config) : gsurf_config_new();
	return self;
}
