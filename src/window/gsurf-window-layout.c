/*
 * gsurf-window-layout.c - Delegate that arranges a window's views
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The interface and its wrappers. The window side (set_layout and the
 * delegation of insert/remove/activate) lives in gsurf-window.c.
 */

#include <gio/gio.h>

#include "window/gsurf-window-layout.h"

G_DEFINE_INTERFACE(GsurfWindowLayout, gsurf_window_layout, G_TYPE_OBJECT)

static void
gsurf_window_layout_default_init(GsurfWindowLayoutInterface *iface)
{
	/* Every vfunc is optional: a missing one is a no-op (or "only the
	 * active view is visible", "cannot serialize/restore"). */
	(void)iface;
}

gpointer
gsurf_window_layout_get_root(GsurfWindowLayout *self)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), NULL);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	return iface->get_root != NULL ? iface->get_root(self) : NULL;
}

void
gsurf_window_layout_attach(GsurfWindowLayout *self, GsurfWindow *window)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->attach != NULL)
		iface->attach(self, window);
}

void
gsurf_window_layout_detach(GsurfWindowLayout *self, GsurfWindow *window)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->detach != NULL)
		iface->detach(self, window);
}

void
gsurf_window_layout_add_view(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));
	g_return_if_fail(GSURF_IS_VIEW(view));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->add_view != NULL)
		iface->add_view(self, window, view);
}

void
gsurf_window_layout_remove_view(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));
	g_return_if_fail(GSURF_IS_VIEW(view));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->remove_view != NULL)
		iface->remove_view(self, window, view);
}

void
gsurf_window_layout_activate_view(GsurfWindowLayout *self, GsurfWindow *window,
                                  GsurfView *view)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));
	g_return_if_fail(GSURF_IS_VIEW(view));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->activate_view != NULL)
		iface->activate_view(self, window, view);
}

gboolean
gsurf_window_layout_is_view_visible(GsurfWindowLayout *self, GsurfWindow *window,
                                    GsurfView *view)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), FALSE);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), FALSE);
	g_return_val_if_fail(GSURF_IS_VIEW(view), FALSE);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->is_view_visible != NULL)
		return iface->is_view_visible(self, window, view);
	/* Default: a layout that shows one view at a time. */
	return gsurf_window_get_active_view(window) == view;
}

gchar *
gsurf_window_layout_serialize(GsurfWindowLayout *self, GsurfWindow *window,
                              GsurfWindowLayoutIndexFunc map, gpointer user_data)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), NULL);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), NULL);
	g_return_val_if_fail(map != NULL, NULL);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	return iface->serialize != NULL ? iface->serialize(self, window, map, user_data) : NULL;
}

gboolean
gsurf_window_layout_restore(GsurfWindowLayout *self, GsurfWindow *window,
                            const gchar *state, GsurfView **views, guint n_views)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), FALSE);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), FALSE);
	g_return_val_if_fail(state != NULL, FALSE);
	g_return_val_if_fail(views != NULL || n_views == 0, FALSE);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	return iface->restore != NULL
		? iface->restore(self, window, state, views, n_views) : FALSE;
}

/* --- Groups (since 0.4.2) --- */

struct _GsurfLayoutGroup
{
	gint     ref_count;
	guint    index;
	gchar   *name;
	gboolean current;
	gboolean last;     /* the previously current group (since 0.4.4) */
	gboolean zoomed;   /* one pane fills the area (since 0.4.4) */
	GArray  *ids;      /* guint64 view ids, pane order */
};

G_DEFINE_BOXED_TYPE(GsurfLayoutGroup, gsurf_layout_group,
	gsurf_layout_group_ref, gsurf_layout_group_unref)

GsurfLayoutGroup *
gsurf_layout_group_new(guint index, const gchar *name, gboolean current)
{
	GsurfLayoutGroup *group = g_new0(GsurfLayoutGroup, 1);

	group->ref_count = 1;
	group->index = index;
	group->name = g_strdup(name != NULL ? name : "");
	group->current = (current != FALSE);
	group->ids = g_array_new(FALSE, FALSE, sizeof(guint64));
	return group;
}

GsurfLayoutGroup *
gsurf_layout_group_ref(GsurfLayoutGroup *group)
{
	g_return_val_if_fail(group != NULL, NULL);

	g_atomic_int_inc(&group->ref_count);
	return group;
}

void
gsurf_layout_group_unref(GsurfLayoutGroup *group)
{
	g_return_if_fail(group != NULL);

	if (!g_atomic_int_dec_and_test(&group->ref_count))
		return;
	g_free(group->name);
	g_array_unref(group->ids);
	g_free(group);
}

void
gsurf_layout_group_add_view_id(GsurfLayoutGroup *group, guint64 view_id)
{
	guint i;

	g_return_if_fail(group != NULL);

	if (view_id == 0)
		return;
	for (i = 0; i < group->ids->len; i++) {
		if (g_array_index(group->ids, guint64, i) == view_id)
			return;
	}
	g_array_append_val(group->ids, view_id);
}

guint
gsurf_layout_group_get_index(GsurfLayoutGroup *group)
{
	g_return_val_if_fail(group != NULL, 0);

	return group->index;
}

const gchar *
gsurf_layout_group_get_name(GsurfLayoutGroup *group)
{
	g_return_val_if_fail(group != NULL, "");

	return group->name;
}

gboolean
gsurf_layout_group_get_current(GsurfLayoutGroup *group)
{
	g_return_val_if_fail(group != NULL, FALSE);

	return group->current;
}

const guint64 *
gsurf_layout_group_get_view_ids(GsurfLayoutGroup *group, guint *n_ids)
{
	g_return_val_if_fail(group != NULL, NULL);
	g_return_val_if_fail(n_ids != NULL, NULL);

	*n_ids = group->ids->len;
	return (const guint64 *)(gpointer)group->ids->data;
}

void
gsurf_layout_group_set_last(GsurfLayoutGroup *group, gboolean last)
{
	g_return_if_fail(group != NULL);

	group->last = (last != FALSE);
}

gboolean
gsurf_layout_group_get_last(GsurfLayoutGroup *group)
{
	g_return_val_if_fail(group != NULL, FALSE);

	return group->last;
}

void
gsurf_layout_group_set_zoomed(GsurfLayoutGroup *group, gboolean zoomed)
{
	g_return_if_fail(group != NULL);

	group->zoomed = (zoomed != FALSE);
}

gboolean
gsurf_layout_group_get_zoomed(GsurfLayoutGroup *group)
{
	g_return_val_if_fail(group != NULL, FALSE);

	return group->zoomed;
}

/* --- Panes (since 0.4.4) --- */

struct _GsurfLayoutPane
{
	gint     ref_count;
	guint    index;
	guint    id;
	guint64  view_id;
	gboolean active;
	gboolean zoomed;
	gint     x;        /* pixels, relative to the layout's area */
	gint     y;
	gint     width;
	gint     height;
	gdouble  fx;       /* fractions (0..1) of the area */
	gdouble  fy;
	gdouble  fwidth;
	gdouble  fheight;
};

G_DEFINE_BOXED_TYPE(GsurfLayoutPane, gsurf_layout_pane,
	gsurf_layout_pane_ref, gsurf_layout_pane_unref)

GsurfLayoutPane *
gsurf_layout_pane_new(guint index, guint id, guint64 view_id, gboolean active)
{
	GsurfLayoutPane *pane = g_new0(GsurfLayoutPane, 1);

	pane->ref_count = 1;
	pane->index = index;
	pane->id = id;
	pane->view_id = view_id;
	pane->active = (active != FALSE);
	return pane;
}

GsurfLayoutPane *
gsurf_layout_pane_ref(GsurfLayoutPane *pane)
{
	g_return_val_if_fail(pane != NULL, NULL);

	g_atomic_int_inc(&pane->ref_count);
	return pane;
}

void
gsurf_layout_pane_unref(GsurfLayoutPane *pane)
{
	g_return_if_fail(pane != NULL);

	if (g_atomic_int_dec_and_test(&pane->ref_count))
		g_free(pane);
}

guint
gsurf_layout_pane_get_index(GsurfLayoutPane *pane)
{
	g_return_val_if_fail(pane != NULL, 0);

	return pane->index;
}

guint
gsurf_layout_pane_get_id(GsurfLayoutPane *pane)
{
	g_return_val_if_fail(pane != NULL, 0);

	return pane->id;
}

guint64
gsurf_layout_pane_get_view_id(GsurfLayoutPane *pane)
{
	g_return_val_if_fail(pane != NULL, 0);

	return pane->view_id;
}

gboolean
gsurf_layout_pane_get_active(GsurfLayoutPane *pane)
{
	g_return_val_if_fail(pane != NULL, FALSE);

	return pane->active;
}

void
gsurf_layout_pane_set_zoomed(GsurfLayoutPane *pane, gboolean zoomed)
{
	g_return_if_fail(pane != NULL);

	pane->zoomed = (zoomed != FALSE);
}

gboolean
gsurf_layout_pane_get_zoomed(GsurfLayoutPane *pane)
{
	g_return_val_if_fail(pane != NULL, FALSE);

	return pane->zoomed;
}

void
gsurf_layout_pane_set_rect(GsurfLayoutPane *pane, gint x, gint y, gint width, gint height)
{
	g_return_if_fail(pane != NULL);

	pane->x = x;
	pane->y = y;
	pane->width = MAX(width, 0);
	pane->height = MAX(height, 0);
}

void
gsurf_layout_pane_get_rect(GsurfLayoutPane *pane, gint *x, gint *y, gint *width, gint *height)
{
	g_return_if_fail(pane != NULL);

	if (x != NULL)
		*x = pane->x;
	if (y != NULL)
		*y = pane->y;
	if (width != NULL)
		*width = pane->width;
	if (height != NULL)
		*height = pane->height;
}

void
gsurf_layout_pane_set_fraction(GsurfLayoutPane *pane, gdouble x, gdouble y,
                               gdouble width, gdouble height)
{
	g_return_if_fail(pane != NULL);

	pane->fx = CLAMP(x, 0.0, 1.0);
	pane->fy = CLAMP(y, 0.0, 1.0);
	pane->fwidth = CLAMP(width, 0.0, 1.0);
	pane->fheight = CLAMP(height, 0.0, 1.0);
}

void
gsurf_layout_pane_get_fraction(GsurfLayoutPane *pane, gdouble *x, gdouble *y,
                               gdouble *width, gdouble *height)
{
	g_return_if_fail(pane != NULL);

	if (x != NULL)
		*x = pane->fx;
	if (y != NULL)
		*y = pane->fy;
	if (width != NULL)
		*width = pane->fwidth;
	if (height != NULL)
		*height = pane->fheight;
}

GPtrArray *
gsurf_window_layout_list_panes(GsurfWindowLayout *self, GsurfWindow *window, guint group)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), NULL);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), NULL);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	return iface->list_panes != NULL ? iface->list_panes(self, window, group) : NULL;
}

gboolean
gsurf_window_layout_run_command(GsurfWindowLayout *self, GsurfWindow *window,
                                const gchar *command, GError **error)
{
	GsurfWindowLayoutInterface *iface;
	GError *local = NULL;
	gboolean ok;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), FALSE);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), FALSE);
	g_return_val_if_fail(command != NULL, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->run_command == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"the window layout has no command language");
		return FALSE;
	}

	/* A failing implementation must say why: a FALSE without an error
	 * gets a generic one, so callers can always report a message. */
	ok = iface->run_command(self, window, command, &local);
	if (!ok && local == NULL)
		local = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
			"the window layout could not run \"%s\"", command);
	if (ok && local != NULL)
		g_clear_error(&local);
	if (local != NULL)
		g_propagate_error(error, local);
	return ok;
}

GVariant *
gsurf_window_layout_describe(GsurfWindowLayout *self, GsurfWindow *window)
{
	GsurfWindowLayoutInterface *iface;
	GVariant *info;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), NULL);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), NULL);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->describe == NULL)
		return NULL;
	info = iface->describe(self, window);
	if (info == NULL)
		return NULL;

	/* Only a vardict is a description; sink a floating one so the
	 * caller always owns a full reference. */
	g_variant_ref_sink(info);
	if (!g_variant_is_of_type(info, G_VARIANT_TYPE_VARDICT)) {
		g_warning("%s: describe() returned a %s, not a{sv}",
			G_OBJECT_TYPE_NAME(self), g_variant_get_type_string(info));
		g_variant_unref(info);
		return NULL;
	}
	return info;
}

GPtrArray *
gsurf_window_layout_list_groups(GsurfWindowLayout *self, GsurfWindow *window)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), NULL);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), NULL);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	return iface->list_groups != NULL ? iface->list_groups(self, window) : NULL;
}

gboolean
gsurf_window_layout_select_group(GsurfWindowLayout *self, GsurfWindow *window, guint index)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), FALSE);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), FALSE);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	return iface->select_group != NULL ? iface->select_group(self, window, index) : FALSE;
}

gboolean
gsurf_window_layout_rename_group(GsurfWindowLayout *self, GsurfWindow *window, guint index,
                                 const gchar *name)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), FALSE);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), FALSE);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	return iface->rename_group != NULL
		? iface->rename_group(self, window, index, name) : FALSE;
}

gboolean
gsurf_window_layout_show_view_at(GsurfWindowLayout *self, GsurfWindow *window,
                                 GsurfView *view, GsurfView *target, GsurfLayoutDropZone zone)
{
	GsurfWindowLayoutInterface *iface;

	g_return_val_if_fail(GSURF_IS_WINDOW_LAYOUT(self), FALSE);
	g_return_val_if_fail(GSURF_IS_WINDOW(window), FALSE);
	g_return_val_if_fail(GSURF_IS_VIEW(view), FALSE);
	g_return_val_if_fail(GSURF_IS_VIEW(target), FALSE);

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	return iface->show_view_at != NULL
		? iface->show_view_at(self, window, view, target, zone) : FALSE;
}

void
gsurf_window_layout_begin_view_drag(GsurfWindowLayout *self, GsurfWindow *window,
                                    GsurfView *view)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));
	g_return_if_fail(GSURF_IS_VIEW(view));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->begin_view_drag != NULL)
		iface->begin_view_drag(self, window, view);
}

void
gsurf_window_layout_end_view_drag(GsurfWindowLayout *self, GsurfWindow *window)
{
	GsurfWindowLayoutInterface *iface;

	g_return_if_fail(GSURF_IS_WINDOW_LAYOUT(self));
	g_return_if_fail(GSURF_IS_WINDOW(window));

	iface = GSURF_WINDOW_LAYOUT_GET_IFACE(self);
	if (iface->end_view_drag != NULL)
		iface->end_view_drag(self, window);
}
