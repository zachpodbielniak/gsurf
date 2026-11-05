/*
 * gsurf-window-layout.h - Delegate that arranges a window's views
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #GsurfWindow normally shows one view at a time: the GTK backends keep
 * every view widget in a private stack and show the active one. A
 * #GsurfWindowLayout replaces that stack with its own native widget
 * (panes, a grid, a carousel, ...). While a layout is installed with
 * gsurf_window_set_layout(), the window hands every view to it instead
 * of the stack: new views (add_view), closed views (remove_view) and the
 * active view (activate_view). Removing the layout puts every view back
 * into the stack and shows the active one again.
 *
 * The layout also reports which views are on screen
 * (gsurf_window_get_view_visible(), #GsurfWindow::visible-views-changed),
 * so chrome such as the tab tree can mark them.
 *
 * Optional extras (since 0.4.2), each a no-op for a layout that leaves
 * the vfunc unset:
 *   - groups: a layout that keeps several arrangements (wmux windows)
 *     lists them as #GsurfLayoutGroup (index, name, current flag and the
 *     views its panes show, in pane order), switches between them on
 *     request, and announces changes with
 *     gsurf_window_layout_notify_groups()
 *     (#GsurfWindow::layout-groups-changed). The tab sidebar groups its
 *     tree by them without linking against the layout's module;
 *   - drops: chrome that drags a view (a sidebar tab row) asks the
 *     layout to put it next to or in place of a view on screen
 *     (gsurf_window_layout_show_view_at()), and brackets the drag with
 *     gsurf_window_layout_begin_view_drag() / _end_view_drag() so the
 *     layout can show its drop targets.
 *
 * And since 0.4.4, for agents and scripts that drive the layout without
 * linking against its module (the MCP server):
 *   - panes: gsurf_window_layout_list_panes() lists the panes of a group
 *     as #GsurfLayoutPane (index, id, the view shown, active, zoomed and
 *     the rectangle in pixels and as a fraction of the layout's area);
 *   - commands: gsurf_window_layout_run_command() runs one line of the
 *     layout's command language (wmux: a tmux-subset command such as
 *     "split-window -h") and reports parse and execution errors;
 *   - description: gsurf_window_layout_describe() returns a vardict of
 *     facts about the layout (wmux: its name and prefix key).
 */

#ifndef GSURF_WINDOW_LAYOUT_H
#define GSURF_WINDOW_LAYOUT_H

#include <glib-object.h>

#include "core/gsurf-view.h"
#include "window/gsurf-window.h"

G_BEGIN_DECLS

#define GSURF_TYPE_WINDOW_LAYOUT (gsurf_window_layout_get_type())

G_DECLARE_INTERFACE(GsurfWindowLayout, gsurf_window_layout, GSURF, WINDOW_LAYOUT, GObject)

/**
 * GsurfWindowLayoutIndexFunc:
 * @view: a view of the window being serialized
 * @user_data: (closure): data passed to gsurf_window_layout_serialize()
 *
 * Maps a view to the stable index a saved state refers to it by (for the
 * session module: the position of its tab in the saved file).
 *
 * Returns: the index of @view, or -1 when @view is not saved (the layout
 *   then leaves it out of the state)
 */
typedef gint (*GsurfWindowLayoutIndexFunc) (GsurfView *view, gpointer user_data);

/* --- Groups --- */

#define GSURF_TYPE_LAYOUT_GROUP (gsurf_layout_group_get_type())

/**
 * GsurfLayoutGroup:
 *
 * One arrangement of a #GsurfWindowLayout that keeps several (a wmux
 * window): its index, its display name, whether it is the one on screen,
 * and the ids of the views its panes show, in pane order. An opaque,
 * reference-counted boxed type; the layout builds them in its
 * @list_groups vfunc.
 *
 * Since: 0.4.2
 */
typedef struct _GsurfLayoutGroup GsurfLayoutGroup;

GType             gsurf_layout_group_get_type(void) G_GNUC_CONST;

/**
 * gsurf_layout_group_new:
 * @index: the group's index (what @select_group takes)
 * @name: (nullable): its display name; %NULL is ""
 * @current: whether the group is the one on screen
 *
 * Returns: (transfer full): a new group without views
 *
 * Since: 0.4.2
 */
GsurfLayoutGroup *gsurf_layout_group_new(guint index, const gchar *name, gboolean current);

/**
 * gsurf_layout_group_ref:
 * @group: a #GsurfLayoutGroup
 *
 * Returns: (transfer full): @group
 *
 * Since: 0.4.2
 */
GsurfLayoutGroup *gsurf_layout_group_ref(GsurfLayoutGroup *group);

/**
 * gsurf_layout_group_unref:
 * @group: (transfer full): a #GsurfLayoutGroup
 *
 * Drops a reference; the last one frees the group.
 *
 * Since: 0.4.2
 */
void              gsurf_layout_group_unref(GsurfLayoutGroup *group);

/**
 * gsurf_layout_group_add_view_id:
 * @group: a #GsurfLayoutGroup
 * @view_id: the id (gsurf_view_get_id()) of a view a pane shows
 *
 * Appends a view to the group's pane order. Zero and ids already listed
 * are ignored.
 *
 * Since: 0.4.2
 */
void              gsurf_layout_group_add_view_id(GsurfLayoutGroup *group, guint64 view_id);

/**
 * gsurf_layout_group_get_index:
 * @group: a #GsurfLayoutGroup
 *
 * Returns: the group's index
 *
 * Since: 0.4.2
 */
guint             gsurf_layout_group_get_index(GsurfLayoutGroup *group);

/**
 * gsurf_layout_group_get_name:
 * @group: a #GsurfLayoutGroup
 *
 * Returns: (transfer none): the display name, never %NULL
 *
 * Since: 0.4.2
 */
const gchar      *gsurf_layout_group_get_name(GsurfLayoutGroup *group);

/**
 * gsurf_layout_group_get_current:
 * @group: a #GsurfLayoutGroup
 *
 * Returns: whether the group is the one on screen
 *
 * Since: 0.4.2
 */
gboolean          gsurf_layout_group_get_current(GsurfLayoutGroup *group);

/**
 * gsurf_layout_group_get_view_ids:
 * @group: a #GsurfLayoutGroup
 * @n_ids: (out): the number of ids
 *
 * Returns: (array length=n_ids) (transfer none): the ids of the views
 *   the group's panes show, in pane order
 *
 * Since: 0.4.2
 */
const guint64    *gsurf_layout_group_get_view_ids(GsurfLayoutGroup *group, guint *n_ids);

/**
 * gsurf_layout_group_set_last:
 * @group: a #GsurfLayoutGroup
 * @last: whether the group is the previously current one (tmux
 *   last-window)
 *
 * Since: 0.4.4
 */
void              gsurf_layout_group_set_last(GsurfLayoutGroup *group, gboolean last);

/**
 * gsurf_layout_group_get_last:
 * @group: a #GsurfLayoutGroup
 *
 * Returns: whether the group is the previously current one; %FALSE for a
 *   layout that does not track it
 *
 * Since: 0.4.4
 */
gboolean          gsurf_layout_group_get_last(GsurfLayoutGroup *group);

/**
 * gsurf_layout_group_set_zoomed:
 * @group: a #GsurfLayoutGroup
 * @zoomed: whether one pane of the group fills its whole area
 *
 * Since: 0.4.4
 */
void              gsurf_layout_group_set_zoomed(GsurfLayoutGroup *group, gboolean zoomed);

/**
 * gsurf_layout_group_get_zoomed:
 * @group: a #GsurfLayoutGroup
 *
 * Returns: whether one pane of the group is zoomed
 *
 * Since: 0.4.4
 */
gboolean          gsurf_layout_group_get_zoomed(GsurfLayoutGroup *group);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfLayoutGroup, gsurf_layout_group_unref)

/* --- Panes (since 0.4.4) --- */

#define GSURF_TYPE_LAYOUT_PANE (gsurf_layout_pane_get_type())

/**
 * GsurfLayoutPane:
 *
 * One pane of a #GsurfWindowLayout group: its index in the group's pane
 * order (what wmux's "select-pane -t" takes), the layout's own id for it
 * (stable while the pane lives), the id of the view it shows (0 for an
 * empty pane), whether it is the group's active pane, whether it is
 * zoomed, and its rectangle in pixels and as fractions (0..1) of the
 * layout's area. An opaque, reference-counted boxed type; the layout
 * builds them in its @list_panes vfunc.
 *
 * Since: 0.4.4
 */
typedef struct _GsurfLayoutPane GsurfLayoutPane;

GType            gsurf_layout_pane_get_type(void) G_GNUC_CONST;

/**
 * gsurf_layout_pane_new:
 * @index: the pane's position in its group's pane order
 * @id: the layout's id for the pane
 * @view_id: the id (gsurf_view_get_id()) of the view it shows, 0 for none
 * @active: whether it is the group's active pane
 *
 * Returns: (transfer full): a new pane with an empty rectangle
 *
 * Since: 0.4.4
 */
GsurfLayoutPane *gsurf_layout_pane_new(guint index, guint id, guint64 view_id, gboolean active);

/**
 * gsurf_layout_pane_ref:
 * @pane: a #GsurfLayoutPane
 *
 * Returns: (transfer full): @pane
 *
 * Since: 0.4.4
 */
GsurfLayoutPane *gsurf_layout_pane_ref(GsurfLayoutPane *pane);

/**
 * gsurf_layout_pane_unref:
 * @pane: (transfer full): a #GsurfLayoutPane
 *
 * Drops a reference; the last one frees the pane.
 *
 * Since: 0.4.4
 */
void             gsurf_layout_pane_unref(GsurfLayoutPane *pane);

/**
 * gsurf_layout_pane_get_index:
 * @pane: a #GsurfLayoutPane
 *
 * Returns: the pane's position in its group's pane order
 *
 * Since: 0.4.4
 */
guint            gsurf_layout_pane_get_index(GsurfLayoutPane *pane);

/**
 * gsurf_layout_pane_get_id:
 * @pane: a #GsurfLayoutPane
 *
 * Returns: the layout's id for the pane
 *
 * Since: 0.4.4
 */
guint            gsurf_layout_pane_get_id(GsurfLayoutPane *pane);

/**
 * gsurf_layout_pane_get_view_id:
 * @pane: a #GsurfLayoutPane
 *
 * Returns: the id of the view the pane shows, 0 for none
 *
 * Since: 0.4.4
 */
guint64          gsurf_layout_pane_get_view_id(GsurfLayoutPane *pane);

/**
 * gsurf_layout_pane_get_active:
 * @pane: a #GsurfLayoutPane
 *
 * Returns: whether the pane is its group's active pane
 *
 * Since: 0.4.4
 */
gboolean         gsurf_layout_pane_get_active(GsurfLayoutPane *pane);

/**
 * gsurf_layout_pane_set_zoomed:
 * @pane: a #GsurfLayoutPane
 * @zoomed: whether the pane fills its group's whole area
 *
 * Since: 0.4.4
 */
void             gsurf_layout_pane_set_zoomed(GsurfLayoutPane *pane, gboolean zoomed);

/**
 * gsurf_layout_pane_get_zoomed:
 * @pane: a #GsurfLayoutPane
 *
 * Returns: whether the pane is zoomed
 *
 * Since: 0.4.4
 */
gboolean         gsurf_layout_pane_get_zoomed(GsurfLayoutPane *pane);

/**
 * gsurf_layout_pane_set_rect:
 * @pane: a #GsurfLayoutPane
 * @x: left edge in pixels, relative to the layout's area
 * @y: top edge in pixels
 * @width: width in pixels
 * @height: height in pixels
 *
 * Since: 0.4.4
 */
void             gsurf_layout_pane_set_rect(GsurfLayoutPane *pane, gint x, gint y,
                                            gint width, gint height);

/**
 * gsurf_layout_pane_get_rect:
 * @pane: a #GsurfLayoutPane
 * @x: (out) (optional): left edge in pixels
 * @y: (out) (optional): top edge in pixels
 * @width: (out) (optional): width in pixels
 * @height: (out) (optional): height in pixels
 *
 * Since: 0.4.4
 */
void             gsurf_layout_pane_get_rect(GsurfLayoutPane *pane, gint *x, gint *y,
                                            gint *width, gint *height);

/**
 * gsurf_layout_pane_set_fraction:
 * @pane: a #GsurfLayoutPane
 * @x: left edge as a fraction (0..1) of the layout's width
 * @y: top edge as a fraction of the layout's height
 * @width: width as a fraction of the layout's width
 * @height: height as a fraction of the layout's height
 *
 * The rectangle independent of the window size (and of gaps). Values are
 * clamped to 0..1.
 *
 * Since: 0.4.4
 */
void             gsurf_layout_pane_set_fraction(GsurfLayoutPane *pane, gdouble x, gdouble y,
                                                gdouble width, gdouble height);

/**
 * gsurf_layout_pane_get_fraction:
 * @pane: a #GsurfLayoutPane
 * @x: (out) (optional): left edge fraction
 * @y: (out) (optional): top edge fraction
 * @width: (out) (optional): width fraction
 * @height: (out) (optional): height fraction
 *
 * Since: 0.4.4
 */
void             gsurf_layout_pane_get_fraction(GsurfLayoutPane *pane, gdouble *x, gdouble *y,
                                                gdouble *width, gdouble *height);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfLayoutPane, gsurf_layout_pane_unref)

/**
 * GsurfWindowLayoutInterface:
 * @parent_iface: the parent interface
 * @get_root: the native widget (a #GtkWidget as gpointer) that takes the
 *   place of the view stack. The layout keeps its own reference to it:
 *   the window only packs and unpacks it.
 * @attach: the layout is being installed in @window (before any
 *   @add_view); build per-window state here
 * @detach: the layout is being removed from @window: unparent every view
 *   widget it holds (the window then packs them into its stack again)
 *   and drop per-window state
 * @add_view: a view joined @window (or was in it at attach time): parent
 *   its widget in a pane or keep it as a hidden background view. Must not
 *   focus the view; activation comes separately.
 * @remove_view: a view leaves @window: unparent its widget. The window
 *   picks the next active view afterwards.
 * @activate_view: @view became the active view: show it (put it in a
 *   pane) and give it keyboard focus
 * @is_view_visible: optional; whether @view is on screen; %NULL means
 *   only the active view is
 * @serialize: optional; a string describing the arrangement, views
 *   named by the index @map gives them; %NULL means unsupported
 * @restore: optional; rebuild an arrangement from a string made by
 *   @serialize, with @views[i] the view saved at index i; %NULL means
 *   unsupported
 * @list_groups: optional (since 0.4.2); the layout's groups in order as
 *   a new #GPtrArray of #GsurfLayoutGroup (with a free function), or
 *   %NULL for a layout without groups
 * @select_group: optional (since 0.4.2); show the group with @index;
 *   %FALSE for an unknown index
 * @show_view_at: optional (since 0.4.2); put @view, a view of @window,
 *   in place of (@zone center) or beside (an edge zone) @target, a view
 *   on screen, and make it active; %FALSE when it cannot
 * @begin_view_drag: optional (since 0.4.2); chrome started dragging
 *   @view: show drop targets over the views on screen
 * @end_view_drag: optional (since 0.4.2); the drag ended (dropped or
 *   not): hide them again
 * @rename_group: optional (since 0.4.3); give the group with @index the
 *   name @name; %NULL or "" goes back to the layout's automatic naming;
 *   %FALSE for an unknown index
 * @list_panes: optional (since 0.4.4); the panes of the group with index
 *   @group (0 for a layout without groups) in pane order, as a new
 *   #GPtrArray of #GsurfLayoutPane (with a free function), or %NULL for an
 *   unknown group
 * @run_command: optional (since 0.4.4); run one line of the layout's
 *   command language on @window; %FALSE with @error set when it does not
 *   parse or cannot be carried out
 * @describe: optional (since 0.4.4); facts about the layout as a vardict
 *   (#GVariant of type a{sv}); may be floating
 *
 * Virtual table for #GsurfWindowLayout. Every vfunc runs on the main
 * thread; the window guarantees @attach before and @detach after every
 * other call for a given window.
 */
struct _GsurfWindowLayoutInterface
{
	GTypeInterface parent_iface;

	gpointer (*get_root)        (GsurfWindowLayout *self);
	void     (*attach)          (GsurfWindowLayout *self, GsurfWindow *window);
	void     (*detach)          (GsurfWindowLayout *self, GsurfWindow *window);
	void     (*add_view)        (GsurfWindowLayout *self, GsurfWindow *window,
	                             GsurfView *view);
	void     (*remove_view)     (GsurfWindowLayout *self, GsurfWindow *window,
	                             GsurfView *view);
	void     (*activate_view)   (GsurfWindowLayout *self, GsurfWindow *window,
	                             GsurfView *view);
	gboolean (*is_view_visible) (GsurfWindowLayout *self, GsurfWindow *window,
	                             GsurfView *view);
	gchar   *(*serialize)       (GsurfWindowLayout *self, GsurfWindow *window,
	                             GsurfWindowLayoutIndexFunc map, gpointer user_data);
	gboolean (*restore)         (GsurfWindowLayout *self, GsurfWindow *window,
	                             const gchar *state, GsurfView **views, guint n_views);

	/* Since 0.4.2 */
	GPtrArray *(*list_groups)     (GsurfWindowLayout *self, GsurfWindow *window);
	gboolean   (*select_group)    (GsurfWindowLayout *self, GsurfWindow *window,
	                               guint index);
	gboolean   (*show_view_at)    (GsurfWindowLayout *self, GsurfWindow *window,
	                               GsurfView *view, GsurfView *target,
	                               GsurfLayoutDropZone zone);
	void       (*begin_view_drag) (GsurfWindowLayout *self, GsurfWindow *window,
	                               GsurfView *view);
	void       (*end_view_drag)   (GsurfWindowLayout *self, GsurfWindow *window);

	/* Since 0.4.3 */
	gboolean   (*rename_group)    (GsurfWindowLayout *self, GsurfWindow *window,
	                               guint index, const gchar *name);

	/* Since 0.4.4 */
	GPtrArray *(*list_panes)      (GsurfWindowLayout *self, GsurfWindow *window,
	                               guint group);
	gboolean   (*run_command)     (GsurfWindowLayout *self, GsurfWindow *window,
	                               const gchar *command, GError **error);
	GVariant  *(*describe)        (GsurfWindowLayout *self, GsurfWindow *window);
};

/* --- Interface wrappers (the window calls these; so may tests) --- */

/**
 * gsurf_window_layout_get_root:
 * @self: a #GsurfWindowLayout
 *
 * Returns: (transfer none) (nullable): the layout's native root widget
 *   (a #GtkWidget as gpointer)
 */
gpointer gsurf_window_layout_get_root(GsurfWindowLayout *self);

/**
 * gsurf_window_layout_attach:
 * @self: a #GsurfWindowLayout
 * @window: the window installing @self
 *
 * Calls the @attach vfunc. Normally only gsurf_window_set_layout() calls
 * this.
 */
void     gsurf_window_layout_attach(GsurfWindowLayout *self, GsurfWindow *window);

/**
 * gsurf_window_layout_detach:
 * @self: a #GsurfWindowLayout
 * @window: the window removing @self
 *
 * Calls the @detach vfunc. Normally only gsurf_window_set_layout() calls
 * this.
 */
void     gsurf_window_layout_detach(GsurfWindowLayout *self, GsurfWindow *window);

/**
 * gsurf_window_layout_add_view:
 * @self: a #GsurfWindowLayout
 * @window: the window @view joined
 * @view: the view
 *
 * Calls the @add_view vfunc.
 */
void     gsurf_window_layout_add_view(GsurfWindowLayout *self, GsurfWindow *window,
                                      GsurfView *view);

/**
 * gsurf_window_layout_remove_view:
 * @self: a #GsurfWindowLayout
 * @window: the window @view leaves
 * @view: the view
 *
 * Calls the @remove_view vfunc.
 */
void     gsurf_window_layout_remove_view(GsurfWindowLayout *self, GsurfWindow *window,
                                         GsurfView *view);

/**
 * gsurf_window_layout_activate_view:
 * @self: a #GsurfWindowLayout
 * @window: the window
 * @view: the new active view
 *
 * Calls the @activate_view vfunc.
 */
void     gsurf_window_layout_activate_view(GsurfWindowLayout *self, GsurfWindow *window,
                                           GsurfView *view);

/**
 * gsurf_window_layout_is_view_visible:
 * @self: a #GsurfWindowLayout
 * @window: the window
 * @view: a view of @window
 *
 * Calls the @is_view_visible vfunc; without one, only the active view of
 * @window is visible.
 *
 * Returns: whether @view is on screen
 */
gboolean gsurf_window_layout_is_view_visible(GsurfWindowLayout *self, GsurfWindow *window,
                                             GsurfView *view);

/**
 * gsurf_window_layout_serialize:
 * @self: a #GsurfWindowLayout
 * @window: the window whose arrangement to describe
 * @map: (scope call) (closure user_data): maps each view to its saved index
 * @user_data: data for @map
 *
 * Describes the arrangement of @window as a string the same layout can
 * restore later (the session module saves it per window).
 *
 * Returns: (transfer full) (nullable): the state, or %NULL when the
 *   layout cannot serialize
 */
gchar   *gsurf_window_layout_serialize(GsurfWindowLayout *self, GsurfWindow *window,
                                       GsurfWindowLayoutIndexFunc map, gpointer user_data);

/**
 * gsurf_window_layout_restore:
 * @self: a #GsurfWindowLayout
 * @window: the window to arrange
 * @state: a string made by gsurf_window_layout_serialize()
 * @views: (array length=n_views): the views by saved index
 * @n_views: the length of @views
 *
 * Rebuilds an arrangement saved with gsurf_window_layout_serialize().
 *
 * Returns: %TRUE when the layout restored @state; %FALSE when it cannot
 *   restore, or @state does not fit @views (the caller keeps the default
 *   arrangement)
 */
gboolean gsurf_window_layout_restore(GsurfWindowLayout *self, GsurfWindow *window,
                                     const gchar *state, GsurfView **views, guint n_views);

/**
 * gsurf_window_layout_list_groups:
 * @self: a #GsurfWindowLayout
 * @window: the window
 *
 * Lists the layout's groups (wmux windows), in order.
 *
 * Returns: (transfer full) (element-type GsurfLayoutGroup) (nullable):
 *   the groups, or %NULL when the layout has none
 *
 * Since: 0.4.2
 */
GPtrArray *gsurf_window_layout_list_groups(GsurfWindowLayout *self, GsurfWindow *window);

/**
 * gsurf_window_layout_select_group:
 * @self: a #GsurfWindowLayout
 * @window: the window
 * @index: a group index (gsurf_layout_group_get_index())
 *
 * Shows the group with @index.
 *
 * Returns: %TRUE when the layout has the group (it is now current)
 *
 * Since: 0.4.2
 */
gboolean   gsurf_window_layout_select_group(GsurfWindowLayout *self, GsurfWindow *window,
                                            guint index);

/**
 * gsurf_window_layout_rename_group:
 * @self: a #GsurfWindowLayout
 * @window: the window
 * @index: a group index (gsurf_layout_group_get_index())
 * @name: (nullable): the new name; %NULL or "" restores automatic naming
 *   (wmux: the window follows its active pane's page title)
 *
 * Renames a group, the way the layout's own rename does (wmux: like
 * =rename-window=), so every place that shows the name agrees. The
 * layout announces the change with gsurf_window_layout_notify_groups().
 *
 * Returns: %TRUE when the layout has the group
 *
 * Since: 0.4.3
 */
gboolean   gsurf_window_layout_rename_group(GsurfWindowLayout *self, GsurfWindow *window,
                                            guint index, const gchar *name);

/**
 * gsurf_window_layout_show_view_at:
 * @self: a #GsurfWindowLayout
 * @window: the window
 * @view: a view of @window (on screen or not)
 * @target: a view of @window on screen
 * @zone: %GSURF_LAYOUT_DROP_CENTER to show @view in @target's place, an
 *   edge to split @target's place and show @view on that side
 *
 * What a drop of @view on @target does; @view becomes the active view.
 * The layout decides what happens to a view @view displaces (wmux: it
 * becomes a background tab, or swaps places with @view when @view was on
 * screen too).
 *
 * Returns: %TRUE when the arrangement changed
 *
 * Since: 0.4.2
 */
gboolean   gsurf_window_layout_show_view_at(GsurfWindowLayout *self, GsurfWindow *window,
                                            GsurfView *view, GsurfView *target,
                                            GsurfLayoutDropZone zone);

/**
 * gsurf_window_layout_begin_view_drag:
 * @self: a #GsurfWindowLayout
 * @window: the window
 * @view: the view being dragged
 *
 * Chrome started a drag of @view that may end on the layout: the layout
 * shows drop targets until gsurf_window_layout_end_view_drag().
 *
 * Since: 0.4.2
 */
void       gsurf_window_layout_begin_view_drag(GsurfWindowLayout *self, GsurfWindow *window,
                                               GsurfView *view);

/**
 * gsurf_window_layout_end_view_drag:
 * @self: a #GsurfWindowLayout
 * @window: the window
 *
 * The drag started with gsurf_window_layout_begin_view_drag() ended.
 * Safe to call without a drag in progress.
 *
 * Since: 0.4.2
 */
void       gsurf_window_layout_end_view_drag(GsurfWindowLayout *self, GsurfWindow *window);

/**
 * gsurf_window_layout_list_panes:
 * @self: a #GsurfWindowLayout
 * @window: the window
 * @group: a group index (gsurf_layout_group_get_index()); 0 for a layout
 *   without groups
 *
 * Lists the panes of a group in pane order (wmux: left to right, top to
 * bottom), with the view each shows and its rectangle.
 *
 * Returns: (transfer full) (element-type GsurfLayoutPane) (nullable): the
 *   panes, or %NULL when the layout cannot list them or has no such group
 *
 * Since: 0.4.4
 */
GPtrArray *gsurf_window_layout_list_panes(GsurfWindowLayout *self, GsurfWindow *window,
                                          guint group);

/**
 * gsurf_window_layout_run_command:
 * @self: a #GsurfWindowLayout
 * @window: the window
 * @command: one line of the layout's command language (wmux: a tmux-subset
 *   command such as "split-window -h -p 30")
 * @error: return location for a #GError
 *
 * Runs @command as if it were typed at the layout's own command prompt,
 * so a program can reach every operation of the layout without linking
 * against its module. A layout without a command language fails with
 * %G_IO_ERROR_NOT_SUPPORTED.
 *
 * Returns: %TRUE when @command ran; %FALSE with @error set when it did not
 *   parse or could not be carried out (the message says why)
 *
 * Since: 0.4.4
 */
gboolean   gsurf_window_layout_run_command(GsurfWindowLayout *self, GsurfWindow *window,
                                           const gchar *command, GError **error);

/**
 * gsurf_window_layout_describe:
 * @self: a #GsurfWindowLayout
 * @window: the window
 *
 * Facts about the layout as a vardict. Keys a layout may set: "name" (s,
 * e.g. "wmux"), "prefix" (s, the prefix key), "status" (b, whether its
 * status line is shown), "mouse" (b), "width" and "height" (i, the area
 * its panes share, in pixels).
 *
 * Returns: (transfer full) (nullable): a non-floating #GVariant of type
 *   a{sv}, or %NULL when the layout does not describe itself
 *
 * Since: 0.4.4
 */
GVariant  *gsurf_window_layout_describe(GsurfWindowLayout *self, GsurfWindow *window);

/**
 * GSURF_LAYOUT_VIEW_MIME_TYPE:
 *
 * The private drag-and-drop target a view drag carries (the view id as
 * decimal text). Same-application only.
 *
 * Since: 0.4.2
 */
#define GSURF_LAYOUT_VIEW_MIME_TYPE "application/x-gsurf-view-id"

/* --- Window side (implemented in gsurf-window.c) --- */

/**
 * gsurf_window_set_layout:
 * @self: a #GsurfWindow
 * @layout: (nullable): the layout to install, or %NULL to remove it
 *
 * Installs @layout in place of the backend's view stack, or removes the
 * current layout with %NULL. Installing takes every view widget out of
 * the stack, then calls the layout's @attach, @add_view for each view in
 * window order, @activate_view for the active view, and packs the
 * layout's root widget where the stack was. Removing calls @detach,
 * unpacks the root and puts every view back into the stack, showing the
 * active one. Installing over another layout removes that one first.
 * Both emit #GsurfWindow::visible-views-changed. Setting the layout that
 * is already set (or %NULL when none is) does nothing.
 *
 * The window keeps a reference to @layout until it is removed or the
 * window is disposed (which removes it first).
 *
 * Kiosk mode (one view by design) and backends without native content
 * packing (libregnum) refuse a layout with a warning.
 *
 * Returns: %TRUE when @layout is now the window's layout
 */
gboolean           gsurf_window_set_layout(GsurfWindow *self, GsurfWindowLayout *layout);

/**
 * gsurf_window_get_layout:
 * @self: a #GsurfWindow
 *
 * Returns: (transfer none) (nullable): the installed layout
 */
GsurfWindowLayout *gsurf_window_get_layout(GsurfWindow *self);

/**
 * gsurf_window_layout_notify_visible:
 * @window: a #GsurfWindow
 *
 * Called by a layout when the set of views on screen changed (a split, a
 * closed pane, a different view shown in a pane): emits
 * #GsurfWindow::visible-views-changed.
 */
void               gsurf_window_layout_notify_visible(GsurfWindow *window);

/**
 * gsurf_window_layout_notify_groups:
 * @window: a #GsurfWindow
 *
 * Called by a layout when its groups changed (one was created, closed,
 * renamed or selected, or the views its panes show changed): emits
 * #GsurfWindow::layout-groups-changed.
 *
 * Since: 0.4.2
 */
void               gsurf_window_layout_notify_groups(GsurfWindow *window);

G_END_DECLS

#endif /* GSURF_WINDOW_LAYOUT_H */
