/*
 * gsurf-wmux-grid-gtk4.c - The wmux pane grid on GTK4
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A GtkWidget that parents the pane frames and places each at its pane's
 * rectangle (see gsurf-wmux-grid.h). A GtkGestureDrag on the grid gets
 * the presses that hit no child (the gaps between panes) and turns them
 * into border drags; a motion controller shows the resize cursor there.
 */

#include "gsurf-wmux-grid.h"
#include "gsurf-wmux-layout.h"

/* Object data on a child: its pane id plus one (0 = not a grid child). */
#define PANE_DATA "gsurf-wmux-grid-pane"

typedef struct _GsurfWmuxGrid      GsurfWmuxGrid;
typedef struct _GsurfWmuxGridClass GsurfWmuxGridClass;

struct _GsurfWmuxGrid
{
	GtkWidget parent_instance;

	GPtrArray                *children; /* GtkWidget*, borrowed (parented) */
	const GsurfWmuxGridFuncs *funcs;
	gpointer                  owner;    /* NULL: cut off from the module */
	gboolean                  dragging;
	gdouble                   start_x;  /* where the drag gesture began */
	gdouble                   start_y;
	gint                      cursor;   /* the border kind the cursor shows */
};

struct _GsurfWmuxGridClass
{
	GtkWidgetClass parent_class;
};

static GType gsurf_wmux_grid_get_type(void);
G_DEFINE_TYPE(GsurfWmuxGrid, gsurf_wmux_grid, GTK_TYPE_WIDGET)

#define GRID(obj) ((GsurfWmuxGrid *)(obj))
#define IS_GRID(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), gsurf_wmux_grid_get_type()))

/* ------------------------------------------------------------------ */
/* Size                                                                */
/* ------------------------------------------------------------------ */

static void
grid_measure(GtkWidget *widget, GtkOrientation orientation, gint for_size,
             gint *minimum, gint *natural, gint *minimum_baseline, gint *natural_baseline)
{
	(void)widget;
	(void)orientation;
	(void)for_size;
	/* The panes share whatever the window gives; the grid asks for
	 * nothing so it never forces the toplevel larger. */
	*minimum = 0;
	*natural = 0;
	*minimum_baseline = -1;
	*natural_baseline = -1;
}

static const GsurfWmuxRect *
find_rect(GArray *rects, guint pane)
{
	guint i;

	if (rects == NULL)
		return NULL;
	for (i = 0; i < rects->len; i++) {
		const GsurfWmuxRect *r = &g_array_index(rects, GsurfWmuxRect, i);

		if (r->pane == pane)
			return r;
	}
	return NULL;
}

static void
grid_size_allocate(GtkWidget *widget, gint width, gint height, gint baseline)
{
	GsurfWmuxGrid *self = GRID(widget);
	g_autoptr(GArray) rects = NULL;
	guint i;

	(void)baseline;
	/* 1. The pane rectangles for this size, from the owner's model. */
	if (self->owner != NULL && self->funcs->geometry != NULL)
		rects = self->funcs->geometry(self->owner, width, height);

	/* 2. Each child at its rectangle; a child without one (another pane
	 *    is zoomed) is not shown. Never below a child's minimum size. */
	for (i = 0; i < self->children->len; i++) {
		GtkWidget *child = g_ptr_array_index(self->children, i);
		const GsurfWmuxRect *r = find_rect(rects, gsurf_wmux_grid_get_pane(child));
		GtkRequisition min;
		GtkAllocation a;

		if (r == NULL) {
			gtk_widget_set_child_visible(child, FALSE);
			continue;
		}
		gtk_widget_set_child_visible(child, TRUE);
		gtk_widget_get_preferred_size(child, &min, NULL);
		a.x = r->x;
		a.y = r->y;
		a.width = MAX(r->width, min.width);
		a.height = MAX(r->height, min.height);
		gtk_widget_size_allocate(child, &a, -1);
	}
}

/* ------------------------------------------------------------------ */
/* Pointer: border drags and the resize cursor                         */
/* ------------------------------------------------------------------ */

static void
update_cursor(GsurfWmuxGrid *self, gint kind)
{
	if (kind == self->cursor)
		return;
	self->cursor = kind;
	if (kind == GSURF_WMUX_GRID_BORDER_VERTICAL)
		gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "col-resize");
	else if (kind == GSURF_WMUX_GRID_BORDER_HORIZONTAL)
		gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "row-resize");
	else
		gtk_widget_set_cursor(GTK_WIDGET(self), NULL);
}

static void
on_drag_begin(GtkGestureDrag *gesture, gdouble x, gdouble y, gpointer user_data)
{
	GsurfWmuxGrid *self = user_data;
	GtkWidget *hit;

	/* A press that landed on a pane belongs to the pane; only the gaps
	 * (where no child is picked) start a drag. */
	hit = gtk_widget_pick(GTK_WIDGET(self), x, y, GTK_PICK_DEFAULT);
	if ((hit != NULL && hit != GTK_WIDGET(self)) ||
	    !gsurf_wmux_grid_drag_begin(GTK_WIDGET(self), (gint)x, (gint)y)) {
		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
		return;
	}
	self->start_x = x;
	self->start_y = y;
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_drag_update(GtkGestureDrag *gesture, gdouble dx, gdouble dy, gpointer user_data)
{
	GsurfWmuxGrid *self = user_data;

	(void)gesture;
	gsurf_wmux_grid_drag_update(GTK_WIDGET(self),
		(gint)(self->start_x + dx), (gint)(self->start_y + dy));
}

static void
on_drag_end(GtkGestureDrag *gesture, gdouble dx, gdouble dy, gpointer user_data)
{
	(void)gesture;
	(void)dx;
	(void)dy;
	gsurf_wmux_grid_drag_end(GTK_WIDGET(user_data));
}

static void
on_motion(GtkEventControllerMotion *motion, gdouble x, gdouble y, gpointer user_data)
{
	GsurfWmuxGrid *self = user_data;

	(void)motion;
	if (!self->dragging)
		update_cursor(self, gsurf_wmux_grid_hit_border(GTK_WIDGET(self), (gint)x, (gint)y));
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

static void
grid_dispose(GObject *object)
{
	GsurfWmuxGrid *self = GRID(object);

	/* Unparent what is left; the owner normally took its frames out. */
	while (self->children != NULL && self->children->len > 0)
		gsurf_wmux_grid_remove(GTK_WIDGET(self), g_ptr_array_index(self->children, 0));
	G_OBJECT_CLASS(gsurf_wmux_grid_parent_class)->dispose(object);
}

static void
grid_finalize(GObject *object)
{
	g_clear_pointer(&GRID(object)->children, g_ptr_array_unref);
	G_OBJECT_CLASS(gsurf_wmux_grid_parent_class)->finalize(object);
}

static void
gsurf_wmux_grid_class_init(GsurfWmuxGridClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	object_class->dispose = grid_dispose;
	object_class->finalize = grid_finalize;
	widget_class->measure = grid_measure;
	widget_class->size_allocate = grid_size_allocate;
	gtk_widget_class_set_css_name(widget_class, "wmuxgrid");
}

static void
gsurf_wmux_grid_init(GsurfWmuxGrid *self)
{
	GtkGesture *drag;
	GtkEventController *motion;

	self->children = g_ptr_array_new();
	gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
	gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
	gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);

	/* Primary-button drags on the gaps. */
	drag = gtk_gesture_drag_new();
	gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(drag), "gsurf-wmux-grid-drag");
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), self);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), self);
	gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drag));

	motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(on_motion), self);
	gtk_widget_add_controller(GTK_WIDGET(self), motion);
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

GtkWidget *
gsurf_wmux_grid_new(const GsurfWmuxGridFuncs *funcs, gpointer data)
{
	GsurfWmuxGrid *self;

	g_return_val_if_fail(funcs != NULL, NULL);
	self = g_object_new(gsurf_wmux_grid_get_type(), NULL);
	self->funcs = funcs;
	self->owner = data;
	return GTK_WIDGET(self);
}

void
gsurf_wmux_grid_set_owner(GtkWidget *grid, gpointer data)
{
	g_return_if_fail(IS_GRID(grid));
	GRID(grid)->owner = data;
	GRID(grid)->dragging = FALSE;
	gtk_widget_queue_allocate(grid);
}

void
gsurf_wmux_grid_put(GtkWidget *grid, GtkWidget *child, guint pane)
{
	GsurfWmuxGrid *self;

	g_return_if_fail(IS_GRID(grid));
	g_return_if_fail(GTK_IS_WIDGET(child));
	self = GRID(grid);

	g_object_set_data(G_OBJECT(child), PANE_DATA, GUINT_TO_POINTER(pane + 1));
	if (gtk_widget_get_parent(child) == grid) {
		gtk_widget_queue_allocate(grid);
		return;
	}
	g_return_if_fail(gtk_widget_get_parent(child) == NULL);
	g_ptr_array_add(self->children, child);
	gtk_widget_set_parent(child, grid);
	gtk_widget_queue_allocate(grid);
}

void
gsurf_wmux_grid_remove(GtkWidget *grid, GtkWidget *child)
{
	GsurfWmuxGrid *self;

	g_return_if_fail(IS_GRID(grid));
	self = GRID(grid);
	if (child == NULL || gtk_widget_get_parent(child) != grid)
		return;
	g_ptr_array_remove(self->children, child);
	g_object_set_data(G_OBJECT(child), PANE_DATA, NULL);
	gtk_widget_unparent(child);
	gtk_widget_queue_allocate(grid);
}

GPtrArray *
gsurf_wmux_grid_get_children(GtkWidget *grid)
{
	g_return_val_if_fail(IS_GRID(grid), NULL);
	return g_ptr_array_copy(GRID(grid)->children, NULL, NULL);
}

guint
gsurf_wmux_grid_get_pane(GtkWidget *child)
{
	guint tag = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(child), PANE_DATA));

	return tag == 0 ? G_MAXUINT : tag - 1;
}

void
gsurf_wmux_grid_relayout(GtkWidget *grid)
{
	g_return_if_fail(IS_GRID(grid));
	gtk_widget_queue_allocate(grid);
}

gboolean
gsurf_wmux_grid_drag_begin(GtkWidget *grid, gint x, gint y)
{
	GsurfWmuxGrid *self;

	g_return_val_if_fail(IS_GRID(grid), FALSE);
	self = GRID(grid);
	if (self->owner == NULL || self->funcs->drag_begin == NULL)
		return FALSE;
	self->dragging = self->funcs->drag_begin(self->owner,
		gtk_widget_get_width(grid), gtk_widget_get_height(grid), x, y);
	return self->dragging;
}

void
gsurf_wmux_grid_drag_update(GtkWidget *grid, gint x, gint y)
{
	GsurfWmuxGrid *self;

	g_return_if_fail(IS_GRID(grid));
	self = GRID(grid);
	if (!self->dragging || self->owner == NULL || self->funcs->drag_update == NULL)
		return;
	self->funcs->drag_update(self->owner,
		gtk_widget_get_width(grid), gtk_widget_get_height(grid), x, y);
}

void
gsurf_wmux_grid_drag_end(GtkWidget *grid)
{
	GsurfWmuxGrid *self;

	g_return_if_fail(IS_GRID(grid));
	self = GRID(grid);
	if (!self->dragging)
		return;
	self->dragging = FALSE;
	if (self->owner != NULL && self->funcs->drag_end != NULL)
		self->funcs->drag_end(self->owner);
}

gint
gsurf_wmux_grid_hit_border(GtkWidget *grid, gint x, gint y)
{
	GsurfWmuxGrid *self;

	g_return_val_if_fail(IS_GRID(grid), GSURF_WMUX_GRID_BORDER_NONE);
	self = GRID(grid);
	if (self->owner == NULL || self->funcs->hit_border == NULL)
		return GSURF_WMUX_GRID_BORDER_NONE;
	return self->funcs->hit_border(self->owner,
		gtk_widget_get_width(grid), gtk_widget_get_height(grid), x, y);
}
