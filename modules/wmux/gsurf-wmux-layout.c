/*
 * gsurf-wmux-layout.c - The wmux pane layout model and window state
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The tree is made of Cells. A leaf cell holds a pane (id, payload, use
 * stamp); a split cell holds two or more children. Every cell has a
 * weight: its share of the parent split's extent, in units of
 * GSURF_WMUX_LAYOUT_SCALE (the root's weight is the whole scale). All
 * operations keep three invariants, checked by gsurf_wmux_layout_check()
 * and the randomized test:
 *   - the weights of a split's children sum to exactly SCALE, each >= 1;
 *   - a split has at least two children;
 *   - a split's child is never a split of the same direction.
 * A hash from pane id to leaf cell gives O(1) lookup; the layout order is
 * the in-order sequence of leaves.
 */

#include "gsurf-wmux-layout.h"

#include <string.h>

G_DEFINE_QUARK(gsurf-wmux-layout-error-quark, gsurf_wmux_layout_error)

/* The virtual square that neighbor() measures on: large enough that
 * every realistic weight gives a non-empty, exactly adjacent rectangle. */
#define VIRTUAL_SIZE (1 << 30)

/* ------------------------------------------------------------------ */
/* Cells                                                               */
/* ------------------------------------------------------------------ */

typedef struct _Cell Cell;

struct _Cell {
	Cell           *parent;
	guint           weight;     /* share of the parent split, of SCALE */
	gboolean        is_split;
	/* split */
	GsurfWmuxSplit  dir;
	GPtrArray      *children;   /* Cell*, owned */
	/* leaf */
	guint           pane;
	guint64         payload;
	guint64         stamp;
};

struct _GsurfWmuxLayout {
	Cell       *root;
	GHashTable *panes;     /* pane id -> leaf Cell*, borrowed */
	guint       next_id;   /* GSURF_WMUX_PANE_NONE once exhausted */
	guint       active;
	guint       last;
	guint       zoomed;
	guint       min_pct;
	guint       main_pct;
	guint64     clock;     /* source of use stamps */
	gint        preset;    /* last preset applied, -1 none */
};

static void cell_free(gpointer data);

static Cell *
leaf_new(guint pane)
{
	Cell *c = g_new0(Cell, 1);

	c->pane = pane;
	c->weight = GSURF_WMUX_LAYOUT_SCALE;
	return c;
}

static Cell *
split_new(GsurfWmuxSplit dir)
{
	Cell *c = g_new0(Cell, 1);

	c->is_split = TRUE;
	c->dir = dir;
	c->children = g_ptr_array_new_with_free_func(cell_free);
	c->weight = GSURF_WMUX_LAYOUT_SCALE;
	c->pane = GSURF_WMUX_PANE_NONE;
	return c;
}

static void
cell_free(gpointer data)
{
	Cell *c = data;

	if (c == NULL)
		return;
	if (c->children != NULL)
		g_ptr_array_unref(c->children);
	g_free(c);
}

static Cell *
child_at(Cell *split, guint i)
{
	return g_ptr_array_index(split->children, i);
}

/* Append @child to @split (weight left as set by the caller). */
static void
add_child(Cell *split, Cell *child)
{
	child->parent = split;
	g_ptr_array_add(split->children, child);
}

/* Index of @c in its parent's children. */
static guint
child_index(Cell *c)
{
	guint i;

	for (i = 0; i < c->parent->children->len; i++)
		if (child_at(c->parent, i) == c)
			return i;
	g_assert_not_reached();
	return 0;
}

/* Put @replacement where @old is (as the root or in old's parent). The
 * replacement takes @old's weight. @old is detached, not freed. */
static void
replace_cell(GsurfWmuxLayout *layout, Cell *old, Cell *replacement)
{
	Cell *parent = old->parent;

	replacement->weight = old->weight;
	replacement->parent = parent;
	if (parent == NULL) {
		layout->root = replacement;
		replacement->weight = GSURF_WMUX_LAYOUT_SCALE;
	} else {
		parent->children->pdata[child_index(old)] = replacement;
	}
	old->parent = NULL;
}

/* First / last leaf of a subtree. */
static Cell *
first_leaf(Cell *c)
{
	while (c->is_split)
		c = child_at(c, 0);
	return c;
}

static Cell *
last_leaf(Cell *c)
{
	while (c->is_split)
		c = child_at(c, c->children->len - 1);
	return c;
}

/* Collect the leaves of @c in layout order. */
static void
collect_leaves(Cell *c, GPtrArray *out)
{
	guint i;

	if (!c->is_split) {
		g_ptr_array_add(out, c);
		return;
	}
	for (i = 0; i < c->children->len; i++)
		collect_leaves(child_at(c, i), out);
}

static GPtrArray *
leaves(GsurfWmuxLayout *layout)
{
	GPtrArray *out = g_ptr_array_new();

	collect_leaves(layout->root, out);
	return out;
}

static Cell *
find_leaf(GsurfWmuxLayout *layout, guint pane)
{
	if (pane == GSURF_WMUX_PANE_NONE)
		return NULL;
	return g_hash_table_lookup(layout->panes, GUINT_TO_POINTER(pane));
}

/* Rebuild the id -> leaf hash from the tree. */
static void
reindex(GsurfWmuxLayout *layout)
{
	g_autoptr(GPtrArray) all = leaves(layout);
	guint i;

	g_hash_table_remove_all(layout->panes);
	for (i = 0; i < all->len; i++) {
		Cell *c = g_ptr_array_index(all, i);

		g_hash_table_insert(layout->panes, GUINT_TO_POINTER(c->pane), c);
	}
}

/* ------------------------------------------------------------------ */
/* Weights                                                             */
/* ------------------------------------------------------------------ */

/* Give @k weights summing to @total equal shares (the first ones get the
 * remainder). */
static void
equal_weights(guint *out, guint k, guint total)
{
	guint i;

	for (i = 0; i < k; i++)
		out[i] = total / k + (i < total % k ? 1 : 0);
}

/* Scale @k weights in @w (sum @sum) to sum exactly @total, by cumulative
 * rounding. A weight that would round to 0 is raised to 1, taking the
 * unit from the largest. Returns FALSE when @total < @k (impossible). */
static gboolean
scale_weights(guint *w, guint k, guint64 sum, guint total)
{
	guint64 cum = 0;
	guint64 prev = 0;
	guint i;

	if (k == 0 || sum == 0 || total < k)
		return FALSE;
	for (i = 0; i < k; i++) {
		guint64 next;

		cum += w[i];
		next = (cum * total + sum / 2) / sum;
		w[i] = (guint)(next - prev);
		prev = next;
	}
	for (i = 0; i < k; i++) {
		if (w[i] == 0) {
			guint j;
			guint big = 0;

			for (j = 1; j < k; j++)
				if (w[j] > w[big])
					big = j;
			w[big]--;
			w[i] = 1;
		}
	}
	return TRUE;
}

/* Replace the same-direction split child @c by its children, scaled to
 * @c's weight. Keeps the parent's weights summing to SCALE. */
static void
flatten(Cell *c)
{
	Cell *parent = c->parent;
	guint idx = child_index(c);
	guint k = c->children->len;
	g_autofree guint *w = g_new(guint, k);
	guint i;

	for (i = 0; i < k; i++)
		w[i] = child_at(c, i)->weight;
	/* c->weight >= k is not guaranteed for tiny cells: when it is too
	 * small to give each child a unit, take units from the siblings. */
	if (c->weight < k) {
		guint need = k - c->weight;

		for (i = 0; i < parent->children->len && need > 0; i++) {
			Cell *s = child_at(parent, i);

			while (s != c && s->weight > 1 && need > 0) {
				s->weight--;
				c->weight++;
				need--;
			}
		}
	}
	scale_weights(w, k, GSURF_WMUX_LAYOUT_SCALE, c->weight);

	/* Move the grandchildren into the parent at c's place. */
	g_ptr_array_steal_index(parent->children, idx);
	for (i = 0; i < k; i++) {
		Cell *g = child_at(c, i);

		g->weight = w[i];
		g->parent = parent;
		g_ptr_array_insert(parent->children, (gint)(idx + i), g);
	}
	/* The children now belong to the parent; free only the shell. */
	g_ptr_array_set_free_func(c->children, NULL);
	cell_free(c);
}

/* Flatten every same-direction split child of @split. */
static void
flatten_children(Cell *split)
{
	guint i = 0;

	while (i < split->children->len) {
		Cell *c = child_at(split, i);

		if (c->is_split && c->dir == split->dir) {
			flatten(c);
			continue;   /* re-check the moved-in children */
		}
		i++;
	}
}

/* The extent of @c along @axis as a fraction of the whole area. */
static gdouble
extent_of(Cell *c, GsurfWmuxSplit axis)
{
	gdouble e = 1.0;

	for (; c->parent != NULL; c = c->parent)
		if (c->parent->dir == axis)
			e *= (gdouble)c->weight / GSURF_WMUX_LAYOUT_SCALE;
	return e;
}

/* ------------------------------------------------------------------ */
/* Life cycle and settings                                             */
/* ------------------------------------------------------------------ */

static GsurfWmuxLayout *
layout_alloc(void)
{
	GsurfWmuxLayout *layout = g_new0(GsurfWmuxLayout, 1);

	layout->panes = g_hash_table_new(g_direct_hash, g_direct_equal);
	layout->active = GSURF_WMUX_PANE_NONE;
	layout->last = GSURF_WMUX_PANE_NONE;
	layout->zoomed = GSURF_WMUX_PANE_NONE;
	layout->min_pct = 10;
	layout->main_pct = 60;
	layout->preset = -1;
	return layout;
}

/* Mark @pane used now. */
static void
touch(GsurfWmuxLayout *layout, guint pane)
{
	Cell *c = find_leaf(layout, pane);

	if (c != NULL)
		c->stamp = ++layout->clock;
}

GsurfWmuxLayout *
gsurf_wmux_layout_new(void)
{
	GsurfWmuxLayout *layout = layout_alloc();

	layout->root = leaf_new(0);
	layout->next_id = 1;
	reindex(layout);
	layout->active = 0;
	touch(layout, 0);
	return layout;
}

static Cell *
cell_copy(Cell *c, Cell *parent)
{
	Cell *copy = g_new0(Cell, 1);
	guint i;

	*copy = *c;
	copy->parent = parent;
	if (c->is_split) {
		copy->children = g_ptr_array_new_with_free_func(cell_free);
		for (i = 0; i < c->children->len; i++)
			g_ptr_array_add(copy->children, cell_copy(child_at(c, i), copy));
	}
	return copy;
}

GsurfWmuxLayout *
gsurf_wmux_layout_copy(GsurfWmuxLayout *layout)
{
	GsurfWmuxLayout *copy;
	GHashTable *panes;

	g_return_val_if_fail(layout != NULL, NULL);
	copy = layout_alloc();
	panes = copy->panes;
	*copy = *layout;
	copy->panes = panes;
	copy->root = cell_copy(layout->root, NULL);
	reindex(copy);
	return copy;
}

void
gsurf_wmux_layout_free(GsurfWmuxLayout *layout)
{
	if (layout == NULL)
		return;
	cell_free(layout->root);
	g_hash_table_destroy(layout->panes);
	g_free(layout);
}

void
gsurf_wmux_layout_set_min_pct(GsurfWmuxLayout *layout, guint pct)
{
	g_return_if_fail(layout != NULL);
	layout->min_pct = MIN(pct, 50);
}

guint
gsurf_wmux_layout_get_min_pct(GsurfWmuxLayout *layout)
{
	g_return_val_if_fail(layout != NULL, 0);
	return layout->min_pct;
}

void
gsurf_wmux_layout_set_main_pct(GsurfWmuxLayout *layout, guint pct)
{
	g_return_if_fail(layout != NULL);
	layout->main_pct = CLAMP(pct, 10, 90);
}

guint
gsurf_wmux_layout_get_main_pct(GsurfWmuxLayout *layout)
{
	g_return_val_if_fail(layout != NULL, 0);
	return layout->main_pct;
}

/* ------------------------------------------------------------------ */
/* Panes                                                               */
/* ------------------------------------------------------------------ */

guint
gsurf_wmux_layout_get_n_panes(GsurfWmuxLayout *layout)
{
	g_return_val_if_fail(layout != NULL, 0);
	return g_hash_table_size(layout->panes);
}

guint
gsurf_wmux_layout_get_nth_pane(GsurfWmuxLayout *layout, guint index)
{
	g_autoptr(GPtrArray) all = NULL;

	g_return_val_if_fail(layout != NULL, GSURF_WMUX_PANE_NONE);
	all = leaves(layout);
	if (index >= all->len)
		return GSURF_WMUX_PANE_NONE;
	return ((Cell *)g_ptr_array_index(all, index))->pane;
}

gint
gsurf_wmux_layout_get_pane_index(GsurfWmuxLayout *layout, guint pane)
{
	g_autoptr(GPtrArray) all = NULL;
	guint i;

	g_return_val_if_fail(layout != NULL, -1);
	if (find_leaf(layout, pane) == NULL)
		return -1;
	all = leaves(layout);
	for (i = 0; i < all->len; i++)
		if (((Cell *)g_ptr_array_index(all, i))->pane == pane)
			return (gint)i;
	return -1;
}

gboolean
gsurf_wmux_layout_has_pane(GsurfWmuxLayout *layout, guint pane)
{
	g_return_val_if_fail(layout != NULL, FALSE);
	return find_leaf(layout, pane) != NULL;
}

GArray *
gsurf_wmux_layout_list_panes(GsurfWmuxLayout *layout)
{
	g_autoptr(GPtrArray) all = NULL;
	GArray *out;
	guint i;

	g_return_val_if_fail(layout != NULL, NULL);
	all = leaves(layout);
	out = g_array_sized_new(FALSE, FALSE, sizeof(guint), all->len);
	for (i = 0; i < all->len; i++)
		g_array_append_val(out, ((Cell *)g_ptr_array_index(all, i))->pane);
	return out;
}

gboolean
gsurf_wmux_layout_set_payload(GsurfWmuxLayout *layout, guint pane, guint64 payload)
{
	Cell *c;

	g_return_val_if_fail(layout != NULL, FALSE);
	c = find_leaf(layout, pane);
	if (c == NULL)
		return FALSE;
	c->payload = payload;
	return TRUE;
}

guint64
gsurf_wmux_layout_get_payload(GsurfWmuxLayout *layout, guint pane)
{
	Cell *c;

	g_return_val_if_fail(layout != NULL, 0);
	c = find_leaf(layout, pane);
	return c != NULL ? c->payload : 0;
}

guint
gsurf_wmux_layout_find_payload(GsurfWmuxLayout *layout, guint64 payload)
{
	g_autoptr(GPtrArray) all = NULL;
	guint i;

	g_return_val_if_fail(layout != NULL, GSURF_WMUX_PANE_NONE);
	all = leaves(layout);
	for (i = 0; i < all->len; i++) {
		Cell *c = g_ptr_array_index(all, i);

		if (c->payload == payload)
			return c->pane;
	}
	return GSURF_WMUX_PANE_NONE;
}

/* ------------------------------------------------------------------ */
/* Active, last, stamps, zoom                                          */
/* ------------------------------------------------------------------ */

guint
gsurf_wmux_layout_get_active(GsurfWmuxLayout *layout)
{
	g_return_val_if_fail(layout != NULL, GSURF_WMUX_PANE_NONE);
	return layout->active;
}

gboolean
gsurf_wmux_layout_set_active(GsurfWmuxLayout *layout, guint pane)
{
	g_return_val_if_fail(layout != NULL, FALSE);
	if (find_leaf(layout, pane) == NULL)
		return FALSE;
	if (pane != layout->active) {
		layout->last = layout->active;
		layout->active = pane;
		if (layout->zoomed != GSURF_WMUX_PANE_NONE && layout->zoomed != pane)
			layout->zoomed = GSURF_WMUX_PANE_NONE;
	}
	touch(layout, pane);
	return TRUE;
}

guint
gsurf_wmux_layout_get_last(GsurfWmuxLayout *layout)
{
	g_return_val_if_fail(layout != NULL, GSURF_WMUX_PANE_NONE);
	return layout->last;
}

gboolean
gsurf_wmux_layout_set_last(GsurfWmuxLayout *layout, guint pane)
{
	g_return_val_if_fail(layout != NULL, FALSE);
	if (pane == GSURF_WMUX_PANE_NONE) {
		layout->last = pane;
		return TRUE;
	}
	if (find_leaf(layout, pane) == NULL || pane == layout->active)
		return FALSE;
	layout->last = pane;
	return TRUE;
}

guint64
gsurf_wmux_layout_get_stamp(GsurfWmuxLayout *layout, guint pane)
{
	Cell *c;

	g_return_val_if_fail(layout != NULL, 0);
	c = find_leaf(layout, pane);
	return c != NULL ? c->stamp : 0;
}

gboolean
gsurf_wmux_layout_toggle_zoom(GsurfWmuxLayout *layout)
{
	g_return_val_if_fail(layout != NULL, FALSE);
	if (layout->zoomed != GSURF_WMUX_PANE_NONE) {
		layout->zoomed = GSURF_WMUX_PANE_NONE;
		return FALSE;
	}
	if (gsurf_wmux_layout_get_n_panes(layout) < 2)
		return FALSE;
	layout->zoomed = layout->active;
	return TRUE;
}

gboolean
gsurf_wmux_layout_set_zoomed(GsurfWmuxLayout *layout, guint pane)
{
	g_return_val_if_fail(layout != NULL, FALSE);
	if (pane != GSURF_WMUX_PANE_NONE && find_leaf(layout, pane) == NULL)
		return FALSE;
	layout->zoomed = pane;
	return TRUE;
}

guint
gsurf_wmux_layout_get_zoomed(GsurfWmuxLayout *layout)
{
	g_return_val_if_fail(layout != NULL, GSURF_WMUX_PANE_NONE);
	return layout->zoomed;
}

/* ------------------------------------------------------------------ */
/* Split and remove                                                    */
/* ------------------------------------------------------------------ */

guint
gsurf_wmux_layout_split(GsurfWmuxLayout *layout, guint pane, GsurfWmuxSplit split,
                        gboolean before, guint pct, GError **error)
{
	Cell *leaf;
	Cell *parent;
	Cell *fresh;
	gdouble e;

	g_return_val_if_fail(layout != NULL, GSURF_WMUX_PANE_NONE);

	/* Checks first, so a refused split changes nothing. */
	leaf = find_leaf(layout, pane);
	if (leaf == NULL) {
		g_set_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_NOT_FOUND,
			"no pane %u", pane);
		return GSURF_WMUX_PANE_NONE;
	}
	if (pct < 1 || pct > 99 || (split != GSURF_WMUX_SPLIT_H && split != GSURF_WMUX_SPLIT_V)) {
		g_set_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_INVALID,
			"split size %u%% is not between 1 and 99", pct);
		return GSURF_WMUX_PANE_NONE;
	}
	if (layout->next_id == GSURF_WMUX_PANE_NONE) {
		g_set_error_literal(error, GSURF_WMUX_LAYOUT_ERROR,
			GSURF_WMUX_LAYOUT_ERROR_IDS_EXHAUSTED, "no pane ids left");
		return GSURF_WMUX_PANE_NONE;
	}
	/* Both halves must stay at least min_pct of the window along the
	 * split axis (a small epsilon absorbs the weight rounding). */
	e = extent_of(leaf, split) * 100.0;
	if (layout->min_pct > 0 &&
	    (e * pct / 100.0 < layout->min_pct - 0.01 ||
	     e * (100 - pct) / 100.0 < layout->min_pct - 0.01)) {
		g_set_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_NO_SPACE,
			"no space for a new pane (minimum %u%%)", layout->min_pct);
		return GSURF_WMUX_PANE_NONE;
	}
	parent = leaf->parent;
	if (parent != NULL && parent->dir == split && leaf->weight < 2) {
		g_set_error_literal(error, GSURF_WMUX_LAYOUT_ERROR,
			GSURF_WMUX_LAYOUT_ERROR_NO_SPACE, "no space for a new pane");
		return GSURF_WMUX_PANE_NONE;
	}

	fresh = leaf_new(layout->next_id);
	layout->next_id = layout->next_id >= GSURF_WMUX_PANE_NONE - 1
		? GSURF_WMUX_PANE_NONE : layout->next_id + 1;

	if (parent != NULL && parent->dir == split) {
		/* Same direction as the parent: the new pane becomes a
		 * sibling and takes @pct of the old pane's weight. */
		guint w = leaf->weight;
		guint wn = CLAMP((w * pct + 50) / 100, 1, w - 1);
		guint idx = child_index(leaf);

		leaf->weight = w - wn;
		fresh->weight = wn;
		fresh->parent = parent;
		g_ptr_array_insert(parent->children, (gint)(before ? idx : idx + 1), fresh);
	} else {
		/* Otherwise a new split takes the pane's place. */
		Cell *s = split_new(split);
		guint wn = (GSURF_WMUX_LAYOUT_SCALE * pct + 50) / 100;

		replace_cell(layout, leaf, s);
		leaf->weight = GSURF_WMUX_LAYOUT_SCALE - wn;
		fresh->weight = wn;
		if (before) {
			add_child(s, fresh);
			add_child(s, leaf);
		} else {
			add_child(s, leaf);
			add_child(s, fresh);
		}
	}
	g_hash_table_insert(layout->panes, GUINT_TO_POINTER(fresh->pane), fresh);
	layout->zoomed = GSURF_WMUX_PANE_NONE;
	return fresh->pane;
}

gboolean
gsurf_wmux_layout_remove(GsurfWmuxLayout *layout, guint pane, GError **error)
{
	Cell *leaf;
	Cell *parent;
	Cell *receiver;
	Cell *nearest;
	guint idx;

	g_return_val_if_fail(layout != NULL, FALSE);
	leaf = find_leaf(layout, pane);
	if (leaf == NULL) {
		g_set_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_NOT_FOUND,
			"no pane %u", pane);
		return FALSE;
	}
	if (leaf->parent == NULL) {
		g_set_error_literal(error, GSURF_WMUX_LAYOUT_ERROR,
			GSURF_WMUX_LAYOUT_ERROR_LAST_PANE, "cannot remove the last pane");
		return FALSE;
	}

	/* The cell before (or after, for the first) inherits the space. */
	parent = leaf->parent;
	idx = child_index(leaf);
	receiver = child_at(parent, idx > 0 ? idx - 1 : 1);
	receiver->weight += leaf->weight;
	nearest = idx > 0 ? last_leaf(receiver) : first_leaf(receiver);
	g_hash_table_remove(layout->panes, GUINT_TO_POINTER(pane));
	g_ptr_array_remove_index(parent->children, idx);   /* frees the leaf */

	/* A split left with one child collapses into it; a collapsed
	 * split's child may then nest in a same-direction grandparent. */
	if (parent->children->len == 1) {
		Cell *only = g_ptr_array_steal_index(parent->children, 0);

		replace_cell(layout, parent, only);
		cell_free(parent);
		if (only->is_split && only->parent != NULL && only->parent->dir == only->dir)
			flatten(only);
	}

	/* Active and last pane bookkeeping. */
	if (layout->active == pane) {
		guint next = layout->last != GSURF_WMUX_PANE_NONE && layout->last != pane
			? layout->last : nearest->pane;

		layout->active = next;
		layout->last = GSURF_WMUX_PANE_NONE;
		touch(layout, next);
	} else if (layout->last == pane) {
		layout->last = GSURF_WMUX_PANE_NONE;
	}
	layout->zoomed = GSURF_WMUX_PANE_NONE;
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

/* Lay out the children of @split inside (x, y, w, h): the child's
 * rectangle goes to @out[i]. Cumulative floor rounding over the extent
 * left after the gaps makes the sizes sum exactly. When the gaps do not
 * fit, they shrink evenly. */
typedef struct {
	gint x;
	gint y;
	gint w;
	gint h;
} Box;

static void
layout_children(Cell *split, Box box, gint gap, Box *out)
{
	guint k = split->children->len;
	gint extent = split->dir == GSURF_WMUX_SPLIT_H ? box.w : box.h;
	gint g = MAX(gap, 0);
	gint avail;
	guint64 cum = 0;
	guint i;

	if (k > 1 && (gint64)g * (k - 1) > extent)
		g = extent / (gint)(k - 1);
	avail = extent - g * (gint)(k - 1);
	for (i = 0; i < k; i++) {
		gint start = (gint)(((guint64)avail * cum) / GSURF_WMUX_LAYOUT_SCALE);
		gint end;

		cum += child_at(split, i)->weight;
		end = (gint)(((guint64)avail * cum) / GSURF_WMUX_LAYOUT_SCALE);
		out[i] = box;
		if (split->dir == GSURF_WMUX_SPLIT_H) {
			out[i].x = box.x + start + (gint)i * g;
			out[i].w = end - start;
		} else {
			out[i].y = box.y + start + (gint)i * g;
			out[i].h = end - start;
		}
	}
}

static void
geometry_cell(Cell *c, Box box, gint gap, GArray *out)
{
	g_autofree Box *boxes = NULL;
	guint i;

	if (!c->is_split) {
		GsurfWmuxRect r = { c->pane, box.x, box.y, box.w, box.h };

		g_array_append_val(out, r);
		return;
	}
	boxes = g_new(Box, c->children->len);
	layout_children(c, box, gap, boxes);
	for (i = 0; i < c->children->len; i++)
		geometry_cell(child_at(c, i), boxes[i], gap, out);
}

/* The rectangles ignoring zoom. */
static GArray *
full_geometry(GsurfWmuxLayout *layout, gint width, gint height, gint gap)
{
	GArray *out = g_array_new(FALSE, FALSE, sizeof(GsurfWmuxRect));
	Box box = { 0, 0, MAX(width, 0), MAX(height, 0) };

	geometry_cell(layout->root, box, gap, out);
	return out;
}

GArray *
gsurf_wmux_layout_geometry(GsurfWmuxLayout *layout, gint width, gint height, gint gap)
{
	g_return_val_if_fail(layout != NULL, NULL);
	if (layout->zoomed != GSURF_WMUX_PANE_NONE) {
		GArray *out = g_array_new(FALSE, FALSE, sizeof(GsurfWmuxRect));
		GsurfWmuxRect r = { layout->zoomed, 0, 0, MAX(width, 0), MAX(height, 0) };

		g_array_append_val(out, r);
		return out;
	}
	return full_geometry(layout, width, height, gap);
}

/* The box of cell @c (by descending from the root). */
static Box
cell_box(GsurfWmuxLayout *layout, Cell *c, gint width, gint height, gint gap)
{
	g_autoptr(GPtrArray) path = g_ptr_array_new();
	Box box = { 0, 0, MAX(width, 0), MAX(height, 0) };
	Cell *p;
	guint i;

	for (p = c; p->parent != NULL; p = p->parent)
		g_ptr_array_insert(path, 0, p);
	for (i = 0; i < path->len; i++) {
		Cell *step = g_ptr_array_index(path, i);
		g_autofree Box *boxes = g_new(Box, step->parent->children->len);

		layout_children(step->parent, box, gap, boxes);
		box = boxes[child_index(step)];
	}
	return box;
}

/* ------------------------------------------------------------------ */
/* Neighbors and order                                                 */
/* ------------------------------------------------------------------ */

guint
gsurf_wmux_layout_neighbor(GsurfWmuxLayout *layout, guint pane,
                           GsurfWmuxDirection direction, gboolean wrap)
{
	g_autoptr(GArray) rects = NULL;
	const GsurfWmuxRect *me = NULL;
	guint best = GSURF_WMUX_PANE_NONE;
	gint best_overlap = 0;
	guint64 best_stamp = 0;
	guint pass;
	guint i;

	g_return_val_if_fail(layout != NULL, GSURF_WMUX_PANE_NONE);
	if (find_leaf(layout, pane) == NULL)
		return GSURF_WMUX_PANE_NONE;
	rects = full_geometry(layout, VIRTUAL_SIZE, VIRTUAL_SIZE, 0);
	for (i = 0; i < rects->len; i++)
		if (g_array_index(rects, GsurfWmuxRect, i).pane == pane)
			me = &g_array_index(rects, GsurfWmuxRect, i);

	/* Pass 0: panes whose facing edge touches ours. Pass 1 (wrap): panes
	 * on the opposite edge of the area. */
	for (pass = 0; pass < (wrap ? 2u : 1u) && best == GSURF_WMUX_PANE_NONE; pass++) {
		for (i = 0; i < rects->len; i++) {
			const GsurfWmuxRect *r = &g_array_index(rects, GsurfWmuxRect, i);
			gboolean touches = FALSE;
			gint overlap;
			guint64 stamp;

			if (r->pane == pane)
				continue;
			switch (direction) {
			case GSURF_WMUX_DIR_LEFT:
				touches = pass == 0 ? r->x + r->width == me->x
				                    : r->x + r->width == VIRTUAL_SIZE;
				break;
			case GSURF_WMUX_DIR_RIGHT:
				touches = pass == 0 ? r->x == me->x + me->width : r->x == 0;
				break;
			case GSURF_WMUX_DIR_UP:
				touches = pass == 0 ? r->y + r->height == me->y
				                    : r->y + r->height == VIRTUAL_SIZE;
				break;
			case GSURF_WMUX_DIR_DOWN:
				touches = pass == 0 ? r->y == me->y + me->height : r->y == 0;
				break;
			default:
				break;
			}
			if (!touches)
				continue;
			if (direction == GSURF_WMUX_DIR_LEFT || direction == GSURF_WMUX_DIR_RIGHT)
				overlap = MIN(me->y + me->height, r->y + r->height) - MAX(me->y, r->y);
			else
				overlap = MIN(me->x + me->width, r->x + r->width) - MAX(me->x, r->x);
			if (overlap <= 0)
				continue;
			/* Most overlap; then most recently used; then order. */
			stamp = find_leaf(layout, r->pane)->stamp;
			if (best == GSURF_WMUX_PANE_NONE || overlap > best_overlap ||
			    (overlap == best_overlap && stamp > best_stamp)) {
				best = r->pane;
				best_overlap = overlap;
				best_stamp = stamp;
			}
		}
	}
	return best;
}

/* The pane @delta places from @pane in layout order, wrapping. */
static guint
step_pane(GsurfWmuxLayout *layout, guint pane, gint delta)
{
	g_autoptr(GPtrArray) all = NULL;
	gint idx = gsurf_wmux_layout_get_pane_index(layout, pane);
	gint n;

	if (idx < 0)
		return GSURF_WMUX_PANE_NONE;
	all = leaves(layout);
	n = (gint)all->len;
	idx = ((idx + delta) % n + n) % n;
	return ((Cell *)g_ptr_array_index(all, idx))->pane;
}

guint
gsurf_wmux_layout_next(GsurfWmuxLayout *layout, guint pane)
{
	g_return_val_if_fail(layout != NULL, GSURF_WMUX_PANE_NONE);
	return step_pane(layout, pane, 1);
}

guint
gsurf_wmux_layout_prev(GsurfWmuxLayout *layout, guint pane)
{
	g_return_val_if_fail(layout != NULL, GSURF_WMUX_PANE_NONE);
	return step_pane(layout, pane, -1);
}

/* ------------------------------------------------------------------ */
/* Swap and rotate                                                     */
/* ------------------------------------------------------------------ */

/* Exchange what two leaves hold (id, payload, stamp). */
static void
swap_contents(Cell *a, Cell *b)
{
	guint pane = a->pane;
	guint64 payload = a->payload;
	guint64 stamp = a->stamp;

	a->pane = b->pane;
	a->payload = b->payload;
	a->stamp = b->stamp;
	b->pane = pane;
	b->payload = payload;
	b->stamp = stamp;
}

gboolean
gsurf_wmux_layout_swap(GsurfWmuxLayout *layout, guint a, guint b)
{
	Cell *ca;
	Cell *cb;

	g_return_val_if_fail(layout != NULL, FALSE);
	ca = find_leaf(layout, a);
	cb = find_leaf(layout, b);
	if (ca == NULL || cb == NULL)
		return FALSE;
	if (ca != cb) {
		swap_contents(ca, cb);
		g_hash_table_insert(layout->panes, GUINT_TO_POINTER(ca->pane), ca);
		g_hash_table_insert(layout->panes, GUINT_TO_POINTER(cb->pane), cb);
	}
	layout->zoomed = GSURF_WMUX_PANE_NONE;
	return TRUE;
}

void
gsurf_wmux_layout_rotate(GsurfWmuxLayout *layout, gboolean down)
{
	g_autoptr(GPtrArray) all = NULL;
	guint i;

	g_return_if_fail(layout != NULL);
	all = leaves(layout);
	/* Rotating by successive swaps moves every pane one place: for -U
	 * the first pane bubbles to the end, for -D the last to the front. */
	if (!down) {
		for (i = 0; i + 1 < all->len; i++)
			swap_contents(g_ptr_array_index(all, i), g_ptr_array_index(all, i + 1));
	} else {
		for (i = all->len; i > 1; i--)
			swap_contents(g_ptr_array_index(all, i - 1), g_ptr_array_index(all, i - 2));
	}
	reindex(layout);
	layout->zoomed = GSURF_WMUX_PANE_NONE;
}

/* ------------------------------------------------------------------ */
/* Resize, borders, spread                                             */
/* ------------------------------------------------------------------ */

/* How many weight units child @s of its split may give up so that no
 * pane inside it drops below min_pct along the split's axis (panes
 * shrink proportionally with their cell). */
static guint
max_shrink(GsurfWmuxLayout *layout, Cell *s)
{
	g_autoptr(GPtrArray) inner = g_ptr_array_new();
	GsurfWmuxSplit axis = s->parent->dir;
	gdouble min_frac = layout->min_pct / 100.0;
	gdouble need = 1.0;   /* weight units the cell must keep */
	guint i;

	if (layout->min_pct == 0)
		return s->weight - 1;
	collect_leaves(s, inner);
	for (i = 0; i < inner->len; i++) {
		gdouble e = extent_of(g_ptr_array_index(inner, i), axis);
		gdouble req = e > 0.0 ? min_frac * s->weight / e : (gdouble)s->weight;

		need = MAX(need, req);
	}
	/* Round up (without libm), forgiving float noise. */
	need -= 1e-9;
	if (need >= (gdouble)s->weight)
		return 0;
	return s->weight - (guint)need - ((gdouble)(guint)need < need ? 1u : 0u);
}

/* Move the border between children @bi and @bi + 1 of @split by @dw
 * units (positive grows @bi), clamped by max_shrink(). Returns whether
 * anything moved. */
static gboolean
shift_border(GsurfWmuxLayout *layout, Cell *split, guint bi, gint64 dw)
{
	Cell *grow = child_at(split, dw > 0 ? bi : bi + 1);
	Cell *shrink = child_at(split, dw > 0 ? bi + 1 : bi);
	guint64 amount = (guint64)(dw > 0 ? dw : -dw);
	guint allowed = max_shrink(layout, shrink);

	amount = MIN(amount, allowed);
	if (amount == 0)
		return FALSE;
	grow->weight += (guint)amount;
	shrink->weight -= (guint)amount;
	return TRUE;
}

gboolean
gsurf_wmux_layout_resize(GsurfWmuxLayout *layout, guint pane, GsurfWmuxDirection direction,
                         guint pct)
{
	GsurfWmuxSplit axis;
	gboolean positive;
	Cell *c;

	g_return_val_if_fail(layout != NULL, FALSE);
	c = find_leaf(layout, pane);
	if (c == NULL || pct == 0)
		return FALSE;
	axis = direction == GSURF_WMUX_DIR_LEFT || direction == GSURF_WMUX_DIR_RIGHT
		? GSURF_WMUX_SPLIT_H : GSURF_WMUX_SPLIT_V;
	positive = direction == GSURF_WMUX_DIR_RIGHT || direction == GSURF_WMUX_DIR_DOWN;

	/* The nearest ancestor split along the axis owns the border. */
	for (; c->parent != NULL; c = c->parent) {
		Cell *split = c->parent;
		guint idx;
		guint k;
		guint bi;
		gdouble span;
		gint64 dw;
		gboolean moved;

		if (split->dir != axis)
			continue;
		idx = child_index(c);
		k = split->children->len;
		if (positive)
			bi = idx < k - 1 ? idx : idx - 1;
		else
			bi = idx > 0 ? idx - 1 : idx;

		/* Convert window percent to the split's weight units. */
		span = extent_of(split, axis);
		dw = (gint64)((pct / 100.0) / span * GSURF_WMUX_LAYOUT_SCALE + 0.5);
		if (dw < 1)
			dw = 1;
		moved = shift_border(layout, split, bi, positive ? dw : -dw);
		if (moved)
			layout->zoomed = GSURF_WMUX_PANE_NONE;
		return moved;
	}
	return FALSE;
}

gboolean
gsurf_wmux_layout_spread_evenly(GsurfWmuxLayout *layout, guint pane)
{
	g_autofree guint *w = NULL;
	Cell *c;
	Cell *split;
	guint i;

	g_return_val_if_fail(layout != NULL, FALSE);
	c = find_leaf(layout, pane);
	if (c == NULL || c->parent == NULL)
		return FALSE;
	split = c->parent;
	w = g_new(guint, split->children->len);
	equal_weights(w, split->children->len, GSURF_WMUX_LAYOUT_SCALE);
	for (i = 0; i < split->children->len; i++)
		child_at(split, i)->weight = w[i];
	layout->zoomed = GSURF_WMUX_PANE_NONE;
	return TRUE;
}

/* Hit-test the borders of @c's subtree, top-down. */
static gboolean
find_border_in(Cell *c, Box box, gint gap, gint x, gint y, GsurfWmuxBorder *out)
{
	g_autofree Box *boxes = NULL;
	guint i;

	if (!c->is_split)
		return FALSE;
	boxes = g_new(Box, c->children->len);
	layout_children(c, box, gap, boxes);
	for (i = 0; i + 1 < c->children->len; i++) {
		gboolean h = c->dir == GSURF_WMUX_SPLIT_H;
		gint a = h ? boxes[i].x + boxes[i].w : boxes[i].y + boxes[i].h;
		gint b = h ? boxes[i + 1].x : boxes[i + 1].y;
		gint pad = b - a >= 4 ? 0 : 2;
		gint along = h ? x : y;
		gint across = h ? y : x;
		gint lo = h ? box.y : box.x;
		gint hi = h ? box.y + box.h : box.x + box.w;

		if (along >= a - pad && along < b + pad && across >= lo && across < hi) {
			out->before = last_leaf(child_at(c, i))->pane;
			out->after = first_leaf(child_at(c, i + 1))->pane;
			out->split = c->dir;
			out->position = a;
			return TRUE;
		}
	}
	for (i = 0; i < c->children->len; i++)
		if (find_border_in(child_at(c, i), boxes[i], gap, x, y, out))
			return TRUE;
	return FALSE;
}

gboolean
gsurf_wmux_layout_find_border(GsurfWmuxLayout *layout, gint width, gint height, gint gap,
                              gint x, gint y, GsurfWmuxBorder *border)
{
	GsurfWmuxBorder found;
	Box box = { 0, 0, MAX(width, 0), MAX(height, 0) };

	g_return_val_if_fail(layout != NULL, FALSE);
	g_return_val_if_fail(border != NULL, FALSE);
	if (layout->zoomed != GSURF_WMUX_PANE_NONE)
		return FALSE;
	if (!find_border_in(layout->root, box, MAX(gap, 0), x, y, &found))
		return FALSE;
	*border = found;
	return TRUE;
}

gboolean
gsurf_wmux_layout_move_border(GsurfWmuxLayout *layout, const GsurfWmuxBorder *border,
                              gint width, gint height, gint gap, gint position)
{
	g_autoptr(GHashTable) above = NULL;
	Cell *before;
	Cell *after;
	Cell *split = NULL;
	Cell *c;
	Cell *branch_before = NULL;
	Cell *branch_after = NULL;
	Box box;
	gint extent;
	gint g;
	gint avail;
	gint start;
	guint bi;
	guint i;
	guint64 cum = 0;
	gint64 target;

	g_return_val_if_fail(layout != NULL, FALSE);
	g_return_val_if_fail(border != NULL, FALSE);
	before = find_leaf(layout, border->before);
	after = find_leaf(layout, border->after);
	if (before == NULL || after == NULL || before == after)
		return FALSE;

	/* The split is the lowest common ancestor of the two panes; they
	 * must sit in adjacent children of it, in that order. */
	above = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (c = before; c != NULL; c = c->parent)
		g_hash_table_add(above, c);
	for (c = after; c != NULL && !g_hash_table_contains(above, c); c = c->parent)
		branch_after = c;
	split = c;
	if (split == NULL || !split->is_split || split->dir != border->split)
		return FALSE;
	for (c = before; c != split; c = c->parent)
		branch_before = c;
	if (branch_before == NULL || branch_after == NULL ||
	    child_index(branch_after) != child_index(branch_before) + 1)
		return FALSE;
	bi = child_index(branch_before);

	/* Where the border is now and where it should be, in weight units
	 * of the split's extent (the same arithmetic as geometry). */
	box = cell_box(layout, split, width, height, MAX(gap, 0));
	extent = split->dir == GSURF_WMUX_SPLIT_H ? box.w : box.h;
	start = split->dir == GSURF_WMUX_SPLIT_H ? box.x : box.y;
	g = MAX(gap, 0);
	if ((gint64)g * (split->children->len - 1) > extent)
		g = extent / (gint)(split->children->len - 1);
	avail = extent - g * (gint)(split->children->len - 1);
	if (avail <= 0)
		return FALSE;
	for (i = 0; i <= bi; i++)
		cum += child_at(split, i)->weight;
	target = ((gint64)(position - start - (gint)bi * g) * GSURF_WMUX_LAYOUT_SCALE + avail / 2)
		/ avail;
	if (target == (gint64)cum)
		return FALSE;
	return shift_border(layout, split, bi, target - (gint64)cum);
}

/* ------------------------------------------------------------------ */
/* Presets                                                             */
/* ------------------------------------------------------------------ */

static const gchar *const preset_names[GSURF_WMUX_PRESET_COUNT] = {
	"even-horizontal", "even-vertical", "main-horizontal", "main-vertical", "tiled",
};

const gchar *
gsurf_wmux_preset_to_string(GsurfWmuxPreset preset)
{
	if ((guint)preset >= GSURF_WMUX_PRESET_COUNT)
		return NULL;
	return preset_names[preset];
}

gboolean
gsurf_wmux_preset_from_string(const gchar *name, GsurfWmuxPreset *preset)
{
	guint i;

	for (i = 0; name != NULL && i < GSURF_WMUX_PRESET_COUNT; i++)
		if (strcmp(name, preset_names[i]) == 0) {
			if (preset != NULL)
				*preset = (GsurfWmuxPreset)i;
			return TRUE;
		}
	return FALSE;
}

/* A split of @dir over leaves[from, from + n) with equal weights, or the
 * single leaf when n == 1. */
static Cell *
even_cell(GPtrArray *leaves_in, guint from, guint n, GsurfWmuxSplit dir)
{
	g_autofree guint *w = NULL;
	Cell *s;
	guint i;

	if (n == 1)
		return g_ptr_array_index(leaves_in, from);
	s = split_new(dir);
	w = g_new(guint, n);
	equal_weights(w, n, GSURF_WMUX_LAYOUT_SCALE);
	for (i = 0; i < n; i++) {
		Cell *leaf = g_ptr_array_index(leaves_in, from + i);

		leaf->weight = w[i];
		add_child(s, leaf);
	}
	return s;
}

void
gsurf_wmux_layout_apply_preset(GsurfWmuxLayout *layout, GsurfWmuxPreset preset)
{
	g_autoptr(GPtrArray) old = NULL;
	g_autoptr(GPtrArray) fresh = NULL;
	Cell *root = NULL;
	guint n;
	guint i;

	g_return_if_fail(layout != NULL);
	g_return_if_fail((guint)preset < GSURF_WMUX_PRESET_COUNT);

	/* Fresh leaves carrying the panes in their current order. */
	old = leaves(layout);
	n = old->len;
	fresh = g_ptr_array_new();
	for (i = 0; i < n; i++) {
		Cell *o = g_ptr_array_index(old, i);
		Cell *l = leaf_new(o->pane);

		l->payload = o->payload;
		l->stamp = o->stamp;
		g_ptr_array_add(fresh, l);
	}

	switch (preset) {
	case GSURF_WMUX_PRESET_EVEN_HORIZONTAL:
		root = even_cell(fresh, 0, n, GSURF_WMUX_SPLIT_H);
		break;
	case GSURF_WMUX_PRESET_EVEN_VERTICAL:
		root = even_cell(fresh, 0, n, GSURF_WMUX_SPLIT_V);
		break;
	case GSURF_WMUX_PRESET_MAIN_HORIZONTAL:
	case GSURF_WMUX_PRESET_MAIN_VERTICAL: {
		gboolean horiz = preset == GSURF_WMUX_PRESET_MAIN_HORIZONTAL;
		Cell *main_cell;
		Cell *rest;

		if (n == 1) {
			root = g_ptr_array_index(fresh, 0);
			break;
		}
		/* main-horizontal: main on top, the rest in a row below;
		 * main-vertical: main on the left, the rest stacked. */
		main_cell = g_ptr_array_index(fresh, 0);
		rest = even_cell(fresh, 1, n - 1, horiz ? GSURF_WMUX_SPLIT_H : GSURF_WMUX_SPLIT_V);
		root = split_new(horiz ? GSURF_WMUX_SPLIT_V : GSURF_WMUX_SPLIT_H);
		main_cell->weight = layout->main_pct * (GSURF_WMUX_LAYOUT_SCALE / 100);
		rest->weight = GSURF_WMUX_LAYOUT_SCALE - main_cell->weight;
		add_child(root, main_cell);
		add_child(root, rest);
		break;
	}
	case GSURF_WMUX_PRESET_TILED: {
		guint cols = 1;
		guint rows;
		g_autofree guint *w = NULL;

		while (cols * cols < n)
			cols++;
		rows = (n + cols - 1) / cols;
		if (rows == 1) {
			root = even_cell(fresh, 0, n, GSURF_WMUX_SPLIT_H);
			break;
		}
		/* Rows of up to @cols panes, stacked equally. */
		root = split_new(GSURF_WMUX_SPLIT_V);
		w = g_new(guint, rows);
		equal_weights(w, rows, GSURF_WMUX_LAYOUT_SCALE);
		for (i = 0; i < rows; i++) {
			guint from = i * cols;
			Cell *row = even_cell(fresh, from, MIN(cols, n - from), GSURF_WMUX_SPLIT_H);

			row->weight = w[i];
			add_child(root, row);
		}
		break;
	}
	default:
		g_assert_not_reached();
	}

	cell_free(layout->root);
	root->parent = NULL;
	root->weight = GSURF_WMUX_LAYOUT_SCALE;
	layout->root = root;
	reindex(layout);
	layout->zoomed = GSURF_WMUX_PANE_NONE;
	layout->preset = (gint)preset;
}

GsurfWmuxPreset
gsurf_wmux_layout_next_preset(GsurfWmuxLayout *layout)
{
	GsurfWmuxPreset next;

	g_return_val_if_fail(layout != NULL, GSURF_WMUX_PRESET_EVEN_HORIZONTAL);
	next = (GsurfWmuxPreset)((layout->preset + 1) % GSURF_WMUX_PRESET_COUNT);
	gsurf_wmux_layout_apply_preset(layout, next);
	return next;
}

gint
gsurf_wmux_layout_get_preset(GsurfWmuxLayout *layout)
{
	g_return_val_if_fail(layout != NULL, -1);
	return layout->preset;
}

/* ------------------------------------------------------------------ */
/* Serialization                                                       */
/* ------------------------------------------------------------------ */

/* A weight as a percentage with up to two decimals ("50", "33.3",
 * "33.34"). */
static void
append_weight(GString *out, guint w)
{
	if (w % 100 == 0)
		g_string_append_printf(out, "%u", w / 100);
	else if (w % 10 == 0)
		g_string_append_printf(out, "%u.%u", w / 100, (w % 100) / 10);
	else
		g_string_append_printf(out, "%u.%02u", w / 100, w % 100);
}

static void
serialize_cell(Cell *c, gboolean with_payload, GString *out)
{
	guint i;

	if (!c->is_split) {
		g_string_append_printf(out, "p%u", c->pane);
		if (with_payload)
			g_string_append_printf(out, "=%" G_GUINT64_FORMAT, c->payload);
		return;
	}
	g_string_append_c(out, c->dir == GSURF_WMUX_SPLIT_H ? 'h' : 'v');
	g_string_append_c(out, '(');
	for (i = 0; i < c->children->len; i++) {
		if (i > 0)
			g_string_append_c(out, ',');
		append_weight(out, child_at(c, i)->weight);
		g_string_append_c(out, ':');
		serialize_cell(child_at(c, i), with_payload, out);
	}
	g_string_append_c(out, ')');
}

gchar *
gsurf_wmux_layout_serialize(GsurfWmuxLayout *layout, gboolean with_payload)
{
	GString *out;

	g_return_val_if_fail(layout != NULL, NULL);
	out = g_string_new(NULL);
	serialize_cell(layout->root, with_payload, out);
	return g_string_free(out, FALSE);
}

/* The recursive-descent parser. Positions are byte offsets in @text. */
typedef struct {
	const gchar *text;
	const gchar *p;
	GHashTable  *seen;     /* pane ids */
	guint        max_id;
	gboolean     any;
	gint        *pos;
	GError     **error;
} Parser;

G_GNUC_PRINTF(4, 5)
static void
parse_fail(Parser *ps, const gchar *at, GsurfWmuxLayoutError code, const gchar *format, ...)
{
	g_autofree gchar *msg = NULL;
	va_list ap;
	gint offset = (gint)(at - ps->text);

	if (ps->pos != NULL)
		*ps->pos = offset;
	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	g_set_error(ps->error, GSURF_WMUX_LAYOUT_ERROR, (gint)code, "position %d: %s", offset, msg);
}

static void
skip_ws(Parser *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
		ps->p++;
}

/* Read decimal digits (at most @max_digits) into @value. */
static gboolean
read_digits(Parser *ps, guint max_digits, guint64 limit, guint64 *value, const gchar *what)
{
	const gchar *start = ps->p;
	guint64 v = 0;

	if (!g_ascii_isdigit(*ps->p)) {
		parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE, "expected %s", what);
		return FALSE;
	}
	while (g_ascii_isdigit(*ps->p)) {
		guint d = (guint)(*ps->p - '0');

		if ((guint)(ps->p - start) >= max_digits || v > (limit - d) / 10) {
			parse_fail(ps, start, GSURF_WMUX_LAYOUT_ERROR_PARSE, "%s is too large", what);
			return FALSE;
		}
		v = v * 10 + d;
		ps->p++;
	}
	*value = v;
	return TRUE;
}

/* weight := digits [ '.' digit [digit] ], in hundredths of a percent. */
static gboolean
read_weight(Parser *ps, guint *weight)
{
	const gchar *start = ps->p;
	guint64 whole = 0;
	guint frac = 0;

	if (!read_digits(ps, 7, G_MAXUINT32 / 100, &whole, "a weight"))
		return FALSE;
	if (*ps->p == '.') {
		ps->p++;
		if (!g_ascii_isdigit(*ps->p)) {
			parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE,
				"expected a digit after '.' in a weight");
			return FALSE;
		}
		frac = (guint)(*ps->p++ - '0') * 10;
		if (g_ascii_isdigit(*ps->p))
			frac += (guint)(*ps->p++ - '0');
		if (g_ascii_isdigit(*ps->p)) {
			parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE,
				"a weight has at most two decimals");
			return FALSE;
		}
	}
	*weight = (guint)whole * 100 + frac;
	if (*weight == 0) {
		parse_fail(ps, start, GSURF_WMUX_LAYOUT_ERROR_PARSE, "a weight must be positive");
		return FALSE;
	}
	return TRUE;
}

static Cell *parse_cell(Parser *ps, guint depth);

/* split := ('h'|'v') '(' weight ':' cell (',' weight ':' cell)+ ')' */
static Cell *
parse_split(Parser *ps, guint depth)
{
	g_autoptr(GArray) weights = g_array_new(FALSE, FALSE, sizeof(guint));
	g_autoptr(GArray) weight_at = g_array_new(FALSE, FALSE, sizeof(const gchar *));
	Cell *split;
	const gchar *open;
	guint64 sum = 0;
	guint i;

	if (depth >= GSURF_WMUX_LAYOUT_MAX_DEPTH) {
		parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE,
			"splits nested more than %d deep", GSURF_WMUX_LAYOUT_MAX_DEPTH);
		return NULL;
	}
	split = split_new(*ps->p == 'h' ? GSURF_WMUX_SPLIT_H : GSURF_WMUX_SPLIT_V);
	ps->p++;
	skip_ws(ps);
	if (*ps->p != '(') {
		parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE, "expected '(' after '%c'",
			split->dir == GSURF_WMUX_SPLIT_H ? 'h' : 'v');
		cell_free(split);
		return NULL;
	}
	open = ps->p++;

	/* Children, separated by commas, up to the closing parenthesis. */
	for (;;) {
		const gchar *at;
		guint w = 0;
		Cell *child;

		skip_ws(ps);
		at = ps->p;
		if (!read_weight(ps, &w))
			goto fail;
		skip_ws(ps);
		if (*ps->p != ':') {
			parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE,
				"expected ':' after a weight");
			goto fail;
		}
		ps->p++;
		child = parse_cell(ps, depth + 1);
		if (child == NULL)
			goto fail;
		add_child(split, child);
		g_array_append_val(weights, w);
		g_array_append_val(weight_at, at);
		sum += w;
		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == ')') {
			ps->p++;
			break;
		}
		parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE, "expected ',' or ')'");
		goto fail;
	}
	if (split->children->len < 2) {
		parse_fail(ps, open, GSURF_WMUX_LAYOUT_ERROR_PARSE,
			"a split needs at least two children");
		goto fail;
	}

	/* Weights are relative: scale them to sum to SCALE. One that would
	 * vanish next to its siblings is an error, not silently raised. */
	{
		guint64 cum = 0;
		guint64 prev = 0;

		for (i = 0; i < weights->len; i++) {
			guint64 next;

			cum += g_array_index(weights, guint, i);
			next = (cum * GSURF_WMUX_LAYOUT_SCALE + sum / 2) / sum;
			if (next == prev) {
				parse_fail(ps, g_array_index(weight_at, const gchar *, i),
					GSURF_WMUX_LAYOUT_ERROR_PARSE,
					"weight is too small next to its siblings");
				goto fail;
			}
			child_at(split, i)->weight = (guint)(next - prev);
			prev = next;
		}
	}
	flatten_children(split);
	return split;

fail:
	cell_free(split);
	return NULL;
}

/* cell := 'p' id [ '=' payload ] | split */
static Cell *
parse_cell(Parser *ps, guint depth)
{
	skip_ws(ps);
	if (*ps->p == 'h' || *ps->p == 'v')
		return parse_split(ps, depth);
	if (*ps->p == 'p') {
		const gchar *at = ps->p++;
		const gchar *after;
		guint64 id = 0;
		guint64 payload = 0;
		Cell *leaf;

		if (!read_digits(ps, 10, (guint64)GSURF_WMUX_PANE_NONE - 1, &id, "a pane id"))
			return NULL;
		if (g_hash_table_contains(ps->seen, GUINT_TO_POINTER((guint)id))) {
			parse_fail(ps, at, GSURF_WMUX_LAYOUT_ERROR_PARSE,
				"pane p%u appears twice", (guint)id);
			return NULL;
		}
		/* Whitespace may precede '=', but is not consumed otherwise:
		 * the state parser needs it as a field separator. */
		after = ps->p;
		skip_ws(ps);
		if (*ps->p != '=')
			ps->p = after;
		if (*ps->p == '=') {
			ps->p++;
			skip_ws(ps);
			if (!read_digits(ps, 20, G_MAXUINT64, &payload, "a payload"))
				return NULL;
		}
		g_hash_table_add(ps->seen, GUINT_TO_POINTER((guint)id));
		if (!ps->any || id > ps->max_id)
			ps->max_id = (guint)id;
		ps->any = TRUE;
		leaf = leaf_new((guint)id);
		leaf->payload = payload;
		return leaf;
	}
	if (*ps->p == '\0')
		parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE, "unexpected end of layout");
	else
		parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE,
			"expected 'p', 'h' or 'v'");
	return NULL;
}

/* Parse a layout starting at @start in @text; on success *@end is just
 * past it. Shared by the layout and state parsers. */
static GsurfWmuxLayout *
parse_layout_at(const gchar *text, const gchar *start, const gchar **end,
                gint *pos, GError **error)
{
	g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	GsurfWmuxLayout *layout;
	Parser ps = { text, start, seen, 0, FALSE, pos, error };
	Cell *root;

	root = parse_cell(&ps, 0);
	if (root == NULL)
		return NULL;
	layout = layout_alloc();
	layout->root = root;
	root->weight = GSURF_WMUX_LAYOUT_SCALE;
	reindex(layout);
	layout->next_id = ps.max_id >= GSURF_WMUX_PANE_NONE - 1
		? GSURF_WMUX_PANE_NONE : ps.max_id + 1;
	layout->active = first_leaf(root)->pane;
	touch(layout, layout->active);
	*end = ps.p;
	return layout;
}

GsurfWmuxLayout *
gsurf_wmux_layout_parse(const gchar *text, gint *error_pos, GError **error)
{
	g_autoptr(GsurfWmuxLayout) layout = NULL;
	const gchar *end = NULL;

	if (error_pos != NULL)
		*error_pos = -1;
	if (text == NULL) {
		if (error_pos != NULL)
			*error_pos = 0;
		g_set_error_literal(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_PARSE,
			"position 0: no layout");
		return NULL;
	}
	layout = parse_layout_at(text, text, &end, error_pos, error);
	if (layout == NULL)
		return NULL;
	while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
		end++;
	if (*end != '\0') {
		Parser ps = { text, end, NULL, 0, FALSE, error_pos, error };

		parse_fail(&ps, end, GSURF_WMUX_LAYOUT_ERROR_PARSE, "unexpected text after the layout");
		return NULL;
	}
	return g_steal_pointer(&layout);
}

/* ------------------------------------------------------------------ */
/* Invariants                                                          */
/* ------------------------------------------------------------------ */

static gboolean
check_cell(GsurfWmuxLayout *layout, Cell *c, GHashTable *seen, gchar **problem)
{
	guint64 sum = 0;
	guint i;

	if (!c->is_split) {
		if (g_hash_table_contains(seen, GUINT_TO_POINTER(c->pane))) {
			*problem = g_strdup_printf("pane %u appears twice", c->pane);
			return FALSE;
		}
		g_hash_table_add(seen, GUINT_TO_POINTER(c->pane));
		if (find_leaf(layout, c->pane) != c) {
			*problem = g_strdup_printf("pane %u is not indexed", c->pane);
			return FALSE;
		}
		if (layout->next_id != GSURF_WMUX_PANE_NONE && c->pane >= layout->next_id) {
			*problem = g_strdup_printf("pane %u is not below the next id %u",
				c->pane, layout->next_id);
			return FALSE;
		}
		return TRUE;
	}
	if (c->children->len < 2) {
		*problem = g_strdup("a split has fewer than two children");
		return FALSE;
	}
	for (i = 0; i < c->children->len; i++) {
		Cell *child = child_at(c, i);

		if (child->parent != c) {
			*problem = g_strdup("a child's parent link is wrong");
			return FALSE;
		}
		if (child->weight == 0) {
			*problem = g_strdup("a child has weight 0");
			return FALSE;
		}
		if (child->is_split && child->dir == c->dir) {
			*problem = g_strdup("a split nests a split of the same direction");
			return FALSE;
		}
		sum += child->weight;
		if (!check_cell(layout, child, seen, problem))
			return FALSE;
	}
	if (sum != GSURF_WMUX_LAYOUT_SCALE) {
		*problem = g_strdup_printf("weights sum to %" G_GUINT64_FORMAT, sum);
		return FALSE;
	}
	return TRUE;
}

gboolean
gsurf_wmux_layout_check(GsurfWmuxLayout *layout, gchar **problem)
{
	g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	gchar *local = NULL;
	gboolean ok;

	g_return_val_if_fail(layout != NULL, FALSE);
	if (layout->root == NULL || layout->root->parent != NULL) {
		local = g_strdup("bad root");
		ok = FALSE;
	} else {
		ok = check_cell(layout, layout->root, seen, &local);
	}
	if (ok && g_hash_table_size(seen) != g_hash_table_size(layout->panes)) {
		local = g_strdup("the pane index has stale entries");
		ok = FALSE;
	}
	if (ok && find_leaf(layout, layout->active) == NULL) {
		local = g_strdup_printf("active pane %u does not exist", layout->active);
		ok = FALSE;
	}
	if (ok && layout->last != GSURF_WMUX_PANE_NONE &&
	    (find_leaf(layout, layout->last) == NULL || layout->last == layout->active)) {
		local = g_strdup_printf("last pane %u is invalid", layout->last);
		ok = FALSE;
	}
	if (ok && layout->zoomed != GSURF_WMUX_PANE_NONE &&
	    find_leaf(layout, layout->zoomed) == NULL) {
		local = g_strdup_printf("zoomed pane %u does not exist", layout->zoomed);
		ok = FALSE;
	}
	if (problem != NULL)
		*problem = local;
	else
		g_free(local);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Window state                                                        */
/* ------------------------------------------------------------------ */

static void
window_state_free(gpointer data)
{
	GsurfWmuxWindowState *w = data;

	g_free(w->name);
	gsurf_wmux_layout_free(w->layout);
	g_free(w);
}

GsurfWmuxState *
gsurf_wmux_state_new(void)
{
	GsurfWmuxState *state = g_new0(GsurfWmuxState, 1);

	state->windows = g_ptr_array_new_with_free_func(window_state_free);
	state->current = 0;
	state->last = -1;
	return state;
}

void
gsurf_wmux_state_free(GsurfWmuxState *state)
{
	if (state == NULL)
		return;
	g_ptr_array_unref(state->windows);
	g_free(state);
}

guint
gsurf_wmux_state_add_window(GsurfWmuxState *state, const gchar *name, gboolean renamed,
                            GsurfWmuxLayout *layout)
{
	GsurfWmuxWindowState *w;

	g_return_val_if_fail(state != NULL, 0);
	g_return_val_if_fail(layout != NULL, 0);
	w = g_new0(GsurfWmuxWindowState, 1);
	w->name = g_strdup(name != NULL ? name : "");
	w->renamed = renamed;
	w->layout = layout;
	g_ptr_array_add(state->windows, w);
	return state->windows->len - 1;
}

/* A name as a double-quoted string: \" \\ \n \t escaped, other control
 * characters dropped (they cannot be typed into a name anyway). */
static void
append_quoted(GString *out, const gchar *s)
{
	g_string_append_c(out, '"');
	for (; *s != '\0'; s++) {
		switch (*s) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\n': g_string_append(out, "\\n");  break;
		case '\t': g_string_append(out, "\\t");  break;
		default:
			if ((guchar)*s >= 0x20 && (guchar)*s != 0x7f)
				g_string_append_c(out, *s);
			break;
		}
	}
	g_string_append_c(out, '"');
}

gchar *
gsurf_wmux_state_serialize(GsurfWmuxState *state)
{
	GString *out;
	guint i;

	g_return_val_if_fail(state != NULL, NULL);
	out = g_string_new("wmux1");
	g_string_append_printf(out, " current=%d", state->current);
	if (state->last >= 0)
		g_string_append_printf(out, " last=%d", state->last);
	for (i = 0; i < state->windows->len; i++) {
		GsurfWmuxWindowState *w = g_ptr_array_index(state->windows, i);
		g_autofree gchar *layout = gsurf_wmux_layout_serialize(w->layout, TRUE);

		g_string_append(out, " [name=");
		append_quoted(out, w->name);
		g_string_append_printf(out, " renamed=%d active=%u", w->renamed ? 1 : 0,
			gsurf_wmux_layout_get_active(w->layout));
		if (gsurf_wmux_layout_get_last(w->layout) != GSURF_WMUX_PANE_NONE)
			g_string_append_printf(out, " last=%u", gsurf_wmux_layout_get_last(w->layout));
		if (gsurf_wmux_layout_get_zoomed(w->layout) != GSURF_WMUX_PANE_NONE)
			g_string_append_printf(out, " zoom=%u", gsurf_wmux_layout_get_zoomed(w->layout));
		g_string_append_printf(out, " layout=%s]", layout);
	}
	return g_string_free(out, FALSE);
}

/* Read a field name "key=" at ps->p; returns the key's length (0 when
 * no identifier followed by '='). */
static gsize
read_key(Parser *ps)
{
	gsize n = 0;

	while (g_ascii_islower(ps->p[n]))
		n++;
	if (n == 0 || ps->p[n] != '=')
		return 0;
	return n;
}

/* Read a double-quoted name. */
static gchar *
read_quoted(Parser *ps)
{
	g_autoptr(GString) s = g_string_new(NULL);
	const gchar *open = ps->p;

	if (*ps->p != '"') {
		parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE, "expected '\"'");
		return NULL;
	}
	ps->p++;
	while (*ps->p != '"') {
		guchar c = (guchar)*ps->p;

		if (c == '\0') {
			parse_fail(ps, open, GSURF_WMUX_LAYOUT_ERROR_PARSE, "unterminated name");
			return NULL;
		}
		if (c < 0x20 || c == 0x7f) {
			parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE,
				"control character in a name");
			return NULL;
		}
		if (c == '\\') {
			switch (ps->p[1]) {
			case '"':  g_string_append_c(s, '"');  break;
			case '\\': g_string_append_c(s, '\\'); break;
			case 'n':  g_string_append_c(s, '\n'); break;
			case 't':  g_string_append_c(s, '\t'); break;
			default:
				parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE,
					"unknown escape in a name");
				return NULL;
			}
			ps->p += 2;
			continue;
		}
		g_string_append_c(s, (gchar)c);
		ps->p++;
	}
	ps->p++;
	if (!g_utf8_validate(s->str, -1, NULL)) {
		parse_fail(ps, open, GSURF_WMUX_LAYOUT_ERROR_PARSE, "name is not valid UTF-8");
		return NULL;
	}
	return g_string_free(g_steal_pointer(&s), FALSE);
}

/* After a value: whitespace, or the closing bracket / end. */
static gboolean
expect_separator(Parser *ps, gboolean in_window)
{
	if (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r' ||
	    (in_window && *ps->p == ']') || (!in_window && *ps->p == '\0'))
		return TRUE;
	if (*ps->p == '\0') {
		parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE, "unterminated window, expected ']'");
		return FALSE;
	}
	parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE, "expected a space after a value");
	return FALSE;
}

/* window := '[' field { ws field } ']' */
static GsurfWmuxWindowState *
parse_window(Parser *ps)
{
	g_autofree gchar *name = NULL;
	g_autoptr(GsurfWmuxLayout) layout = NULL;
	const gchar *open = ps->p;
	const gchar *field_at[3] = { NULL, NULL, NULL };   /* active, last, zoom */
	guint64 field_val[3] = { 0, 0, 0 };
	gboolean renamed = FALSE;
	gboolean have_renamed = FALSE;
	GsurfWmuxWindowState *w;
	guint i;

	ps->p++;   /* '[' */
	for (;;) {
		const gchar *at;
		gsize klen;

		skip_ws(ps);
		if (*ps->p == ']') {
			ps->p++;
			break;
		}
		if (*ps->p == '\0') {
			parse_fail(ps, open, GSURF_WMUX_LAYOUT_ERROR_PARSE, "unterminated window '['");
			return NULL;
		}
		at = ps->p;
		klen = read_key(ps);
		if (klen == 0) {
			parse_fail(ps, at, GSURF_WMUX_LAYOUT_ERROR_PARSE, "expected key=value");
			return NULL;
		}
		ps->p += klen + 1;

		if (klen == 4 && strncmp(at, "name", 4) == 0) {
			if (name != NULL) {
				parse_fail(ps, at, GSURF_WMUX_LAYOUT_ERROR_PARSE, "name= given twice");
				return NULL;
			}
			name = read_quoted(ps);
			if (name == NULL)
				return NULL;
		} else if (klen == 7 && strncmp(at, "renamed", 7) == 0) {
			if (have_renamed) {
				parse_fail(ps, at, GSURF_WMUX_LAYOUT_ERROR_PARSE, "renamed= given twice");
				return NULL;
			}
			if ((*ps->p != '0' && *ps->p != '1')) {
				parse_fail(ps, ps->p, GSURF_WMUX_LAYOUT_ERROR_PARSE,
					"renamed= must be 0 or 1");
				return NULL;
			}
			renamed = *ps->p++ == '1';
			have_renamed = TRUE;
		} else if (klen == 6 && strncmp(at, "layout", 6) == 0) {
			const gchar *end = NULL;

			if (layout != NULL) {
				parse_fail(ps, at, GSURF_WMUX_LAYOUT_ERROR_PARSE, "layout= given twice");
				return NULL;
			}
			layout = parse_layout_at(ps->text, ps->p, &end, ps->pos, ps->error);
			if (layout == NULL)
				return NULL;
			ps->p = end;
		} else {
			static const gchar *const ids[3] = { "active", "last", "zoom" };
			gint slot = -1;

			for (i = 0; i < 3; i++)
				if (klen == strlen(ids[i]) && strncmp(at, ids[i], klen) == 0)
					slot = (gint)i;
			if (slot < 0) {
				parse_fail(ps, at, GSURF_WMUX_LAYOUT_ERROR_PARSE,
					"unknown window field \"%.*s\"", (gint)klen, at);
				return NULL;
			}
			if (field_at[slot] != NULL) {
				parse_fail(ps, at, GSURF_WMUX_LAYOUT_ERROR_PARSE,
					"%s= given twice", ids[slot]);
				return NULL;
			}
			field_at[slot] = at;
			if (!read_digits(ps, 10, (guint64)GSURF_WMUX_PANE_NONE - 1,
					&field_val[slot], "a pane id"))
				return NULL;
		}
		if (!expect_separator(ps, TRUE))
			return NULL;
	}
	if (layout == NULL) {
		parse_fail(ps, open, GSURF_WMUX_LAYOUT_ERROR_PARSE, "window without layout=");
		return NULL;
	}

	/* active, last and zoom must name panes of this layout. */
	if (field_at[0] != NULL &&
	    !gsurf_wmux_layout_set_active(layout, (guint)field_val[0])) {
		parse_fail(ps, field_at[0], GSURF_WMUX_LAYOUT_ERROR_PARSE,
			"active=%u is not a pane of the layout", (guint)field_val[0]);
		return NULL;
	}
	gsurf_wmux_layout_set_last(layout, GSURF_WMUX_PANE_NONE);
	if (field_at[1] != NULL &&
	    !gsurf_wmux_layout_set_last(layout, (guint)field_val[1])) {
		parse_fail(ps, field_at[1], GSURF_WMUX_LAYOUT_ERROR_PARSE,
			"last=%u is not another pane of the layout", (guint)field_val[1]);
		return NULL;
	}
	if (field_at[2] != NULL &&
	    !gsurf_wmux_layout_set_zoomed(layout, (guint)field_val[2])) {
		parse_fail(ps, field_at[2], GSURF_WMUX_LAYOUT_ERROR_PARSE,
			"zoom=%u is not a pane of the layout", (guint)field_val[2]);
		return NULL;
	}

	w = g_new0(GsurfWmuxWindowState, 1);
	w->name = name != NULL ? g_steal_pointer(&name) : g_strdup("");
	w->renamed = renamed;
	w->layout = g_steal_pointer(&layout);
	return w;
}

GsurfWmuxState *
gsurf_wmux_state_parse(const gchar *text, gint *error_pos, GError **error)
{
	g_autoptr(GsurfWmuxState) state = NULL;
	Parser ps = { text, text, NULL, 0, FALSE, error_pos, error };
	const gchar *current_at = NULL;
	const gchar *last_at = NULL;
	guint64 current = 0;
	guint64 last = 0;

	if (error_pos != NULL)
		*error_pos = -1;
	if (text == NULL) {
		if (error_pos != NULL)
			*error_pos = 0;
		g_set_error_literal(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_PARSE,
			"position 0: no state");
		return NULL;
	}

	/* The version tag. */
	skip_ws(&ps);
	if (strncmp(ps.p, "wmux1", 5) != 0) {
		parse_fail(&ps, ps.p, GSURF_WMUX_LAYOUT_ERROR_PARSE, "expected \"wmux1\"");
		return NULL;
	}
	ps.p += 5;
	if (!expect_separator(&ps, FALSE))
		return NULL;

	/* Header fields, then windows. */
	state = gsurf_wmux_state_new();
	for (;;) {
		const gchar *at;
		gsize klen;

		skip_ws(&ps);
		if (*ps.p == '\0')
			break;
		at = ps.p;
		if (*ps.p == '[') {
			GsurfWmuxWindowState *w = parse_window(&ps);

			if (w == NULL)
				return NULL;
			g_ptr_array_add(state->windows, w);
		} else {
			klen = read_key(&ps);
			if (state->windows->len > 0 || klen == 0) {
				parse_fail(&ps, at, GSURF_WMUX_LAYOUT_ERROR_PARSE,
					state->windows->len > 0 ? "expected '[' (a window)"
					                        : "expected current=, last= or '['");
				return NULL;
			}
			ps.p += klen + 1;
			if (klen == 7 && strncmp(at, "current", 7) == 0 && current_at == NULL) {
				current_at = at;
				if (!read_digits(&ps, 6, 999999, &current, "a window index"))
					return NULL;
			} else if (klen == 4 && strncmp(at, "last", 4) == 0 && last_at == NULL) {
				last_at = at;
				if (!read_digits(&ps, 6, 999999, &last, "a window index"))
					return NULL;
			} else {
				parse_fail(&ps, at, GSURF_WMUX_LAYOUT_ERROR_PARSE,
					"unknown or repeated field \"%.*s\"", (gint)klen, at);
				return NULL;
			}
		}
		if (!expect_separator(&ps, FALSE))
			return NULL;
	}

	if (state->windows->len == 0) {
		parse_fail(&ps, ps.p, GSURF_WMUX_LAYOUT_ERROR_PARSE, "a state needs a window");
		return NULL;
	}
	if (current >= state->windows->len) {
		parse_fail(&ps, current_at, GSURF_WMUX_LAYOUT_ERROR_PARSE,
			"current=%u is not a window", (guint)current);
		return NULL;
	}
	if (last_at != NULL && last >= state->windows->len) {
		parse_fail(&ps, last_at, GSURF_WMUX_LAYOUT_ERROR_PARSE,
			"last=%u is not a window", (guint)last);
		return NULL;
	}
	state->current = (gint)current;
	state->last = last_at != NULL ? (gint)last : -1;
	return g_steal_pointer(&state);
}
