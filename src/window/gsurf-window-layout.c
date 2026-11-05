/*
 * gsurf-window-layout.c - Delegate that arranges a window's views
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The interface and its wrappers. The window side (set_layout and the
 * delegation of insert/remove/activate) lives in gsurf-window.c.
 */

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
