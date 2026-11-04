/*
 * gsurf-wmux-grid-gtk3.c - The wmux pane grid on GTK3
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A GtkContainer that places each child at its pane's rectangle (see
 * gsurf-wmux-grid.h). It has its own GdkWindow: the children's windows
 * become its children, so a press on a gap between panes lands on the
 * grid's window and can start a border drag, while presses on a pane go to
 * the pane's widgets as usual.
 */

#include "gsurf-wmux-grid.h"
#include "gsurf-wmux-layout.h"

/* Object data on a child: its pane id (GUINT_TO_POINTER(pane + 1), so 0
 * means "not a grid child"). */
#define PANE_DATA "gsurf-wmux-grid-pane"

typedef struct _GsurfWmuxGrid      GsurfWmuxGrid;
typedef struct _GsurfWmuxGridClass GsurfWmuxGridClass;

struct _GsurfWmuxGrid
{
	GtkContainer parent_instance;

	GPtrArray                *children; /* GtkWidget*, borrowed (the container's refs) */
	const GsurfWmuxGridFuncs *funcs;
	gpointer                  owner;    /* NULL: cut off from the module */
	gboolean                  dragging;
	gint                      cursor;   /* the border kind the cursor shows */
};

struct _GsurfWmuxGridClass
{
	GtkContainerClass parent_class;
};

static GType gsurf_wmux_grid_get_type(void);
G_DEFINE_TYPE(GsurfWmuxGrid, gsurf_wmux_grid, GTK_TYPE_CONTAINER)

#define GRID(obj) ((GsurfWmuxGrid *)(obj))
#define IS_GRID(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), gsurf_wmux_grid_get_type()))

/* ------------------------------------------------------------------ */
/* Children                                                            */
/* ------------------------------------------------------------------ */

static void
grid_add(GtkContainer *container, GtkWidget *child)
{
	/* A plain gtk_container_add() puts the child on pane 0. */
	gsurf_wmux_grid_put(GTK_WIDGET(container), child, 0);
}

static void
grid_remove(GtkContainer *container, GtkWidget *child)
{
	GsurfWmuxGrid *self = GRID(container);
	gboolean was_visible;

	if (!g_ptr_array_remove(self->children, child))
		return;
	was_visible = gtk_widget_get_visible(child);
	g_object_set_data(G_OBJECT(child), PANE_DATA, NULL);
	gtk_widget_unparent(child);
	if (was_visible && gtk_widget_get_visible(GTK_WIDGET(container)))
		gtk_widget_queue_resize(GTK_WIDGET(container));
}

static void
grid_forall(GtkContainer *container, gboolean include_internals,
            GtkCallback callback, gpointer callback_data)
{
	GsurfWmuxGrid *self = GRID(container);
	g_autoptr(GPtrArray) copy = NULL;
	guint i;

	(void)include_internals;
	/* The callback may remove children (destroy): walk a copy. */
	copy = g_ptr_array_copy(self->children, NULL, NULL);
	for (i = 0; i < copy->len; i++)
		callback(g_ptr_array_index(copy, i), callback_data);
}

static GType
grid_child_type(GtkContainer *container)
{
	(void)container;
	return GTK_TYPE_WIDGET;
}

/* ------------------------------------------------------------------ */
/* Window, size and drawing                                            */
/* ------------------------------------------------------------------ */

static void
grid_realize(GtkWidget *widget)
{
	GtkAllocation allocation;
	GdkWindowAttr attributes;
	GdkWindow *window;
	gint mask;

	/* An input-output child window at the grid's allocation; the
	 * children's windows are created inside it. */
	gtk_widget_set_realized(widget, TRUE);
	gtk_widget_get_allocation(widget, &allocation);
	attributes.window_type = GDK_WINDOW_CHILD;
	attributes.x = allocation.x;
	attributes.y = allocation.y;
	attributes.width = allocation.width;
	attributes.height = allocation.height;
	attributes.wclass = GDK_INPUT_OUTPUT;
	attributes.visual = gtk_widget_get_visual(widget);
	attributes.event_mask = gtk_widget_get_events(widget) | GDK_EXPOSURE_MASK |
		GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
		GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK;
	mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL;
	window = gdk_window_new(gtk_widget_get_parent_window(widget), &attributes, mask);
	gtk_widget_set_window(widget, window);
	gtk_widget_register_window(widget, window);
}

static void
grid_get_preferred_width(GtkWidget *widget, gint *minimum, gint *natural)
{
	(void)widget;
	/* The panes share whatever the window gives; the grid asks for
	 * nothing so it never forces the toplevel wider. */
	*minimum = 0;
	*natural = 0;
}

static void
grid_get_preferred_height(GtkWidget *widget, gint *minimum, gint *natural)
{
	(void)widget;
	*minimum = 0;
	*natural = 0;
}

/* The rectangle of @pane in @rects, or NULL. */
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
grid_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
	GsurfWmuxGrid *self = GRID(widget);
	g_autoptr(GArray) rects = NULL;
	guint i;

	/* 1. The grid's own allocation and window. */
	gtk_widget_set_allocation(widget, allocation);
	if (gtk_widget_get_realized(widget))
		gdk_window_move_resize(gtk_widget_get_window(widget),
			allocation->x, allocation->y, allocation->width, allocation->height);

	/* 2. The pane rectangles for this size, from the owner's model. */
	if (self->owner != NULL && self->funcs->geometry != NULL)
		rects = self->funcs->geometry(self->owner, allocation->width, allocation->height);

	/* 3. Each child at its rectangle (relative to the grid's window);
	 *    a child without one (another pane is zoomed) is not shown. GTK3
	 *    wants a size request before an allocation, and never less than
	 *    the minimum. */
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
		gtk_widget_size_allocate(child, &a);
	}
}

static gboolean
grid_draw(GtkWidget *widget, cairo_t *cr)
{
	GtkStyleContext *context = gtk_widget_get_style_context(widget);

	/* The gaps show the grid's CSS background; the children draw on top. */
	gtk_render_background(context, cr, 0, 0,
		gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget));
	return GTK_WIDGET_CLASS(gsurf_wmux_grid_parent_class)->draw(widget, cr);
}

/* ------------------------------------------------------------------ */
/* Pointer: border drags and the resize cursor                         */
/* ------------------------------------------------------------------ */

/* Show the resize cursor over a border, the default one elsewhere. */
static void
update_cursor(GsurfWmuxGrid *self, gint kind)
{
	GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(self));
	g_autoptr(GdkCursor) cursor = NULL;

	if (kind == self->cursor || window == NULL)
		return;
	self->cursor = kind;
	if (kind == GSURF_WMUX_GRID_BORDER_VERTICAL)
		cursor = gdk_cursor_new_from_name(gdk_window_get_display(window), "col-resize");
	else if (kind == GSURF_WMUX_GRID_BORDER_HORIZONTAL)
		cursor = gdk_cursor_new_from_name(gdk_window_get_display(window), "row-resize");
	gdk_window_set_cursor(window, cursor);
}

static gboolean
grid_button_press(GtkWidget *widget, GdkEventButton *event)
{
	/* Only presses on the grid's own window: on a gap. */
	if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_PRIMARY ||
	    event->window != gtk_widget_get_window(widget))
		return FALSE;
	return gsurf_wmux_grid_drag_begin(widget, (gint)event->x, (gint)event->y);
}

static gboolean
grid_motion_notify(GtkWidget *widget, GdkEventMotion *event)
{
	GsurfWmuxGrid *self = GRID(widget);

	if (event->window != gtk_widget_get_window(widget))
		return FALSE;
	if (self->dragging) {
		gsurf_wmux_grid_drag_update(widget, (gint)event->x, (gint)event->y);
		return TRUE;
	}
	update_cursor(self, gsurf_wmux_grid_hit_border(widget, (gint)event->x, (gint)event->y));
	return FALSE;
}

static gboolean
grid_button_release(GtkWidget *widget, GdkEventButton *event)
{
	GsurfWmuxGrid *self = GRID(widget);

	if (!self->dragging || event->button != GDK_BUTTON_PRIMARY)
		return FALSE;
	gsurf_wmux_grid_drag_end(widget);
	return TRUE;
}

static gboolean
grid_leave_notify(GtkWidget *widget, GdkEventCrossing *event)
{
	(void)event;
	if (!GRID(widget)->dragging)
		update_cursor(GRID(widget), GSURF_WMUX_GRID_BORDER_NONE);
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

static void
grid_finalize(GObject *object)
{
	GsurfWmuxGrid *self = GRID(object);

	g_clear_pointer(&self->children, g_ptr_array_unref);
	G_OBJECT_CLASS(gsurf_wmux_grid_parent_class)->finalize(object);
}

static void
gsurf_wmux_grid_class_init(GsurfWmuxGridClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	GtkContainerClass *container_class = GTK_CONTAINER_CLASS(klass);

	object_class->finalize = grid_finalize;
	widget_class->realize = grid_realize;
	widget_class->get_preferred_width = grid_get_preferred_width;
	widget_class->get_preferred_height = grid_get_preferred_height;
	widget_class->size_allocate = grid_size_allocate;
	widget_class->draw = grid_draw;
	widget_class->button_press_event = grid_button_press;
	widget_class->button_release_event = grid_button_release;
	widget_class->motion_notify_event = grid_motion_notify;
	widget_class->leave_notify_event = grid_leave_notify;
	container_class->add = grid_add;
	container_class->remove = grid_remove;
	container_class->forall = grid_forall;
	container_class->child_type = grid_child_type;
	gtk_widget_class_set_css_name(widget_class, "wmuxgrid");
}

static void
gsurf_wmux_grid_init(GsurfWmuxGrid *self)
{
	self->children = g_ptr_array_new();
	gtk_widget_set_has_window(GTK_WIDGET(self), TRUE);
	gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
	gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
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
	gtk_widget_queue_resize(grid);
}

void
gsurf_wmux_grid_put(GtkWidget *grid, GtkWidget *child, guint pane)
{
	GsurfWmuxGrid *self;

	g_return_if_fail(IS_GRID(grid));
	g_return_if_fail(GTK_IS_WIDGET(child));
	self = GRID(grid);

	/* Retag a child we already hold; adopt a parentless one. */
	g_object_set_data(G_OBJECT(child), PANE_DATA, GUINT_TO_POINTER(pane + 1));
	if (gtk_widget_get_parent(child) == grid) {
		gtk_widget_queue_resize(grid);
		return;
	}
	g_return_if_fail(gtk_widget_get_parent(child) == NULL);
	g_ptr_array_add(self->children, child);
	gtk_widget_set_parent(child, grid);
	gtk_widget_queue_resize(grid);
}

void
gsurf_wmux_grid_remove(GtkWidget *grid, GtkWidget *child)
{
	g_return_if_fail(IS_GRID(grid));
	if (child != NULL && gtk_widget_get_parent(child) == grid)
		gtk_container_remove(GTK_CONTAINER(grid), child);
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
	gtk_widget_queue_resize(grid);
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
		gtk_widget_get_allocated_width(grid), gtk_widget_get_allocated_height(grid), x, y);
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
		gtk_widget_get_allocated_width(grid), gtk_widget_get_allocated_height(grid), x, y);
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
		gtk_widget_get_allocated_width(grid), gtk_widget_get_allocated_height(grid), x, y);
}
