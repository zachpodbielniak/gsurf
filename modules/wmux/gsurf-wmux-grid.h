/*
 * gsurf-wmux-grid.h - The wmux pane grid (one custom container per toolkit)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The grid is the widget that shows the panes of the current tmux window.
 * Each child is a pane frame tagged with its pane id; the grid asks the
 * module for the pane rectangles of its current size (the layout model's
 * gsurf_wmux_layout_geometry()) and allocates every child to its pane's
 * rectangle. A child whose pane has no rectangle (another pane is zoomed)
 * is kept but not shown (child-visible off), so zooming never reparents a
 * web view; the owner parks the frames of off-screen views the same way
 * (GSURF_WMUX_GRID_PARKED).
 *
 * The grid also turns a mouse drag on the gap between two panes into a
 * border move: a press on a border starts a drag, motion moves it, the
 * release ends it. The hit test and the move are the module's (the model's
 * find_border()/move_border()); the grid only reports pixel positions in
 * its own coordinates. The same entry points are public, so a test can
 * drive a drag without synthesizing pointer events.
 *
 * Two implementations share this header and are selected by GTK_BACKEND in
 * the module Makefile: gsurf-wmux-grid-gtk3.c (a GtkContainer with its own
 * GdkWindow, so presses on the gaps reach it) and gsurf-wmux-grid-gtk4.c (a
 * GtkWidget with a GtkGestureDrag). Module-private (hidden symbols).
 */

#ifndef GSURF_WMUX_GRID_H
#define GSURF_WMUX_GRID_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * GSURF_WMUX_GRID_BORDER_NONE:
 * gsurf_wmux_grid_hit_border(): the point is not on a border.
 */
#define GSURF_WMUX_GRID_BORDER_NONE       (0)

/**
 * GSURF_WMUX_GRID_BORDER_VERTICAL:
 * A vertical line between two columns (drag left and right).
 */
#define GSURF_WMUX_GRID_BORDER_VERTICAL   (1)

/**
 * GSURF_WMUX_GRID_BORDER_HORIZONTAL:
 * A horizontal line between two rows (drag up and down).
 */
#define GSURF_WMUX_GRID_BORDER_HORIZONTAL (2)

/**
 * GSURF_WMUX_GRID_PARKED:
 * The pane tag of a parked child: kept in the grid, never shown (no pane
 * id reaches this value).
 */
#define GSURF_WMUX_GRID_PARKED (G_MAXUINT - 1)

/**
 * GsurfWmuxGridFuncs:
 * @geometry: the pane rectangles (#GsurfWmuxRect, transfer full) for an
 *   area of @width x @height pixels, or %NULL for none
 * @hit_border: which kind of border (GSURF_WMUX_GRID_BORDER_*) lies under
 *   (@x, @y), for the pointer cursor
 * @drag_begin: a press at (@x, @y): %TRUE when it starts a border drag
 * @drag_update: the pointer moved to (@x, @y) during a drag
 * @drag_end: the drag ended
 *
 * What the grid asks of its owner. Every callback gets the data passed to
 * gsurf_wmux_grid_new() and the grid's current size.
 */
typedef struct {
	GArray  *(*geometry)    (gpointer data, gint width, gint height);
	gint     (*hit_border)  (gpointer data, gint width, gint height, gint x, gint y);
	gboolean (*drag_begin)  (gpointer data, gint width, gint height, gint x, gint y);
	void     (*drag_update) (gpointer data, gint width, gint height, gint x, gint y);
	void     (*drag_end)    (gpointer data);
} GsurfWmuxGridFuncs;

/**
 * gsurf_wmux_grid_new:
 * @funcs: (not nullable): the owner's callbacks; must outlive the grid or
 *   be cut off with gsurf_wmux_grid_set_owner() first
 * @data: the owner, passed to every callback
 *
 * Returns: (transfer floating): a new, empty grid that expands both ways
 */
G_GNUC_INTERNAL GtkWidget *gsurf_wmux_grid_new(const GsurfWmuxGridFuncs *funcs, gpointer data);

/**
 * gsurf_wmux_grid_set_owner:
 * @grid: a grid
 * @data: (nullable): the new owner data; %NULL disconnects the grid from
 *   its owner (no callback runs any more; the grid allocates nothing)
 */
G_GNUC_INTERNAL void       gsurf_wmux_grid_set_owner(GtkWidget *grid, gpointer data);

/**
 * gsurf_wmux_grid_put:
 * @grid: a grid
 * @child: a widget without a parent, or a child of @grid
 * @pane: the pane @child shows
 *
 * Adds @child for @pane, or retags an existing child with another pane
 * (swap, rotate). Queues a new allocation.
 */
G_GNUC_INTERNAL void       gsurf_wmux_grid_put(GtkWidget *grid, GtkWidget *child, guint pane);

/**
 * gsurf_wmux_grid_remove:
 * @grid: a grid
 * @child: a child of @grid (anything else is ignored)
 *
 * Removes @child from the grid. A caller that wants to keep @child holds
 * its own reference first.
 */
G_GNUC_INTERNAL void       gsurf_wmux_grid_remove(GtkWidget *grid, GtkWidget *child);

/**
 * gsurf_wmux_grid_get_children:
 * @grid: a grid
 *
 * Returns: (transfer container) (element-type GtkWidget): the children,
 *   in the order they were added
 */
G_GNUC_INTERNAL GPtrArray *gsurf_wmux_grid_get_children(GtkWidget *grid);

/**
 * gsurf_wmux_grid_get_pane:
 * @child: a child of a grid
 *
 * Returns: the pane @child shows (G_MAXUINT when it is not a grid child)
 */
G_GNUC_INTERNAL guint      gsurf_wmux_grid_get_pane(GtkWidget *child);

/**
 * gsurf_wmux_grid_relayout:
 * @grid: a grid
 *
 * The geometry changed (a split, a resize, a zoom): allocate again.
 */
G_GNUC_INTERNAL void       gsurf_wmux_grid_relayout(GtkWidget *grid);

/* Border dragging, in grid coordinates. The toolkit's pointer handlers
 * call these; so may tests. drag_begin returns whether a drag started
 * (the point is on a border and the owner allows it). */
G_GNUC_INTERNAL gboolean   gsurf_wmux_grid_drag_begin(GtkWidget *grid, gint x, gint y);
G_GNUC_INTERNAL void       gsurf_wmux_grid_drag_update(GtkWidget *grid, gint x, gint y);
G_GNUC_INTERNAL void       gsurf_wmux_grid_drag_end(GtkWidget *grid);

/* The kind of border under (@x, @y) (GSURF_WMUX_GRID_BORDER_*). */
G_GNUC_INTERNAL gint       gsurf_wmux_grid_hit_border(GtkWidget *grid, gint x, gint y);

G_END_DECLS

#endif /* GSURF_WMUX_GRID_H */
