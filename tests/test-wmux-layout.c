/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-wmux-layout.c - the wmux pane layout model and window state
 *
 * Compiles modules/wmux/gsurf-wmux-layout.c straight into the test and
 * checks docs/wmux.org, "Layout string grammar" and the layout rules:
 * every operation (split, remove, neighbor with the overlap and
 * most-recently-used tie-break, next/prev/last, swap, rotate, resize with
 * the min_pct clamp, zoom, spread, the five presets and next-layout,
 * border hit-testing and dragging), exact integer geometry, payloads and
 * stable ids, serialize/parse round trips and every malformed input with
 * its position, the window-state string, a 5000-operation randomized
 * stress run that checks every invariant after every operation, and fuzz
 * runs over both parsers (random, truncated and mutated input must give a
 * layout or a GError, never a crash).
 *
 * The seed defaults to a constant; set GSURF_WMUX_SEED to replay
 * another. Failures print it.
 */
#include <glib.h>
#include <string.h>

#include "modules/wmux/gsurf-wmux-layout.c"

#define NONE GSURF_WMUX_PANE_NONE

static guint32
test_seed(void)
{
	const gchar *env = g_getenv("GSURF_WMUX_SEED");
	guint32 seed = env != NULL ? (guint32)g_ascii_strtoull(env, NULL, 10) : 20261104u;

	g_test_message("GSURF_WMUX_SEED=%u", seed);
	return seed;
}

/* --- Helpers --- */

static GsurfWmuxLayout *
parse_ok(const gchar *text)
{
	g_autoptr(GError) error = NULL;
	GsurfWmuxLayout *layout = gsurf_wmux_layout_parse(text, NULL, &error);

	if (layout == NULL)
		g_error("parse(\"%s\") failed: %s", text, error->message);
	return layout;
}

static void
assert_shape(GsurfWmuxLayout *layout, const gchar *expected)
{
	g_autofree gchar *got = gsurf_wmux_layout_serialize(layout, FALSE);

	if (strcmp(got, expected) != 0)
		g_error("layout is \"%s\", expected \"%s\"", got, expected);
}

static void
assert_order(GsurfWmuxLayout *layout, const gchar *expected)
{
	g_autoptr(GArray) panes = gsurf_wmux_layout_list_panes(layout);
	g_autoptr(GString) s = g_string_new(NULL);
	guint i;

	for (i = 0; i < panes->len; i++)
		g_string_append_printf(s, "%s%u", i > 0 ? " " : "", g_array_index(panes, guint, i));
	g_assert_cmpstr(s->str, ==, expected);
}

/* The rectangle of @pane in @rects. */
static const GsurfWmuxRect *
rect_of(GArray *rects, guint pane)
{
	guint i;

	for (i = 0; i < rects->len; i++)
		if (g_array_index(rects, GsurfWmuxRect, i).pane == pane)
			return &g_array_index(rects, GsurfWmuxRect, i);
	g_error("pane %u has no rectangle", pane);
	return NULL;
}

/* Geometry at w x h with @gap: rectangles in layout order, inside the
 * area, never overlapping; with gap 0 they cover every pixel exactly
 * once; with a gap (and no pane squeezed to nothing), every uncovered
 * pixel has a covered pixel within @gap in both directions, so it lies in
 * a gap strip, never in a hole. Returns FALSE with @why on failure. */
static gboolean
geometry_ok(GsurfWmuxLayout *layout, gint w, gint h, gint gap, gchar **why)
{
	g_autoptr(GArray) rects = gsurf_wmux_layout_geometry(layout, w, h, gap);
	g_autoptr(GArray) order = gsurf_wmux_layout_list_panes(layout);
	g_autofree guint8 *cover = g_new0(guint8, (gsize)MAX(w, 1) * (gsize)MAX(h, 1));
	gboolean zoomed = gsurf_wmux_layout_get_zoomed(layout) != NONE;
	gboolean empty_pane = FALSE;
	gint x;
	gint y;
	guint i;

	if (!zoomed && rects->len != order->len) {
		*why = g_strdup_printf("%u rects for %u panes", rects->len, order->len);
		return FALSE;
	}
	for (i = 0; i < rects->len; i++) {
		const GsurfWmuxRect *r = &g_array_index(rects, GsurfWmuxRect, i);

		if (!zoomed && r->pane != g_array_index(order, guint, i)) {
			*why = g_strdup("rects are not in layout order");
			return FALSE;
		}
		if (r->x < 0 || r->y < 0 || r->width < 0 || r->height < 0 ||
		    r->x + r->width > w || r->y + r->height > h) {
			*why = g_strdup_printf("pane %u (%d,%d %dx%d) leaves %dx%d", r->pane, r->x,
				r->y, r->width, r->height, w, h);
			return FALSE;
		}
		if (r->width == 0 || r->height == 0)
			empty_pane = TRUE;
		for (y = r->y; y < r->y + r->height; y++)
			for (x = r->x; x < r->x + r->width; x++)
				if (cover[y * w + x]++ != 0) {
					*why = g_strdup_printf("pane %u overlaps at %d,%d", r->pane, x, y);
					return FALSE;
				}
	}
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			gboolean near = FALSE;
			gint dx;
			gint dy;

			if (cover[y * w + x] != 0)
				continue;
			if (gap <= 0) {
				*why = g_strdup_printf("pixel %d,%d uncovered with gap 0", x, y);
				return FALSE;
			}
			if (empty_pane)
				continue;
			for (dy = -gap; dy <= gap && !near; dy++)
				for (dx = -gap; dx <= gap && !near; dx++)
					if (x + dx >= 0 && x + dx < w && y + dy >= 0 && y + dy < h &&
					    cover[(y + dy) * w + x + dx] != 0)
						near = TRUE;
			if (!near) {
				*why = g_strdup_printf("pixel %d,%d is in a hole, not a gap", x, y);
				return FALSE;
			}
		}
	}
	return TRUE;
}

static void
assert_valid(GsurfWmuxLayout *layout)
{
	gchar *why = NULL;

	if (!gsurf_wmux_layout_check(layout, &why))
		g_error("invariant: %s", why);
	if (!geometry_ok(layout, 97, 61, 0, &why) || !geometry_ok(layout, 97, 61, 3, &why))
		g_error("geometry: %s", why);
}

static GsurfWmuxLayout *
layout_with(guint n)
{
	GsurfWmuxLayout *layout = gsurf_wmux_layout_new();
	guint i;

	gsurf_wmux_layout_set_min_pct(layout, 0);
	for (i = 1; i < n; i++)
		g_assert_cmpuint(gsurf_wmux_layout_split(layout, i - 1, GSURF_WMUX_SPLIT_H, FALSE,
			50, NULL), ==, i);
	return layout;
}

/* --- Basics --- */

static void
test_new(void)
{
	g_autoptr(GsurfWmuxLayout) layout = gsurf_wmux_layout_new();
	g_autoptr(GArray) panes = NULL;

	assert_shape(layout, "p0");
	g_assert_cmpuint(gsurf_wmux_layout_get_n_panes(layout), ==, 1);
	g_assert_cmpuint(gsurf_wmux_layout_get_nth_pane(layout, 0), ==, 0);
	g_assert_cmpuint(gsurf_wmux_layout_get_nth_pane(layout, 1), ==, NONE);
	g_assert_cmpint(gsurf_wmux_layout_get_pane_index(layout, 0), ==, 0);
	g_assert_cmpint(gsurf_wmux_layout_get_pane_index(layout, 7), ==, -1);
	g_assert_true(gsurf_wmux_layout_has_pane(layout, 0));
	g_assert_false(gsurf_wmux_layout_has_pane(layout, NONE));
	g_assert_cmpuint(gsurf_wmux_layout_get_active(layout), ==, 0);
	g_assert_cmpuint(gsurf_wmux_layout_get_last(layout), ==, NONE);
	g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(layout), ==, NONE);
	g_assert_cmpint(gsurf_wmux_layout_get_preset(layout), ==, -1);
	g_assert_cmpuint(gsurf_wmux_layout_get_stamp(layout, 0), >, 0);
	g_assert_cmpuint(gsurf_wmux_layout_get_stamp(layout, 5), ==, 0);
	panes = gsurf_wmux_layout_list_panes(layout);
	g_assert_cmpuint(panes->len, ==, 1);

	/* Settings and their clamps. */
	g_assert_cmpuint(gsurf_wmux_layout_get_min_pct(layout), ==, 10);
	g_assert_cmpuint(gsurf_wmux_layout_get_main_pct(layout), ==, 60);
	gsurf_wmux_layout_set_min_pct(layout, 70);
	g_assert_cmpuint(gsurf_wmux_layout_get_min_pct(layout), ==, 50);
	gsurf_wmux_layout_set_min_pct(layout, 0);
	g_assert_cmpuint(gsurf_wmux_layout_get_min_pct(layout), ==, 0);
	gsurf_wmux_layout_set_main_pct(layout, 5);
	g_assert_cmpuint(gsurf_wmux_layout_get_main_pct(layout), ==, 10);
	gsurf_wmux_layout_set_main_pct(layout, 95);
	g_assert_cmpuint(gsurf_wmux_layout_get_main_pct(layout), ==, 90);

	/* Payloads. */
	g_assert_cmpuint(gsurf_wmux_layout_get_payload(layout, 0), ==, 0);
	g_assert_true(gsurf_wmux_layout_set_payload(layout, 0, G_MAXUINT64));
	g_assert_cmpuint(gsurf_wmux_layout_get_payload(layout, 0), ==, G_MAXUINT64);
	g_assert_false(gsurf_wmux_layout_set_payload(layout, 3, 1));
	g_assert_cmpuint(gsurf_wmux_layout_get_payload(layout, 3), ==, 0);
	g_assert_cmpuint(gsurf_wmux_layout_find_payload(layout, G_MAXUINT64), ==, 0);
	g_assert_cmpuint(gsurf_wmux_layout_find_payload(layout, 42), ==, NONE);
	assert_valid(layout);
	gsurf_wmux_layout_free(NULL);
}

static void
test_split(void)
{
	g_autoptr(GsurfWmuxLayout) layout = gsurf_wmux_layout_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	guint id;

	g_assert_cmpuint(gsurf_wmux_layout_split(layout, 0, GSURF_WMUX_SPLIT_H, FALSE, 50, NULL),
		==, 1);
	assert_shape(layout, "h(50:p0,50:p1)");
	g_assert_cmpuint(gsurf_wmux_layout_split(layout, 1, GSURF_WMUX_SPLIT_V, FALSE, 50, NULL),
		==, 2);
	assert_shape(layout, "h(50:p0,50:v(50:p1,50:p2))");
	/* Same direction as the parent: a sibling, @pct of the pane's cell. */
	g_assert_cmpuint(gsurf_wmux_layout_split(layout, 0, GSURF_WMUX_SPLIT_H, TRUE, 30, NULL),
		==, 3);
	assert_shape(layout, "h(15:p3,35:p0,50:v(50:p1,50:p2))");
	assert_order(layout, "3 0 1 2");
	/* The active pane does not change; new panes have payload 0. */
	g_assert_cmpuint(gsurf_wmux_layout_get_active(layout), ==, 0);
	g_assert_cmpuint(gsurf_wmux_layout_get_payload(layout, 3), ==, 0);
	assert_valid(layout);

	/* Ids are never reused. */
	g_assert_true(gsurf_wmux_layout_remove(layout, 3, NULL));
	g_assert_cmpuint(gsurf_wmux_layout_split(layout, 2, GSURF_WMUX_SPLIT_V, FALSE, 50, NULL),
		==, 4);
	assert_shape(layout, "h(50:p0,50:v(50:p1,25:p2,25:p4))");

	/* -b on a single pane puts the new pane first. */
	{
		g_autoptr(GsurfWmuxLayout) l = gsurf_wmux_layout_new();

		gsurf_wmux_layout_split(l, 0, GSURF_WMUX_SPLIT_V, TRUE, 25, NULL);
		assert_shape(l, "v(25:p1,75:p0)");
	}

	/* Refusals change nothing. */
	before = gsurf_wmux_layout_serialize(layout, TRUE);
	g_assert_cmpuint(gsurf_wmux_layout_split(layout, 9, GSURF_WMUX_SPLIT_H, FALSE, 50, &error),
		==, NONE);
	g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_cmpuint(gsurf_wmux_layout_split(layout, 0, GSURF_WMUX_SPLIT_H, FALSE, 0, &error),
		==, NONE);
	g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_cmpuint(gsurf_wmux_layout_split(layout, 0, GSURF_WMUX_SPLIT_H, FALSE, 100, &error),
		==, NONE);
	g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_cmpuint(gsurf_wmux_layout_split(layout, 0, (GsurfWmuxSplit)7, FALSE, 50, &error),
		==, NONE);
	g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_INVALID);
	g_clear_error(&error);
	/* p2 is 25% high: halves of 12.5% pass min 10, then 6.25% fail. */
	id = gsurf_wmux_layout_split(layout, 2, GSURF_WMUX_SPLIT_V, FALSE, 50, &error);
	g_assert_no_error(error);
	after = gsurf_wmux_layout_serialize(layout, TRUE);
	g_assert_cmpuint(gsurf_wmux_layout_split(layout, id, GSURF_WMUX_SPLIT_V, FALSE, 50, &error),
		==, NONE);
	g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_NO_SPACE);
	g_clear_error(&error);
	g_assert_cmpuint(gsurf_wmux_layout_split(layout, 2, GSURF_WMUX_SPLIT_V, FALSE, 90, &error),
		==, NONE);
	g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_NO_SPACE);
	g_clear_error(&error);
	{
		g_autofree gchar *now = gsurf_wmux_layout_serialize(layout, TRUE);

		g_assert_cmpstr(now, ==, after);
		g_assert_cmpstr(before, !=, after);
	}
	/* min_pct 0 allows it; a cell of weight 1 cannot split further. */
	gsurf_wmux_layout_set_min_pct(layout, 0);
	g_assert_cmpuint(gsurf_wmux_layout_split(layout, id, GSURF_WMUX_SPLIT_V, FALSE, 50, NULL),
		!=, NONE);
	{
		g_autoptr(GsurfWmuxLayout) tiny = parse_ok("v(1:p0,9999:p1)");

		gsurf_wmux_layout_set_min_pct(tiny, 0);
		g_assert_cmpuint(gsurf_wmux_layout_split(tiny, 0, GSURF_WMUX_SPLIT_V, FALSE, 50,
			&error), ==, NONE);
		g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_NO_SPACE);
		g_clear_error(&error);
	}
	/* Ids exhausted: the largest id is in use. */
	{
		g_autoptr(GsurfWmuxLayout) full = parse_ok("p4294967294");

		g_assert_cmpuint(gsurf_wmux_layout_split(full, 4294967294u, GSURF_WMUX_SPLIT_H, FALSE,
			50, &error), ==, NONE);
		g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_IDS_EXHAUSTED);
		g_clear_error(&error);
	}
	{
		g_autoptr(GsurfWmuxLayout) almost = parse_ok("p4294967293");

		g_assert_cmpuint(gsurf_wmux_layout_split(almost, 4294967293u, GSURF_WMUX_SPLIT_H,
			FALSE, 50, NULL), ==, 4294967294u);
		g_assert_cmpuint(gsurf_wmux_layout_split(almost, 4294967293u, GSURF_WMUX_SPLIT_H,
			FALSE, 50, &error), ==, NONE);
		g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_IDS_EXHAUSTED);
		g_clear_error(&error);
		assert_valid(almost);
	}
	assert_valid(layout);
}

static void
test_remove(void)
{
	g_autoptr(GError) error = NULL;

	/* The cell before inherits the space; the first pane's goes to the
	 * cell after it. */
	{
		g_autoptr(GsurfWmuxLayout) l = parse_ok("h(20:p0,30:p1,50:p2)");

		g_assert_true(gsurf_wmux_layout_remove(l, 1, NULL));
		assert_shape(l, "h(50:p0,50:p2)");
		assert_valid(l);
	}
	{
		g_autoptr(GsurfWmuxLayout) l = parse_ok("h(20:p0,30:p1,50:p2)");

		g_assert_true(gsurf_wmux_layout_remove(l, 0, NULL));
		assert_shape(l, "h(50:p1,50:p2)");
		g_assert_true(gsurf_wmux_layout_remove(l, 2, NULL));
		assert_shape(l, "p1");
		assert_valid(l);
		/* The last pane stays. */
		g_assert_false(gsurf_wmux_layout_remove(l, 1, &error));
		g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_LAST_PANE);
		g_clear_error(&error);
		g_assert_false(gsurf_wmux_layout_remove(l, 8, &error));
		g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_NOT_FOUND);
		g_clear_error(&error);
	}
	/* A collapsing split whose survivor matches the grandparent's
	 * direction is flattened into it, scaled to its share. */
	{
		g_autoptr(GsurfWmuxLayout) l =
			parse_ok("h(50:p0,50:v(50:p1,50:h(50:p2,50:p3)))");

		g_assert_true(gsurf_wmux_layout_remove(l, 1, NULL));
		assert_shape(l, "h(50:p0,25:p2,25:p3)");
		assert_valid(l);
	}
	/* The root split collapsing into a split becomes the root. */
	{
		g_autoptr(GsurfWmuxLayout) l = parse_ok("v(50:p0,50:h(50:p1,50:p2))");

		g_assert_true(gsurf_wmux_layout_remove(l, 0, NULL));
		assert_shape(l, "h(50:p1,50:p2)");
		assert_valid(l);
	}
	/* Active fallback: the last pane, else the nearest pane of the cell
	 * that took the space; last is cleared when it goes. */
	{
		g_autoptr(GsurfWmuxLayout) l = parse_ok("h(25:p0,25:p1,25:p2,25:p3)");

		gsurf_wmux_layout_set_active(l, 3);
		gsurf_wmux_layout_set_active(l, 1);
		g_assert_cmpuint(gsurf_wmux_layout_get_last(l), ==, 3);
		g_assert_true(gsurf_wmux_layout_remove(l, 1, NULL));
		g_assert_cmpuint(gsurf_wmux_layout_get_active(l), ==, 3);
		g_assert_cmpuint(gsurf_wmux_layout_get_last(l), ==, NONE);
		g_assert_true(gsurf_wmux_layout_remove(l, 3, NULL));
		g_assert_cmpuint(gsurf_wmux_layout_get_active(l), ==, 2);
		gsurf_wmux_layout_set_active(l, 0);
		g_assert_cmpuint(gsurf_wmux_layout_get_last(l), ==, 2);
		g_assert_true(gsurf_wmux_layout_remove(l, 2, NULL));
		g_assert_cmpuint(gsurf_wmux_layout_get_active(l), ==, 0);
		g_assert_cmpuint(gsurf_wmux_layout_get_last(l), ==, NONE);
		assert_valid(l);
	}
	{
		g_autoptr(GsurfWmuxLayout) l = parse_ok("h(50:p0,50:v(50:p1,50:p2))");

		/* p0 active, no last: the first pane after it inherits. */
		g_assert_true(gsurf_wmux_layout_remove(l, 0, NULL));
		g_assert_cmpuint(gsurf_wmux_layout_get_active(l), ==, 1);
	}
}

static void
test_active_last_next(void)
{
	g_autoptr(GsurfWmuxLayout) l = parse_ok("h(25:p4,25:p1,25:p9,25:p2)");
	guint64 s;

	g_assert_cmpuint(gsurf_wmux_layout_get_active(l), ==, 4);
	g_assert_false(gsurf_wmux_layout_set_active(l, 7));
	g_assert_true(gsurf_wmux_layout_set_active(l, 9));
	g_assert_cmpuint(gsurf_wmux_layout_get_last(l), ==, 4);
	s = gsurf_wmux_layout_get_stamp(l, 9);
	g_assert_cmpuint(s, >, gsurf_wmux_layout_get_stamp(l, 4));
	/* Re-selecting the active pane keeps last, refreshes the stamp. */
	g_assert_true(gsurf_wmux_layout_set_active(l, 9));
	g_assert_cmpuint(gsurf_wmux_layout_get_last(l), ==, 4);
	g_assert_cmpuint(gsurf_wmux_layout_get_stamp(l, 9), >, s);
	/* set_last validation. */
	g_assert_false(gsurf_wmux_layout_set_last(l, 9));
	g_assert_false(gsurf_wmux_layout_set_last(l, 77));
	g_assert_true(gsurf_wmux_layout_set_last(l, 2));
	g_assert_cmpuint(gsurf_wmux_layout_get_last(l), ==, 2);
	g_assert_true(gsurf_wmux_layout_set_last(l, NONE));
	g_assert_cmpuint(gsurf_wmux_layout_get_last(l), ==, NONE);

	/* next / prev follow layout order and wrap. */
	g_assert_cmpuint(gsurf_wmux_layout_next(l, 4), ==, 1);
	g_assert_cmpuint(gsurf_wmux_layout_next(l, 2), ==, 4);
	g_assert_cmpuint(gsurf_wmux_layout_prev(l, 4), ==, 2);
	g_assert_cmpuint(gsurf_wmux_layout_prev(l, 9), ==, 1);
	g_assert_cmpuint(gsurf_wmux_layout_next(l, 77), ==, NONE);
	g_assert_cmpuint(gsurf_wmux_layout_prev(l, NONE), ==, NONE);
	{
		g_autoptr(GsurfWmuxLayout) one = gsurf_wmux_layout_new();

		g_assert_cmpuint(gsurf_wmux_layout_next(one, 0), ==, 0);
		g_assert_cmpuint(gsurf_wmux_layout_prev(one, 0), ==, 0);
	}
}

static void
test_neighbor(void)
{
	g_autoptr(GsurfWmuxLayout) l = parse_ok("h(50:p0,50:v(50:p1,50:p2))");

	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 1, GSURF_WMUX_DIR_LEFT, FALSE), ==, 0);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 2, GSURF_WMUX_DIR_LEFT, FALSE), ==, 0);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 1, GSURF_WMUX_DIR_DOWN, FALSE), ==, 2);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 2, GSURF_WMUX_DIR_UP, FALSE), ==, 1);
	/* Nothing above p1 / left of p0 without wrapping. */
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 1, GSURF_WMUX_DIR_UP, FALSE), ==, NONE);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 0, GSURF_WMUX_DIR_LEFT, FALSE), ==, NONE);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 0, GSURF_WMUX_DIR_UP, FALSE), ==, NONE);
	/* Wrapping goes to the far edge. */
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 1, GSURF_WMUX_DIR_UP, TRUE), ==, 2);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 2, GSURF_WMUX_DIR_DOWN, TRUE), ==, 1);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 1, GSURF_WMUX_DIR_RIGHT, TRUE), ==, 0);
	/* p0 spans the full height: nothing to wrap to vertically. */
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 0, GSURF_WMUX_DIR_UP, TRUE), ==, NONE);

	/* Equal overlap: the most recently used wins, either way round. */
	gsurf_wmux_layout_set_active(l, 1);
	gsurf_wmux_layout_set_active(l, 2);
	gsurf_wmux_layout_set_active(l, 0);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 0, GSURF_WMUX_DIR_RIGHT, FALSE), ==, 2);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 0, GSURF_WMUX_DIR_LEFT, TRUE), ==, 2);
	gsurf_wmux_layout_set_active(l, 1);
	gsurf_wmux_layout_set_active(l, 0);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 0, GSURF_WMUX_DIR_RIGHT, FALSE), ==, 1);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 0, GSURF_WMUX_DIR_LEFT, TRUE), ==, 1);

	/* Never used: layout order breaks the tie. */
	{
		g_autoptr(GsurfWmuxLayout) t = parse_ok("h(50:p0,50:v(50:p1,50:p2))");

		g_assert_cmpuint(gsurf_wmux_layout_neighbor(t, 0, GSURF_WMUX_DIR_RIGHT, FALSE), ==, 1);
	}
	/* More overlap beats more recent use. */
	{
		g_autoptr(GsurfWmuxLayout) t = parse_ok("v(50:h(20:p0,80:p1),50:h(60:p2,40:p3))");

		gsurf_wmux_layout_set_active(t, 0);
		gsurf_wmux_layout_set_active(t, 2);
		g_assert_cmpuint(gsurf_wmux_layout_neighbor(t, 2, GSURF_WMUX_DIR_UP, FALSE), ==, 1);
		g_assert_cmpuint(gsurf_wmux_layout_neighbor(t, 0, GSURF_WMUX_DIR_DOWN, FALSE), ==, 2);
		g_assert_cmpuint(gsurf_wmux_layout_neighbor(t, 1, GSURF_WMUX_DIR_DOWN, FALSE), ==, 2);
		g_assert_cmpuint(gsurf_wmux_layout_neighbor(t, 3, GSURF_WMUX_DIR_UP, FALSE), ==, 1);
		g_assert_cmpuint(gsurf_wmux_layout_neighbor(t, 3, GSURF_WMUX_DIR_LEFT, FALSE), ==, 2);
	}
	/* Zoom does not matter; unknown panes have no neighbors. */
	gsurf_wmux_layout_toggle_zoom(l);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 1, GSURF_WMUX_DIR_LEFT, FALSE), ==, 0);
	g_assert_cmpuint(gsurf_wmux_layout_neighbor(l, 5, GSURF_WMUX_DIR_LEFT, TRUE), ==, NONE);
	{
		g_autoptr(GsurfWmuxLayout) one = gsurf_wmux_layout_new();

		g_assert_cmpuint(gsurf_wmux_layout_neighbor(one, 0, GSURF_WMUX_DIR_RIGHT, TRUE),
			==, NONE);
	}
}

static void
test_swap_rotate(void)
{
	g_autoptr(GsurfWmuxLayout) l = parse_ok("h(20:p0=10,30:p1=11,50:v(50:p2=12,50:p3=13))");

	gsurf_wmux_layout_set_active(l, 1);
	g_assert_true(gsurf_wmux_layout_swap(l, 1, 3));
	assert_shape(l, "h(20:p0,30:p3,50:v(50:p2,50:p1))");
	/* The pane keeps its payload and stays active, now elsewhere. */
	g_assert_cmpuint(gsurf_wmux_layout_get_payload(l, 1), ==, 11);
	g_assert_cmpuint(gsurf_wmux_layout_get_payload(l, 3), ==, 13);
	g_assert_cmpuint(gsurf_wmux_layout_get_active(l), ==, 1);
	g_assert_cmpint(gsurf_wmux_layout_get_pane_index(l, 1), ==, 3);
	g_assert_true(gsurf_wmux_layout_swap(l, 2, 2));
	g_assert_false(gsurf_wmux_layout_swap(l, 2, 9));
	g_assert_false(gsurf_wmux_layout_swap(l, 9, 2));
	assert_valid(l);

	/* rotate -U: every pane one place earlier, the first goes last. */
	assert_order(l, "0 3 2 1");
	gsurf_wmux_layout_rotate(l, FALSE);
	assert_order(l, "3 2 1 0");
	assert_shape(l, "h(20:p3,30:p2,50:v(50:p1,50:p0))");
	g_assert_cmpuint(gsurf_wmux_layout_get_payload(l, 0), ==, 10);
	gsurf_wmux_layout_rotate(l, TRUE);
	assert_order(l, "0 3 2 1");
	gsurf_wmux_layout_rotate(l, TRUE);
	assert_order(l, "1 0 3 2");
	g_assert_cmpuint(gsurf_wmux_layout_find_payload(l, 12), ==, 2);
	assert_valid(l);
	{
		g_autoptr(GsurfWmuxLayout) one = gsurf_wmux_layout_new();

		gsurf_wmux_layout_rotate(one, FALSE);
		gsurf_wmux_layout_rotate(one, TRUE);
		assert_shape(one, "p0");
	}
}

static void
test_resize(void)
{
	g_autoptr(GsurfWmuxLayout) l = parse_ok("h(50:p0,50:p1)");

	/* -R grows p0 (its right border moves right); on the last pane the
	 * left border moves right instead. -L mirrors it. */
	g_assert_true(gsurf_wmux_layout_resize(l, 0, GSURF_WMUX_DIR_RIGHT, 10));
	assert_shape(l, "h(60:p0,40:p1)");
	g_assert_true(gsurf_wmux_layout_resize(l, 1, GSURF_WMUX_DIR_RIGHT, 10));
	assert_shape(l, "h(70:p0,30:p1)");
	g_assert_true(gsurf_wmux_layout_resize(l, 0, GSURF_WMUX_DIR_LEFT, 5));
	assert_shape(l, "h(65:p0,35:p1)");
	g_assert_true(gsurf_wmux_layout_resize(l, 1, GSURF_WMUX_DIR_LEFT, 5));
	assert_shape(l, "h(60:p0,40:p1)");
	/* Clamped at min_pct (10). */
	g_assert_true(gsurf_wmux_layout_resize(l, 0, GSURF_WMUX_DIR_RIGHT, 100));
	assert_shape(l, "h(90:p0,10:p1)");
	g_assert_false(gsurf_wmux_layout_resize(l, 0, GSURF_WMUX_DIR_RIGHT, 1));
	g_assert_true(gsurf_wmux_layout_resize(l, 0, GSURF_WMUX_DIR_LEFT, 100));
	assert_shape(l, "h(10:p0,90:p1)");
	/* No border along that axis; unknown pane; zero amount. */
	g_assert_false(gsurf_wmux_layout_resize(l, 0, GSURF_WMUX_DIR_UP, 5));
	g_assert_false(gsurf_wmux_layout_resize(l, 7, GSURF_WMUX_DIR_LEFT, 5));
	g_assert_false(gsurf_wmux_layout_resize(l, 0, GSURF_WMUX_DIR_RIGHT, 0));
	assert_valid(l);

	/* Nested: the nearest ancestor along the axis owns the border; the
	 * amount is a share of the window, not of the split. */
	{
		g_autoptr(GsurfWmuxLayout) n = parse_ok("h(50:p0,50:v(50:p1,50:p2))");

		g_assert_true(gsurf_wmux_layout_resize(n, 1, GSURF_WMUX_DIR_LEFT, 10));
		assert_shape(n, "h(40:p0,60:v(50:p1,50:p2))");
		g_assert_true(gsurf_wmux_layout_resize(n, 1, GSURF_WMUX_DIR_DOWN, 10));
		assert_shape(n, "h(40:p0,60:v(60:p1,40:p2))");
		g_assert_true(gsurf_wmux_layout_resize(n, 2, GSURF_WMUX_DIR_UP, 30));
		assert_shape(n, "h(40:p0,60:v(30:p1,70:p2))");
		g_assert_false(gsurf_wmux_layout_resize(n, 0, GSURF_WMUX_DIR_UP, 5));
	}
	/* A border inside a half-width split: 10% of the window is 20% of
	 * the split. */
	{
		g_autoptr(GsurfWmuxLayout) n = parse_ok("v(50:p0,50:h(50:p1,50:p2))");

		g_assert_false(gsurf_wmux_layout_resize(n, 1, GSURF_WMUX_DIR_RIGHT, 0));
		g_assert_true(gsurf_wmux_layout_resize(n, 1, GSURF_WMUX_DIR_RIGHT, 10));
		assert_shape(n, "v(50:p0,50:h(60:p1,40:p2))");
	}
	{
		g_autoptr(GsurfWmuxLayout) n = parse_ok("h(50:p0,50:v(50:p1,50:h(50:p3,50:p4)))");

		g_assert_true(gsurf_wmux_layout_resize(n, 3, GSURF_WMUX_DIR_RIGHT, 5));
		assert_shape(n, "h(50:p0,50:v(50:p1,50:h(60:p3,40:p4)))");
	}
	/* The clamp protects every pane on the shrinking side, which shrink
	 * proportionally: p2 and p3 are 35% wide each, so the v cell may
	 * only shrink to 20% of the window. */
	{
		g_autoptr(GsurfWmuxLayout) n = parse_ok("h(30:p0,70:v(50:p1,50:h(50:p2,50:p3)))");
		g_autoptr(GArray) rects = NULL;

		g_assert_true(gsurf_wmux_layout_resize(n, 0, GSURF_WMUX_DIR_RIGHT, 100));
		assert_shape(n, "h(80:p0,20:v(50:p1,50:h(50:p2,50:p3)))");
		rects = gsurf_wmux_layout_geometry(n, 1000, 100, 0);
		g_assert_cmpint(rect_of(rects, 2)->width, ==, 100);
		g_assert_cmpint(rect_of(rects, 3)->width, ==, 100);
		assert_valid(n);
	}
	/* min_pct 0: down to one weight unit. */
	{
		g_autoptr(GsurfWmuxLayout) n = parse_ok("h(50:p0,50:p1)");

		gsurf_wmux_layout_set_min_pct(n, 0);
		g_assert_true(gsurf_wmux_layout_resize(n, 0, GSURF_WMUX_DIR_RIGHT, 100));
		assert_shape(n, "h(99.99:p0,0.01:p1)");
		g_assert_false(gsurf_wmux_layout_resize(n, 0, GSURF_WMUX_DIR_RIGHT, 100));
		assert_valid(n);
	}
	/* Panes already below the minimum (from a preset) do not block the
	 * other direction, and are never shrunk further. */
	{
		g_autoptr(GsurfWmuxLayout) n = parse_ok("h(5:p0,95:p1)");

		g_assert_false(gsurf_wmux_layout_resize(n, 0, GSURF_WMUX_DIR_LEFT, 1));
		g_assert_true(gsurf_wmux_layout_resize(n, 0, GSURF_WMUX_DIR_RIGHT, 1));
		assert_shape(n, "h(6:p0,94:p1)");
	}
}

static void
test_zoom_and_spread(void)
{
	g_autoptr(GsurfWmuxLayout) l = gsurf_wmux_layout_new();
	g_autoptr(GArray) rects = NULL;

	/* One pane: nothing to zoom. */
	g_assert_false(gsurf_wmux_layout_toggle_zoom(l));
	g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(l), ==, NONE);
	gsurf_wmux_layout_split(l, 0, GSURF_WMUX_SPLIT_H, FALSE, 50, NULL);
	gsurf_wmux_layout_set_active(l, 1);
	g_assert_true(gsurf_wmux_layout_toggle_zoom(l));
	g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(l), ==, 1);
	rects = gsurf_wmux_layout_geometry(l, 800, 600, 4);
	g_assert_cmpuint(rects->len, ==, 1);
	g_assert_cmpuint(g_array_index(rects, GsurfWmuxRect, 0).pane, ==, 1);
	g_assert_cmpint(g_array_index(rects, GsurfWmuxRect, 0).width, ==, 800);
	g_assert_cmpint(g_array_index(rects, GsurfWmuxRect, 0).height, ==, 600);
	assert_valid(l);
	g_assert_false(gsurf_wmux_layout_toggle_zoom(l));
	g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(l), ==, NONE);

	/* Selecting the zoomed pane keeps the zoom; another pane unzooms. */
	gsurf_wmux_layout_toggle_zoom(l);
	gsurf_wmux_layout_set_active(l, 1);
	g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(l), ==, 1);
	gsurf_wmux_layout_set_active(l, 0);
	g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(l), ==, NONE);

	/* Structural operations unzoom. */
#define ZOOM_THEN(op) \
	G_STMT_START { \
		g_assert_true(gsurf_wmux_layout_set_zoomed(l, 0)); \
		op; \
		g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(l), ==, NONE); \
	} G_STMT_END
	ZOOM_THEN(gsurf_wmux_layout_split(l, 1, GSURF_WMUX_SPLIT_V, FALSE, 50, NULL));
	ZOOM_THEN(gsurf_wmux_layout_remove(l, 2, NULL));
	ZOOM_THEN(gsurf_wmux_layout_swap(l, 0, 1));
	ZOOM_THEN(gsurf_wmux_layout_rotate(l, FALSE));
	ZOOM_THEN(gsurf_wmux_layout_resize(l, 0, GSURF_WMUX_DIR_RIGHT, 5));
	ZOOM_THEN(gsurf_wmux_layout_spread_evenly(l, 0));
	ZOOM_THEN(gsurf_wmux_layout_apply_preset(l, GSURF_WMUX_PRESET_TILED));
#undef ZOOM_THEN
	/* set_zoomed: any pane, NONE, never an unknown pane. */
	g_assert_false(gsurf_wmux_layout_set_zoomed(l, 9));
	g_assert_true(gsurf_wmux_layout_set_zoomed(l, NONE));

	/* Spread evenly over the pane's split. */
	{
		g_autoptr(GsurfWmuxLayout) s = parse_ok("h(10:p0,20:p1,70:v(30:p2,70:p3))");

		g_assert_true(gsurf_wmux_layout_spread_evenly(s, 1));
		assert_shape(s, "h(33.34:p0,33.33:p1,33.33:v(30:p2,70:p3))");
		g_assert_true(gsurf_wmux_layout_spread_evenly(s, 3));
		assert_shape(s, "h(33.34:p0,33.33:p1,33.33:v(50:p2,50:p3))");
		g_assert_false(gsurf_wmux_layout_spread_evenly(s, 9));
		assert_valid(s);
	}
	{
		g_autoptr(GsurfWmuxLayout) one = gsurf_wmux_layout_new();

		g_assert_false(gsurf_wmux_layout_spread_evenly(one, 0));
	}
}

/* The five presets for 1..6 panes. */
static void
test_presets(void)
{
	static const struct {
		guint           n;
		GsurfWmuxPreset preset;
		const gchar    *shape;
	} cases[] = {
		{ 1, GSURF_WMUX_PRESET_EVEN_HORIZONTAL, "p0" },
		{ 1, GSURF_WMUX_PRESET_MAIN_VERTICAL, "p0" },
		{ 1, GSURF_WMUX_PRESET_TILED, "p0" },
		{ 2, GSURF_WMUX_PRESET_MAIN_HORIZONTAL, "v(60:p0,40:p1)" },
		{ 2, GSURF_WMUX_PRESET_MAIN_VERTICAL, "h(60:p0,40:p1)" },
		{ 2, GSURF_WMUX_PRESET_TILED, "h(50:p0,50:p1)" },
		{ 3, GSURF_WMUX_PRESET_EVEN_HORIZONTAL, "h(33.34:p0,33.33:p1,33.33:p2)" },
		{ 3, GSURF_WMUX_PRESET_EVEN_VERTICAL, "v(33.34:p0,33.33:p1,33.33:p2)" },
		{ 3, GSURF_WMUX_PRESET_TILED, "v(50:h(50:p0,50:p1),50:p2)" },
		{ 4, GSURF_WMUX_PRESET_EVEN_HORIZONTAL, "h(25:p0,25:p1,25:p2,25:p3)" },
		{ 4, GSURF_WMUX_PRESET_EVEN_VERTICAL, "v(25:p0,25:p1,25:p2,25:p3)" },
		{ 4, GSURF_WMUX_PRESET_MAIN_HORIZONTAL, "v(60:p0,40:h(33.34:p1,33.33:p2,33.33:p3))" },
		{ 4, GSURF_WMUX_PRESET_MAIN_VERTICAL, "h(60:p0,40:v(33.34:p1,33.33:p2,33.33:p3))" },
		{ 4, GSURF_WMUX_PRESET_TILED, "v(50:h(50:p0,50:p1),50:h(50:p2,50:p3))" },
		{ 5, GSURF_WMUX_PRESET_TILED,
		  "v(50:h(33.34:p0,33.33:p1,33.33:p2),50:h(50:p3,50:p4))" },
		{ 6, GSURF_WMUX_PRESET_TILED,
		  "v(50:h(33.34:p0,33.33:p1,33.33:p2),50:h(33.34:p3,33.33:p4,33.33:p5))" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(GsurfWmuxLayout) l = layout_with(cases[i].n);

		gsurf_wmux_layout_apply_preset(l, cases[i].preset);
		assert_shape(l, cases[i].shape);
		g_assert_cmpint(gsurf_wmux_layout_get_preset(l), ==, (gint)cases[i].preset);
		assert_valid(l);
	}

	/* Presets keep pane order, payloads and the active pane; main_pct
	 * sizes the main pane. */
	{
		g_autoptr(GsurfWmuxLayout) l = parse_ok("v(30:p5=50,70:h(20:p2=20,80:p7=70))");

		gsurf_wmux_layout_set_active(l, 7);
		gsurf_wmux_layout_set_main_pct(l, 70);
		gsurf_wmux_layout_apply_preset(l, GSURF_WMUX_PRESET_MAIN_VERTICAL);
		assert_shape(l, "h(70:p5,30:v(50:p2,50:p7))");
		g_assert_cmpuint(gsurf_wmux_layout_get_payload(l, 7), ==, 70);
		g_assert_cmpuint(gsurf_wmux_layout_get_active(l), ==, 7);
		assert_order(l, "5 2 7");
		assert_valid(l);
	}

	/* next-layout cycles from even-horizontal and wraps. */
	{
		g_autoptr(GsurfWmuxLayout) l = layout_with(3);

		for (i = 0; i < GSURF_WMUX_PRESET_COUNT * 2; i++) {
			GsurfWmuxPreset p = gsurf_wmux_layout_next_preset(l);

			g_assert_cmpint(p, ==, (gint)(i % GSURF_WMUX_PRESET_COUNT));
			g_assert_cmpint(gsurf_wmux_layout_get_preset(l), ==, (gint)p);
			assert_valid(l);
		}
	}

	/* Names. */
	for (i = 0; i < GSURF_WMUX_PRESET_COUNT; i++) {
		GsurfWmuxPreset p = GSURF_WMUX_PRESET_COUNT;

		g_assert_true(gsurf_wmux_preset_from_string(
			gsurf_wmux_preset_to_string((GsurfWmuxPreset)i), &p));
		g_assert_cmpint(p, ==, (gint)i);
	}
	g_assert_cmpstr(gsurf_wmux_preset_to_string(GSURF_WMUX_PRESET_TILED), ==, "tiled");
	g_assert_null(gsurf_wmux_preset_to_string(GSURF_WMUX_PRESET_COUNT));
	g_assert_false(gsurf_wmux_preset_from_string("bogus", NULL));
	g_assert_false(gsurf_wmux_preset_from_string(NULL, NULL));
	g_assert_true(gsurf_wmux_preset_from_string("main-horizontal", NULL));
}

static void
test_geometry(void)
{
	g_autoptr(GsurfWmuxLayout) l = parse_ok("h(33.34:p0,33.33:p1,33.33:p2)");
	g_autoptr(GArray) r = NULL;
	gchar *why = NULL;

	/* Cumulative rounding: no drift, the last cell takes the rest. */
	r = gsurf_wmux_layout_geometry(l, 100, 50, 0);
	g_assert_cmpint(rect_of(r, 0)->x, ==, 0);
	g_assert_cmpint(rect_of(r, 0)->width, ==, 33);
	g_assert_cmpint(rect_of(r, 1)->x, ==, 33);
	g_assert_cmpint(rect_of(r, 1)->width, ==, 33);
	g_assert_cmpint(rect_of(r, 2)->x, ==, 66);
	g_assert_cmpint(rect_of(r, 2)->width, ==, 34);
	g_assert_cmpint(rect_of(r, 2)->height, ==, 50);
	g_clear_pointer(&r, g_array_unref);

	/* Gaps come off first. */
	r = gsurf_wmux_layout_geometry(l, 100, 50, 2);
	g_assert_cmpint(rect_of(r, 0)->width, ==, 32);
	g_assert_cmpint(rect_of(r, 1)->x, ==, 34);
	g_assert_cmpint(rect_of(r, 1)->width, ==, 32);
	g_assert_cmpint(rect_of(r, 2)->x, ==, 68);
	g_assert_cmpint(rect_of(r, 2)->width, ==, 32);
	g_clear_pointer(&r, g_array_unref);

	/* Too small for the gaps: they shrink; negative sizes are 0. */
	r = gsurf_wmux_layout_geometry(l, 3, 5, 2);
	g_assert_cmpint(rect_of(r, 0)->width + rect_of(r, 1)->width + rect_of(r, 2)->width, ==, 1);
	g_assert_cmpint(rect_of(r, 2)->x + rect_of(r, 2)->width, ==, 3);
	g_clear_pointer(&r, g_array_unref);
	r = gsurf_wmux_layout_geometry(l, -5, -1, -3);
	g_assert_cmpint(rect_of(r, 2)->width, ==, 0);
	g_assert_cmpint(rect_of(r, 2)->height, ==, 0);
	g_clear_pointer(&r, g_array_unref);

	/* Nested gaps. */
	{
		g_autoptr(GsurfWmuxLayout) n = parse_ok("h(50:p0,50:v(50:p1,50:p2))");

		r = gsurf_wmux_layout_geometry(n, 101, 100, 1);
		g_assert_cmpint(rect_of(r, 0)->width, ==, 50);
		g_assert_cmpint(rect_of(r, 1)->x, ==, 51);
		g_assert_cmpint(rect_of(r, 1)->width, ==, 50);
		g_assert_cmpint(rect_of(r, 1)->height, ==, 49);
		g_assert_cmpint(rect_of(r, 2)->y, ==, 50);
		g_assert_cmpint(rect_of(r, 2)->height, ==, 50);
		g_clear_pointer(&r, g_array_unref);
		g_assert_true(geometry_ok(n, 101, 100, 1, &why));
	}
	/* Exact tiling at awkward sizes and gaps. */
	{
		g_autoptr(GsurfWmuxLayout) n =
			parse_ok("v(17:h(1:p0,2:p1,3:p2),29:p3,54:h(7:p4,11:v(13:p5,17:p6),19:p7))");
		gint w;
		gint g;

		for (w = 0; w < 140; w += 7)
			for (g = 0; g < 6; g++)
				if (!geometry_ok(n, w, 73, g, &why) || !geometry_ok(n, 73, w, g, &why))
					g_error("%dx73 gap %d: %s", w, g, why);
	}
}

static void
test_borders(void)
{
	g_autoptr(GsurfWmuxLayout) l = parse_ok("h(50:p0,50:v(50:p1,50:p2))");
	g_autoptr(GArray) r = NULL;
	GsurfWmuxBorder b;
	GsurfWmuxBorder stale;

	/* 100x100, gap 2: p0 is 49 wide, the gap is x 49..50; in the right
	 * column (x 51.., 49 wide) the row gap is y 49..50. */
	g_assert_true(gsurf_wmux_layout_find_border(l, 100, 100, 2, 50, 10, &b));
	g_assert_cmpuint(b.before, ==, 0);
	g_assert_cmpuint(b.after, ==, 1);
	g_assert_cmpint(b.split, ==, GSURF_WMUX_SPLIT_H);
	g_assert_cmpint(b.position, ==, 49);
	g_assert_true(gsurf_wmux_layout_find_border(l, 100, 100, 2, 70, 50, &b));
	g_assert_cmpuint(b.before, ==, 1);
	g_assert_cmpuint(b.after, ==, 2);
	g_assert_cmpint(b.split, ==, GSURF_WMUX_SPLIT_V);
	g_assert_cmpint(b.position, ==, 49);
	/* Inside panes, outside the area. */
	g_assert_false(gsurf_wmux_layout_find_border(l, 100, 100, 2, 10, 10, &b));
	g_assert_false(gsurf_wmux_layout_find_border(l, 100, 100, 2, 70, 20, &b));
	g_assert_false(gsurf_wmux_layout_find_border(l, 100, 100, 2, 50, 150, &b));
	/* Gap 0: a band of two pixels each side of the boundary. */
	g_assert_true(gsurf_wmux_layout_find_border(l, 100, 100, 0, 48, 10, &b));
	g_assert_cmpint(b.position, ==, 50);
	g_assert_true(gsurf_wmux_layout_find_border(l, 100, 100, 0, 51, 10, &b));
	g_assert_false(gsurf_wmux_layout_find_border(l, 100, 100, 0, 52, 10, &b));

	/* Dragging moves the boundary to the pointer (within rounding). */
	g_assert_true(gsurf_wmux_layout_find_border(l, 100, 100, 2, 50, 10, &b));
	g_assert_true(gsurf_wmux_layout_move_border(l, &b, 100, 100, 2, 70));
	r = gsurf_wmux_layout_geometry(l, 100, 100, 2);
	g_assert_cmpint(ABS(rect_of(r, 0)->width - 70), <=, 1);
	g_clear_pointer(&r, g_array_unref);
	/* Same place again: no move. */
	g_assert_true(gsurf_wmux_layout_find_border(l, 100, 100, 2,
		rect_of((r = gsurf_wmux_layout_geometry(l, 100, 100, 2)), 0)->width, 10, &b));
	g_clear_pointer(&r, g_array_unref);
	g_assert_false(gsurf_wmux_layout_move_border(l, &b, 100, 100, 2, b.position));
	/* Clamped at min_pct: the right column keeps 10%. */
	g_assert_true(gsurf_wmux_layout_move_border(l, &b, 100, 100, 2, 99));
	assert_shape(l, "h(90:p0,10:v(50:p1,50:p2))");
	/* The row border inside the right column. */
	g_assert_true(gsurf_wmux_layout_find_border(l, 100, 100, 2, 95, 49, &b));
	g_assert_cmpint(b.split, ==, GSURF_WMUX_SPLIT_V);
	g_assert_true(gsurf_wmux_layout_move_border(l, &b, 100, 100, 2, 25));
	r = gsurf_wmux_layout_geometry(l, 100, 100, 2);
	g_assert_cmpint(ABS(rect_of(r, 1)->height - 25), <=, 1);
	g_clear_pointer(&r, g_array_unref);
	assert_valid(l);

	/* Stale or bogus borders are refused. */
	stale = b;
	stale.split = GSURF_WMUX_SPLIT_H;
	g_assert_false(gsurf_wmux_layout_move_border(l, &stale, 100, 100, 2, 30));
	stale = b;
	stale.after = 9;
	g_assert_false(gsurf_wmux_layout_move_border(l, &stale, 100, 100, 2, 30));
	stale = b;
	stale.after = stale.before;
	g_assert_false(gsurf_wmux_layout_move_border(l, &stale, 100, 100, 2, 30));
	stale = b;
	stale.before = 2;
	stale.after = 1;
	g_assert_false(gsurf_wmux_layout_move_border(l, &stale, 100, 100, 2, 30));
	g_assert_false(gsurf_wmux_layout_move_border(l, &b, 0, 0, 2, 30));
	{
		g_autoptr(GsurfWmuxLayout) t = parse_ok("h(30:p0,30:p1,40:p2)");
		GsurfWmuxBorder far = { 0, 2, GSURF_WMUX_SPLIT_H, 30 };

		/* Not adjacent children. */
		g_assert_false(gsurf_wmux_layout_move_border(t, &far, 100, 100, 0, 20));
	}
	/* Zoomed: no borders. */
	gsurf_wmux_layout_set_zoomed(l, 0);
	g_assert_false(gsurf_wmux_layout_find_border(l, 100, 100, 2, 89, 10, &b));
}

/* --- Serialization --- */

static void
test_serialize_parse(void)
{
	static const gchar *const round_trips[] = {
		"p0",
		"p17",
		"h(50:p0,50:p1)",
		"v(50:p0,50:h(50:p1,50:p2))",
		"h(33.34:p0,33.33:p1,33.33:p2)",
		"h(0.01:p0,99.99:p1)",
		"v(12.5:p3,87.5:h(10:p1,20:v(50:p2,50:p0),70:p9))",
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(round_trips); i++) {
		g_autoptr(GsurfWmuxLayout) l = parse_ok(round_trips[i]);

		assert_shape(l, round_trips[i]);
		assert_valid(l);
	}

	/* Payloads. */
	{
		g_autoptr(GsurfWmuxLayout) l = parse_ok("h(50:p3=7,50:p1=18446744073709551615)");
		g_autofree gchar *with = gsurf_wmux_layout_serialize(l, TRUE);

		g_assert_cmpstr(with, ==, "h(50:p3=7,50:p1=18446744073709551615)");
		assert_shape(l, "h(50:p3,50:p1)");
		g_assert_cmpuint(gsurf_wmux_layout_get_payload(l, 1), ==, G_MAXUINT64);
		/* First pane active; next id one more than the largest. */
		g_assert_cmpuint(gsurf_wmux_layout_get_active(l), ==, 3);
		g_assert_cmpuint(gsurf_wmux_layout_split(l, 1, GSURF_WMUX_SPLIT_V, FALSE, 50, NULL),
			==, 4);
	}
	/* Whitespace, relative weights, decimals, same-direction flattening. */
	{
		g_autoptr(GsurfWmuxLayout) a = parse_ok(" h ( 50 : p0 , 50 : p1 = 3 ) \n");
		g_autoptr(GsurfWmuxLayout) b = parse_ok("h(1:p0,1:p1,2:p2)");
		g_autoptr(GsurfWmuxLayout) c = parse_ok("h(33.3:p0,66.7:p1)");
		g_autoptr(GsurfWmuxLayout) d = parse_ok("h(50:p0,50:h(50:p1,50:v(1:p2,1:p3)))");
		g_autoptr(GsurfWmuxLayout) e = parse_ok("h(3:p0,3:p1,3:p2)");

		assert_shape(a, "h(50:p0,50:p1)");
		g_assert_cmpuint(gsurf_wmux_layout_get_payload(a, 1), ==, 3);
		assert_shape(b, "h(25:p0,25:p1,50:p2)");
		assert_shape(c, "h(33.3:p0,66.7:p1)");
		assert_shape(d, "h(50:p0,25:p1,25:v(50:p2,50:p3))");
		assert_shape(e, "h(33.33:p0,33.34:p1,33.33:p2)");
		assert_valid(d);
		assert_valid(e);
	}
	/* Copies are deep and equal. */
	{
		g_autoptr(GsurfWmuxLayout) l = parse_ok("h(50:p0=1,50:v(50:p1=2,50:p2=3))");
		g_autoptr(GsurfWmuxLayout) c = NULL;
		g_autofree gchar *sl = NULL;
		g_autofree gchar *sc = NULL;

		gsurf_wmux_layout_set_active(l, 2);
		gsurf_wmux_layout_toggle_zoom(l);
		c = gsurf_wmux_layout_copy(l);
		sl = gsurf_wmux_layout_serialize(l, TRUE);
		sc = gsurf_wmux_layout_serialize(c, TRUE);
		g_assert_cmpstr(sl, ==, sc);
		g_assert_cmpuint(gsurf_wmux_layout_get_active(c), ==, 2);
		g_assert_cmpuint(gsurf_wmux_layout_get_last(c), ==, 0);
		g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(c), ==, 2);
		g_assert_cmpuint(gsurf_wmux_layout_get_stamp(c, 2), ==,
			gsurf_wmux_layout_get_stamp(l, 2));
		gsurf_wmux_layout_remove(c, 1, NULL);
		assert_shape(l, "h(50:p0,50:v(50:p1,50:p2))");
		g_assert_cmpuint(gsurf_wmux_layout_split(c, 0, GSURF_WMUX_SPLIT_V, FALSE, 50, NULL),
			==, 3);
		assert_valid(c);
		assert_valid(l);
	}
}

static void
assert_parse_fail(const gchar *text, gint pos)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfWmuxLayout) l = NULL;
	g_autofree gchar *prefix = g_strdup_printf("position %d: ", pos);
	gint got = 99;

	l = gsurf_wmux_layout_parse(text, &got, &error);
	if (l != NULL)
		g_error("parse(\"%s\") should fail", text);
	g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_PARSE);
	if (got != pos || !g_str_has_prefix(error->message, prefix))
		g_error("parse(\"%s\"): expected position %d, got %d (%s)", text, pos, got,
			error->message);
}

static void
test_parse_errors(void)
{
	g_autoptr(GString) deep = g_string_new(NULL);
	guint i;

	assert_parse_fail("", 0);
	assert_parse_fail("   ", 3);
	assert_parse_fail("x", 0);
	assert_parse_fail("P0", 0);
	assert_parse_fail("p", 1);
	assert_parse_fail("p-1", 1);
	assert_parse_fail("p0 p1", 3);
	assert_parse_fail("p0)", 2);
	assert_parse_fail("h", 1);
	assert_parse_fail("h[", 1);
	assert_parse_fail("h(", 2);
	assert_parse_fail("h(50", 4);
	assert_parse_fail("h(50p0", 4);
	assert_parse_fail("h(50:", 5);
	assert_parse_fail("h(50:p0", 7);
	assert_parse_fail("h(50:p0;50:p1)", 7);
	assert_parse_fail("h(50:p0)", 1);
	assert_parse_fail("h(50:p0,)", 8);
	assert_parse_fail("h(50:p0,50:p0)", 11);
	assert_parse_fail("v(50:p0,50:h(50:p1,50:p0))", 22);
	assert_parse_fail("h(0:p0,50:p1)", 2);
	assert_parse_fail("h(0.00:p0,50:p1)", 2);
	assert_parse_fail("h(-1:p0,50:p1)", 2);
	assert_parse_fail("h(1.234:p0,50:p1)", 6);
	assert_parse_fail("h(5.:p0,50:p1)", 4);
	assert_parse_fail("h(.5:p0,50:p1)", 2);
	assert_parse_fail("h(99999999:p0,50:p1)", 2);
	assert_parse_fail("h(1:p0,99999:p1)", 2);
	assert_parse_fail("p4294967295", 1);
	assert_parse_fail("p99999999999", 1);
	assert_parse_fail("p0=", 3);
	assert_parse_fail("p0=x", 3);
	assert_parse_fail("p0=18446744073709551616", 3);
	assert_parse_fail("p0=1=2", 4);
	assert_parse_fail("h(50:p0,50:p1) junk", 15);

	/* Nesting deeper than the limit. */
	for (i = 0; i < GSURF_WMUX_LAYOUT_MAX_DEPTH + 1; i++)
		g_string_append(deep, i % 2 == 0 ? "h(1:p999," : "v(1:p998,");
	g_string_append(deep, "1:p0");
	for (i = 0; i < GSURF_WMUX_LAYOUT_MAX_DEPTH + 1; i++)
		g_string_append_c(deep, ')');
	{
		g_autoptr(GError) error = NULL;

		g_assert_null(gsurf_wmux_layout_parse(deep->str, NULL, &error));
		g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_PARSE);
	}
	{
		g_autoptr(GError) error = NULL;
		gint pos = 99;

		g_assert_null(gsurf_wmux_layout_parse(NULL, &pos, &error));
		g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_PARSE);
		g_assert_cmpint(pos, ==, 0);
	}
	g_assert_null(gsurf_wmux_layout_parse("bogus", NULL, NULL));
}

/* --- Window state --- */

static void
test_state(void)
{
	g_autoptr(GsurfWmuxState) state = gsurf_wmux_state_new();
	g_autoptr(GsurfWmuxState) back = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *again = NULL;
	GsurfWmuxLayout *a = parse_ok("h(50:p0=0,50:p2=1)");
	GsurfWmuxLayout *b = parse_ok("p0=2");
	GsurfWmuxLayout *c = parse_ok("v(60:p1=3,40:h(50:p4=4,50:p7=5))");
	GsurfWmuxWindowState *w;

	gsurf_wmux_layout_set_active(a, 2);
	gsurf_wmux_layout_toggle_zoom(a);
	gsurf_wmux_layout_set_active(c, 4);
	gsurf_wmux_layout_set_active(c, 7);
	g_assert_cmpuint(gsurf_wmux_state_add_window(state, "web \"x\" \\ é", TRUE, a), ==, 0);
	g_assert_cmpuint(gsurf_wmux_state_add_window(state, NULL, FALSE, b), ==, 1);
	g_assert_cmpuint(gsurf_wmux_state_add_window(state, "tab\there\nline", FALSE, c), ==, 2);
	state->current = 2;
	state->last = 0;

	text = gsurf_wmux_state_serialize(state);
	g_assert_cmpstr(text, ==,
		"wmux1 current=2 last=0"
		" [name=\"web \\\"x\\\" \\\\ é\" renamed=1 active=2 last=0 zoom=2 layout=h(50:p0=0,50:p2=1)]"
		" [name=\"\" renamed=0 active=0 layout=p0=2]"
		" [name=\"tab\\there\\nline\" renamed=0 active=7 last=4 layout=v(60:p1=3,40:h(50:p4=4,50:p7=5))]");

	back = gsurf_wmux_state_parse(text, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(back->windows->len, ==, 3);
	g_assert_cmpint(back->current, ==, 2);
	g_assert_cmpint(back->last, ==, 0);
	w = g_ptr_array_index(back->windows, 0);
	g_assert_cmpstr(w->name, ==, "web \"x\" \\ é");
	g_assert_true(w->renamed);
	g_assert_cmpuint(gsurf_wmux_layout_get_active(w->layout), ==, 2);
	g_assert_cmpuint(gsurf_wmux_layout_get_last(w->layout), ==, 0);
	g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(w->layout), ==, 2);
	g_assert_cmpuint(gsurf_wmux_layout_get_payload(w->layout, 2), ==, 1);
	w = g_ptr_array_index(back->windows, 1);
	g_assert_cmpstr(w->name, ==, "");
	g_assert_false(w->renamed);
	w = g_ptr_array_index(back->windows, 2);
	g_assert_cmpstr(w->name, ==, "tab\there\nline");
	g_assert_cmpuint(gsurf_wmux_layout_get_last(w->layout), ==, 4);
	g_assert_cmpuint(gsurf_wmux_layout_get_zoomed(w->layout), ==, NONE);
	again = gsurf_wmux_state_serialize(back);
	g_assert_cmpstr(again, ==, text);
	for (guint i = 0; i < back->windows->len; i++)
		assert_valid(((GsurfWmuxWindowState *)g_ptr_array_index(back->windows, i))->layout);

	/* Minimal and reordered forms. */
	{
		g_autoptr(GsurfWmuxState) s = gsurf_wmux_state_parse("wmux1 [layout=p0]", NULL, &error);

		g_assert_no_error(error);
		g_assert_cmpint(s->current, ==, 0);
		g_assert_cmpint(s->last, ==, -1);
		w = g_ptr_array_index(s->windows, 0);
		g_assert_cmpstr(w->name, ==, "");
		g_assert_false(w->renamed);
	}
	{
		g_autoptr(GsurfWmuxState) s = gsurf_wmux_state_parse(
			"  wmux1\tlast=0  current=1\n[ layout=p0  name=\"a\" ] [active=1 layout=h( 50:p0 , 50:p1 ) renamed=0]",
			NULL, &error);

		g_assert_no_error(error);
		g_assert_cmpint(s->current, ==, 1);
		g_assert_cmpint(s->last, ==, 0);
		w = g_ptr_array_index(s->windows, 1);
		g_assert_cmpuint(gsurf_wmux_layout_get_active(w->layout), ==, 1);
		g_assert_cmpuint(gsurf_wmux_layout_get_last(w->layout), ==, NONE);
	}
	gsurf_wmux_state_free(NULL);
}

static void
assert_state_fail(const gchar *text, gint pos)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfWmuxState) s = NULL;
	g_autofree gchar *prefix = g_strdup_printf("position %d: ", pos);
	gint got = 99;

	s = gsurf_wmux_state_parse(text, &got, &error);
	if (s != NULL)
		g_error("state_parse(\"%s\") should fail", text);
	g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_PARSE);
	if (got != pos || !g_str_has_prefix(error->message, prefix))
		g_error("state_parse(\"%s\"): expected position %d, got %d (%s)", text, pos, got,
			error->message);
}

static void
test_state_errors(void)
{
	assert_state_fail("", 0);
	assert_state_fail("wmux2 [layout=p0]", 0);
	assert_state_fail("wmux1", 5);
	assert_state_fail("wmux1x", 5);
	assert_state_fail("wmux1 ", 6);
	assert_state_fail("wmux1 current=1 [layout=p0]", 6);
	assert_state_fail("wmux1 last=3 [layout=p0]", 6);
	assert_state_fail("wmux1 current=x [layout=p0]", 14);
	assert_state_fail("wmux1 current=0 current=0 [layout=p0]", 16);
	assert_state_fail("wmux1 bogus=1 [layout=p0]", 6);
	assert_state_fail("wmux1 zzz [layout=p0]", 6);
	assert_state_fail("wmux1 [layout=p0] current=0", 18);
	assert_state_fail("wmux1 [layout=p0]x", 17);
	assert_state_fail("wmux1 [layout=p0", 16);
	assert_state_fail("wmux1 [layout=p0 ", 6);
	assert_state_fail("wmux1 [name=\"a\"]", 6);
	assert_state_fail("wmux1 [layout=p0 layout=p0]", 17);
	assert_state_fail("wmux1 [name=\"a\" name=\"b\"]", 16);
	assert_state_fail("wmux1 [layout=p0 active=5]", 17);
	assert_state_fail("wmux1 [layout=h(50:p0,50:p1) last=0]", 29);
	assert_state_fail("wmux1 [layout=p0 last=4]", 17);
	assert_state_fail("wmux1 [layout=p0 zoom=9]", 17);
	assert_state_fail("wmux1 [layout=p0 active=0 active=0]", 26);
	assert_state_fail("wmux1 [layout=p0 renamed=2]", 25);
	assert_state_fail("wmux1 [layout=p0 renamed=1 renamed=1]", 27);
	assert_state_fail("wmux1 [layout=p0 renamed=10]", 26);
	assert_state_fail("wmux1 [layout=p0 foo=1]", 17);
	assert_state_fail("wmux1 [layout=p0 Name=\"a\"]", 17);
	assert_state_fail("wmux1 [layout=p0 name=abc]", 22);
	assert_state_fail("wmux1 [layout=p0 name=\"abc]", 22);
	assert_state_fail("wmux1 [layout=p0 name=\"a\\qb\"]", 24);
	assert_state_fail("wmux1 [layout=p0 name=\"a\x01\"]", 24);
	assert_state_fail("wmux1 [layout=p0 name=\"\xff\"]", 22);
	assert_state_fail("wmux1 [layout=h(50:p0", 21);
	assert_state_fail("wmux1 [layout=h(50:p0,50:p1)x]", 28);
	assert_state_fail("wmux1 [layout=p0 active=]", 24);
	{
		g_autoptr(GError) error = NULL;
		gint pos = 99;

		g_assert_null(gsurf_wmux_state_parse(NULL, &pos, &error));
		g_assert_error(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_PARSE);
		g_assert_cmpint(pos, ==, 0);
	}
	g_assert_null(gsurf_wmux_state_parse("nope", NULL, NULL));
}

/* --- Stress --- */

/* 5000 random operations on one layout; after every one: all structural
 * invariants, exact geometry at a random size and gap, ids unique and
 * never reused, payloads kept with their panes, serialize/parse
 * round trip. */
static void
test_stress(void)
{
	guint32 seed = test_seed();
	g_autoptr(GRand) rand = g_rand_new_with_seed(seed);
	g_autoptr(GsurfWmuxLayout) l = gsurf_wmux_layout_new();
	g_autoptr(GHashTable) ever = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_autoptr(GHashTable) payloads = g_hash_table_new(g_direct_hash, g_direct_equal);
	guint64 next_payload = 1;
	guint max_panes = 1;
	guint splits = 0;
	guint removes = 0;
	guint op;

	g_hash_table_add(ever, GUINT_TO_POINTER(0));
	g_hash_table_insert(payloads, GUINT_TO_POINTER(0), GUINT_TO_POINTER(0));

	for (op = 0; op < 5000; op++) {
		guint n = gsurf_wmux_layout_get_n_panes(l);
		guint pane = gsurf_wmux_layout_get_nth_pane(l, (guint)g_rand_int_range(rand, 0, (gint32)n));
		guint other = gsurf_wmux_layout_get_nth_pane(l, (guint)g_rand_int_range(rand, 0, (gint32)n));
		GsurfWmuxDirection dir = (GsurfWmuxDirection)g_rand_int_range(rand, 0, 4);
		gint kind = g_rand_int_range(rand, 0, 18);
		gchar *why = NULL;
		GHashTableIter it;
		gpointer key;
		gpointer value;
		gint w;
		gint h;

		if (op % 500 == 0)
			gsurf_wmux_layout_set_min_pct(l, (guint)g_rand_int_range(rand, 0, 21));

		switch (kind) {
		case 0:
		case 1:
		case 2: {
			guint fresh = gsurf_wmux_layout_split(l, pane,
				g_rand_boolean(rand) ? GSURF_WMUX_SPLIT_H : GSURF_WMUX_SPLIT_V,
				g_rand_boolean(rand), (guint)g_rand_int_range(rand, 1, 100), NULL);

			if (fresh != NONE) {
				splits++;
				if (g_hash_table_contains(ever, GUINT_TO_POINTER(fresh)))
					g_error("seed %u op %u: pane id %u reused", seed, op, fresh);
				g_hash_table_add(ever, GUINT_TO_POINTER(fresh));
				g_hash_table_insert(payloads, GUINT_TO_POINTER(fresh), GUINT_TO_POINTER(0));
			}
			break;
		}
		case 3:
		case 4:
			/* Keep the layout busy: remove more when it is big. */
			if (n > 12 || g_rand_boolean(rand)) {
				gboolean ok = gsurf_wmux_layout_remove(l, pane, NULL);

				if (ok != (n > 1))
					g_error("seed %u op %u: remove returned %d with %u panes", seed, op,
						ok, n);
				if (ok) {
					removes++;
					g_hash_table_remove(payloads, GUINT_TO_POINTER(pane));
				}
			}
			break;
		case 5:
			gsurf_wmux_layout_set_active(l, pane);
			break;
		case 6: {
			guint nb = gsurf_wmux_layout_neighbor(l, gsurf_wmux_layout_get_active(l), dir,
				g_rand_boolean(rand));

			if (nb != NONE)
				gsurf_wmux_layout_set_active(l, nb);
			break;
		}
		case 7:
			gsurf_wmux_layout_set_active(l, g_rand_boolean(rand)
				? gsurf_wmux_layout_next(l, gsurf_wmux_layout_get_active(l))
				: gsurf_wmux_layout_prev(l, gsurf_wmux_layout_get_active(l)));
			break;
		case 8:
			gsurf_wmux_layout_swap(l, pane, other);
			break;
		case 9:
			gsurf_wmux_layout_rotate(l, g_rand_boolean(rand));
			break;
		case 10:
			gsurf_wmux_layout_resize(l, pane, dir, (guint)g_rand_int_range(rand, 1, 30));
			break;
		case 11:
			gsurf_wmux_layout_toggle_zoom(l);
			break;
		case 12:
			gsurf_wmux_layout_spread_evenly(l, pane);
			break;
		case 13:
			if (g_rand_boolean(rand))
				gsurf_wmux_layout_apply_preset(l,
					(GsurfWmuxPreset)g_rand_int_range(rand, 0, GSURF_WMUX_PRESET_COUNT));
			else
				gsurf_wmux_layout_next_preset(l);
			break;
		case 14:
			gsurf_wmux_layout_set_payload(l, pane, next_payload);
			g_hash_table_insert(payloads, GUINT_TO_POINTER(pane),
				GUINT_TO_POINTER((guint)next_payload));
			next_payload++;
			break;
		case 15: {
			/* Drag a border found at a random point. */
			GsurfWmuxBorder b;
			gint bw = g_rand_int_range(rand, 50, 400);
			gint bh = g_rand_int_range(rand, 50, 400);
			gint gap = g_rand_int_range(rand, 0, 6);

			if (gsurf_wmux_layout_find_border(l, bw, bh, gap, g_rand_int_range(rand, 0, bw),
					g_rand_int_range(rand, 0, bh), &b))
				gsurf_wmux_layout_move_border(l, &b, bw, bh, gap,
					g_rand_int_range(rand, -10, MAX(bw, bh) + 10));
			break;
		}
		case 16: {
			/* Round trip through the string (ids, payloads kept). */
			g_autofree gchar *s = gsurf_wmux_layout_serialize(l, TRUE);
			g_autoptr(GError) error = NULL;
			GsurfWmuxLayout *p = gsurf_wmux_layout_parse(s, NULL, &error);
			g_autofree gchar *s2 = NULL;

			if (p == NULL)
				g_error("seed %u op %u: \"%s\" does not parse: %s", seed, op, s,
					error->message);
			s2 = gsurf_wmux_layout_serialize(p, TRUE);
			if (strcmp(s, s2) != 0)
				g_error("seed %u op %u: \"%s\" reparses as \"%s\"", seed, op, s, s2);
			gsurf_wmux_layout_free(p);
			break;
		}
		default: {
			/* Continue on a copy. */
			GsurfWmuxLayout *c = gsurf_wmux_layout_copy(l);

			gsurf_wmux_layout_free(l);
			l = c;
			break;
		}
		}

		/* Invariants. */
		max_panes = MAX(max_panes, gsurf_wmux_layout_get_n_panes(l));
		if (!gsurf_wmux_layout_check(l, &why))
			g_error("seed %u op %u (kind %d): %s", seed, op, kind, why);
		w = g_rand_int_range(rand, 0, 120);
		h = g_rand_int_range(rand, 0, 80);
		if (!geometry_ok(l, w, h, 0, &why) ||
		    !geometry_ok(l, w, h, g_rand_int_range(rand, 1, 5), &why))
			g_error("seed %u op %u (kind %d) at %dx%d: %s", seed, op, kind, w, h, why);
		if (g_hash_table_size(payloads) != gsurf_wmux_layout_get_n_panes(l))
			g_error("seed %u op %u: %u panes, %u expected", seed, op,
				gsurf_wmux_layout_get_n_panes(l), g_hash_table_size(payloads));
		g_hash_table_iter_init(&it, payloads);
		while (g_hash_table_iter_next(&it, &key, &value))
			if (gsurf_wmux_layout_get_payload(l, GPOINTER_TO_UINT(key)) !=
			    GPOINTER_TO_UINT(value))
				g_error("seed %u op %u: pane %u lost its payload", seed, op,
					GPOINTER_TO_UINT(key));
	}
	/* The run must have exercised real layouts, not one pane. */
	g_test_message("%u splits, %u removes, at most %u panes", splits, removes, max_panes);
	g_assert_cmpuint(splits, >, 200);
	g_assert_cmpuint(removes, >, 200);
	g_assert_cmpuint(max_panes, >=, 8);
}

/* --- Fuzz --- */

/* A parse outcome must be a valid layout (that round-trips) or a parse
 * error with a position inside the text. */
static void
check_layout_text(const gchar *text, guint32 seed)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfWmuxLayout) l = NULL;
	gint pos = 99;

	l = gsurf_wmux_layout_parse(text, &pos, &error);
	if (l != NULL) {
		g_autofree gchar *s = gsurf_wmux_layout_serialize(l, TRUE);
		g_autoptr(GsurfWmuxLayout) again = gsurf_wmux_layout_parse(s, NULL, NULL);
		g_autofree gchar *s2 = NULL;
		gchar *why = NULL;

		if (!gsurf_wmux_layout_check(l, &why))
			g_error("seed %u: \"%s\" parsed invalid: %s", seed, text, why);
		if (again == NULL)
			g_error("seed %u: \"%s\" -> \"%s\" does not reparse", seed, text, s);
		s2 = gsurf_wmux_layout_serialize(again, TRUE);
		if (strcmp(s, s2) != 0)
			g_error("seed %u: \"%s\" is not stable (\"%s\" / \"%s\")", seed, text, s, s2);
		return;
	}
	if (!g_error_matches(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_PARSE))
		g_error("seed %u: \"%s\" failed without a parse error", seed, text);
	if (pos < 0 || pos > (gint)strlen(text))
		g_error("seed %u: \"%s\": position %d outside the text", seed, text, pos);
}

static void
check_state_text(const gchar *text, guint32 seed)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfWmuxState) s = NULL;
	gint pos = 99;

	s = gsurf_wmux_state_parse(text, &pos, &error);
	if (s != NULL) {
		g_autofree gchar *out = gsurf_wmux_state_serialize(s);
		g_autoptr(GsurfWmuxState) again = gsurf_wmux_state_parse(out, NULL, NULL);
		g_autofree gchar *out2 = NULL;

		if (again == NULL)
			g_error("seed %u: state \"%s\" -> \"%s\" does not reparse", seed, text, out);
		out2 = gsurf_wmux_state_serialize(again);
		if (strcmp(out, out2) != 0)
			g_error("seed %u: state \"%s\" is not stable", seed, text);
		return;
	}
	if (!g_error_matches(error, GSURF_WMUX_LAYOUT_ERROR, GSURF_WMUX_LAYOUT_ERROR_PARSE))
		g_error("seed %u: state \"%s\" failed without a parse error", seed, text);
	if (pos < 0 || pos > (gint)strlen(text))
		g_error("seed %u: state \"%s\": position %d outside the text", seed, text, pos);
}

/* Mutate @text with 1..3 random byte edits. */
static gchar *
mutate(GRand *rand, const gchar *text)
{
	gchar *line = g_strdup(text);
	guint edits = (guint)g_rand_int_range(rand, 1, 4);
	guint e;

	for (e = 0; e < edits; e++) {
		gsize at = (gsize)g_rand_int_range(rand, 0, (gint32)strlen(line) + 1);
		gchar c = (gchar)g_rand_int_range(rand, 1, 256);
		gchar *next;

		switch (g_rand_int_range(rand, 0, 3)) {
		case 0:
			if (line[at] != '\0')
				line[at] = c;
			break;
		case 1:
			if (line[at] != '\0')
				memmove(line + at, line + at + 1, strlen(line + at));
			break;
		default:
			next = g_strdup_printf("%.*s%c%s", (gint)at, line, c, line + at);
			g_free(line);
			line = next;
			break;
		}
	}
	return line;
}

static void
test_fuzz(void)
{
	static const gchar *const atoms[] = {
		"h", "v", "p", "(", ")", ":", ",", "=", "0", "1", "5", "50", "33.3", ".",
		"99999999999", " ", "p0", "p1", "50:p", "h(", "v(", "wmux1", "[", "]",
		"layout=", "name=", "\"", "\\", "active=", "zoom=", "last=", "current=",
		"renamed=1", "\xff", "\x01", "-",
	};
	static const gchar *const layouts[] = {
		"v(50:p0,50:h(50:p1=7,50:p2))",
		"h(33.34:p0,33.33:p1,33.33:v(12.5:p3,87.5:p4=18446744073709551615))",
	};
	static const gchar *const states[] = {
		"wmux1 current=1 last=0 [name=\"a \\\"b\\\"\" renamed=1 active=1 last=0 zoom=1 layout=h(50:p0=1,50:p1=2)] [name=\"\" renamed=0 active=3 layout=v(50:p2,50:p3)]",
	};
	guint32 seed = test_seed();
	g_autoptr(GRand) rand = g_rand_new_with_seed(seed);
	guint i;

	/* Random token soup. */
	for (i = 0; i < 30000; i++) {
		g_autoptr(GString) s = g_string_new(NULL);
		guint n = (guint)g_rand_int_range(rand, 0, 14);
		guint j;

		for (j = 0; j < n; j++)
			g_string_append(s, atoms[g_rand_int_range(rand, 0, G_N_ELEMENTS(atoms))]);
		check_layout_text(s->str, seed);
		check_state_text(s->str, seed);
	}
	/* Every prefix of valid strings, then mutations. */
	for (i = 0; i < G_N_ELEMENTS(layouts); i++) {
		gsize cut;
		guint m;

		for (cut = 0; cut <= strlen(layouts[i]); cut++) {
			g_autofree gchar *prefix = g_strndup(layouts[i], cut);

			check_layout_text(prefix, seed);
		}
		for (m = 0; m < 5000; m++) {
			g_autofree gchar *line = mutate(rand, layouts[i]);

			check_layout_text(line, seed);
		}
	}
	for (i = 0; i < G_N_ELEMENTS(states); i++) {
		gsize cut;
		guint m;

		for (cut = 0; cut <= strlen(states[i]); cut++) {
			g_autofree gchar *prefix = g_strndup(states[i], cut);

			check_state_text(prefix, seed);
		}
		for (m = 0; m < 10000; m++) {
			g_autofree gchar *line = mutate(rand, states[i]);

			check_state_text(line, seed);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Drop zones                                                          */
/* ------------------------------------------------------------------ */

/* The outer 25% of each side is that edge, the rest the centre; where
 * two bands meet the one the point is deeper into wins. */
static void
test_drop_zone(void)
{
	/* 1. Centre, and the four edge bands of a 400x200 pane. */
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 200, 100, 25), ==, GSURF_WMUX_DROP_CENTER);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 100, 100, 25), ==, GSURF_WMUX_DROP_CENTER);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 99, 100, 25), ==, GSURF_WMUX_DROP_LEFT);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 0, 100, 25), ==, GSURF_WMUX_DROP_LEFT);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 301, 100, 25), ==, GSURF_WMUX_DROP_RIGHT);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 400, 100, 25), ==, GSURF_WMUX_DROP_RIGHT);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 200, 49, 25), ==, GSURF_WMUX_DROP_TOP);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 200, 151, 25), ==, GSURF_WMUX_DROP_BOTTOM);

	/* 2. Corners: the relatively nearer edge. */
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 10, 40, 25), ==, GSURF_WMUX_DROP_LEFT);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 80, 5, 25), ==, GSURF_WMUX_DROP_TOP);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 395, 195, 25), ==, GSURF_WMUX_DROP_RIGHT);

	/* 3. Out of range points clamp; a band of 0 is all centre; bands
	 *    over 50% are 50%; a pane without size is all centre. */
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, -50, 100, 25), ==, GSURF_WMUX_DROP_LEFT);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 999, 100, 25), ==, GSURF_WMUX_DROP_RIGHT);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 0, 0, 0), ==, GSURF_WMUX_DROP_CENTER);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, 200, 190, 100, 90), ==, GSURF_WMUX_DROP_LEFT);
	g_assert_cmpint(gsurf_wmux_drop_zone(0, 200, 0, 0, 25), ==, GSURF_WMUX_DROP_CENTER);
	g_assert_cmpint(gsurf_wmux_drop_zone(400, -1, 0, 0, 25), ==, GSURF_WMUX_DROP_CENTER);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/wmux-layout/new", test_new);
	g_test_add_func("/gsurf/wmux-layout/split", test_split);
	g_test_add_func("/gsurf/wmux-layout/remove", test_remove);
	g_test_add_func("/gsurf/wmux-layout/active-last-next", test_active_last_next);
	g_test_add_func("/gsurf/wmux-layout/neighbor", test_neighbor);
	g_test_add_func("/gsurf/wmux-layout/swap-rotate", test_swap_rotate);
	g_test_add_func("/gsurf/wmux-layout/resize", test_resize);
	g_test_add_func("/gsurf/wmux-layout/zoom-spread", test_zoom_and_spread);
	g_test_add_func("/gsurf/wmux-layout/presets", test_presets);
	g_test_add_func("/gsurf/wmux-layout/geometry", test_geometry);
	g_test_add_func("/gsurf/wmux-layout/borders", test_borders);
	g_test_add_func("/gsurf/wmux-layout/serialize-parse", test_serialize_parse);
	g_test_add_func("/gsurf/wmux-layout/parse-errors", test_parse_errors);
	g_test_add_func("/gsurf/wmux-layout/state", test_state);
	g_test_add_func("/gsurf/wmux-layout/state-errors", test_state_errors);
	g_test_add_func("/gsurf/wmux-layout/drop-zone", test_drop_zone);
	g_test_add_func("/gsurf/wmux-layout/stress", test_stress);
	g_test_add_func("/gsurf/wmux-layout/fuzz", test_fuzz);
	return g_test_run();
}
