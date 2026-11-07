/*
 * gsurf-lrg-scene.h - the --lrg=3d tab space
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #GsurfLrgScene puts a window's tabs into libregnum's 3D scene
 * (#Lrg3DSurface, the term/ module cmacs lrgterm uses).  Every tab is a panel
 * that shows the tab's own page texture by reference, laid out by an
 * arrangement (carousel, grid, stack, free, single-panel) inside an
 * environment (studio, sky, workshop, cockpit, void).  On top of that the
 * scene adds what a browser needs:
 *
 * - refresh throttling: the active tab captures every frame, visible
 *   background tabs at a lower rate, hidden ones rarely;
 * - a reading mode that flies the camera head-on to the active tab and then
 *   hands the window back to the pixel-exact 2D path;
 * - a history-depth mode that shows the active tab's back/forward list as a
 *   stack of page thumbnails;
 * - picking that maps a pointer position to a tab (or history entry) and a
 *   page pixel.
 *
 * The scene owns the raylib window (raylib has one per process); the owning
 * #GsurfLrgWindow borrows it for its 2D chrome and input.  Only built with
 * LRG_3D=1 (GSURF_HAVE_LRG_3D).
 */

#ifndef GSURF_LRG_SCENE_H
#define GSURF_LRG_SCENE_H

#include <glib-object.h>
#include "core/gsurf-view.h"

G_BEGIN_DECLS

#define GSURF_TYPE_LRG_SCENE (gsurf_lrg_scene_get_type())
G_DECLARE_FINAL_TYPE(GsurfLrgScene, gsurf_lrg_scene, GSURF, LRG_SCENE, GObject)

/**
 * GSURF_LRG_SCENE_DEFAULT_ARRANGEMENT:
 *
 * The arrangement a scene starts with when none is requested.
 */
#define GSURF_LRG_SCENE_DEFAULT_ARRANGEMENT "carousel"

/**
 * GSURF_LRG_SCENE_DEFAULT_ENVIRONMENT:
 *
 * The environment a scene starts with when none is requested.
 */
#define GSURF_LRG_SCENE_DEFAULT_ENVIRONMENT "studio"

/**
 * GsurfLrgPickKind:
 * @GSURF_LRG_PICK_NONE: the pointer is over empty space
 * @GSURF_LRG_PICK_TAB: the pointer is over a tab's panel
 * @GSURF_LRG_PICK_HISTORY: the pointer is over a history-depth thumbnail
 *
 * What gsurf_lrg_scene_pick() hit.
 */
typedef enum {
	GSURF_LRG_PICK_NONE = 0,
	GSURF_LRG_PICK_TAB,
	GSURF_LRG_PICK_HISTORY
} GsurfLrgPickKind;

/**
 * GsurfLrgPick:
 * @kind: what was hit
 * @view: (nullable): the tab, for %GSURF_LRG_PICK_TAB
 * @history_offset: steps from the current page (negative = back), for
 *   %GSURF_LRG_PICK_HISTORY
 * @x: the page pixel x under the pointer (tabs only)
 * @y: the page pixel y under the pointer (tabs only)
 *
 * The result of gsurf_lrg_scene_pick().  @view is borrowed.
 */
typedef struct {
	GsurfLrgPickKind kind;
	GsurfView       *view;
	gint             history_offset;
	gint             x;
	gint             y;
} GsurfLrgPick;

/**
 * GsurfLrgSceneHudFunc:
 * @scene: the scene
 * @user_data: data given to gsurf_lrg_scene_set_hud_func()
 *
 * Draws the window's 2D chrome (address bar, status, hover URL) on top of the
 * 3D scene, in screen pixels, right before the buffer swap.
 */
typedef void (*GsurfLrgSceneHudFunc)(GsurfLrgScene *scene, gpointer user_data);

/**
 * gsurf_lrg_scene_new:
 * @width: initial window width in pixels
 * @height: initial window height in pixels
 * @title: (nullable): window title
 * @arrangement: (nullable): arrangement id, %NULL for the default
 * @environment: (nullable): environment id, %NULL for the default
 *
 * Creates the scene and its raylib window.  An unknown @arrangement or
 * @environment logs a warning and falls back to the default.
 *
 * Returns: (transfer full): a new scene; check gsurf_lrg_scene_get_window()
 *   for %NULL (no display)
 */
GsurfLrgScene *gsurf_lrg_scene_new(gint width, gint height, const gchar *title,
                                   const gchar *arrangement,
                                   const gchar *environment);

/**
 * gsurf_lrg_scene_get_window:
 * @self: a #GsurfLrgScene
 *
 * Returns: (transfer none) (nullable): the scene's #GrlWindow
 */
gpointer gsurf_lrg_scene_get_window(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_get_surface:
 * @self: a #GsurfLrgScene
 *
 * Returns: (transfer none): the #Lrg3DSurface, for callers that need the full
 *   libregnum API
 */
gpointer gsurf_lrg_scene_get_surface(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_set_hud_func:
 * @self: a #GsurfLrgScene
 * @func: (nullable) (scope notified): the chrome drawer
 * @user_data: data for @func
 * @destroy: (nullable): frees @user_data
 */
void gsurf_lrg_scene_set_hud_func(GsurfLrgScene *self, GsurfLrgSceneHudFunc func,
                                  gpointer user_data, GDestroyNotify destroy);

/**
 * gsurf_lrg_scene_set_background_fps:
 * @self: a #GsurfLrgScene
 * @fps: refresh rate for visible tabs that are not active (clamped to
 *   [0.2, 60]); defaults to 4
 */
void gsurf_lrg_scene_set_background_fps(GsurfLrgScene *self, gdouble fps);

/**
 * gsurf_lrg_scene_get_background_fps:
 * @self: a #GsurfLrgScene
 *
 * Returns: the background refresh rate
 */
gdouble gsurf_lrg_scene_get_background_fps(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_set_history_limit:
 * @self: a #GsurfLrgScene
 * @limit: entries shown in each direction in history depth (clamped to
 *   [1, 32]); defaults to 8
 */
void gsurf_lrg_scene_set_history_limit(GsurfLrgScene *self, guint limit);

/**
 * gsurf_lrg_scene_get_history_limit:
 * @self: a #GsurfLrgScene
 *
 * Returns: the history-depth entry limit
 */
guint gsurf_lrg_scene_get_history_limit(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_frame:
 * @self: a #GsurfLrgScene
 * @views: (element-type GsurfView): the window's tabs, in tab order (borrowed)
 * @active: (nullable): the active tab
 * @body_width: page width in pixels (every tab is laid out at this size)
 * @body_height: page height in pixels
 * @dt: seconds since the previous frame
 *
 * Runs one 3D frame: lays every tab out at the body size, refreshes the
 * textures that are due (see gsurf_lrg_scene_set_background_fps()), syncs the
 * panels, advances the animation, renders the scene and the HUD, and swaps.
 */
void gsurf_lrg_scene_frame(GsurfLrgScene *self, GPtrArray *views, GsurfView *active,
                           gint body_width, gint body_height, gfloat dt);

/**
 * gsurf_lrg_scene_wants_flat:
 * @self: a #GsurfLrgScene
 *
 * Returns: %TRUE when the window should draw the active tab with the flat,
 *   pixel-exact 2D path this frame: reading mode is on and the camera has
 *   finished flying in
 */
gboolean gsurf_lrg_scene_wants_flat(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_note_active:
 * @self: a #GsurfLrgScene
 * @active: (nullable): the window's active tab right now
 *
 * Tells the scene which tab is active before gsurf_lrg_scene_wants_flat() is
 * asked: while reading flat no 3D frame runs, so without this a tab switch
 * would not fly the camera to the new tab.
 */
void gsurf_lrg_scene_note_active(GsurfLrgScene *self, GsurfView *active);

/**
 * gsurf_lrg_scene_note_flat_frame:
 * @self: a #GsurfLrgScene
 * @active: (nullable): the active tab
 *
 * Tells the scene the window drew a flat frame of @active itself, so the
 * scene can keep its history thumbnails fresh while it is not rendering.
 */
void gsurf_lrg_scene_note_flat_frame(GsurfLrgScene *self, GsurfView *active);

/**
 * gsurf_lrg_scene_pick:
 * @self: a #GsurfLrgScene
 * @x: pointer x in window pixels
 * @y: pointer y in window pixels
 * @out: (out caller-allocates): what is under the pointer
 *
 * Ray-casts the pointer into the scene.
 *
 * Returns: %TRUE if something was hit
 */
gboolean gsurf_lrg_scene_pick(GsurfLrgScene *self, gfloat x, gfloat y,
                              GsurfLrgPick *out);

/* --- modes ----------------------------------------------------------- */

/**
 * gsurf_lrg_scene_set_arrangement:
 * @self: a #GsurfLrgScene
 * @id: an arrangement id ("carousel", "grid", "stack", "free",
 *   "single-panel", "per-window")
 *
 * Leaves history depth if it was on.
 *
 * Returns: %TRUE if @id is known
 */
gboolean gsurf_lrg_scene_set_arrangement(GsurfLrgScene *self, const gchar *id);

/**
 * gsurf_lrg_scene_get_arrangement:
 * @self: a #GsurfLrgScene
 *
 * Returns: (transfer none): the arrangement id the tabs use (the one restored
 *   after history depth, while history depth is on)
 */
const gchar *gsurf_lrg_scene_get_arrangement(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_cycle_arrangement:
 * @self: a #GsurfLrgScene
 *
 * Steps to the next tab arrangement (carousel, grid, stack, free).
 *
 * Returns: (transfer none): the new arrangement id
 */
const gchar *gsurf_lrg_scene_cycle_arrangement(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_set_environment:
 * @self: a #GsurfLrgScene
 * @id: an environment id ("studio", "sky", "workshop", "cockpit", "void")
 *
 * Returns: %TRUE if @id is known
 */
gboolean gsurf_lrg_scene_set_environment(GsurfLrgScene *self, const gchar *id);

/**
 * gsurf_lrg_scene_get_environment:
 * @self: a #GsurfLrgScene
 *
 * Returns: (transfer none): the environment id
 */
const gchar *gsurf_lrg_scene_get_environment(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_cycle_environment:
 * @self: a #GsurfLrgScene
 *
 * Returns: (transfer none): the new environment id
 */
const gchar *gsurf_lrg_scene_cycle_environment(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_set_reading:
 * @self: a #GsurfLrgScene
 * @reading: %TRUE to fly to the active tab and read it flat
 *
 * Reading mode frames the active tab head-on, edge to edge; once the camera
 * settles, gsurf_lrg_scene_wants_flat() turns %TRUE.  Leaving it restores the
 * camera that was in use before.
 */
void gsurf_lrg_scene_set_reading(GsurfLrgScene *self, gboolean reading);

/**
 * gsurf_lrg_scene_reframe:
 * @self: a #GsurfLrgScene
 *
 * Aims the camera at the current layout after the next panel sync: the
 * whole wall for a grid, the default view otherwise.
 */
void gsurf_lrg_scene_reframe(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_get_reading:
 * @self: a #GsurfLrgScene
 *
 * Returns: whether reading mode is on
 */
gboolean gsurf_lrg_scene_get_reading(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_set_history:
 * @self: a #GsurfLrgScene
 * @history: %TRUE to show the active tab's back/forward list in depth
 *
 * History depth swaps the arrangement to "stack" and shows only the active
 * tab plus thumbnails of its history; leaving it restores the arrangement.
 */
void gsurf_lrg_scene_set_history(GsurfLrgScene *self, gboolean history);

/**
 * gsurf_lrg_scene_get_history:
 * @self: a #GsurfLrgScene
 *
 * Returns: whether history depth is on
 */
gboolean gsurf_lrg_scene_get_history(GsurfLrgScene *self);

/* --- tabs -------------------------------------------------------------- */

/**
 * gsurf_lrg_scene_focus_view:
 * @self: a #GsurfLrgScene
 * @view: a tab
 *
 * Flies the camera to frame @view's panel (with a margin) and focuses it.
 *
 * Returns: %TRUE if @view has a panel
 */
gboolean gsurf_lrg_scene_focus_view(GsurfLrgScene *self, GsurfView *view);

/**
 * gsurf_lrg_scene_pin_view:
 * @self: a #GsurfLrgScene
 * @view: a tab
 *
 * Pins @view's panel where it is, so arrangements stop moving it.
 *
 * Returns: %TRUE if @view has a panel
 */
gboolean gsurf_lrg_scene_pin_view(GsurfLrgScene *self, GsurfView *view);

/**
 * gsurf_lrg_scene_unpin_view:
 * @self: a #GsurfLrgScene
 * @view: (nullable): a tab, or %NULL to unpin every tab
 */
void gsurf_lrg_scene_unpin_view(GsurfLrgScene *self, GsurfView *view);

/**
 * gsurf_lrg_scene_is_view_pinned:
 * @self: a #GsurfLrgScene
 * @view: a tab
 *
 * Returns: whether @view's panel is pinned
 */
gboolean gsurf_lrg_scene_is_view_pinned(GsurfLrgScene *self, GsurfView *view);

/**
 * gsurf_lrg_scene_get_view_placement:
 * @self: a #GsurfLrgScene
 * @view: a tab
 *
 * Returns: (transfer full) (nullable): the panel's transform as
 *   "x y z yaw w h" (C locale), for persisting a pin, or %NULL
 */
gchar *gsurf_lrg_scene_get_view_placement(GsurfLrgScene *self, GsurfView *view);

/**
 * gsurf_lrg_scene_set_view_placement:
 * @self: a #GsurfLrgScene
 * @view: a tab
 * @placement: a string from gsurf_lrg_scene_get_view_placement()
 *
 * Pins @view's panel at a persisted transform (applied when the panel
 * appears, if it does not exist yet).
 *
 * Returns: %TRUE if @placement parsed
 */
gboolean gsurf_lrg_scene_set_view_placement(GsurfLrgScene *self, GsurfView *view,
                                            const gchar *placement);

/* --- camera ------------------------------------------------------------ */

/**
 * gsurf_lrg_scene_orbit:
 * @self: a #GsurfLrgScene
 * @dyaw: degrees around the room (positive = to the right)
 * @dpitch: degrees up (positive) or down
 * @eased: %TRUE to animate (keys), %FALSE to apply now (pointer drags)
 */
void gsurf_lrg_scene_orbit(GsurfLrgScene *self, gfloat dyaw, gfloat dpitch,
                           gboolean eased);

/**
 * gsurf_lrg_scene_look:
 * @self: a #GsurfLrgScene
 * @dyaw: degrees to turn the view in place
 * @dpitch: degrees to tilt the view in place
 */
void gsurf_lrg_scene_look(GsurfLrgScene *self, gfloat dyaw, gfloat dpitch);

/**
 * gsurf_lrg_scene_zoom:
 * @self: a #GsurfLrgScene
 * @factor: distance multiplier (<1 closer)
 * @eased: %TRUE to animate, %FALSE to apply now
 */
void gsurf_lrg_scene_zoom(GsurfLrgScene *self, gfloat factor, gboolean eased);

/**
 * gsurf_lrg_scene_reset_camera:
 * @self: a #GsurfLrgScene
 *
 * Eases the camera back to the default 3/4 view and leaves reading mode.
 */
void gsurf_lrg_scene_reset_camera(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_overview:
 * @self: a #GsurfLrgScene
 *
 * Pulls the camera back and up until every tab is in view.
 */
void gsurf_lrg_scene_overview(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_grab:
 * @self: a #GsurfLrgScene
 * @view: the tab to move
 * @x: pointer x in window pixels
 * @y: pointer y in window pixels
 *
 * Starts moving @view's panel under the pointer.
 *
 * Returns: %TRUE if the grab started
 */
gboolean gsurf_lrg_scene_grab(GsurfLrgScene *self, GsurfView *view, gfloat x,
                              gfloat y);

/**
 * gsurf_lrg_scene_drag:
 * @self: a #GsurfLrgScene
 * @x: pointer x in window pixels
 * @y: pointer y in window pixels
 */
void gsurf_lrg_scene_drag(GsurfLrgScene *self, gfloat x, gfloat y);

/**
 * gsurf_lrg_scene_release:
 * @self: a #GsurfLrgScene
 *
 * Ends a grab; the panel stays pinned where it was dropped.
 */
void gsurf_lrg_scene_release(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_save_view:
 * @self: a #GsurfLrgScene
 * @name: bookmark name
 */
void gsurf_lrg_scene_save_view(GsurfLrgScene *self, const gchar *name);

/**
 * gsurf_lrg_scene_restore_view:
 * @self: a #GsurfLrgScene
 * @name: bookmark name
 *
 * Returns: %TRUE if the bookmark exists
 */
gboolean gsurf_lrg_scene_restore_view(GsurfLrgScene *self, const gchar *name);

/**
 * gsurf_lrg_scene_list_views:
 * @self: a #GsurfLrgScene
 *
 * Returns: (transfer full): sorted bookmark names; free with g_strfreev()
 */
gchar **gsurf_lrg_scene_list_views(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_get_panel_count:
 * @self: a #GsurfLrgScene
 *
 * Returns: the number of panels in the scene
 */
guint gsurf_lrg_scene_get_panel_count(GsurfLrgScene *self);

/**
 * gsurf_lrg_scene_is_animating:
 * @self: a #GsurfLrgScene
 *
 * Returns: whether the camera or a panel is still moving
 */
gboolean gsurf_lrg_scene_is_animating(GsurfLrgScene *self);

/* --- screenshots ----------------------------------------------------------- */

/**
 * GsurfLrgSceneShotFunc:
 * @scene: the scene
 * @path: the file that was (or was not) written
 * @ok: whether the PNG was written
 * @user_data: data given to gsurf_lrg_scene_request_screenshot()
 */
typedef void (*GsurfLrgSceneShotFunc)(GsurfLrgScene *scene, const gchar *path,
                                      gboolean ok, gpointer user_data);

/**
 * gsurf_lrg_scene_request_screenshot:
 * @self: a #GsurfLrgScene
 * @path: the PNG to write
 * @func: (scope async): called once the frame is written
 * @user_data: data for @func
 *
 * Writes the next presented frame -- the whole window as the user sees it,
 * scene and chrome -- to @path.  The 3D frame does it itself; the flat
 * reading path calls gsurf_lrg_scene_flush_screenshots().
 */
void gsurf_lrg_scene_request_screenshot(GsurfLrgScene *self, const gchar *path,
                                        GsurfLrgSceneShotFunc func,
                                        gpointer user_data);

/**
 * gsurf_lrg_scene_flush_screenshots:
 * @self: a #GsurfLrgScene
 *
 * Writes pending screenshots from the current back buffer.  Call after the
 * frame is drawn and before the swap.
 */
void gsurf_lrg_scene_flush_screenshots(GsurfLrgScene *self);

/* --- pure helpers (exposed for tests) ------------------------------------- */

/**
 * gsurf_lrg_scene_refresh_interval:
 * @is_active: the tab is the active one
 * @coverage: the panel's on-screen height in pixels (0 = not visible)
 * @background_fps: the configured background refresh rate
 *
 * The refresh policy: how long a tab's texture may go without a new capture.
 * The active tab refreshes every frame; visible tabs at @background_fps
 * (slower when tiny on screen); hidden tabs every few seconds so their
 * panel is not stale when the camera swings round.
 *
 * Returns: the interval in microseconds (0 = every frame)
 */
gint64 gsurf_lrg_scene_refresh_interval(gboolean is_active, gfloat coverage,
                                        gdouble background_fps);

/**
 * gsurf_lrg_scene_history_key:
 * @offset: steps from the current page (negative = back), non-zero
 *
 * Returns: the panel key a history entry uses (never collides with a tab key,
 *   which is a pointer)
 */
guint64 gsurf_lrg_scene_history_key(gint offset);

/**
 * gsurf_lrg_scene_history_offset:
 * @key: a panel key
 * @out_offset: (out): the history offset when @key is a history key
 *
 * Returns: %TRUE if @key came from gsurf_lrg_scene_history_key()
 */
gboolean gsurf_lrg_scene_history_offset(guint64 key, gint *out_offset);

G_END_DECLS

#endif /* GSURF_LRG_SCENE_H */
