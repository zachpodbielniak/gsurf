/*
 * gsurf-window.c - Abstract browser window
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "window/gsurf-window.h"

typedef struct {
	GPtrArray *views;     /* owned refs to GsurfView */
	GsurfView *active;    /* borrowed (element of views) */
	gboolean   fullscreen;
} GsurfWindowPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(GsurfWindow, gsurf_window, G_TYPE_OBJECT)

enum {
	SIG_VIEW_ADDED,
	SIG_VIEW_REMOVED,
	SIG_ACTIVE_VIEW_CHANGED,
	SIG_CLOSE_REQUEST,
	SIG_KEY_PRESS,
	SIG_BUTTON_PRESS,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gsurf_window_constructed(GObject *object)
{
	GsurfWindow *self = GSURF_WINDOW(object);
	GsurfWindowClass *klass = GSURF_WINDOW_GET_CLASS(self);

	G_OBJECT_CLASS(gsurf_window_parent_class)->constructed(object);

	/* Build the native toplevel/container now that the concrete
	 * instance has been initialized. */
	if (klass->realize != NULL)
		klass->realize(self);
}

static void
gsurf_window_finalize(GObject *object)
{
	GsurfWindow *self = GSURF_WINDOW(object);
	GsurfWindowPrivate *priv = gsurf_window_get_instance_private(self);

	g_clear_pointer(&priv->views, g_ptr_array_unref);

	G_OBJECT_CLASS(gsurf_window_parent_class)->finalize(object);
}

static void
gsurf_window_class_init(GsurfWindowClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->constructed = gsurf_window_constructed;
	object_class->finalize = gsurf_window_finalize;

	signals[SIG_VIEW_ADDED] = g_signal_new(
		"view-added", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_VIEW);

	signals[SIG_VIEW_REMOVED] = g_signal_new(
		"view-removed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_VIEW);

	signals[SIG_ACTIVE_VIEW_CHANGED] = g_signal_new(
		"active-view-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_VIEW);

	/**
	 * GsurfWindow::close-request:
	 *
	 * Emitted when the user requests to close the window. Returning
	 * %TRUE prevents the default close.
	 */
	signals[SIG_CLOSE_REQUEST] = g_signal_new(
		"close-request", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_true_handled, NULL, NULL,
		G_TYPE_BOOLEAN, 0);

	/**
	 * GsurfWindow::key-press:
	 * @window: the window
	 * @keyval: the key value
	 * @keycode: the hardware keycode
	 * @state: the modifier state (#GsurfKeyMod-compatible)
	 *
	 * Returns: %TRUE if the key was consumed.
	 */
	signals[SIG_KEY_PRESS] = g_signal_new(
		"key-press", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_true_handled, NULL, NULL,
		G_TYPE_BOOLEAN, 3, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);

	/**
	 * GsurfWindow::button-press:
	 * @window: the window
	 * @button: the button number
	 * @state: the modifier state
	 *
	 * Returns: %TRUE if the press was consumed.
	 */
	signals[SIG_BUTTON_PRESS] = g_signal_new(
		"button-press", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_true_handled, NULL, NULL,
		G_TYPE_BOOLEAN, 2, G_TYPE_UINT, G_TYPE_UINT);
}

static void
gsurf_window_init(GsurfWindow *self)
{
	GsurfWindowPrivate *priv = gsurf_window_get_instance_private(self);

	priv->views = g_ptr_array_new_with_free_func(g_object_unref);
	priv->active = NULL;
	priv->fullscreen = FALSE;
}

/* --- View management --- */

void
gsurf_window_add_view(GsurfWindow *self, GsurfView *view)
{
	GsurfWindowPrivate *priv;
	GsurfWindowClass *klass;
	gboolean first;

	g_return_if_fail(GSURF_IS_WINDOW(self));
	g_return_if_fail(GSURF_IS_VIEW(view));

	priv = gsurf_window_get_instance_private(self);
	klass = GSURF_WINDOW_GET_CLASS(self);

	first = (priv->views->len == 0);
	g_ptr_array_add(priv->views, g_object_ref(view));

	if (klass->insert_view != NULL)
		klass->insert_view(self, view);

	g_signal_emit(self, signals[SIG_VIEW_ADDED], 0, view);

	if (first)
		gsurf_window_set_active_view(self, view);
}

void
gsurf_window_remove_view(GsurfWindow *self, GsurfView *view)
{
	GsurfWindowPrivate *priv;
	GsurfWindowClass *klass;
	guint idx;

	g_return_if_fail(GSURF_IS_WINDOW(self));
	g_return_if_fail(GSURF_IS_VIEW(view));

	priv = gsurf_window_get_instance_private(self);
	klass = GSURF_WINDOW_GET_CLASS(self);

	if (!g_ptr_array_find(priv->views, view, &idx))
		return;

	if (klass->remove_view != NULL)
		klass->remove_view(self, view);

	/* Hold a ref across the removal so we can still emit the signal. */
	g_object_ref(view);
	g_ptr_array_remove_index(priv->views, idx);

	if (priv->active == view) {
		priv->active = priv->views->len > 0
			? g_ptr_array_index(priv->views, MIN(idx, priv->views->len - 1))
			: NULL;
		if (priv->active != NULL)
			gsurf_window_set_active_view(self, priv->active);
	}

	g_signal_emit(self, signals[SIG_VIEW_REMOVED], 0, view);
	g_object_unref(view);
}

void
gsurf_window_set_active_view(GsurfWindow *self, GsurfView *view)
{
	GsurfWindowPrivate *priv;
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));
	g_return_if_fail(GSURF_IS_VIEW(view));

	priv = gsurf_window_get_instance_private(self);
	klass = GSURF_WINDOW_GET_CLASS(self);

	if (priv->active == view)
		return;

	priv->active = view;

	if (klass->set_active_view != NULL)
		klass->set_active_view(self, view);

	g_signal_emit(self, signals[SIG_ACTIVE_VIEW_CHANGED], 0, view);
}

GsurfView *
gsurf_window_get_active_view(GsurfWindow *self)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), NULL);

	priv = gsurf_window_get_instance_private(self);
	return priv->active;
}

GPtrArray *
gsurf_window_get_views(GsurfWindow *self)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), NULL);

	priv = gsurf_window_get_instance_private(self);
	return priv->views;
}

guint
gsurf_window_get_n_views(GsurfWindow *self)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), 0);

	priv = gsurf_window_get_instance_private(self);
	return priv->views->len;
}

GsurfView *
gsurf_window_get_nth_view(GsurfWindow *self, guint index)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), NULL);

	priv = gsurf_window_get_instance_private(self);
	if (index >= priv->views->len)
		return NULL;
	return g_ptr_array_index(priv->views, index);
}

/* --- Native operations --- */

void
gsurf_window_present(GsurfWindow *self)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	if (klass->present != NULL)
		klass->present(self);
}

void
gsurf_window_set_title(GsurfWindow *self, const gchar *title)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	if (klass->set_title != NULL)
		klass->set_title(self, title);
}

void
gsurf_window_set_default_size(GsurfWindow *self, gint width, gint height)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	if (klass->set_default_size != NULL)
		klass->set_default_size(self, width, height);
}

void
gsurf_window_set_fullscreen(GsurfWindow *self, gboolean fullscreen)
{
	GsurfWindowPrivate *priv;
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	priv = gsurf_window_get_instance_private(self);
	klass = GSURF_WINDOW_GET_CLASS(self);

	priv->fullscreen = fullscreen;
	if (klass->set_fullscreen != NULL)
		klass->set_fullscreen(self, fullscreen);
}

gboolean
gsurf_window_get_fullscreen(GsurfWindow *self)
{
	GsurfWindowPrivate *priv;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), FALSE);

	priv = gsurf_window_get_instance_private(self);
	return priv->fullscreen;
}

gpointer
gsurf_window_get_native_widget(GsurfWindow *self)
{
	GsurfWindowClass *klass;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), NULL);

	klass = GSURF_WINDOW_GET_CLASS(self);
	return klass->get_native_widget != NULL ? klass->get_native_widget(self) : NULL;
}

void
gsurf_window_add_top_widget(GsurfWindow *self, gpointer widget)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	if (klass->add_chrome_widget != NULL)
		klass->add_chrome_widget(self, widget, TRUE);
}

void
gsurf_window_add_bottom_widget(GsurfWindow *self, gpointer widget)
{
	GsurfWindowClass *klass;

	g_return_if_fail(GSURF_IS_WINDOW(self));

	klass = GSURF_WINDOW_GET_CLASS(self);
	if (klass->add_chrome_widget != NULL)
		klass->add_chrome_widget(self, widget, FALSE);
}

/* --- Event emission helpers --- */

gboolean
gsurf_window_emit_key_press(GsurfWindow *self, guint keyval, guint keycode, guint state)
{
	gboolean handled = FALSE;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), FALSE);

	g_signal_emit(self, signals[SIG_KEY_PRESS], 0, keyval, keycode, state, &handled);
	return handled;
}

gboolean
gsurf_window_emit_button_press(GsurfWindow *self, guint button, guint state)
{
	gboolean handled = FALSE;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), FALSE);

	g_signal_emit(self, signals[SIG_BUTTON_PRESS], 0, button, state, &handled);
	return handled;
}

gboolean
gsurf_window_emit_close_request(GsurfWindow *self)
{
	gboolean handled = FALSE;

	g_return_val_if_fail(GSURF_IS_WINDOW(self), FALSE);

	g_signal_emit(self, signals[SIG_CLOSE_REQUEST], 0, &handled);
	return handled;
}
