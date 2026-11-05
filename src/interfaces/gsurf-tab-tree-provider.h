/*
 * gsurf-tab-tree-provider.h - Module hook: a window's tab tree
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A module that keeps a tree of a window's tabs (the tabs module: each tab
 * under the tab it was opened from, with collapse state and drag-and-drop
 * re-parenting) implements #GsurfTabTreeProvider, so other code (the MCP
 * server, scripts) can read and change the tree without linking against
 * that module. The window's view order stays the source of truth: a
 * provider that moves a tab reorders the window to match.
 *
 * Find a provider by walking gsurf_module_manager_get_modules() for an
 * active module with GSURF_IS_TAB_TREE_PROVIDER().
 *
 * Since: 0.4.4
 */

#ifndef GSURF_TAB_TREE_PROVIDER_H
#define GSURF_TAB_TREE_PROVIDER_H

#include <glib-object.h>

#include "gsurf-enums.h"
#include "core/gsurf-view.h"
#include "window/gsurf-window.h"

G_BEGIN_DECLS

#define GSURF_TYPE_TAB_TREE_PROVIDER (gsurf_tab_tree_provider_get_type())

G_DECLARE_INTERFACE(GsurfTabTreeProvider, gsurf_tab_tree_provider, GSURF, TAB_TREE_PROVIDER,
                    GObject)

/**
 * GsurfTabTreeProviderInterface:
 * @parent_iface: the parent interface
 * @get_parent: the id of @view's parent in the tree of @window, 0 for a
 *   root (or a window or view the provider does not track)
 * @get_collapsed: whether @view's subtree is collapsed
 * @set_collapsed: collapse or expand @view's subtree; %FALSE when @view
 *   has no children or is not in the tree
 * @move_view: move @view with its subtree to @position relative to
 *   @target; a %NULL @target makes @view a root at the end of the window.
 *   %FALSE when the move is impossible (@target in @view's subtree, a
 *   view not in the tree)
 *
 * Virtual table for #GsurfTabTreeProvider. Every vfunc is optional and
 * runs on the main thread.
 *
 * Since: 0.4.4
 */
struct _GsurfTabTreeProviderInterface
{
	GTypeInterface parent_iface;

	guint64  (*get_parent)    (GsurfTabTreeProvider *self, GsurfWindow *window,
	                           GsurfView *view);
	gboolean (*get_collapsed) (GsurfTabTreeProvider *self, GsurfWindow *window,
	                           GsurfView *view);
	gboolean (*set_collapsed) (GsurfTabTreeProvider *self, GsurfWindow *window,
	                           GsurfView *view, gboolean collapsed);
	gboolean (*move_view)     (GsurfTabTreeProvider *self, GsurfWindow *window,
	                           GsurfView *view, GsurfView *target,
	                           GsurfTabTreePosition position);
};

/**
 * gsurf_tab_tree_provider_get_parent:
 * @self: a #GsurfTabTreeProvider
 * @window: the window holding @view
 * @view: a view of @window
 *
 * The tab @view sits under in the tree: initially its opener
 * (gsurf_view_get_opener_id()), then whatever closing, promoting and
 * moving made of it.
 *
 * Returns: the parent's view id, or 0 for a root
 *
 * Since: 0.4.4
 */
guint64  gsurf_tab_tree_provider_get_parent(GsurfTabTreeProvider *self, GsurfWindow *window,
                                            GsurfView *view);

/**
 * gsurf_tab_tree_provider_get_collapsed:
 * @self: a #GsurfTabTreeProvider
 * @window: the window holding @view
 * @view: a view of @window
 *
 * Returns: whether @view's descendants are hidden in the tree
 *
 * Since: 0.4.4
 */
gboolean gsurf_tab_tree_provider_get_collapsed(GsurfTabTreeProvider *self,
                                               GsurfWindow *window, GsurfView *view);

/**
 * gsurf_tab_tree_provider_set_collapsed:
 * @self: a #GsurfTabTreeProvider
 * @window: the window holding @view
 * @view: a view of @window with children
 * @collapsed: %TRUE to collapse @view's subtree, %FALSE to expand it
 *
 * Collapses or expands a subtree, as a click on its twisty does.
 * Setting the current state again succeeds and changes nothing.
 *
 * Returns: %TRUE when @view now has the requested state; %FALSE when it
 *   has no children or the provider does not track it
 *
 * Since: 0.4.4
 */
gboolean gsurf_tab_tree_provider_set_collapsed(GsurfTabTreeProvider *self,
                                               GsurfWindow *window, GsurfView *view,
                                               gboolean collapsed);

/**
 * gsurf_tab_tree_provider_move_view:
 * @self: a #GsurfTabTreeProvider
 * @window: the window holding @view
 * @view: the view to move, with its subtree
 * @target: (nullable): the view to move it next to or under; %NULL makes
 *   @view a root at the end of the window
 * @position: before, after or under @target (ignored for a %NULL @target)
 *
 * Re-parents and reorders a tab like a sidebar drag-and-drop; the
 * window's view order follows (gsurf_window_move_view()).
 *
 * Returns: %TRUE when @view moved (or already was there)
 *
 * Since: 0.4.4
 */
gboolean gsurf_tab_tree_provider_move_view(GsurfTabTreeProvider *self, GsurfWindow *window,
                                           GsurfView *view, GsurfView *target,
                                           GsurfTabTreePosition position);

G_END_DECLS

#endif /* GSURF_TAB_TREE_PROVIDER_H */
