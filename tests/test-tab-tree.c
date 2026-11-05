/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-tab-tree.c - the tabs module's tree model and its configuration
 *
 * Part 1 compiles the toolkit-neutral model (modules/tabs/gsurf-tab-tree.c)
 * straight into the test and checks the rules in docs/tabs.org: placement
 * after the opener's last descendant, nesting, promote vs close_children,
 * collapse and expansion on activation, moving subtrees, max_depth,
 * orphaned openers, rebuilding from the window order alone, handing a
 * tab's place to its replacement (reopen in another container), drag and
 * drop re-parenting (move_to), the sidebar's display rows (the opener
 * tree, and grouped by a window layout's groups such as wmux windows),
 * and a randomized stress run against the structural invariants.
 *
 * Part 2 loads the real tabs.so (no window, no display) and checks its
 * configuration through the keys it reports: defaults, custom keys, a
 * second configure() returning to the defaults, and bad values.
 */
#include <gsurf.h>
#include <glib.h>

#include "modules/tabs/gsurf-tab-tree.c"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* The tree's order as "1 2 3" for readable assertions. */
static gchar *
order_string(GsurfTabTree *tree)
{
	GString *s = g_string_new(NULL);
	guint i;

	for (i = 0; i < gsurf_tab_tree_get_n_tabs(tree); i++)
		g_string_append_printf(s, "%s%" G_GUINT64_FORMAT, i > 0 ? " " : "",
			gsurf_tab_tree_get_nth(tree, i));
	return g_string_free(s, FALSE);
}

static void
assert_order(GsurfTabTree *tree, const gchar *expected)
{
	g_autofree gchar *got = order_string(tree);

	g_assert_cmpstr(got, ==, expected);
}

/* Every block (a tab and its descendants) is contiguous, a parent
 * precedes its children at depth - 1, and the counts add up. */
static void
assert_invariants(GsurfTabTree *tree)
{
	guint n = gsurf_tab_tree_get_n_tabs(tree);
	guint i;
	guint j;

	for (i = 0; i < n; i++) {
		guint64 id = gsurf_tab_tree_get_nth(tree, i);
		guint64 parent = gsurf_tab_tree_get_parent(tree, id);
		guint depth = gsurf_tab_tree_get_depth(tree, id);
		guint size = gsurf_tab_tree_get_subtree_size(tree, id);
		guint children = 0;

		g_assert_cmpint(gsurf_tab_tree_get_index(tree, id), ==, (gint)i);
		g_assert_cmpuint(size, >=, 1);
		g_assert_cmpuint(i + size, <=, n);
		g_assert_cmpint(gsurf_tab_tree_get_last_descendant_index(tree, id), ==,
			(gint)(i + size - 1));
		if (parent == 0) {
			g_assert_cmpuint(depth, ==, 0);
		} else {
			gint pi = gsurf_tab_tree_get_index(tree, parent);

			g_assert_cmpint(pi, >=, 0);
			g_assert_cmpint(pi, <, (gint)i);
			g_assert_cmpuint(gsurf_tab_tree_get_depth(tree, parent) + 1, ==, depth);
			g_assert_cmpint(gsurf_tab_tree_get_last_descendant_index(tree, parent), >=,
				(gint)(i + size - 1));
		}
		/* Everything inside the block is deeper; the next tab is not. */
		for (j = i + 1; j < i + size; j++) {
			guint64 inner = gsurf_tab_tree_get_nth(tree, j);

			g_assert_cmpuint(gsurf_tab_tree_get_depth(tree, inner), >, depth);
			if (gsurf_tab_tree_get_parent(tree, inner) == id)
				children++;
		}
		if (i + size < n)
			g_assert_cmpuint(gsurf_tab_tree_get_depth(tree,
				gsurf_tab_tree_get_nth(tree, i + size)), <=, depth);
		g_assert_cmpuint(children, ==, gsurf_tab_tree_get_n_children(tree, id));
	}
}

/* ------------------------------------------------------------------ */
/* Placement                                                           */
/* ------------------------------------------------------------------ */

/* Views without an opener stay where the window put them (appended). */
static void
test_roots_append(void)
{
	g_autoptr(GsurfTabTree) tree = gsurf_tab_tree_new();

	g_assert_cmpuint(gsurf_tab_tree_add(tree, 1, 0, 0), ==, 0);
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 2, 0, 1), ==, 1);
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 3, 0, 2), ==, 2);
	assert_order(tree, "1 2 3");
	g_assert_cmpuint(gsurf_tab_tree_get_depth(tree, 3), ==, 0);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 3), ==, 0);
	/* A root inserted mid-list by the window keeps its index. */
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 4, 0, 1), ==, 1);
	assert_order(tree, "1 4 2 3");
	/* Re-adding is a no-op; an index past the end appends. */
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 4, 0, 3), ==, 1);
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 5, 0, 99), ==, 4);
	assert_invariants(tree);
}

/* A child goes right after its opener's last descendant. */
static void
test_insert_after_last_descendant(void)
{
	g_autoptr(GsurfTabTree) tree = gsurf_tab_tree_new();

	gsurf_tab_tree_add(tree, 1, 0, 0);
	gsurf_tab_tree_add(tree, 2, 0, 1);
	/* 3 opened from 1 lands right after 1, before 2. */
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 3, 1, 2), ==, 1);
	assert_order(tree, "1 3 2");
	/* 4 opened from 1 lands after 3 (1's last descendant). */
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 4, 1, 3), ==, 2);
	assert_order(tree, "1 3 4 2");
	/* 5 opened from 3 nests under 3, before 4. */
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 5, 3, 4), ==, 2);
	assert_order(tree, "1 3 5 4 2");
	/* 6 opened from 1 again goes after 4, the last of 1's block. */
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 6, 1, 5), ==, 4);
	assert_order(tree, "1 3 5 4 6 2");

	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 3), ==, 1);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 5), ==, 3);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 6), ==, 1);
	g_assert_cmpuint(gsurf_tab_tree_get_depth(tree, 5), ==, 2);
	g_assert_cmpuint(gsurf_tab_tree_get_n_children(tree, 1), ==, 3);
	g_assert_cmpuint(gsurf_tab_tree_get_subtree_size(tree, 1), ==, 5);
	g_assert_cmpint(gsurf_tab_tree_get_last_descendant_index(tree, 1), ==, 4);
	g_assert_cmpint(gsurf_tab_tree_get_last_descendant_index(tree, 2), ==, 5);
	g_assert_cmpuint(gsurf_tab_tree_get_opener(tree, 5), ==, 3);
	assert_invariants(tree);
}

/* Deep chains nest one level per opener. */
static void
test_nested_children(void)
{
	g_autoptr(GsurfTabTree) tree = gsurf_tab_tree_new();
	guint64 id;

	gsurf_tab_tree_add(tree, 1, 0, 0);
	for (id = 2; id <= 10; id++)
		gsurf_tab_tree_add(tree, id, id - 1, (guint)id - 1);
	assert_order(tree, "1 2 3 4 5 6 7 8 9 10");
	g_assert_cmpuint(gsurf_tab_tree_get_depth(tree, 10), ==, 9);
	g_assert_cmpuint(gsurf_tab_tree_get_subtree_size(tree, 1), ==, 10);
	/* A sibling of 2 goes after the whole chain. */
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 11, 1, 0), ==, 10);
	g_assert_cmpuint(gsurf_tab_tree_get_depth(tree, 11), ==, 1);
	assert_invariants(tree);
}

/* ------------------------------------------------------------------ */
/* Closing a parent                                                    */
/* ------------------------------------------------------------------ */

/* Build: 1 [ 2 [ 3 4 ] 5 ] 6 */
static GsurfTabTree *
sample_tree(void)
{
	GsurfTabTree *tree = gsurf_tab_tree_new();

	gsurf_tab_tree_add(tree, 1, 0, 0);
	gsurf_tab_tree_add(tree, 6, 0, 1);
	gsurf_tab_tree_add(tree, 2, 1, 2);
	gsurf_tab_tree_add(tree, 3, 2, 3);
	gsurf_tab_tree_add(tree, 4, 2, 4);
	gsurf_tab_tree_add(tree, 5, 1, 5);
	assert_order(tree, "1 2 3 4 5 6");
	return tree;
}

static void
test_close_promote(void)
{
	g_autoptr(GsurfTabTree) tree = sample_tree();
	g_autoptr(GArray) closed = g_array_new(FALSE, FALSE, sizeof(guint64));

	/* Closing 2: 3 and 4 move up to 1, keeping their order. */
	gsurf_tab_tree_remove(tree, 2, GSURF_TAB_TREE_CLOSE_PROMOTE, closed);
	g_assert_cmpuint(closed->len, ==, 0);
	assert_order(tree, "1 3 4 5 6");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 3), ==, 1);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 4), ==, 1);
	g_assert_cmpuint(gsurf_tab_tree_get_depth(tree, 4), ==, 1);
	g_assert_cmpuint(gsurf_tab_tree_get_n_children(tree, 1), ==, 3);
	/* The opener id is kept although the opener is gone. */
	g_assert_cmpuint(gsurf_tab_tree_get_opener(tree, 3), ==, 2);

	/* Closing the root 1: its children become roots. */
	gsurf_tab_tree_remove(tree, 1, GSURF_TAB_TREE_CLOSE_PROMOTE, NULL);
	assert_order(tree, "3 4 5 6");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 3), ==, 0);
	g_assert_cmpuint(gsurf_tab_tree_get_depth(tree, 5), ==, 0);
	/* Unknown ids are ignored. */
	gsurf_tab_tree_remove(tree, 42, GSURF_TAB_TREE_CLOSE_PROMOTE, NULL);
	assert_order(tree, "3 4 5 6");
	assert_invariants(tree);
}

/* gsurf_tab_tree_replace(): the replacement takes the old tab's place,
 * parent, children and collapse state, wherever it was added. */
static void
test_replace(void)
{
	g_autoptr(GsurfTabTree) tree = sample_tree();

	/* 1. Replace the middle parent 2 with 7, added as a root at the end
	 *    (where the window first puts a reopened view's tree node when
	 *    its opener is not 2's parent). */
	gsurf_tab_tree_set_collapsed(tree, 2, TRUE);
	gsurf_tab_tree_add(tree, 7, 0, 6);
	assert_order(tree, "1 2 3 4 5 6 7");
	g_assert_true(gsurf_tab_tree_replace(tree, 2, 7));
	assert_order(tree, "1 7 3 4 5 6");
	g_assert_false(gsurf_tab_tree_contains(tree, 2));
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 7), ==, 1);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 3), ==, 7);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 4), ==, 7);
	g_assert_cmpuint(gsurf_tab_tree_get_n_children(tree, 7), ==, 2);
	g_assert_true(gsurf_tab_tree_get_collapsed(tree, 7));
	g_assert_false(gsurf_tab_tree_is_visible(tree, 3));
	assert_invariants(tree);

	/* 2. A replacement not in the tree yet just takes the place. */
	g_assert_true(gsurf_tab_tree_replace(tree, 1, 8));
	assert_order(tree, "8 7 3 4 5 6");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 7), ==, 8);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 5), ==, 8);
	assert_invariants(tree);

	/* 3. A replacement already placed as a child of the old tab (it
	 *    was opened from it) leaves that spot. */
	gsurf_tab_tree_add(tree, 9, 5, 0);
	assert_order(tree, "8 7 3 4 5 9 6");
	g_assert_true(gsurf_tab_tree_replace(tree, 5, 9));
	assert_order(tree, "8 7 3 4 9 6");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 9), ==, 8);
	assert_invariants(tree);

	/* 4. Refusals change nothing. */
	g_assert_false(gsurf_tab_tree_replace(tree, 42, 10));
	g_assert_false(gsurf_tab_tree_replace(tree, 9, 9));
	g_assert_false(gsurf_tab_tree_replace(tree, 0, 10));
	g_assert_false(gsurf_tab_tree_replace(tree, 9, 0));
	assert_order(tree, "8 7 3 4 9 6");
	assert_invariants(tree);
}

static void
test_close_children(void)
{
	g_autoptr(GsurfTabTree) tree = sample_tree();
	g_autoptr(GArray) closed = g_array_new(FALSE, FALSE, sizeof(guint64));

	gsurf_tab_tree_remove(tree, 1, GSURF_TAB_TREE_CLOSE_CHILDREN, closed);
	g_assert_cmpuint(closed->len, ==, 4);
	g_assert_cmpuint(g_array_index(closed, guint64, 0), ==, 2);
	g_assert_cmpuint(g_array_index(closed, guint64, 1), ==, 3);
	g_assert_cmpuint(g_array_index(closed, guint64, 2), ==, 4);
	g_assert_cmpuint(g_array_index(closed, guint64, 3), ==, 5);
	assert_order(tree, "6");
	g_assert_false(gsurf_tab_tree_contains(tree, 3));
	/* A leaf closes alone. */
	g_array_set_size(closed, 0);
	gsurf_tab_tree_remove(tree, 6, GSURF_TAB_TREE_CLOSE_CHILDREN, closed);
	g_assert_cmpuint(closed->len, ==, 0);
	g_assert_cmpuint(gsurf_tab_tree_get_n_tabs(tree), ==, 0);
}

/* ------------------------------------------------------------------ */
/* Collapse                                                            */
/* ------------------------------------------------------------------ */

static void
test_collapse_expand(void)
{
	g_autoptr(GsurfTabTree) tree = sample_tree();

	g_assert_true(gsurf_tab_tree_is_visible(tree, 3));
	gsurf_tab_tree_set_collapsed(tree, 1, TRUE);
	g_assert_true(gsurf_tab_tree_get_collapsed(tree, 1));
	/* The collapsed tab stays visible; its descendants hide but stay. */
	g_assert_true(gsurf_tab_tree_is_visible(tree, 1));
	g_assert_false(gsurf_tab_tree_is_visible(tree, 2));
	g_assert_false(gsurf_tab_tree_is_visible(tree, 3));
	g_assert_false(gsurf_tab_tree_is_visible(tree, 5));
	g_assert_true(gsurf_tab_tree_is_visible(tree, 6));
	g_assert_cmpuint(gsurf_tab_tree_get_n_tabs(tree), ==, 6);

	/* Collapse an inner level too, then expand the outer one: 2's
	 * children stay hidden behind 2's own collapse. */
	gsurf_tab_tree_set_collapsed(tree, 2, TRUE);
	gsurf_tab_tree_set_collapsed(tree, 1, FALSE);
	g_assert_true(gsurf_tab_tree_is_visible(tree, 2));
	g_assert_false(gsurf_tab_tree_is_visible(tree, 4));

	/* Activating a hidden tab expands every collapsed ancestor. */
	gsurf_tab_tree_set_collapsed(tree, 1, TRUE);
	g_assert_true(gsurf_tab_tree_expand_ancestors(tree, 4));
	g_assert_false(gsurf_tab_tree_get_collapsed(tree, 1));
	g_assert_false(gsurf_tab_tree_get_collapsed(tree, 2));
	g_assert_true(gsurf_tab_tree_is_visible(tree, 4));
	/* Nothing left to expand; the tab's own state is not an ancestor's. */
	g_assert_false(gsurf_tab_tree_expand_ancestors(tree, 4));
	gsurf_tab_tree_set_collapsed(tree, 2, TRUE);
	g_assert_false(gsurf_tab_tree_expand_ancestors(tree, 2));
	g_assert_true(gsurf_tab_tree_get_collapsed(tree, 2));
	g_assert_false(gsurf_tab_tree_expand_ancestors(tree, 99));

	/* Collapse survives a sync from the same order. */
	{
		const guint64 ids[] = { 1, 2, 3, 4, 5, 6 };
		const guint64 openers[] = { 0, 1, 2, 2, 1, 0 };

		gsurf_tab_tree_sync(tree, ids, openers, G_N_ELEMENTS(ids));
		g_assert_true(gsurf_tab_tree_get_collapsed(tree, 2));
		g_assert_false(gsurf_tab_tree_is_visible(tree, 3));
	}
	assert_invariants(tree);
}

/* ------------------------------------------------------------------ */
/* Moving subtrees                                                     */
/* ------------------------------------------------------------------ */

static void
test_move_subtrees(void)
{
	g_autoptr(GsurfTabTree) tree = sample_tree();

	/* 2 has no previous sibling under 1. */
	g_assert_false(gsurf_tab_tree_move_subtree(tree, 2, -1));
	/* 2 [3 4] moves below 5, taking its children along. */
	g_assert_true(gsurf_tab_tree_move_subtree(tree, 2, +1));
	assert_order(tree, "1 5 2 3 4 6");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 3), ==, 2);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 2), ==, 1);
	/* ...and is the last child now. */
	g_assert_false(gsurf_tab_tree_move_subtree(tree, 2, +1));
	g_assert_true(gsurf_tab_tree_move_subtree(tree, 2, -1));
	assert_order(tree, "1 2 3 4 5 6");

	/* Leaves move among their siblings only. */
	g_assert_true(gsurf_tab_tree_move_subtree(tree, 4, -1));
	assert_order(tree, "1 2 4 3 5 6");
	g_assert_false(gsurf_tab_tree_move_subtree(tree, 4, -1));
	g_assert_false(gsurf_tab_tree_move_subtree(tree, 3, +1));

	/* Roots: 6 moves above the whole block of 1. */
	g_assert_true(gsurf_tab_tree_move_subtree(tree, 6, -1));
	assert_order(tree, "6 1 2 4 3 5");
	g_assert_false(gsurf_tab_tree_move_subtree(tree, 6, -1));
	g_assert_true(gsurf_tab_tree_move_subtree(tree, 6, +1));
	assert_order(tree, "1 2 4 3 5 6");
	g_assert_false(gsurf_tab_tree_move_subtree(tree, 6, +1));

	/* Nonsense requests change nothing. */
	g_assert_false(gsurf_tab_tree_move_subtree(tree, 99, +1));
	g_assert_false(gsurf_tab_tree_move_subtree(tree, 1, 0));
	assert_invariants(tree);
}

/* ------------------------------------------------------------------ */
/* Depth cap, orphans and rebuilding                                   */
/* ------------------------------------------------------------------ */
/* Drag and drop re-parenting                                          */
/* ------------------------------------------------------------------ */

/* sample_tree(): 1 [ 2 [ 3 4 ] 5 ] 6 */
static void
test_move_to(void)
{
	g_autoptr(GsurfTabTree) tree = sample_tree();

	/* 1. Never into its own subtree, never onto itself, never unknown. */
	g_assert_false(gsurf_tab_tree_move_to(tree, 2, 3, GSURF_TAB_TREE_DROP_CHILD));
	g_assert_false(gsurf_tab_tree_move_to(tree, 2, 2, GSURF_TAB_TREE_DROP_BEFORE));
	g_assert_false(gsurf_tab_tree_move_to(tree, 1, 4, GSURF_TAB_TREE_DROP_AFTER));
	g_assert_false(gsurf_tab_tree_move_to(tree, 42, 1, GSURF_TAB_TREE_DROP_CHILD));
	g_assert_false(gsurf_tab_tree_move_to(tree, 1, 42, GSURF_TAB_TREE_DROP_CHILD));
	assert_order(tree, "1 2 3 4 5 6");
	g_assert_true(gsurf_tab_tree_is_in_subtree(tree, 4, 1));
	g_assert_true(gsurf_tab_tree_is_in_subtree(tree, 2, 2));
	g_assert_false(gsurf_tab_tree_is_in_subtree(tree, 6, 1));
	g_assert_false(gsurf_tab_tree_is_in_subtree(tree, 1, 2));

	/* 2. Under another tab: 2's whole block becomes 6's last child. */
	g_assert_true(gsurf_tab_tree_move_to(tree, 2, 6, GSURF_TAB_TREE_DROP_CHILD));
	assert_order(tree, "1 5 6 2 3 4");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 2), ==, 6);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 3), ==, 2);
	g_assert_cmpuint(gsurf_tab_tree_get_depth(tree, 4), ==, 2);
	assert_invariants(tree);

	/* 3. Before a tab: a sibling of it, at its parent. */
	g_assert_true(gsurf_tab_tree_move_to(tree, 4, 1, GSURF_TAB_TREE_DROP_BEFORE));
	assert_order(tree, "4 1 5 6 2 3");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 4), ==, 0);
	g_assert_true(gsurf_tab_tree_move_to(tree, 3, 5, GSURF_TAB_TREE_DROP_BEFORE));
	assert_order(tree, "4 1 3 5 6 2");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 3), ==, 1);
	assert_invariants(tree);

	/* 4. After a tab: after its whole block, at its parent. */
	g_assert_true(gsurf_tab_tree_move_to(tree, 4, 1, GSURF_TAB_TREE_DROP_AFTER));
	assert_order(tree, "1 3 5 4 6 2");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 4), ==, 0);
	/* A child moved after its own parent leaves it. */
	g_assert_true(gsurf_tab_tree_move_to(tree, 2, 6, GSURF_TAB_TREE_DROP_AFTER));
	assert_order(tree, "1 3 5 4 6 2");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 2), ==, 0);
	/* A descendant moved under an ancestor becomes its last child. */
	g_assert_true(gsurf_tab_tree_move_to(tree, 3, 1, GSURF_TAB_TREE_DROP_CHILD));
	assert_order(tree, "1 5 3 4 6 2");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 3), ==, 1);
	assert_invariants(tree);
}

/* ------------------------------------------------------------------ */
/* Display rows                                                        */
/* ------------------------------------------------------------------ */

/* The rows as "key:depth[flags] ..." where a group key is shown as
 * "G<index>" ("Gbg" for the background group) and flags are c (has
 * children), x (collapsed) and h (hidden). */
static gchar *
rows_string(GArray *rows)
{
	GString *s = g_string_new(NULL);
	guint i;

	for (i = 0; i < rows->len; i++) {
		GsurfTabDisplayRow *r = &g_array_index(rows, GsurfTabDisplayRow, i);

		if (i > 0)
			g_string_append_c(s, ' ');
		if (r->kind == GSURF_TAB_ROW_GROUP) {
			g_assert_true(r->key == GSURF_TAB_GROUP_KEY(r->group));
			if (r->group == GSURF_TAB_GROUP_BACKGROUND)
				g_string_append(s, "Gbg");
			else
				g_string_append_printf(s, "G%u", r->group);
		} else {
			g_string_append_printf(s, "%" G_GUINT64_FORMAT, r->key);
		}
		g_string_append_printf(s, ":%u%s%s%s", r->depth, r->has_children ? "c" : "",
			r->collapsed ? "x" : "", r->visible ? "" : "h");
	}
	return g_string_free(s, FALSE);
}

#define ASSERT_ROWS(rows, expected) G_STMT_START { \
	g_autofree gchar *got_ = rows_string(rows); \
	g_assert_cmpstr(got_, ==, (expected)); \
} G_STMT_END

/* Without groups the rows are the opener tree, exactly. */
static void
test_rows_flat(void)
{
	g_autoptr(GsurfTabTree) tree = sample_tree();
	g_autoptr(GArray) rows = gsurf_tab_tree_build_rows(tree);
	g_autoptr(GArray) after = NULL;

	ASSERT_ROWS(rows, "1:0c 2:1c 3:2 4:2 5:1 6:0");
	gsurf_tab_tree_set_collapsed(tree, 2, TRUE);
	after = gsurf_tab_tree_build_rows(tree);
	ASSERT_ROWS(after, "1:0c 2:1cx 3:2h 4:2h 5:1 6:0");
	/* Collapsing a leaf shows nothing as collapsed. */
	gsurf_tab_tree_set_collapsed(tree, 6, TRUE);
	g_clear_pointer(&after, g_array_unref);
	after = gsurf_tab_tree_build_rows(tree);
	ASSERT_ROWS(after, "1:0c 2:1cx 3:2h 4:2h 5:1 6:0");
}

/* Grouped: pane tabs under their group in pane order, each with its
 * background descendants; descendants in panes are listed under their
 * own pane; the rest under the background group. */
static void
test_rows_grouped(void)
{
	g_autoptr(GsurfTabTree) tree = sample_tree();
	g_autoptr(GArray) rows = NULL;
	/* Window 0 shows 5 and 1 (pane order), window 1 shows 3; 7 is a
	 * stale id the tree does not know. */
	static const guint64 win0[] = { 5, 1, 7 };
	static const guint64 win1[] = { 3 };
	GsurfTabGroupSpec groups[2] = {
		{ win0, G_N_ELEMENTS(win0), 0, FALSE },
		{ win1, G_N_ELEMENTS(win1), 1, FALSE },
	};

	/* 1. 1's subtree: 2 (background) with 4 under it; 3 is in a pane
	 *    (window 1) and listed there; 5 is a pane of its own, listed
	 *    first. 6 is in no pane and no pane tab's subtree. */
	rows = gsurf_tab_tree_build_grouped_rows(tree, groups, 2, FALSE);
	ASSERT_ROWS(rows, "G0:0c 5:1 1:1c 2:2c 4:3 G1:0c 3:1 Gbg:0c 6:1");
	g_clear_pointer(&rows, g_array_unref);

	/* 2. A collapsed group heading hides its whole group; a collapsed
	 *    tab its nested rows; the background heading collapses too. */
	groups[0].collapsed = TRUE;
	gsurf_tab_tree_set_collapsed(tree, 2, TRUE);
	rows = gsurf_tab_tree_build_grouped_rows(tree, groups, 2, TRUE);
	ASSERT_ROWS(rows, "G0:0cx 5:1h 1:1ch 2:2cxh 4:3h G1:0c 3:1 Gbg:0cx 6:1h");
	g_clear_pointer(&rows, g_array_unref);
	groups[0].collapsed = FALSE;
	rows = gsurf_tab_tree_build_grouped_rows(tree, groups, 2, FALSE);
	ASSERT_ROWS(rows, "G0:0c 5:1 1:1c 2:2cx 4:3h G1:0c 3:1 Gbg:0c 6:1");
	g_clear_pointer(&rows, g_array_unref);
	gsurf_tab_tree_set_collapsed(tree, 2, FALSE);

	/* 3. Every tab in a pane: no background group. An empty group still
	 *    has its heading, without children. A tab listed by two groups
	 *    shows in the first only. */
	{
		static const guint64 all[] = { 1, 6 };
		static const guint64 dup[] = { 6 };
		GsurfTabGroupSpec g2[3] = {
			{ all, G_N_ELEMENTS(all), 0, FALSE },
			{ NULL, 0, 4, FALSE },
			{ dup, G_N_ELEMENTS(dup), 5, FALSE },
		};

		rows = gsurf_tab_tree_build_grouped_rows(tree, g2, 3, FALSE);
		ASSERT_ROWS(rows, "G0:0c 1:1c 2:2c 3:3 4:3 5:2 6:1 G4:0 G5:0");
		g_clear_pointer(&rows, g_array_unref);
	}

	/* 4. No groups at all: everything is background, at depth + 1. */
	rows = gsurf_tab_tree_build_grouped_rows(tree, NULL, 0, FALSE);
	ASSERT_ROWS(rows, "Gbg:0c 1:1c 2:2c 3:3 4:3 5:2 6:1");
	g_clear_pointer(&rows, g_array_unref);

	/* 5. An empty tree: no rows but the group headings. */
	{
		g_autoptr(GsurfTabTree) empty = gsurf_tab_tree_new();

		rows = gsurf_tab_tree_build_grouped_rows(empty, groups, 2, FALSE);
		ASSERT_ROWS(rows, "G0:0 G1:0");
	}
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */

static void
test_max_depth(void)
{
	g_autoptr(GsurfTabTree) tree = gsurf_tab_tree_new();
	guint64 id;

	gsurf_tab_tree_add(tree, 1, 0, 0);
	for (id = 2; id <= 6; id++)
		gsurf_tab_tree_add(tree, id, id - 1, (guint)id - 1);
	/* Deeper tabs draw at the cap but keep their real parent. */
	g_assert_cmpuint(gsurf_tab_tree_get_depth(tree, 6), ==, 5);
	g_assert_cmpuint(gsurf_tab_tree_get_display_depth(tree, 6, 3), ==, 3);
	g_assert_cmpuint(gsurf_tab_tree_get_display_depth(tree, 4, 3), ==, 3);
	g_assert_cmpuint(gsurf_tab_tree_get_display_depth(tree, 3, 3), ==, 2);
	g_assert_cmpuint(gsurf_tab_tree_get_display_depth(tree, 6, 0), ==, 0);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 6), ==, 5);
}

/* An opener that is closed or was never in the window leaves a root. */
static void
test_orphaned_opener(void)
{
	g_autoptr(GsurfTabTree) tree = gsurf_tab_tree_new();
	const guint64 ids[] = { 10, 11, 12 };
	const guint64 openers[] = { 0, 77, 10 };

	/* Opener 77 is elsewhere: 2 is a root at the window's index. */
	gsurf_tab_tree_add(tree, 1, 0, 0);
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 2, 77, 1), ==, 1);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 2), ==, 0);
	g_assert_cmpuint(gsurf_tab_tree_get_opener(tree, 2), ==, 77);
	/* A self-opener is ignored. */
	g_assert_cmpuint(gsurf_tab_tree_add(tree, 3, 3, 2), ==, 2);
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 3), ==, 0);

	/* Sync: the same applies to a whole window. */
	gsurf_tab_tree_sync(tree, ids, openers, G_N_ELEMENTS(ids));
	assert_order(tree, "10 11 12");
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 11), ==, 0);
	g_assert_cmpuint(gsurf_tab_tree_get_opener(tree, 11), ==, 77);
	/* 12's opener 10 is in the window but 11 (a root) cuts it off. */
	g_assert_cmpuint(gsurf_tab_tree_get_parent(tree, 12), ==, 0);
	g_assert_false(gsurf_tab_tree_contains(tree, 1));
	assert_invariants(tree);
}

/* A session restored in window order with openers rebuilds the same
 * tree without the tree having seen the original adds. */
static void
test_rebuild_from_window_order(void)
{
	g_autoptr(GsurfTabTree) original = sample_tree();
	g_autoptr(GsurfTabTree) rebuilt = gsurf_tab_tree_new();
	g_autoptr(GsurfTabTree) replayed = gsurf_tab_tree_new();
	const guint64 ids[] = { 1, 2, 3, 4, 5, 6, 6, 0 };
	const guint64 openers[] = { 0, 1, 2, 2, 1, 0, 0, 0 };
	guint i;

	/* sync() with duplicates and a zero id skips them. */
	gsurf_tab_tree_sync(rebuilt, ids, openers, G_N_ELEMENTS(ids));
	/* add() in the saved order places every child where it was. */
	for (i = 0; i < 6; i++)
		g_assert_cmpuint(gsurf_tab_tree_add(replayed, ids[i], openers[i], i), ==, i);

	for (i = 0; i < 6; i++) {
		g_assert_cmpuint(gsurf_tab_tree_get_parent(rebuilt, ids[i]), ==,
			gsurf_tab_tree_get_parent(original, ids[i]));
		g_assert_cmpuint(gsurf_tab_tree_get_parent(replayed, ids[i]), ==,
			gsurf_tab_tree_get_parent(original, ids[i]));
	}
	assert_order(rebuilt, "1 2 3 4 5 6");

	/* An external reorder that pulls a child out of its parent's block
	 * makes it a root, and it stays one when moved back. */
	{
		const guint64 moved[] = { 1, 2, 4, 5, 6, 3 };

		gsurf_tab_tree_sync(rebuilt, moved, NULL, G_N_ELEMENTS(moved));
		g_assert_cmpuint(gsurf_tab_tree_get_parent(rebuilt, 3), ==, 0);
		gsurf_tab_tree_sync(rebuilt, ids, openers, 6);
		g_assert_cmpuint(gsurf_tab_tree_get_parent(rebuilt, 3), ==, 0);
	}
	gsurf_tab_tree_sync(rebuilt, NULL, NULL, 0);
	g_assert_cmpuint(gsurf_tab_tree_get_n_tabs(rebuilt), ==, 0);
	assert_invariants(rebuilt);
}

/* Many tabs under random operations keep the invariants. */
static void
test_stress(void)
{
	g_autoptr(GsurfTabTree) tree = gsurf_tab_tree_new();
	g_autoptr(GRand) rand = g_rand_new_with_seed(20261104);
	guint64 next_id = 1;
	guint step;

	for (step = 0; step < 4000; step++) {
		guint n = gsurf_tab_tree_get_n_tabs(tree);
		gint op = g_rand_int_range(rand, 0, 10);

		if (n < 2 || op < 5) {
			/* Add, opened from a random tab half of the time. */
			guint64 opener = (n > 0 && g_rand_boolean(rand))
				? gsurf_tab_tree_get_nth(tree, g_rand_int_range(rand, 0, (gint32)n)) : 0;

			gsurf_tab_tree_add(tree, next_id++, opener, n);
		} else if (op < 7) {
			guint64 id = gsurf_tab_tree_get_nth(tree, g_rand_int_range(rand, 0, (gint32)n));

			gsurf_tab_tree_remove(tree, id, g_rand_boolean(rand)
				? GSURF_TAB_TREE_CLOSE_PROMOTE : GSURF_TAB_TREE_CLOSE_CHILDREN, NULL);
		} else if (op < 9) {
			guint64 id = gsurf_tab_tree_get_nth(tree, g_rand_int_range(rand, 0, (gint32)n));

			gsurf_tab_tree_move_subtree(tree, id, g_rand_boolean(rand) ? 1 : -1);
		} else {
			guint64 id = gsurf_tab_tree_get_nth(tree, g_rand_int_range(rand, 0, (gint32)n));

			gsurf_tab_tree_set_collapsed(tree, id, !gsurf_tab_tree_get_collapsed(tree, id));
			gsurf_tab_tree_expand_ancestors(tree, id);
			g_assert_true(gsurf_tab_tree_is_visible(tree, id));
		}
		if (step % 97 == 0)
			assert_invariants(tree);
	}
	assert_invariants(tree);

	/* A large window built in one sync. */
	{
		g_autofree guint64 *ids = g_new(guint64, 2000);
		g_autofree guint64 *openers = g_new(guint64, 2000);
		guint i;

		for (i = 0; i < 2000; i++) {
			ids[i] = 100000 + i;
			openers[i] = i > 0 ? 100000 + g_rand_int_range(rand, 0, (gint32)i) : 0;
		}
		gsurf_tab_tree_sync(tree, ids, openers, 2000);
		g_assert_cmpuint(gsurf_tab_tree_get_n_tabs(tree), ==, 2000);
		assert_invariants(tree);
	}
}

/* ------------------------------------------------------------------ */
/* Module configuration (real tabs.so, no window)                      */
/* ------------------------------------------------------------------ */

static gchar *
tabs_so_path(void)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);

	return g_build_filename(dir, "modules", "tabs.so", NULL);
}

/* The key the module reports for @action, or NULL. */
static const gchar *
key_for(GsurfModule *module, const gchar *action, GPtrArray *entries)
{
	guint i;

	g_ptr_array_set_size(entries, 0);
	gsurf_keybind_provider_list_keybinds(GSURF_KEYBIND_PROVIDER(module), entries);
	for (i = 0; i < entries->len; i++) {
		GsurfKeybindHelp *row = g_ptr_array_index(entries, i);

		if (g_strcmp0(gsurf_keybind_help_get_action(row), action) == 0)
			return gsurf_keybind_help_get_key(row);
	}
	return NULL;
}

static void
configure_with(GsurfModule *module, const gchar *yaml)
{
	g_autoptr(GsurfConfig) config = gsurf_config_new();

	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, NULL));
	gsurf_module_configure(module, config);
}

static void
test_module_config(void)
{
	g_autofree gchar *path = tabs_so_path();
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfModuleManager) manager = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	GsurfModule *module;

	if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
		g_test_skip("tabs.so not built");
		return;
	}
	manager = gsurf_module_manager_new();
	module = gsurf_module_manager_load_module(manager, path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(module);
	entries = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_keybind_help_free);

	/* Defaults, before and without any tabs: block. */
	g_assert_cmpstr(key_for(module, "tab-sidebar-toggle", entries), ==, "Ctrl+Shift+greater");
	g_assert_cmpstr(key_for(module, "tab-tree-collapse", entries), ==, "Ctrl+Shift+bracketleft");
	g_assert_cmpstr(key_for(module, "tab-move-up", entries), ==, "Ctrl+Shift+Page_Up");
	g_assert_cmpstr(key_for(module, "tab-move-down", entries), ==, "Ctrl+Shift+Page_Down");
	g_assert_cmpstr(key_for(module, "tab-new", entries), ==, "Ctrl+t");

	/* Custom keys; an empty key unbinds. */
	configure_with(module,
		"modules:\n  tabs:\n    enabled: true\n"
		"    key_sidebar: \"Ctrl+Shift+period\"\n"
		"    key_collapse: \"\"\n"
		"    key_move_up: \"Alt+Up\"\n");
	g_assert_cmpstr(key_for(module, "tab-sidebar-toggle", entries), ==, "Ctrl+Shift+period");
	g_assert_null(key_for(module, "tab-tree-collapse", entries));
	g_assert_cmpstr(key_for(module, "tab-move-up", entries), ==, "Alt+Up");

	/* A second configure() without the keys restores the defaults. */
	configure_with(module, "modules:\n  tabs:\n    enabled: true\n");
	g_assert_cmpstr(key_for(module, "tab-sidebar-toggle", entries), ==, "Ctrl+Shift+greater");
	g_assert_cmpstr(key_for(module, "tab-tree-collapse", entries), ==, "Ctrl+Shift+bracketleft");
	g_assert_cmpstr(key_for(module, "tab-move-up", entries), ==, "Ctrl+Shift+Page_Up");
	configure_with(module, "modules: {}\n");
	g_assert_cmpstr(key_for(module, "tab-new", entries), ==, "Ctrl+t");

	/* The shipped default config parses without a warning (warnings are
	 * fatal here) and spells out the defaults. */
	{
		g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
		g_autofree gchar *dir = g_path_get_dirname(exe);
		g_autofree gchar *file = g_build_filename(dir, "..", "..", "data",
			"default-config.yaml", NULL);
		g_autofree gchar *yaml = NULL;

		if (g_file_get_contents(file, &yaml, NULL, NULL)) {
			configure_with(module, yaml);
			g_assert_cmpstr(key_for(module, "tab-sidebar-toggle", entries), ==,
				"Ctrl+Shift+greater");
			g_assert_cmpstr(key_for(module, "tab-move-down", entries), ==,
				"Ctrl+Shift+Page_Down");
		}
	}

	/* Activation without a window is a no-op, and so is deactivation. */
	g_assert_true(gsurf_module_activate(module));
	gsurf_module_deactivate(module);
	gsurf_module_deactivate(module);
}

/* Bad values warn and keep the defaults; the rest of the block applies. */
static void
test_module_bad_config(void)
{
	g_autofree gchar *path = tabs_so_path();
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfModuleManager) manager = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	GsurfModule *module;

	if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
		g_test_skip("tabs.so not built");
		return;
	}
	manager = gsurf_module_manager_new();
	module = gsurf_module_manager_load_module(manager, path, &error);
	g_assert_no_error(error);
	entries = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_keybind_help_free);

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*tabs.position*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*tabs.fg_color*not a CSS colour*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
		"*tabs.visible_color*not a CSS colour*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*tabs.sidebar_position*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*tabs.sidebar_width*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*tabs.indent*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*tabs.max_depth*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*tabs.on_close_parent*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
		"*tabs.background_label must be a non-empty string*");
	configure_with(module,
		"modules:\n  tabs:\n    enabled: true\n"
		"    position: sideways\n"
		"    fg_color: \"not-a-colour; } * { color: red\"\n"
		"    visible_color: 42\n"
		"    sidebar_position: middle\n"
		"    sidebar_width: wide\n"
		"    indent: -3\n"
		"    max_depth: 1000\n"
		"    on_close_parent: explode\n"
		"    wmux_groups: false\n"
		"    background_label: \"\"\n"
		"    key_move_down: \"Alt+Down\"\n");
	g_test_assert_expected_messages();
	g_assert_cmpstr(key_for(module, "tab-move-down", entries), ==, "Alt+Down");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/tab-tree/roots-append", test_roots_append);
	g_test_add_func("/gsurf/tab-tree/insert-after-last-descendant", test_insert_after_last_descendant);
	g_test_add_func("/gsurf/tab-tree/nested", test_nested_children);
	g_test_add_func("/gsurf/tab-tree/close-promote", test_close_promote);
	g_test_add_func("/gsurf/tab-tree/close-children", test_close_children);
	g_test_add_func("/gsurf/tab-tree/replace", test_replace);
	g_test_add_func("/gsurf/tab-tree/collapse", test_collapse_expand);
	g_test_add_func("/gsurf/tab-tree/move", test_move_subtrees);
	g_test_add_func("/gsurf/tab-tree/max-depth", test_max_depth);
	g_test_add_func("/gsurf/tab-tree/orphan", test_orphaned_opener);
	g_test_add_func("/gsurf/tab-tree/rebuild", test_rebuild_from_window_order);
	g_test_add_func("/gsurf/tab-tree/move-to", test_move_to);
	g_test_add_func("/gsurf/tab-tree/rows-flat", test_rows_flat);
	g_test_add_func("/gsurf/tab-tree/rows-grouped", test_rows_grouped);
	g_test_add_func("/gsurf/tab-tree/stress", test_stress);
	g_test_add_func("/gsurf/tabs/config", test_module_config);
	g_test_add_func("/gsurf/tabs/bad-config", test_module_bad_config);
	return g_test_run();
}
