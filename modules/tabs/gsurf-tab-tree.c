/*
 * gsurf-tab-tree.c - Toolkit-neutral tab tree model for the tabs module
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Storage: the window order as an array of ids plus a hash of nodes. Each
 * node keeps the state that cannot be recomputed (its opener id, its
 * logical parent and its collapse flag); everything else (index, derived
 * parent, depth, child count, block size, visibility) is re-derived in
 * one O(n) pass after every structural change. See derive() for the
 * algorithm and docs/tabs.org for the rules it implements.
 */

#include "gsurf-tab-tree.h"

typedef struct _TabNode TabNode;

struct _TabNode
{
	guint64  id;          /* view id; also the hash key (points here) */
	guint64  opener_id;   /* as given at add/sync; informational */
	guint64  hint;        /* logical parent id, 0 = root */
	gboolean collapsed;   /* hides the descendants */

	/* Derived by derive(). */
	guint    index;
	guint64  parent;
	guint    depth;
	guint    n_children;
	guint    size;        /* itself plus all descendants */
	gboolean visible;     /* no collapsed ancestor */
	gboolean on_stack;    /* scratch flag for derive() */
};

struct _GsurfTabTree
{
	GArray     *order;    /* guint64 ids in window order */
	GHashTable *nodes;    /* &node->id -> TabNode* (owned) */
};

/* --- Construction --- */

GsurfTabTree *
gsurf_tab_tree_new(void)
{
	GsurfTabTree *tree;

	tree = g_new0(GsurfTabTree, 1);
	tree->order = g_array_new(FALSE, FALSE, sizeof(guint64));
	tree->nodes = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);
	return tree;
}

void
gsurf_tab_tree_free(GsurfTabTree *tree)
{
	if (tree == NULL)
		return;
	g_array_unref(tree->order);
	g_hash_table_unref(tree->nodes);
	g_free(tree);
}

static TabNode *
lookup(GsurfTabTree *tree, guint64 id)
{
	return g_hash_table_lookup(tree->nodes, &id);
}

static TabNode *
node_at(GsurfTabTree *tree, guint index)
{
	return lookup(tree, g_array_index(tree->order, guint64, index));
}

/*
 * derive:
 *
 * Recomputes every derived field from the order and the logical parents.
 *
 * Walk the order keeping a stack: the path from a root to the previous
 * tab. For each tab, if its logical parent is on that stack, pop down to
 * the parent and push the tab as its child; otherwise the tab is a root
 * and the stack restarts with it. A tab therefore only keeps a parent
 * that precedes it and whose block it directly continues, so every block
 * (a tab and its descendants) is contiguous in the order. A tab whose
 * parent is absent, later in the order, or cut off by an unrelated tab
 * becomes a root.
 *
 * Popping a tab at position i closes its block: size = i - index.
 *
 * Finally the logical parents are normalized to the derived ones, so a
 * later reorder can never re-attach a tab to a parent it already lost.
 */
static void
derive(GsurfTabTree *tree)
{
	g_autoptr(GPtrArray) stack = NULL;
	guint i;
	guint n;

	stack = g_ptr_array_new();
	n = tree->order->len;

	for (i = 0; i < n; i++) {
		TabNode *node = node_at(tree, i);
		TabNode *parent = NULL;

		node->index = i;
		node->n_children = 0;

		/* 1. Find the logical parent on the current path, if any. */
		if (node->hint != 0 && node->hint != node->id) {
			TabNode *candidate = lookup(tree, node->hint);

			if (candidate != NULL && candidate->on_stack)
				parent = candidate;
		}

		/* 2. Pop (closing blocks) down to the parent, or empty the stack
		 *    for a root. */
		while (stack->len > 0) {
			TabNode *top = g_ptr_array_index(stack, stack->len - 1);

			if (top == parent)
				break;
			top->size = i - top->index;
			top->on_stack = FALSE;
			g_ptr_array_remove_index(stack, stack->len - 1);
		}

		/* 3. Attach. */
		if (parent != NULL) {
			node->parent = parent->id;
			node->depth = parent->depth + 1;
			node->visible = parent->visible && !parent->collapsed;
			parent->n_children++;
		} else {
			node->parent = 0;
			node->depth = 0;
			node->visible = TRUE;
		}
		node->on_stack = TRUE;
		g_ptr_array_add(stack, node);
	}

	/* Close the blocks still open at the end of the order. */
	while (stack->len > 0) {
		TabNode *top = g_ptr_array_index(stack, stack->len - 1);

		top->size = n - top->index;
		top->on_stack = FALSE;
		g_ptr_array_remove_index(stack, stack->len - 1);
	}

	/* Normalize the logical parents to the derived tree. */
	for (i = 0; i < n; i++) {
		TabNode *node = node_at(tree, i);

		node->hint = node->parent;
	}
}

static TabNode *
node_new(guint64 id, guint64 opener_id)
{
	TabNode *node;

	node = g_new0(TabNode, 1);
	node->id = id;
	node->opener_id = opener_id;
	node->hint = opener_id;
	return node;
}

/* --- Order --- */

guint
gsurf_tab_tree_get_n_tabs(GsurfTabTree *tree)
{
	return tree->order->len;
}

guint64
gsurf_tab_tree_get_nth(GsurfTabTree *tree, guint index)
{
	if (index >= tree->order->len)
		return 0;
	return g_array_index(tree->order, guint64, index);
}

gint
gsurf_tab_tree_get_index(GsurfTabTree *tree, guint64 id)
{
	TabNode *node = lookup(tree, id);

	return node != NULL ? (gint)node->index : -1;
}

gboolean
gsurf_tab_tree_contains(GsurfTabTree *tree, guint64 id)
{
	return lookup(tree, id) != NULL;
}

/* --- Structure --- */

guint
gsurf_tab_tree_add(GsurfTabTree *tree, guint64 id, guint64 opener_id, guint index)
{
	TabNode *node;
	TabNode *opener;
	guint at;

	g_return_val_if_fail(id != 0, 0);

	node = lookup(tree, id);
	if (node != NULL)
		return node->index;

	/* A child goes right after its opener's last descendant, so it ends
	 * up as the opener's last child; anything else stays where the
	 * window put it. */
	opener = (opener_id != 0 && opener_id != id) ? lookup(tree, opener_id) : NULL;
	if (opener != NULL)
		at = opener->index + opener->size;
	else
		at = MIN(index, tree->order->len);

	node = node_new(id, opener_id);
	if (opener == NULL)
		node->hint = 0;
	g_hash_table_insert(tree->nodes, &node->id, node);
	g_array_insert_val(tree->order, at, node->id);
	derive(tree);
	return node->index;
}

void
gsurf_tab_tree_remove(GsurfTabTree *tree, guint64 id, GsurfTabTreeClosePolicy policy,
                      GArray *closed)
{
	TabNode *node;
	guint start;
	guint count;
	guint i;

	node = lookup(tree, id);
	if (node == NULL)
		return;

	start = node->index;
	if (policy == GSURF_TAB_TREE_CLOSE_CHILDREN) {
		/* The whole block goes; report the descendants in order. */
		count = node->size;
		for (i = start + 1; i < start + count; i++) {
			guint64 child = g_array_index(tree->order, guint64, i);

			if (closed != NULL)
				g_array_append_val(closed, child);
			g_hash_table_remove(tree->nodes, &child);
		}
	} else {
		/* Promote: each direct child takes the closed tab's parent. The
		 * children sit right after the closed tab, inside its parent's
		 * block, so the parent is still on derive()'s path for them. */
		count = 1;
		for (i = start + 1; i < start + node->size; i++) {
			TabNode *child = node_at(tree, i);

			if (child->parent == id)
				child->hint = node->parent;
		}
	}

	g_array_remove_range(tree->order, start, count);
	g_hash_table_remove(tree->nodes, &id);
	derive(tree);
}

void
gsurf_tab_tree_sync(GsurfTabTree *tree, const guint64 *ids, const guint64 *opener_ids,
                    guint n)
{
	g_autoptr(GHashTable) keep = NULL;
	GHashTableIter iter;
	gpointer key;
	guint i;

	/* 1. Rebuild the order from @ids, creating nodes for new ids. */
	keep = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_array_set_size(tree->order, 0);
	for (i = 0; i < n; i++) {
		TabNode *node;

		if (ids[i] == 0 || g_hash_table_contains(keep, &ids[i]))
			continue;
		node = lookup(tree, ids[i]);
		if (node == NULL) {
			node = node_new(ids[i], opener_ids != NULL ? opener_ids[i] : 0);
			g_hash_table_insert(tree->nodes, &node->id, node);
		}
		g_hash_table_add(keep, &node->id);
		g_array_append_val(tree->order, node->id);
	}

	/* 2. Drop the nodes the window no longer has. */
	g_hash_table_iter_init(&iter, tree->nodes);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		if (!g_hash_table_contains(keep, key))
			g_hash_table_iter_remove(&iter);
	}

	derive(tree);
}

/* Moves the block [from, from + len) so that it starts at @to (an index
 * computed with the block still in place, @to <= from or @to >= from +
 * len). */
static void
move_block(GsurfTabTree *tree, guint from, guint len, guint to)
{
	g_autoptr(GArray) block = NULL;

	block = g_array_sized_new(FALSE, FALSE, sizeof(guint64), len);
	g_array_append_vals(block, &g_array_index(tree->order, guint64, from), len);
	g_array_remove_range(tree->order, from, len);
	if (to > from)
		to -= len;
	g_array_insert_vals(tree->order, to, block->data, len);
}

gboolean
gsurf_tab_tree_move_subtree(GsurfTabTree *tree, guint64 id, gint direction)
{
	TabNode *node;
	TabNode *other;
	guint i;

	node = lookup(tree, id);
	if (node == NULL || direction == 0)
		return FALSE;

	if (direction < 0) {
		/* The previous sibling is the nearest earlier tab at our depth,
		 * provided no shallower tab (our parent) comes first. */
		other = NULL;
		for (i = node->index; i > 0; i--) {
			TabNode *candidate = node_at(tree, i - 1);

			if (candidate->depth < node->depth)
				break;
			if (candidate->depth == node->depth) {
				other = candidate;
				break;
			}
		}
		if (other == NULL)
			return FALSE;
		move_block(tree, node->index, node->size, other->index);
	} else {
		/* The next sibling starts right after our block. */
		guint next = node->index + node->size;

		if (next >= tree->order->len)
			return FALSE;
		other = node_at(tree, next);
		if (other->parent != node->parent)
			return FALSE;
		move_block(tree, node->index, node->size, next + other->size);
	}
	derive(tree);
	return TRUE;
}

/* --- Queries --- */

guint64
gsurf_tab_tree_get_parent(GsurfTabTree *tree, guint64 id)
{
	TabNode *node = lookup(tree, id);

	return node != NULL ? node->parent : 0;
}

guint64
gsurf_tab_tree_get_opener(GsurfTabTree *tree, guint64 id)
{
	TabNode *node = lookup(tree, id);

	return node != NULL ? node->opener_id : 0;
}

guint
gsurf_tab_tree_get_depth(GsurfTabTree *tree, guint64 id)
{
	TabNode *node = lookup(tree, id);

	return node != NULL ? node->depth : 0;
}

guint
gsurf_tab_tree_get_n_children(GsurfTabTree *tree, guint64 id)
{
	TabNode *node = lookup(tree, id);

	return node != NULL ? node->n_children : 0;
}

guint
gsurf_tab_tree_get_subtree_size(GsurfTabTree *tree, guint64 id)
{
	TabNode *node = lookup(tree, id);

	return node != NULL ? node->size : 0;
}

gint
gsurf_tab_tree_get_last_descendant_index(GsurfTabTree *tree, guint64 id)
{
	TabNode *node = lookup(tree, id);

	return node != NULL ? (gint)(node->index + node->size - 1) : -1;
}

guint
gsurf_tab_tree_get_display_depth(GsurfTabTree *tree, guint64 id, guint max_depth)
{
	return MIN(gsurf_tab_tree_get_depth(tree, id), max_depth);
}

/* --- Collapse --- */

gboolean
gsurf_tab_tree_get_collapsed(GsurfTabTree *tree, guint64 id)
{
	TabNode *node = lookup(tree, id);

	return node != NULL && node->collapsed;
}

void
gsurf_tab_tree_set_collapsed(GsurfTabTree *tree, guint64 id, gboolean collapsed)
{
	TabNode *node = lookup(tree, id);

	if (node == NULL || node->collapsed == (collapsed != FALSE))
		return;
	node->collapsed = (collapsed != FALSE);
	derive(tree);
}

gboolean
gsurf_tab_tree_is_visible(GsurfTabTree *tree, guint64 id)
{
	TabNode *node = lookup(tree, id);

	return node != NULL && node->visible;
}

gboolean
gsurf_tab_tree_expand_ancestors(GsurfTabTree *tree, guint64 id)
{
	TabNode *node = lookup(tree, id);
	gboolean changed = FALSE;

	if (node == NULL)
		return FALSE;
	for (node = lookup(tree, node->parent); node != NULL; node = lookup(tree, node->parent)) {
		if (node->collapsed) {
			node->collapsed = FALSE;
			changed = TRUE;
		}
	}
	if (changed)
		derive(tree);
	return changed;
}
