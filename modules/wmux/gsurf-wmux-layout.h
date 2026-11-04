/*
 * gsurf-wmux-layout.h - The wmux pane layout model and window state
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A GsurfWmuxLayout is one tmux window's pane arrangement: an n-ary tree
 * whose leaves are panes and whose inner cells are splits. A split lays
 * its children out side by side ('h', left to right) or stacked ('v', top
 * to bottom); each child has an integer weight and the weights of a
 * split's children always sum to exactly GSURF_WMUX_LAYOUT_SCALE, so
 * there is no floating-point drift. The tree is kept normalized: a split
 * has at least two children, and a split never directly contains a split
 * of the same direction (that would be one split; it is flattened).
 *
 * Panes have small ids that are stable for the pane's lifetime and never
 * reused within a layout, an index (their position in layout order: the
 * left-to-right, top-to-bottom order of the leaves), and an opaque
 * guint64 payload (the module stores the shown view's id there). The
 * layout also tracks the active pane, the last (previously active) pane,
 * a most-recently-used stamp per pane, and the zoomed pane.
 *
 * Geometry is computed on demand for any pixel size and gap; it tiles the
 * area exactly (integer arithmetic with cumulative rounding).
 *
 * The second half of this header is the window *state*: a set of tmux
 * windows (name, renamed flag, layout with its active/last/zoom and
 * per-pane payloads) and the current/last window, serialized to one
 * string for the session module and parsed back strictly.
 *
 * Pure GLib, module-private (hidden symbols), compiled straight into
 * tests/test-wmux-layout.c. The grammars are in docs/wmux.org.
 */

#ifndef GSURF_WMUX_LAYOUT_H
#define GSURF_WMUX_LAYOUT_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GSURF_WMUX_LAYOUT_SCALE:
 *
 * The sum of the weights of a split's children (100.00%).
 */
#define GSURF_WMUX_LAYOUT_SCALE (10000u)

/**
 * GSURF_WMUX_PANE_NONE:
 *
 * "No pane". Pane ids start at 0, so this is %G_MAXUINT.
 */
#define GSURF_WMUX_PANE_NONE (G_MAXUINT)

/**
 * GSURF_WMUX_LAYOUT_MAX_DEPTH:
 *
 * The deepest split nesting the parser accepts.
 */
#define GSURF_WMUX_LAYOUT_MAX_DEPTH (64)

/**
 * GSURF_WMUX_LAYOUT_ERROR:
 *
 * Error domain for layout operations and parsing.
 */
#define GSURF_WMUX_LAYOUT_ERROR (gsurf_wmux_layout_error_quark())

/**
 * GsurfWmuxLayoutError:
 * @GSURF_WMUX_LAYOUT_ERROR_NOT_FOUND: no such pane
 * @GSURF_WMUX_LAYOUT_ERROR_LAST_PANE: the last pane cannot be removed
 * @GSURF_WMUX_LAYOUT_ERROR_NO_SPACE: a split would make a pane smaller
 *   than the minimum pane size
 * @GSURF_WMUX_LAYOUT_ERROR_INVALID: an argument is out of range
 * @GSURF_WMUX_LAYOUT_ERROR_IDS_EXHAUSTED: no pane id is left
 * @GSURF_WMUX_LAYOUT_ERROR_PARSE: a layout or state string is malformed;
 *   the message starts with "position N: "
 *
 * Error codes in %GSURF_WMUX_LAYOUT_ERROR.
 */
typedef enum {
	GSURF_WMUX_LAYOUT_ERROR_NOT_FOUND,
	GSURF_WMUX_LAYOUT_ERROR_LAST_PANE,
	GSURF_WMUX_LAYOUT_ERROR_NO_SPACE,
	GSURF_WMUX_LAYOUT_ERROR_INVALID,
	GSURF_WMUX_LAYOUT_ERROR_IDS_EXHAUSTED,
	GSURF_WMUX_LAYOUT_ERROR_PARSE
} GsurfWmuxLayoutError;

G_GNUC_INTERNAL GQuark gsurf_wmux_layout_error_quark(void);

/**
 * GsurfWmuxSplit:
 * @GSURF_WMUX_SPLIT_H: children side by side, left to right (tmux
 *   split-window -h); serialized 'h'
 * @GSURF_WMUX_SPLIT_V: children stacked, top to bottom (tmux
 *   split-window -v); serialized 'v'
 *
 * The direction of a split.
 */
typedef enum {
	GSURF_WMUX_SPLIT_H,
	GSURF_WMUX_SPLIT_V
} GsurfWmuxSplit;

/**
 * GsurfWmuxDirection:
 * @GSURF_WMUX_DIR_LEFT: left (-L)
 * @GSURF_WMUX_DIR_RIGHT: right (-R)
 * @GSURF_WMUX_DIR_UP: up (-U)
 * @GSURF_WMUX_DIR_DOWN: down (-D)
 *
 * A direction for neighbor and resize.
 */
typedef enum {
	GSURF_WMUX_DIR_LEFT,
	GSURF_WMUX_DIR_RIGHT,
	GSURF_WMUX_DIR_UP,
	GSURF_WMUX_DIR_DOWN
} GsurfWmuxDirection;

/**
 * GsurfWmuxPreset:
 * @GSURF_WMUX_PRESET_EVEN_HORIZONTAL: all panes side by side, equal
 * @GSURF_WMUX_PRESET_EVEN_VERTICAL: all panes stacked, equal
 * @GSURF_WMUX_PRESET_MAIN_HORIZONTAL: the first pane across the top
 *   (main_pct of the height), the others side by side below
 * @GSURF_WMUX_PRESET_MAIN_VERTICAL: the first pane on the left (main_pct
 *   of the width), the others stacked on the right
 * @GSURF_WMUX_PRESET_TILED: a grid of ceil(sqrt(n)) columns; rows with
 *   fewer panes (the last) share their row equally
 * @GSURF_WMUX_PRESET_COUNT: the number of presets (not a preset)
 *
 * The tmux preset layouts, in next-layout order.
 */
typedef enum {
	GSURF_WMUX_PRESET_EVEN_HORIZONTAL,
	GSURF_WMUX_PRESET_EVEN_VERTICAL,
	GSURF_WMUX_PRESET_MAIN_HORIZONTAL,
	GSURF_WMUX_PRESET_MAIN_VERTICAL,
	GSURF_WMUX_PRESET_TILED,
	GSURF_WMUX_PRESET_COUNT
} GsurfWmuxPreset;

/**
 * GsurfWmuxRect:
 * @pane: the pane id
 * @x: left edge, in pixels
 * @y: top edge, in pixels
 * @width: width, in pixels (may be 0 for a tiny area)
 * @height: height, in pixels (may be 0 for a tiny area)
 *
 * A pane's allocation from gsurf_wmux_layout_geometry().
 */
typedef struct {
	guint pane;
	gint  x;
	gint  y;
	gint  width;
	gint  height;
} GsurfWmuxRect;

/**
 * GsurfWmuxBorder:
 * @before: a pane touching the border from the left (or top)
 * @after: a pane touching the border from the right (or bottom)
 * @split: %GSURF_WMUX_SPLIT_H for a vertical line between columns,
 *   %GSURF_WMUX_SPLIT_V for a horizontal line between rows
 * @position: the border's pixel coordinate along the split axis (the
 *   first pixel after the @before side's cell)
 *
 * A draggable boundary between two cells of one split, as found by
 * gsurf_wmux_layout_find_border(). It names the split by two panes on
 * either side, so it stays valid while the layout keeps that split.
 */
typedef struct {
	guint          before;
	guint          after;
	GsurfWmuxSplit split;
	gint           position;
} GsurfWmuxBorder;

typedef struct _GsurfWmuxLayout GsurfWmuxLayout;

/* --- Life cycle --- */

/* A layout with one pane (id 0), active. min_pct 10, main_pct 60. */
G_GNUC_INTERNAL GsurfWmuxLayout *gsurf_wmux_layout_new(void);
/* A deep copy: tree, ids, payloads, active/last/zoom, stamps, settings. */
G_GNUC_INTERNAL GsurfWmuxLayout *gsurf_wmux_layout_copy(GsurfWmuxLayout *layout);
G_GNUC_INTERNAL void             gsurf_wmux_layout_free(GsurfWmuxLayout *layout);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfWmuxLayout, gsurf_wmux_layout_free)

/* --- Settings --- */

/* Minimum pane size for split and resize, in percent of the window's
 * width (for h) or height (for v), 0..50 (clamped). Presets and parsed
 * layouts are not held to it. */
G_GNUC_INTERNAL void  gsurf_wmux_layout_set_min_pct(GsurfWmuxLayout *layout, guint pct);
G_GNUC_INTERNAL guint gsurf_wmux_layout_get_min_pct(GsurfWmuxLayout *layout);
/* Size of the main pane in the main-* presets, percent 10..90 (clamped). */
G_GNUC_INTERNAL void  gsurf_wmux_layout_set_main_pct(GsurfWmuxLayout *layout, guint pct);
G_GNUC_INTERNAL guint gsurf_wmux_layout_get_main_pct(GsurfWmuxLayout *layout);

/* --- Panes --- */

G_GNUC_INTERNAL guint    gsurf_wmux_layout_get_n_panes(GsurfWmuxLayout *layout);
/* The pane at @index in layout order, GSURF_WMUX_PANE_NONE when out of range. */
G_GNUC_INTERNAL guint    gsurf_wmux_layout_get_nth_pane(GsurfWmuxLayout *layout, guint index);
/* The index of @pane in layout order, -1 when absent. */
G_GNUC_INTERNAL gint     gsurf_wmux_layout_get_pane_index(GsurfWmuxLayout *layout, guint pane);
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_has_pane(GsurfWmuxLayout *layout, guint pane);
/* All pane ids in layout order (transfer full, element-type guint). */
G_GNUC_INTERNAL GArray  *gsurf_wmux_layout_list_panes(GsurfWmuxLayout *layout);

/* Per-pane payload (0 for a new pane). set returns FALSE for an unknown
 * pane; get returns 0. find returns the first pane in layout order with
 * that payload, or GSURF_WMUX_PANE_NONE. */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_set_payload(GsurfWmuxLayout *layout, guint pane,
                                                       guint64 payload);
G_GNUC_INTERNAL guint64  gsurf_wmux_layout_get_payload(GsurfWmuxLayout *layout, guint pane);
G_GNUC_INTERNAL guint    gsurf_wmux_layout_find_payload(GsurfWmuxLayout *layout,
                                                        guint64 payload);

/* --- Active, last, most recently used --- */

G_GNUC_INTERNAL guint    gsurf_wmux_layout_get_active(GsurfWmuxLayout *layout);
/**
 * gsurf_wmux_layout_set_active:
 * @layout: a layout
 * @pane: a pane
 *
 * Makes @pane active and the most recently used; the previously active
 * pane becomes the last pane. Selecting another pane while zoomed
 * unzooms (as in tmux).
 *
 * Returns: %FALSE when @pane is unknown
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_set_active(GsurfWmuxLayout *layout, guint pane);
/* The previously active pane (tmux last-pane), or GSURF_WMUX_PANE_NONE. */
G_GNUC_INTERNAL guint    gsurf_wmux_layout_get_last(GsurfWmuxLayout *layout);
/* Set the last pane directly (state restore); FALSE when unknown or active. */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_set_last(GsurfWmuxLayout *layout, guint pane);
/* The pane's use stamp: higher is more recent; 0 = never active. */
G_GNUC_INTERNAL guint64  gsurf_wmux_layout_get_stamp(GsurfWmuxLayout *layout, guint pane);

/* --- Structure --- */

/**
 * gsurf_wmux_layout_split:
 * @layout: a layout
 * @pane: the pane to split
 * @split: %GSURF_WMUX_SPLIT_H (new pane beside) or %GSURF_WMUX_SPLIT_V
 *   (new pane below/above)
 * @before: put the new pane left of / above @pane (tmux -b)
 * @pct: the new pane's share of @pane's cell, 1..99 (tmux -p)
 * @error: return location for a #GError
 *
 * Splits @pane's cell in two. The new pane gets a fresh id and payload 0;
 * the active pane does not change (the caller selects the new pane unless
 * it was split-window -d). Unzooms.
 *
 * Returns: the new pane's id, or %GSURF_WMUX_PANE_NONE with @error set
 *   (unknown pane, @pct out of range, either pane would be smaller than
 *   min_pct, ids exhausted)
 */
G_GNUC_INTERNAL guint gsurf_wmux_layout_split(GsurfWmuxLayout *layout, guint pane,
                                              GsurfWmuxSplit split, gboolean before,
                                              guint pct, GError **error);

/**
 * gsurf_wmux_layout_remove:
 * @layout: a layout
 * @pane: the pane to remove
 * @error: return location for a #GError
 *
 * Removes @pane; its space goes to the cell before it in its split (or
 * after it, when it was first). A split left with one child is replaced
 * by that child, and same-direction nesting is flattened. When @pane was
 * active, the last pane becomes active, else the nearest pane of the cell
 * that received the space. Unzooms.
 *
 * Returns: %FALSE for an unknown pane or the last pane
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_remove(GsurfWmuxLayout *layout, guint pane,
                                                  GError **error);

/**
 * gsurf_wmux_layout_neighbor:
 * @layout: a layout
 * @pane: the pane to move from
 * @direction: where to look
 * @wrap: when nothing is adjacent in @direction, wrap to the far edge
 *   (tmux behaviour)
 *
 * The pane next to @pane in @direction, by the zoom-less geometry on the
 * unit square: of the panes whose opposite edge touches @pane's edge and
 * that overlap it across, the one with the most overlap; ties go to the
 * most recently used, then to layout order.
 *
 * Returns: the neighbor, or %GSURF_WMUX_PANE_NONE
 */
G_GNUC_INTERNAL guint gsurf_wmux_layout_neighbor(GsurfWmuxLayout *layout, guint pane,
                                                 GsurfWmuxDirection direction,
                                                 gboolean wrap);

/* The pane after / before @pane in layout order, wrapping; NONE when
 * unknown. */
G_GNUC_INTERNAL guint gsurf_wmux_layout_next(GsurfWmuxLayout *layout, guint pane);
G_GNUC_INTERNAL guint gsurf_wmux_layout_prev(GsurfWmuxLayout *layout, guint pane);

/**
 * gsurf_wmux_layout_swap:
 * @layout: a layout
 * @a: a pane
 * @b: a pane
 *
 * Swaps the places of two panes; ids, payloads and stamps move with the
 * panes (the active pane stays the same pane, now elsewhere). Unzooms.
 *
 * Returns: %FALSE when either pane is unknown
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_swap(GsurfWmuxLayout *layout, guint a, guint b);

/**
 * gsurf_wmux_layout_rotate:
 * @layout: a layout
 * @down: %FALSE (tmux default, -U): every pane moves one place earlier in
 *   layout order and the first goes last; %TRUE (-D): the reverse
 *
 * Rotates the panes through the places of the layout. Unzooms.
 */
G_GNUC_INTERNAL void gsurf_wmux_layout_rotate(GsurfWmuxLayout *layout, gboolean down);

/**
 * gsurf_wmux_layout_resize:
 * @layout: a layout
 * @pane: the pane
 * @direction: the way the border moves
 * @pct: how far, in percent of the window's width (L/R) or height (U/D)
 *
 * Moves a border of @pane like tmux resize-pane: -R/-D move @pane's
 * right/bottom border (or its left/top border, when @pane is last in that
 * direction), -L/-U the left/top border (or right/bottom, when first).
 * The move is clamped so no pane on the shrinking side gets smaller than
 * min_pct. Unzooms.
 *
 * Returns: %TRUE when a border moved
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_resize(GsurfWmuxLayout *layout, guint pane,
                                                  GsurfWmuxDirection direction, guint pct);

/**
 * gsurf_wmux_layout_spread_evenly:
 * @layout: a layout
 * @pane: a pane
 *
 * Gives every child of @pane's split an equal share (tmux select-layout
 * -E). Unzooms.
 *
 * Returns: %FALSE when @pane is unknown or the only pane
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_spread_evenly(GsurfWmuxLayout *layout, guint pane);

/* --- Zoom --- */

/* Zoom the active pane, or unzoom; zooming needs two or more panes.
 * Returns whether the layout is zoomed afterwards. */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_toggle_zoom(GsurfWmuxLayout *layout);
/* Zoom @pane (NONE unzooms); FALSE when unknown. Does not change active. */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_set_zoomed(GsurfWmuxLayout *layout, guint pane);
/* The zoomed pane, or GSURF_WMUX_PANE_NONE. */
G_GNUC_INTERNAL guint    gsurf_wmux_layout_get_zoomed(GsurfWmuxLayout *layout);

/* --- Presets --- */

/* Rebuilds the tree as @preset from the current pane order (ids, payloads
 * and the active pane are kept). Unzooms. */
G_GNUC_INTERNAL void            gsurf_wmux_layout_apply_preset(GsurfWmuxLayout *layout,
                                                               GsurfWmuxPreset preset);
/* Applies the preset after the last one applied (even-horizontal first)
 * and returns it. */
G_GNUC_INTERNAL GsurfWmuxPreset gsurf_wmux_layout_next_preset(GsurfWmuxLayout *layout);
/* The last preset applied, or -1. */
G_GNUC_INTERNAL gint            gsurf_wmux_layout_get_preset(GsurfWmuxLayout *layout);
G_GNUC_INTERNAL const gchar    *gsurf_wmux_preset_to_string(GsurfWmuxPreset preset);
G_GNUC_INTERNAL gboolean        gsurf_wmux_preset_from_string(const gchar *name,
                                                              GsurfWmuxPreset *preset);

/* --- Geometry --- */

/**
 * gsurf_wmux_layout_geometry:
 * @layout: a layout
 * @width: the area's width in pixels (>= 0)
 * @height: the area's height in pixels (>= 0)
 * @gap: pixels between neighbouring cells (>= 0; reduced where the area
 *   is too small to hold the gaps)
 *
 * Allocates every pane, in layout order; when zoomed, only the zoomed
 * pane, filling the area. The rectangles never overlap, stay inside the
 * area, and cover all of it except the gaps between cells.
 *
 * Returns: (transfer full) (element-type GsurfWmuxRect): the rectangles
 */
G_GNUC_INTERNAL GArray *gsurf_wmux_layout_geometry(GsurfWmuxLayout *layout,
                                                   gint width, gint height, gint gap);

/**
 * gsurf_wmux_layout_find_border:
 * @layout: a layout
 * @width: area width, as for geometry()
 * @height: area height
 * @gap: gap, as for geometry()
 * @x: pointer x
 * @y: pointer y
 * @border: (out): the border found
 *
 * Hit-tests the boundaries between cells: a point on a gap (or, with gap
 * < 4, within 2 pixels of the boundary) between two children of a split.
 * Ignored while zoomed.
 *
 * Returns: %TRUE when (@x, @y) is on a border
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_find_border(GsurfWmuxLayout *layout,
                                                       gint width, gint height, gint gap,
                                                       gint x, gint y,
                                                       GsurfWmuxBorder *border);

/**
 * gsurf_wmux_layout_move_border:
 * @layout: a layout
 * @border: a border from find_border()
 * @width: area width
 * @height: area height
 * @gap: gap
 * @position: where the border should be (pixel coordinate along the split
 *   axis, like @border->position)
 *
 * Drags a border, clamped like resize() so no pane on the shrinking side
 * gets smaller than min_pct.
 *
 * Returns: %TRUE when the border moved; %FALSE when it did not or the
 *   border no longer exists
 */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_move_border(GsurfWmuxLayout *layout,
                                                       const GsurfWmuxBorder *border,
                                                       gint width, gint height, gint gap,
                                                       gint position);

/* --- Serialization --- */

/**
 * gsurf_wmux_layout_serialize:
 * @layout: a layout
 * @with_payload: append "=payload" to each pane
 *
 * The layout string, e.g. "v(50:p0,50:h(50:p1,50:p2))". Weights are
 * percentages with up to two decimals that sum to exactly 100 in each
 * split. Active, last and zoom are not part of it (see the state).
 *
 * Returns: (transfer full): the string
 */
G_GNUC_INTERNAL gchar *gsurf_wmux_layout_serialize(GsurfWmuxLayout *layout,
                                                   gboolean with_payload);

/**
 * gsurf_wmux_layout_parse:
 * @text: a layout string
 * @error_pos: (out) (optional): the byte offset of the error, -1 on success
 * @error: return location for a #GError
 *
 * Parses a layout string strictly: the grammar of docs/wmux.org, unique
 * pane ids, positive weights (relative: they are scaled to sum to 100 in
 * each split), at least two children per split, nesting no deeper than
 * %GSURF_WMUX_LAYOUT_MAX_DEPTH, nothing after the layout. Whitespace is
 * allowed between tokens. The first pane is active; the next new pane id
 * is one more than the largest.
 *
 * Returns: (transfer full) (nullable): the layout, or %NULL with @error
 */
G_GNUC_INTERNAL GsurfWmuxLayout *gsurf_wmux_layout_parse(const gchar *text, gint *error_pos,
                                                         GError **error);

/* Check every structural invariant (weights sum to SCALE, >= 2 children
 * per split, no same-direction nesting, unique ids below next id, parent
 * links, active/last/zoom valid). Describes the first violation in
 * @problem. For tests and debug builds. */
G_GNUC_INTERNAL gboolean gsurf_wmux_layout_check(GsurfWmuxLayout *layout, gchar **problem);

/* ------------------------------------------------------------------ */
/* Window state (a set of tmux windows) for session persistence        */
/* ------------------------------------------------------------------ */

/**
 * GsurfWmuxWindowState:
 * @name: the window name ("" for none)
 * @renamed: %TRUE when the user named it (automatic naming is off)
 * @layout: the window's layout, with active, last, zoom and payloads
 *
 * One tmux window in a #GsurfWmuxState.
 */
typedef struct {
	gchar           *name;
	gboolean         renamed;
	GsurfWmuxLayout *layout;
} GsurfWmuxWindowState;

/**
 * GsurfWmuxState:
 * @windows: (element-type GsurfWmuxWindowState): the windows, in order
 * @current: index of the current window
 * @last: index of the last window, or -1
 *
 * All tmux windows of one browser window. The session module maps view
 * ids to saved tab indices in the pane payloads before serializing, and
 * back after parsing.
 */
typedef struct {
	GPtrArray *windows;
	gint       current;
	gint       last;
} GsurfWmuxState;

G_GNUC_INTERNAL GsurfWmuxState *gsurf_wmux_state_new(void);
G_GNUC_INTERNAL void            gsurf_wmux_state_free(GsurfWmuxState *state);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfWmuxState, gsurf_wmux_state_free)

/* Appends a window (takes @layout) and returns its index. */
G_GNUC_INTERNAL guint gsurf_wmux_state_add_window(GsurfWmuxState *state, const gchar *name,
                                                  gboolean renamed, GsurfWmuxLayout *layout);

/**
 * gsurf_wmux_state_serialize:
 * @state: a state with at least one window
 *
 * One line: "wmux1 current=0 last=1 [name=\"web\" renamed=1 active=2
 * last=0 zoom=2 layout=h(50:p0=0,50:p2=1)] [...]". last= and zoom= are
 * left out when unset.
 *
 * Returns: (transfer full): the state string
 */
G_GNUC_INTERNAL gchar *gsurf_wmux_state_serialize(GsurfWmuxState *state);

/**
 * gsurf_wmux_state_parse:
 * @text: a state string
 * @error_pos: (out) (optional): the byte offset of the error, -1 on success
 * @error: return location for a #GError
 *
 * Parses a state string strictly: the "wmux1" version tag, at least one
 * window, each field at most once, layout= required, active/last/zoom
 * naming panes of that window's layout, current/last naming windows.
 *
 * Returns: (transfer full) (nullable): the state, or %NULL with @error
 */
G_GNUC_INTERNAL GsurfWmuxState *gsurf_wmux_state_parse(const gchar *text, gint *error_pos,
                                                       GError **error);

G_END_DECLS

#endif /* GSURF_WMUX_LAYOUT_H */
