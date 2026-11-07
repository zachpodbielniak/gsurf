/*
 * gsurf-lrg-scene.c - the --lrg=3d tab space
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * See gsurf-lrg-scene.h for the model.  Implementation notes:
 *
 * - Panel keys: a tab's key is its #GsurfView pointer; a history entry's key
 *   has the top bit set (gsurf_lrg_scene_history_key()), so the two can never
 *   collide.  The scene holds a reference on every tab it drew last frame, so
 *   mapping a picked key back to a view never touches freed memory.
 * - Every panel is a libregnum *texture panel*: it borrows the tab's own
 *   #GrlTexture, so nothing is read back from the GPU (capture is disabled on
 *   the surface).
 * - The frame is driven entirely by the owning window's 16 ms tick: begin the
 *   surface frame, sync panels, step the easing, end the frame (which renders
 *   the scene, calls the HUD and swaps).
 */

#include "backend/lrg/gsurf-lrg-scene.h"
#include "backend/lrg/gsurf-lrg-view.h"
#include "core/gsurf-container-registry.h"

#include <libregnum.h>
#include <math.h>
#include <string.h>

#include <glib/gstdio.h>

/* The top bit marks history-entry keys; the low bits carry offset + bias. */
#define SCENE_HISTORY_BIT   G_GUINT64_CONSTANT(0x8000000000000000)
#define SCENE_HISTORY_BIAS  1024

/* Page thumbnails for history depth: how many to keep, how often the active
 * page's thumbnail is refreshed, and how much smaller than the page they are. */
#define SCENE_THUMB_MAX         64
#define SCENE_THUMB_REFRESH_US  (2 * G_USEC_PER_SEC)
#define SCENE_THUMB_DIVISOR     2

/* Hidden tabs still refresh now and then so their panel is not stale when the
 * camera swings round; panels this small on screen refresh at half rate. */
#define SCENE_HIDDEN_REFRESH_US (3 * G_USEC_PER_SEC)
#define SCENE_SMALL_COVERAGE    120.0f

/* Arrangements the cycle steps through.  single-panel and per-window are
 * left out: with one texture per tab they stack every panel on one plane. */
static const gchar *const scene_tab_arrangements[] = {
	"carousel", "grid", "stack", "free", NULL
};

static const gchar *const scene_environments[] = {
	"studio", "sky", "workshop", "cockpit", "void", NULL
};

/* Per-tab refresh bookkeeping. */
typedef struct {
	gint64   last_capture_us;
	gint64   last_thumb_us;
	gboolean captured_once;
	guint64  mip_serial;      /* texture serial the panel's mipmaps match */
} SceneViewState;

/* A page thumbnail: the CPU image, its texture once uploaded (created lazily
 * inside a frame, where the GL context is current), and an LRU stamp.
 * @real is FALSE for a placeholder (title card) not yet replaced by pixels. */
typedef struct {
	GrlImage   *image;
	GrlTexture *texture;
	gint64      used_us;
	gboolean    real;
} SceneThumb;

struct _GsurfLrgScene {
	GObject parent_instance;

	Lrg3DSurface *surface;

	gchar   *arrangement;     /* the tab arrangement (restored after history) */
	gchar   *environment;
	gdouble  background_fps;
	guint    history_limit;

	gboolean reading;
	guint64  reading_key;     /* the panel reading mode framed */
	LrgPose *pre_reading;     /* camera to return to when reading ends */
	gboolean history;
	gboolean reframe;         /* re-aim the camera after the next layout */
	gboolean camera_touched;  /* the user moved the camera since the last aim */
	guint    last_panel_count;

	GHashTable *view_state;   /* GsurfView* (not deref'd) -> SceneViewState* */
	GHashTable *thumbs;       /* uri -> SceneThumb* */
	GHashTable *placements;   /* GsurfView* (not deref'd) -> gfloat[6] */
	GPtrArray  *frame_views;  /* GsurfView*, reffed: last frame's tabs */
	GsurfView  *active;       /* borrowed from frame_views */
	gint        body_w;
	gint        body_h;

	GsurfLrgSceneHudFunc hud;
	gpointer             hud_data;
	GDestroyNotify       hud_destroy;

	GSList *shots;            /* SceneShot*, oldest last */
};

/* A pending screenshot request. */
typedef struct {
	gchar                *path;
	GsurfLrgSceneShotFunc func;
	gpointer              user_data;
} SceneShot;

G_DEFINE_FINAL_TYPE(GsurfLrgScene, gsurf_lrg_scene, G_TYPE_OBJECT)

/* ======================================================================== *
 *  Pure helpers
 * ======================================================================== */

gint64
gsurf_lrg_scene_refresh_interval(gboolean is_active, gfloat coverage,
                                 gdouble background_fps)
{
	gint64 base;

	/* The tab being read always gets every frame. */
	if (is_active)
		return 0;
	/* Behind the camera or out of view: just keep it from going stale. */
	if (coverage <= 0.0f)
		return SCENE_HIDDEN_REFRESH_US;

	background_fps = CLAMP(background_fps, 0.2, 60.0);
	base = (gint64)((gdouble)G_USEC_PER_SEC / background_fps);
	/* A thumbnail-sized panel shows no detail worth the snapshot cost. */
	if (coverage < SCENE_SMALL_COVERAGE)
		base *= 2;
	return base;
}

guint64
gsurf_lrg_scene_history_key(gint offset)
{
	return SCENE_HISTORY_BIT | (guint64)(offset + SCENE_HISTORY_BIAS);
}

gboolean
gsurf_lrg_scene_history_offset(guint64 key, gint *out_offset)
{
	if ((key & SCENE_HISTORY_BIT) == 0)
		return FALSE;
	if (out_offset != NULL)
		*out_offset = (gint)(key & ~SCENE_HISTORY_BIT) - SCENE_HISTORY_BIAS;
	return TRUE;
}

/* A tab's panel key: the view pointer (never has the top bit set in user
 * space on any platform gsurf runs on). */
static guint64
view_key(GsurfView *view)
{
	return (guint64)GPOINTER_TO_SIZE(view);
}

/* Cluster id for a tab: its container, so the grid/carousel keep named
 * containers together.  The default container -- and temporary containers,
 * which isolate one tab each and would otherwise make every tab its own
 * cluster -- are group 0 (sorted first). */
static guint
view_group(GsurfView *view)
{
	const gchar *container = gsurf_view_get_container_id(view);
	GsurfContainer *c;

	if (container == NULL || *container == '\0')
		return 0;
	c = gsurf_container_registry_lookup(gsurf_container_registry_get_default(),
	                                    container);
	if (c == NULL || gsurf_container_get_temporary(c))
		return 0;
	return (g_str_hash(container) & 0x7fffffffu) | 1u;
}

/* ======================================================================== *
 *  Thumbnails (history depth)
 * ======================================================================== */

static void
scene_thumb_free(gpointer data)
{
	SceneThumb *t = data;

	g_clear_object(&t->texture);
	g_clear_object(&t->image);
	g_free(t);
}

/* Drop the least recently used thumbnail once the cache is over its cap. */
static void
scene_thumbs_evict(GsurfLrgScene *self)
{
	while (g_hash_table_size(self->thumbs) > SCENE_THUMB_MAX) {
		GHashTableIter it;
		gpointer k, v, oldest_key = NULL;
		gint64 oldest = G_MAXINT64;

		g_hash_table_iter_init(&it, self->thumbs);
		while (g_hash_table_iter_next(&it, &k, &v)) {
			SceneThumb *t = v;
			if (t->used_us < oldest) {
				oldest = t->used_us;
				oldest_key = k;
			}
		}
		if (oldest_key == NULL)
			break;
		g_hash_table_remove(self->thumbs, oldest_key);
	}
}

/* Store a downscaled copy of @view's current frame as the thumbnail of its
 * current URI, at most every SCENE_THUMB_REFRESH_US per tab. */
static void
scene_note_thumbnail(GsurfLrgScene *self, GsurfView *view,
                     SceneViewState *st, gint64 now)
{
	const guint8 *px;
	const gchar *uri;
	SceneThumb *t;
	GrlImage *img;
	int w = 0, h = 0;

	if (now - st->last_thumb_us < SCENE_THUMB_REFRESH_US)
		return;
	uri = gsurf_view_get_uri(view);
	if (uri == NULL || *uri == '\0')
		return;
	px = gsurf_lrg_view_peek_pixels(GSURF_LRG_VIEW(view), &w, &h);
	if (px == NULL || w < SCENE_THUMB_DIVISOR || h < SCENE_THUMB_DIVISOR)
		return;

	img = grl_image_new_from_pixels(w, h, GRL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
	                                px);
	if (img == NULL)
		return;
	grl_image_resize(img, w / SCENE_THUMB_DIVISOR, h / SCENE_THUMB_DIVISOR);
	st->last_thumb_us = now;

	t = g_hash_table_lookup(self->thumbs, uri);
	if (t == NULL) {
		t = g_new0(SceneThumb, 1);
		g_hash_table_replace(self->thumbs, g_strdup(uri), t);
	}
	/* New pixels: the old texture has the wrong contents (and maybe size). */
	g_clear_object(&t->texture);
	g_clear_object(&t->image);
	t->image = img;
	t->real = TRUE;
	t->used_us = now;
	scene_thumbs_evict(self);
}

/* A title card for a history entry no thumbnail exists for yet: the page
 * title (or URI) on a dark card the size of a real thumbnail. */
static GrlImage *
scene_placeholder_image(GsurfLrgScene *self, const gchar *uri,
                        const gchar *title)
{
	g_autoptr(GrlColor) bg = grl_color_new(30, 32, 40, 255);
	g_autoptr(GrlColor) fg = grl_color_new(210, 214, 224, 255);
	g_autoptr(GrlColor) dim = grl_color_new(130, 136, 150, 255);
	gint w = MAX(self->body_w / SCENE_THUMB_DIVISOR, 160);
	gint h = MAX(self->body_h / SCENE_THUMB_DIVISOR, 100);
	GrlImage *img = grl_image_new_color(w, h, bg);
	g_autofree gchar *head = NULL;
	g_autofree gchar *tail = NULL;

	if (img == NULL)
		return NULL;
	grl_image_set_format(img, GRL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

	/* Keep both lines inside the card: raylib's bitmap font is ~0.6 em wide. */
	head = g_utf8_substring(title != NULL ? title : uri, 0,
	                        MAX((w - 32) / 12, 8));
	tail = g_utf8_substring(uri, 0, MAX((w - 32) / 8, 8));
	grl_image_draw_text(img, head, 16, h / 2 - 24, 20, fg);
	grl_image_draw_text(img, tail, 16, h / 2 + 8, 14, dim);
	return img;
}

/* The texture for a history entry: its real thumbnail if one was taken,
 * else a title card.  Must run inside a frame (uploads to the GPU). */
static GrlTexture *
scene_history_texture(GsurfLrgScene *self, const GsurfLrgHistoryEntry *e,
                      gint64 now)
{
	SceneThumb *t = g_hash_table_lookup(self->thumbs, e->uri);

	if (t == NULL) {
		GrlImage *img = scene_placeholder_image(self, e->uri, e->title);

		if (img == NULL)
			return NULL;
		t = g_new0(SceneThumb, 1);
		t->image = img;
		t->real = FALSE;
		g_hash_table_replace(self->thumbs, g_strdup(e->uri), t);
		scene_thumbs_evict(self);
		/* Eviction never drops the entry just added (it is the newest). */
		t = g_hash_table_lookup(self->thumbs, e->uri);
		if (t == NULL)
			return NULL;
	}
	t->used_us = now;
	if (t->texture == NULL && t->image != NULL)
		t->texture = grl_texture_new_from_image(t->image);
	return t->texture;
}

/* ======================================================================== *
 *  Frame
 * ======================================================================== */

static SceneViewState *
scene_view_state(GsurfLrgScene *self, GsurfView *view)
{
	SceneViewState *st = g_hash_table_lookup(self->view_state, view);

	if (st == NULL) {
		st = g_new0(SceneViewState, 1);
		g_hash_table_insert(self->view_state, view, st);
	}
	return st;
}

/* Refresh @view's texture if it is due and sync its panel at @ordinal. */
static void
scene_sync_view(GsurfLrgScene *self, GsurfView *view, gint ordinal, gint64 now)
{
	GsurfLrgView *lv = GSURF_LRG_VIEW(view);
	SceneViewState *st = scene_view_state(self, view);
	guint64 key = view_key(view);
	gboolean is_active = (view == self->active);
	gboolean fresh = FALSE;
	gint64 interval;
	GrlTexture *tex;
	int tw = 0, th = 0;

	/* Every tab is laid out at the page size, so a tab brought to the front
	 * (or read flat) is already pixel-exact. */
	gsurf_lrg_view_resize(lv, self->body_w, self->body_h, 1.0);

	/* 1. Capture if due.  A tab with no panel yet has coverage 0, so it gets
	 *    the hidden-tab rate -- but its first capture happens immediately. */
	interval = gsurf_lrg_scene_refresh_interval(is_active,
		lrg_3d_surface_get_panel_coverage(self->surface, key),
		self->background_fps);
	if (!st->captured_once || now - st->last_capture_us >= interval) {
		fresh = gsurf_lrg_view_capture(lv);
		st->last_capture_us = now;
		st->captured_once = TRUE;
	}

	/* 2. Upload (only when the capture produced new pixels) and sync.  A tab
	 *    with no first frame yet has no texture and gets no panel. */
	tex = gsurf_lrg_view_get_texture(lv, &tw, &th);
	if (tex == NULL)
		return;
	lrg_3d_surface_sync_texture_panel(self->surface, key, tex);
	/* Regenerate the mipmaps whenever the texture changed since they were
	 * built -- by this capture or by anyone else (the flat reading path
	 * uploads too).  Stale mip levels show old pixels at a distance. */
	if (gsurf_lrg_view_get_texture_serial(lv) != st->mip_serial) {
		lrg_3d_surface_panel_texture_updated(self->surface, key);
		st->mip_serial = gsurf_lrg_view_get_texture_serial(lv);
	}
	lrg_3d_surface_set_panel_slot(self->surface, key, view_group(view), ordinal);

	/* 3. The active page feeds the history thumbnails. */
	if (is_active && fresh)
		scene_note_thumbnail(self, view, st, now);
}

/* History depth: the active tab's back list recedes left (ordinals 1..n),
 * its forward list right (-1..-n). */
static void
scene_sync_history(GsurfLrgScene *self, gint64 now)
{
	g_autoptr(GPtrArray) back = NULL;
	g_autoptr(GPtrArray) fwd = NULL;
	guint i;

	if (self->active == NULL)
		return;
	gsurf_lrg_view_get_history(GSURF_LRG_VIEW(self->active),
	                           self->history_limit, &back, &fwd);

	for (i = 0; i < back->len; i++) {
		gint offset = -(gint)(i + 1);
		guint64 key = gsurf_lrg_scene_history_key(offset);
		GrlTexture *tex = scene_history_texture(self,
			g_ptr_array_index(back, i), now);

		if (tex == NULL)
			continue;
		lrg_3d_surface_sync_texture_panel(self->surface, key, tex);
		lrg_3d_surface_set_panel_slot(self->surface, key, 0, (gint)(i + 1));
	}
	for (i = 0; i < fwd->len; i++) {
		gint offset = (gint)(i + 1);
		guint64 key = gsurf_lrg_scene_history_key(offset);
		GrlTexture *tex = scene_history_texture(self,
			g_ptr_array_index(fwd, i), now);

		if (tex == NULL)
			continue;
		lrg_3d_surface_sync_texture_panel(self->surface, key, tex);
		lrg_3d_surface_set_panel_slot(self->surface, key, 0, -(gint)(i + 1));
	}
}

/* Apply persisted placements to panels that now exist. */
static void
scene_apply_placements(GsurfLrgScene *self)
{
	GHashTableIter it;
	gpointer k, v;

	if (g_hash_table_size(self->placements) == 0)
		return;
	g_hash_table_iter_init(&it, self->placements);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		const gfloat *t = v;

		if (lrg_3d_surface_set_panel_transform(self->surface,
		        view_key((GsurfView *)k), t[0], t[1], t[2], t[3], t[4], t[5]))
			g_hash_table_iter_remove(&it);
	}
}

/* Forget per-tab state for tabs that are gone (pointers are only compared). */
static void
scene_prune_view_state(GsurfLrgScene *self)
{
	GHashTableIter it;
	gpointer k;

	g_hash_table_iter_init(&it, self->view_state);
	while (g_hash_table_iter_next(&it, &k, NULL)) {
		guint i;
		gboolean alive = FALSE;

		for (i = 0; i < self->frame_views->len && !alive; i++)
			alive = (g_ptr_array_index(self->frame_views, i) == k);
		if (!alive)
			g_hash_table_iter_remove(&it);
	}
}

void
gsurf_lrg_scene_frame(GsurfLrgScene *self, GPtrArray *views, GsurfView *active,
                      gint body_width, gint body_height, gfloat dt)
{
	LrgFrameSurface *fs;
	gint64 now = g_get_monotonic_time();
	guint vi;
	gint index = 0, active_index = 0;

	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	if (gsurf_lrg_scene_get_window(self) == NULL)
		return;
	fs = LRG_FRAME_SURFACE(self->surface);

	/* 1. Remember (and keep alive) this frame's tabs for picking. */
	g_ptr_array_set_size(self->frame_views, 0);
	for (vi = 0; views != NULL && vi < views->len; vi++) {
		GsurfView *v = g_ptr_array_index(views, vi);

		if (!GSURF_IS_LRG_VIEW(v))
			continue;
		if (v == active)
			active_index = index;
		g_ptr_array_add(self->frame_views, g_object_ref(v));
		index++;
	}
	self->active = (active != NULL && GSURF_IS_LRG_VIEW(active)) ? active : NULL;
	self->body_w = MAX(body_width, 1);
	self->body_h = MAX(body_height, 1);

	/* 2. Begin the frame (begin drawing; picks up a window resize). */
	lrg_frame_surface_begin_frame(fs);

	/* 3. Sync the panels: every tab, slotted relative to the active one so the
	 *    carousel spins to it -- or, in history depth, the active tab and its
	 *    back/forward entries. */
	lrg_3d_surface_begin_window_sync(self->surface);
	if (self->history) {
		if (self->active != NULL)
			scene_sync_view(self, self->active, 0, now);
		scene_sync_history(self, now);
	} else {
		guint i;

		for (i = 0; i < self->frame_views->len; i++)
			scene_sync_view(self, g_ptr_array_index(self->frame_views, i),
			                (gint)i - active_index, now);
	}
	lrg_3d_surface_end_window_sync(self->surface);
	scene_apply_placements(self);

	/* Aim the camera at the layout after an arrangement switch, and again
	 * whenever tabs come or go -- until the user moves the camera
	 * themselves.  A grid wall needs the whole wall in view; the others
	 * start from the default view, which looks at the front panel. */
	if (lrg_3d_surface_get_panel_count(self->surface) != self->last_panel_count) {
		self->last_panel_count = lrg_3d_surface_get_panel_count(self->surface);
		if (!self->camera_touched)
			self->reframe = TRUE;
	}
	if (self->reframe && self->last_panel_count > 0) {
		self->reframe = FALSE;
		if (g_strcmp0(self->arrangement, "grid") == 0 && !self->history)
			gsurf_lrg_scene_overview(self);
		else if (!self->reading)
			lrg_spatial_camera_reset(lrg_3d_surface_get_camera(self->surface));
	}

	/* 4. Depth of field on the active tab; reading mode follows tab switches. */
	if (self->active != NULL) {
		guint64 key = view_key(self->active);

		lrg_3d_surface_set_focus_window(self->surface, key);
		if (self->reading && self->reading_key != key
		    && lrg_3d_surface_maximize_panel(self->surface, key))
			self->reading_key = key;
	}

	/* 5. Ease, render (environment, panels, HUD) and swap. */
	lrg_3d_surface_step(self->surface, dt);
	lrg_frame_surface_end_frame(fs);

	scene_prune_view_state(self);
}

void
gsurf_lrg_scene_note_active(GsurfLrgScene *self, GsurfView *active)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	/* Only compared (wants_flat) until the next frame re-borrows it from
	 * frame_views; the window owns the view for the whole tick. */
	self->active = (active != NULL && GSURF_IS_LRG_VIEW(active)) ? active : NULL;
}

gboolean
gsurf_lrg_scene_wants_flat(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);

	/* Flat only once the fly-in has finished, and only while the panel being
	 * read is still the active tab (a tab switch flies again first). */
	return self->reading && !self->history
	    && self->active != NULL
	    && self->reading_key == view_key(self->active)
	    && !lrg_3d_surface_is_animating(self->surface);
}

void
gsurf_lrg_scene_note_flat_frame(GsurfLrgScene *self, GsurfView *active)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	if (active == NULL || !GSURF_IS_LRG_VIEW(active))
		return;
	scene_note_thumbnail(self, active, scene_view_state(self, active),
	                     g_get_monotonic_time());
}

gboolean
gsurf_lrg_scene_pick(GsurfLrgScene *self, gfloat x, gfloat y, GsurfLrgPick *out)
{
	gfloat px = 0.0f, py = 0.0f;
	guint64 key = 0;
	gint offset;
	guint i;

	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);
	g_return_val_if_fail(out != NULL, FALSE);

	memset(out, 0, sizeof(*out));
	if (!lrg_3d_surface_pick_panel(self->surface, x, y, &px, &py, &key))
		return FALSE;

	if (gsurf_lrg_scene_history_offset(key, &offset)) {
		out->kind = GSURF_LRG_PICK_HISTORY;
		out->history_offset = offset;
		return TRUE;
	}
	for (i = 0; i < self->frame_views->len; i++) {
		GsurfView *v = g_ptr_array_index(self->frame_views, i);

		if (view_key(v) == key) {
			/* Texture panels pick in texture pixels = page pixels. */
			out->kind = GSURF_LRG_PICK_TAB;
			out->view = v;
			out->x = (gint)px;
			out->y = (gint)py;
			return TRUE;
		}
	}
	return FALSE;
}

/* ======================================================================== *
 *  Modes
 * ======================================================================== */

/* Look an id up in a NULL-terminated table; -1 if absent. */
static gint
table_index(const gchar *const *table, const gchar *id)
{
	gint i;

	for (i = 0; id != NULL && table[i] != NULL; i++)
		if (g_strcmp0(table[i], id) == 0)
			return i;
	return -1;
}

gboolean
gsurf_lrg_scene_set_arrangement(GsurfLrgScene *self, const gchar *id)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);

	if (id == NULL)
		return FALSE;
	if (self->history)
		gsurf_lrg_scene_set_history(self, FALSE);
	if (!lrg_3d_surface_set_arrangement_id(self->surface, id))
		return FALSE;
	/* Aim the camera at the layout once its panels exist. */
	self->reframe = TRUE;
	self->camera_touched = FALSE;
	g_free(self->arrangement);
	self->arrangement = g_strdup(id);
	return TRUE;
}

const gchar *
gsurf_lrg_scene_get_arrangement(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), NULL);
	return self->arrangement;
}

const gchar *
gsurf_lrg_scene_cycle_arrangement(GsurfLrgScene *self)
{
	gint i;

	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), NULL);

	i = table_index(scene_tab_arrangements, self->arrangement);
	i = (i + 1) % (gint)(G_N_ELEMENTS(scene_tab_arrangements) - 1);
	gsurf_lrg_scene_set_arrangement(self, scene_tab_arrangements[i]);
	return self->arrangement;
}

gboolean
gsurf_lrg_scene_set_environment(GsurfLrgScene *self, const gchar *id)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);

	if (id == NULL || !lrg_3d_surface_set_environment_id(self->surface, id))
		return FALSE;
	g_free(self->environment);
	self->environment = g_strdup(id);
	return TRUE;
}

const gchar *
gsurf_lrg_scene_get_environment(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), NULL);
	return self->environment;
}

const gchar *
gsurf_lrg_scene_cycle_environment(GsurfLrgScene *self)
{
	gint i;

	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), NULL);

	i = table_index(scene_environments, self->environment);
	i = (i + 1) % (gint)(G_N_ELEMENTS(scene_environments) - 1);
	gsurf_lrg_scene_set_environment(self, scene_environments[i]);
	return self->environment;
}

void
gsurf_lrg_scene_set_reading(GsurfLrgScene *self, gboolean reading)
{
	LrgSpatialCamera *cam;

	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	if (reading == self->reading)
		return;
	cam = lrg_3d_surface_get_camera(self->surface);

	if (reading) {
		/* Reading and history depth exclude each other. */
		if (self->history)
			gsurf_lrg_scene_set_history(self, FALSE);
		g_clear_pointer(&self->pre_reading, lrg_pose_free);
		self->pre_reading = lrg_spatial_camera_get_pose(cam);
		self->reading = TRUE;
		/* The frame flies to the active panel (reading_key differs). */
		self->reading_key = 0;
		if (self->active != NULL
		    && lrg_3d_surface_maximize_panel(self->surface,
		                                     view_key(self->active)))
			self->reading_key = view_key(self->active);
	} else {
		self->reading = FALSE;
		self->reading_key = 0;
		if (self->pre_reading != NULL)
			lrg_spatial_camera_set_target_pose(cam, self->pre_reading);
		g_clear_pointer(&self->pre_reading, lrg_pose_free);
	}
}

void
gsurf_lrg_scene_reframe(GsurfLrgScene *self)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));
	self->reframe = TRUE;
	self->camera_touched = FALSE;
}

gboolean
gsurf_lrg_scene_get_reading(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);
	return self->reading;
}

void
gsurf_lrg_scene_set_history(GsurfLrgScene *self, gboolean history)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	if (history == self->history)
		return;
	if (history) {
		if (self->reading)
			gsurf_lrg_scene_set_reading(self, FALSE);
		self->history = TRUE;
		/* The stack puts the current page at the origin, where the default
		 * camera looks. */
		lrg_3d_surface_set_arrangement_id(self->surface, "stack");
		lrg_spatial_camera_reset(lrg_3d_surface_get_camera(self->surface));
	} else {
		/* History panels drop out at the next sync (they are no longer
		 * synced); the tabs come back under their arrangement. */
		self->history = FALSE;
		lrg_3d_surface_set_arrangement_id(self->surface, self->arrangement);
	}
}

gboolean
gsurf_lrg_scene_get_history(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);
	return self->history;
}

/* ======================================================================== *
 *  Tabs
 * ======================================================================== */

gboolean
gsurf_lrg_scene_focus_view(GsurfLrgScene *self, GsurfView *view)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);
	g_return_val_if_fail(view != NULL, FALSE);
	self->camera_touched = TRUE;
	return lrg_3d_surface_focus_panel(self->surface, view_key(view));
}

gboolean
gsurf_lrg_scene_pin_view(GsurfLrgScene *self, GsurfView *view)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);
	g_return_val_if_fail(view != NULL, FALSE);
	return lrg_3d_surface_pin_panel(self->surface, view_key(view));
}

void
gsurf_lrg_scene_unpin_view(GsurfLrgScene *self, GsurfView *view)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	if (view == NULL) {
		lrg_3d_surface_unpin_all(self->surface);
		g_hash_table_remove_all(self->placements);
		return;
	}
	g_hash_table_remove(self->placements, view);
	lrg_3d_surface_unpin_panel(self->surface, view_key(view));
}

gboolean
gsurf_lrg_scene_is_view_pinned(GsurfLrgScene *self, GsurfView *view)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);
	g_return_val_if_fail(view != NULL, FALSE);
	return lrg_3d_surface_is_panel_pinned(self->surface, view_key(view));
}

gchar *
gsurf_lrg_scene_get_view_placement(GsurfLrgScene *self, GsurfView *view)
{
	gfloat v[6];
	gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
	GString *out;
	guint i;

	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), NULL);
	g_return_val_if_fail(view != NULL, NULL);

	if (!lrg_3d_surface_get_panel_geometry(self->surface, view_key(view),
	        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]))
		return NULL;
	out = g_string_new(NULL);
	for (i = 0; i < 6; i++) {
		if (i > 0)
			g_string_append_c(out, ' ');
		g_string_append(out, g_ascii_dtostr(buf, sizeof(buf), (gdouble)v[i]));
	}
	return g_string_free(out, FALSE);
}

gboolean
gsurf_lrg_scene_set_view_placement(GsurfLrgScene *self, GsurfView *view,
                                   const gchar *placement)
{
	g_auto(GStrv) parts = NULL;
	gfloat *t;
	guint i, n = 0;

	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);
	g_return_val_if_fail(view != NULL && placement != NULL, FALSE);

	/* Six finite numbers, width and height positive. */
	t = g_new0(gfloat, 6);
	parts = g_strsplit_set(placement, " \t", -1);
	for (i = 0; parts[i] != NULL; i++) {
		gchar *end = NULL;
		gdouble d;

		if (parts[i][0] == '\0')
			continue;
		d = g_ascii_strtod(parts[i], &end);
		if (n >= 6 || end == parts[i] || *end != '\0' || !isfinite(d)) {
			g_free(t);
			return FALSE;
		}
		t[n++] = (gfloat)d;
	}
	if (n != 6 || t[4] <= 0.0f || t[5] <= 0.0f) {
		g_free(t);
		return FALSE;
	}

	/* Apply now if the panel exists; otherwise when it first appears. */
	if (lrg_3d_surface_set_panel_transform(self->surface, view_key(view),
	        t[0], t[1], t[2], t[3], t[4], t[5]))
		g_free(t);
	else
		g_hash_table_replace(self->placements, view, t);
	return TRUE;
}

/* ======================================================================== *
 *  Camera
 * ======================================================================== */

void
gsurf_lrg_scene_orbit(GsurfLrgScene *self, gfloat dyaw, gfloat dpitch,
                      gboolean eased)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	self->camera_touched = TRUE;
	if (eased)
		lrg_spatial_camera_orbit(lrg_3d_surface_get_camera(self->surface),
		                         dyaw, dpitch);
	else
		/* Pointer drags circle the whole room, not one panel. */
		lrg_3d_surface_orbit_room(self->surface, dyaw, dpitch);
}

void
gsurf_lrg_scene_look(GsurfLrgScene *self, gfloat dyaw, gfloat dpitch)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));
	self->camera_touched = TRUE;
	lrg_spatial_camera_look_drag(lrg_3d_surface_get_camera(self->surface),
	                             dyaw, dpitch);
}

void
gsurf_lrg_scene_zoom(GsurfLrgScene *self, gfloat factor, gboolean eased)
{
	LrgSpatialCamera *cam;

	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	if (factor <= 0.0f)
		return;
	self->camera_touched = TRUE;
	cam = lrg_3d_surface_get_camera(self->surface);
	if (eased)
		lrg_spatial_camera_zoom(cam, factor);
	else
		lrg_spatial_camera_dolly_drag(cam, factor);
}

void
gsurf_lrg_scene_reset_camera(GsurfLrgScene *self)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	/* A reset is a fresh start: no camera to return to after reading. */
	self->reading = FALSE;
	self->reading_key = 0;
	g_clear_pointer(&self->pre_reading, lrg_pose_free);
	self->camera_touched = FALSE;
	lrg_spatial_camera_reset(lrg_3d_surface_get_camera(self->surface));
}

void
gsurf_lrg_scene_overview(GsurfLrgScene *self)
{
	g_autoptr(LrgPose) pose = NULL;
	gfloat cx = 0.0f, cy = 0.0f, cz = 0.0f, radius = 1.0f, dist;
	guint i, n = 0;

	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	if (self->reading)
		gsurf_lrg_scene_set_reading(self, FALSE);

	/* Bounding sphere of the tab panels: centroid, then the farthest panel
	 * edge from it. */
	for (i = 0; i < self->frame_views->len; i++) {
		gfloat px, py, pz;

		if (lrg_3d_surface_get_panel_geometry(self->surface,
		        view_key(g_ptr_array_index(self->frame_views, i)),
		        &px, &py, &pz, NULL, NULL, NULL)) {
			cx += px; cy += py; cz += pz;
			n++;
		}
	}
	if (n == 0)
		return;
	cx /= (gfloat)n; cy /= (gfloat)n; cz /= (gfloat)n;
	for (i = 0; i < self->frame_views->len; i++) {
		gfloat px, py, pz, w, h, d;

		if (!lrg_3d_surface_get_panel_geometry(self->surface,
		        view_key(g_ptr_array_index(self->frame_views, i)),
		        &px, &py, &pz, NULL, &w, &h))
			continue;
		d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)
		          + (pz - cz) * (pz - cz)) + 0.5f * MAX(w, h);
		radius = MAX(radius, d);
	}

	/* Far enough that the sphere fits the 45 degree view with a margin, from
	 * a raised 3/4 direction (up 0.45, back 1, normalised). */
	dist = radius / tanf(22.5f * (gfloat)(G_PI / 180.0)) * 1.1f;
	pose = lrg_pose_new(cx, cy + dist * 0.41f, cz + dist * 0.91f,
	                    cx, cy, cz, 0.0f, 1.0f, 0.0f, 45.0f);
	lrg_spatial_camera_set_target_pose(lrg_3d_surface_get_camera(self->surface),
	                                   pose);
}

gboolean
gsurf_lrg_scene_grab(GsurfLrgScene *self, GsurfView *view, gfloat x, gfloat y)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);
	g_return_val_if_fail(view != NULL, FALSE);
	return lrg_3d_surface_grab_panel(self->surface, view_key(view), x, y);
}

void
gsurf_lrg_scene_drag(GsurfLrgScene *self, gfloat x, gfloat y)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));
	lrg_3d_surface_drag_panel(self->surface, x, y);
}

void
gsurf_lrg_scene_release(GsurfLrgScene *self)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));
	lrg_3d_surface_release_panel(self->surface);
}

void
gsurf_lrg_scene_save_view(GsurfLrgScene *self, const gchar *name)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));
	lrg_3d_surface_save_view(self->surface, name);
}

gboolean
gsurf_lrg_scene_restore_view(GsurfLrgScene *self, const gchar *name)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);
	if (self->reading)
		gsurf_lrg_scene_set_reading(self, FALSE);
	self->camera_touched = TRUE;
	return lrg_3d_surface_restore_view(self->surface, name);
}

gchar **
gsurf_lrg_scene_list_views(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), NULL);
	return lrg_3d_surface_list_views(self->surface);
}

guint
gsurf_lrg_scene_get_panel_count(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), 0);
	return lrg_3d_surface_get_panel_count(self->surface);
}

gboolean
gsurf_lrg_scene_is_animating(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), FALSE);
	return lrg_3d_surface_is_animating(self->surface);
}

/* ======================================================================== *
 *  Settings, accessors, lifecycle
 * ======================================================================== */

void
gsurf_lrg_scene_set_background_fps(GsurfLrgScene *self, gdouble fps)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));
	self->background_fps = CLAMP(fps, 0.2, 60.0);
}

gdouble
gsurf_lrg_scene_get_background_fps(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), 0.0);
	return self->background_fps;
}

void
gsurf_lrg_scene_set_history_limit(GsurfLrgScene *self, guint limit)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));
	self->history_limit = CLAMP(limit, 1, 32);
}

guint
gsurf_lrg_scene_get_history_limit(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), 0);
	return self->history_limit;
}

gpointer
gsurf_lrg_scene_get_window(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), NULL);
	return lrg_frame_surface_get_window(LRG_FRAME_SURFACE(self->surface));
}

gpointer
gsurf_lrg_scene_get_surface(GsurfLrgScene *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_SCENE(self), NULL);
	return self->surface;
}

/* The surface's HUD hook forwards to the window's chrome drawer, then takes
 * any requested screenshot (the frame is complete, the swap comes next). */
static void
scene_overlay(Lrg3DSurface *surface, gpointer user_data)
{
	GsurfLrgScene *self = user_data;

	(void)surface;
	if (self->hud != NULL)
		self->hud(self, self->hud_data);
	gsurf_lrg_scene_flush_screenshots(self);
}

void
gsurf_lrg_scene_request_screenshot(GsurfLrgScene *self, const gchar *path,
                                   GsurfLrgSceneShotFunc func, gpointer user_data)
{
	SceneShot *shot;

	g_return_if_fail(GSURF_IS_LRG_SCENE(self));
	g_return_if_fail(path != NULL && func != NULL);

	shot = g_new0(SceneShot, 1);
	shot->path = g_strdup(path);
	shot->func = func;
	shot->user_data = user_data;
	self->shots = g_slist_prepend(self->shots, shot);
}

void
gsurf_lrg_scene_flush_screenshots(GsurfLrgScene *self)
{
	g_autoptr(GrlImage) frame = NULL;
	GSList *pending, *l;

	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	if (self->shots == NULL)
		return;
	/* Detach first: a callback may queue another request for a later frame. */
	pending = g_slist_reverse(self->shots);
	self->shots = NULL;

	/* raylib batches draws: a scissor begin/end flushes them so the read
	 * sees the finished frame. */
	grl_draw_begin_scissor_mode(0, 0, 1, 1);
	grl_draw_end_scissor_mode();
	frame = grl_image_new_from_screen();

	for (l = pending; l != NULL; l = l->next) {
		SceneShot *shot = l->data;
		gboolean ok = frame != NULL && grl_image_export(frame, shot->path);

		/* Owner-only, like page.screenshot: a frame can show private pages. */
		if (ok)
			g_chmod(shot->path, 0600);

		shot->func(self, shot->path, ok, shot->user_data);
		g_free(shot->path);
		g_free(shot);
	}
	g_slist_free(pending);
}

void
gsurf_lrg_scene_set_hud_func(GsurfLrgScene *self, GsurfLrgSceneHudFunc func,
                             gpointer user_data, GDestroyNotify destroy)
{
	g_return_if_fail(GSURF_IS_LRG_SCENE(self));

	if (self->hud_destroy != NULL)
		self->hud_destroy(self->hud_data);
	self->hud = func;
	self->hud_data = user_data;
	self->hud_destroy = destroy;
}

GsurfLrgScene *
gsurf_lrg_scene_new(gint width, gint height, const gchar *title,
                    const gchar *arrangement, const gchar *environment)
{
	GsurfLrgScene *self = g_object_new(GSURF_TYPE_LRG_SCENE, NULL);

	self->surface = lrg_3d_surface_new(MAX(width, 1), MAX(height, 1),
	                                   title != NULL ? title : "gsurf");
	/* Every panel brings its own texture: no flat-frame read-back. */
	lrg_3d_surface_set_capture_enabled(self->surface, FALSE);
	lrg_3d_surface_set_overlay_func(self->surface, scene_overlay, self, NULL);

	if (arrangement == NULL
	    || !gsurf_lrg_scene_set_arrangement(self, arrangement)) {
		if (arrangement != NULL)
			g_warning("gsurf LRG: unknown 3D arrangement '%s'; using '%s'",
			          arrangement, GSURF_LRG_SCENE_DEFAULT_ARRANGEMENT);
		gsurf_lrg_scene_set_arrangement(self,
			GSURF_LRG_SCENE_DEFAULT_ARRANGEMENT);
	}
	if (environment == NULL
	    || !gsurf_lrg_scene_set_environment(self, environment)) {
		if (environment != NULL)
			g_warning("gsurf LRG: unknown 3D environment '%s'; using '%s'",
			          environment, GSURF_LRG_SCENE_DEFAULT_ENVIRONMENT);
		gsurf_lrg_scene_set_environment(self,
			GSURF_LRG_SCENE_DEFAULT_ENVIRONMENT);
	}
	return self;
}

static void
gsurf_lrg_scene_dispose(GObject *object)
{
	GsurfLrgScene *self = GSURF_LRG_SCENE(object);

	if (self->hud_destroy != NULL)
		self->hud_destroy(self->hud_data);
	self->hud = NULL;
	self->hud_destroy = NULL;

	/* Requests still pending fail (the window is going away). */
	while (self->shots != NULL) {
		SceneShot *shot = self->shots->data;

		self->shots = g_slist_delete_link(self->shots, self->shots);
		shot->func(self, shot->path, FALSE, shot->user_data);
		g_free(shot->path);
		g_free(shot);
	}

	/* GPU objects (thumbnail textures) go while the surface still owns the
	 * GL context; the surface's panels drop their texture refs with it. */
	g_clear_pointer(&self->thumbs, g_hash_table_unref);
	g_clear_pointer(&self->frame_views, g_ptr_array_unref);
	if (self->surface != NULL)
		lrg_3d_surface_set_overlay_func(self->surface, NULL, NULL, NULL);
	g_clear_object(&self->surface);
	g_clear_pointer(&self->view_state, g_hash_table_unref);
	g_clear_pointer(&self->placements, g_hash_table_unref);
	g_clear_pointer(&self->pre_reading, lrg_pose_free);
	g_clear_pointer(&self->arrangement, g_free);
	g_clear_pointer(&self->environment, g_free);

	G_OBJECT_CLASS(gsurf_lrg_scene_parent_class)->dispose(object);
}

static void
gsurf_lrg_scene_class_init(GsurfLrgSceneClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = gsurf_lrg_scene_dispose;
}

static void
gsurf_lrg_scene_init(GsurfLrgScene *self)
{
	self->background_fps = 4.0;
	self->history_limit = 8;
	self->view_state = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                         NULL, g_free);
	self->thumbs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                     scene_thumb_free);
	self->placements = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                         NULL, g_free);
	self->frame_views = g_ptr_array_new_with_free_func(g_object_unref);
}
