/*
 * gsurf-tab-tree.h - Toolkit-neutral tab tree model for the tabs module
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A GsurfTabTree mirrors one window's view order (a flat list of view
 * ids) and derives a tree from it: every tab has a logical parent, which
 * starts as its opener (gsurf_view_get_opener_id()). The parent only
 * counts while it precedes the tab and the tab lies inside the parent's
 * contiguous block of descendants, so the visible tree always agrees with
 * the window order. See docs/tabs.org for the exact rules.
 *
 * The model knows nothing about GTK or GsurfView; the module feeds it
 * ids. It is module-private (built into tabs.so, hidden symbols) and is
 * compiled straight into tests/test-tab-tree.c.
 */

#ifndef GSURF_TAB_TREE_H
#define GSURF_TAB_TREE_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GsurfTabTreeClosePolicy:
 * @GSURF_TAB_TREE_CLOSE_PROMOTE: the closed tab's children move up one
 *   level (to the closed tab's parent, or to the root), keeping order
 * @GSURF_TAB_TREE_CLOSE_CHILDREN: the closed tab's whole subtree closes
 *   with it
 *
 * What happens to the children of a tab that closes.
 */
typedef enum {
	GSURF_TAB_TREE_CLOSE_PROMOTE,
	GSURF_TAB_TREE_CLOSE_CHILDREN
} GsurfTabTreeClosePolicy;

typedef struct _GsurfTabTree GsurfTabTree;

G_GNUC_INTERNAL GsurfTabTree *gsurf_tab_tree_new(void);
G_GNUC_INTERNAL void          gsurf_tab_tree_free(GsurfTabTree *tree);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfTabTree, gsurf_tab_tree_free)

/* --- Order --- */

/* Number of tabs; the id at @index (0 when out of range); the index of
 * @id (-1 when absent); whether @id is in the tree. */
G_GNUC_INTERNAL guint    gsurf_tab_tree_get_n_tabs(GsurfTabTree *tree);
G_GNUC_INTERNAL guint64  gsurf_tab_tree_get_nth(GsurfTabTree *tree, guint index);
G_GNUC_INTERNAL gint     gsurf_tab_tree_get_index(GsurfTabTree *tree, guint64 id);
G_GNUC_INTERNAL gboolean gsurf_tab_tree_contains(GsurfTabTree *tree, guint64 id);

/* --- Structure --- */

/*
 * gsurf_tab_tree_add:
 * @id: the new tab's view id (non-zero)
 * @opener_id: its opener's view id, 0 for none
 * @index: where the window put it
 *
 * Adds a tab. When @opener_id is in the tree the tab becomes the opener's
 * last child: it is placed right after the opener's last descendant.
 * Otherwise it is a root at @index (clamped to the end). Adding an id
 * that is already present changes nothing.
 *
 * Returns: the tab's index in the tree's order (the caller moves the
 * window's view there)
 */
G_GNUC_INTERNAL guint    gsurf_tab_tree_add(GsurfTabTree *tree, guint64 id,
                                            guint64 opener_id, guint index);

/*
 * gsurf_tab_tree_remove:
 * @closed: (nullable) (element-type guint64): receives, for
 *   %GSURF_TAB_TREE_CLOSE_CHILDREN, the ids of the removed tab's
 *   descendants in order
 *
 * Removes a tab. With %GSURF_TAB_TREE_CLOSE_PROMOTE its direct children
 * are re-parented to its parent (or become roots). With
 * %GSURF_TAB_TREE_CLOSE_CHILDREN its descendants are removed from the
 * tree too and listed in @closed for the caller to close. Unknown ids
 * are ignored.
 */
G_GNUC_INTERNAL void     gsurf_tab_tree_remove(GsurfTabTree *tree, guint64 id,
                                               GsurfTabTreeClosePolicy policy,
                                               GArray *closed);

/*
 * gsurf_tab_tree_replace:
 * @old_id: a tab in the tree
 * @new_id: its replacement (in the tree or not)
 *
 * Hands @old_id's place to @new_id: @new_id leaves its own position (if
 * it was in the tree) and takes @old_id's position, parent, children and
 * collapse state; @old_id leaves the tree. Used when a tab is reopened
 * (another container). Returns %FALSE, changing nothing, when @old_id is
 * unknown or the ids are equal or zero.
 */
G_GNUC_INTERNAL gboolean gsurf_tab_tree_replace(GsurfTabTree *tree, guint64 old_id,
                                                guint64 new_id);

/*
 * gsurf_tab_tree_sync:
 * @ids: (array length=n): the window's view ids in order
 * @opener_ids: (array length=n): each view's opener id (0 = none)
 *
 * Makes the tree's order exactly @ids: tabs not listed are dropped, new
 * ones are added (with their opener as parent, without being moved), and
 * existing tabs keep their parent and collapse state. The tree is then
 * re-derived from the new order. Duplicate and zero ids are skipped.
 */
G_GNUC_INTERNAL void     gsurf_tab_tree_sync(GsurfTabTree *tree, const guint64 *ids,
                                             const guint64 *opener_ids, guint n);

/*
 * gsurf_tab_tree_move_subtree:
 * @direction: negative to move up, positive to move down
 *
 * Swaps the tab's block (itself and its descendants) with the block of
 * its previous (up) or next (down) sibling. Tabs never change parent.
 *
 * Returns: %TRUE when the order changed (the caller re-applies it to the
 * window); %FALSE at the first/last sibling or for an unknown id
 */
G_GNUC_INTERNAL gboolean gsurf_tab_tree_move_subtree(GsurfTabTree *tree, guint64 id,
                                                     gint direction);

/**
 * GsurfTabTreeDrop:
 * @GSURF_TAB_TREE_DROP_BEFORE: the moved tab becomes the target's
 *   previous sibling
 * @GSURF_TAB_TREE_DROP_AFTER: the moved tab becomes the target's next
 *   sibling (after the target's whole subtree)
 * @GSURF_TAB_TREE_DROP_CHILD: the moved tab becomes the target's last
 *   child
 *
 * Where gsurf_tab_tree_move_to() puts a tab, relative to the tab it was
 * dropped on.
 */
typedef enum {
	GSURF_TAB_TREE_DROP_BEFORE,
	GSURF_TAB_TREE_DROP_AFTER,
	GSURF_TAB_TREE_DROP_CHILD
} GsurfTabTreeDrop;

/*
 * gsurf_tab_tree_move_to:
 * @id: the tab to move, with its subtree
 * @target: the tab it was dropped on
 * @where: before, after or under @target
 *
 * Moves @id's block (itself and its descendants) next to @target or to
 * the end of @target's block, and re-parents @id (to @target's parent,
 * or to @target). A tab cannot move into its own subtree.
 *
 * Returns: %TRUE when the tree changed (the caller re-applies the order
 * to the window); %FALSE for unknown ids, @id == @target, or a @target
 * inside @id's subtree
 */
G_GNUC_INTERNAL gboolean gsurf_tab_tree_move_to(GsurfTabTree *tree, guint64 id,
                                                guint64 target, GsurfTabTreeDrop where);

/* Whether @id is @ancestor or lies inside @ancestor's subtree. */
G_GNUC_INTERNAL gboolean gsurf_tab_tree_is_in_subtree(GsurfTabTree *tree, guint64 id,
                                                      guint64 ancestor);

/* --- Queries --- */

/* The derived parent (0 for a root); the opener id given at add/sync
 * time (kept after the opener closes); the depth (roots are 0); the
 * number of direct children; the size of the tab's block (itself plus
 * all descendants, 0 when absent); the index of the last tab in its
 * block (itself for a leaf, -1 when absent). */
G_GNUC_INTERNAL guint64  gsurf_tab_tree_get_parent(GsurfTabTree *tree, guint64 id);
G_GNUC_INTERNAL guint64  gsurf_tab_tree_get_opener(GsurfTabTree *tree, guint64 id);
G_GNUC_INTERNAL guint    gsurf_tab_tree_get_depth(GsurfTabTree *tree, guint64 id);
G_GNUC_INTERNAL guint    gsurf_tab_tree_get_n_children(GsurfTabTree *tree, guint64 id);
G_GNUC_INTERNAL guint    gsurf_tab_tree_get_subtree_size(GsurfTabTree *tree, guint64 id);
G_GNUC_INTERNAL gint     gsurf_tab_tree_get_last_descendant_index(GsurfTabTree *tree,
                                                                  guint64 id);

/* The indentation level to draw: the depth capped at @max_depth. The
 * tab keeps its real parent; only its drawing stops moving right. */
G_GNUC_INTERNAL guint    gsurf_tab_tree_get_display_depth(GsurfTabTree *tree, guint64 id,
                                                          guint max_depth);

/* --- Collapse --- */

/* A collapsed tab hides its descendants (they stay open). A tab is
 * visible when no ancestor is collapsed. expand_ancestors() expands every
 * ancestor of @id and returns whether anything changed. */
G_GNUC_INTERNAL gboolean gsurf_tab_tree_get_collapsed(GsurfTabTree *tree, guint64 id);
G_GNUC_INTERNAL void     gsurf_tab_tree_set_collapsed(GsurfTabTree *tree, guint64 id,
                                                      gboolean collapsed);
G_GNUC_INTERNAL gboolean gsurf_tab_tree_is_visible(GsurfTabTree *tree, guint64 id);
G_GNUC_INTERNAL gboolean gsurf_tab_tree_expand_ancestors(GsurfTabTree *tree, guint64 id);

/* --- Display rows --- */

/**
 * GsurfTabRowKind:
 * @GSURF_TAB_ROW_TAB: a tab (its view id in @id)
 * @GSURF_TAB_ROW_GROUP: a group heading (a window layout's group, or the
 *   background group)
 *
 * The kinds of sidebar rows.
 */
typedef enum {
	GSURF_TAB_ROW_TAB,
	GSURF_TAB_ROW_GROUP
} GsurfTabRowKind;

/* The group index of the "background tabs" group. */
#define GSURF_TAB_GROUP_BACKGROUND (G_MAXUINT)

/* The row key of a group heading: view ids never set the top bit. */
#define GSURF_TAB_GROUP_KEY(index) (G_GUINT64_CONSTANT(0x8000000000000000) | (guint64)(index))

/**
 * GsurfTabDisplayRow:
 * @kind: tab or group heading
 * @key: the row's identity: the view id of a tab, GSURF_TAB_GROUP_KEY()
 *   of a group
 * @group: the group the row belongs to (its own index for a heading;
 *   GSURF_TAB_GROUP_BACKGROUND for the background group; 0 in a flat
 *   tree)
 * @depth: the indentation level (a heading is 0, its tabs 1 and deeper)
 * @has_children: rows nested under it follow
 * @collapsed: it hides the rows nested under it (only with children)
 * @visible: no row it is nested under is collapsed
 *
 * One line of the tab sidebar, in display order.
 */
typedef struct {
	GsurfTabRowKind kind;
	guint64         key;
	guint           group;
	guint           depth;
	gboolean        has_children;
	gboolean        collapsed;
	gboolean        visible;
} GsurfTabDisplayRow;

/**
 * GsurfTabGroupSpec:
 * @ids: (array length=n_ids): the views a group's panes show, in pane
 *   order
 * @n_ids: their number
 * @index: the group's index (its heading's key)
 * @collapsed: whether the group's heading is collapsed
 *
 * One group for gsurf_tab_tree_build_grouped_rows().
 */
typedef struct {
	const guint64 *ids;
	guint          n_ids;
	guint          index;
	gboolean       collapsed;
} GsurfTabGroupSpec;

/*
 * gsurf_tab_tree_build_rows:
 *
 * The opener tree as rows, in tree order: every tab at its depth, its
 * collapse state and visibility as the tree has them.
 *
 * Returns: (transfer full) (element-type GsurfTabDisplayRow): the rows
 */
G_GNUC_INTERNAL GArray *gsurf_tab_tree_build_rows(GsurfTabTree *tree);

/*
 * gsurf_tab_tree_build_grouped_rows:
 * @groups: (array length=n_groups): the layout's groups, in order
 * @background_collapsed: whether the background heading is collapsed
 *
 * The tree grouped by a window layout's groups (wmux windows):
 *   1. for each group, a heading (depth 0), then each tab its panes show
 *      (depth 1, pane order), each followed by its descendants in the
 *      opener tree that no pane shows (relative depth below it); a
 *      descendant shown in a pane is skipped with its whole subtree (it
 *      is listed under its own pane);
 *   2. a final background heading (only when there are such tabs) with
 *      every tab not listed yet, in tree order, at 1 + its tree depth.
 * Ids that are not in the tree, or listed by an earlier group, are
 * ignored. A tab's collapse flag (the tree's) hides the rows nested under
 * it; a collapsed heading hides its whole group.
 *
 * Returns: (transfer full) (element-type GsurfTabDisplayRow): the rows
 */
G_GNUC_INTERNAL GArray *gsurf_tab_tree_build_grouped_rows(GsurfTabTree *tree,
                                                          const GsurfTabGroupSpec *groups,
                                                          guint n_groups,
                                                          gboolean background_collapsed);

G_END_DECLS

#endif /* GSURF_TAB_TREE_H */
