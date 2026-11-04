/*
 * gsurf-tabs-private.h - Shared state of the tabs module's core and UI
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The tabs module is three parts, all built into tabs.so:
 *   - gsurf-tab-tree.{c,h}: the toolkit-neutral tree model;
 *   - gsurf-tabs-module.c: the module core (config, keys, actions, window
 *     and view signals, keeping the model and the window order in step);
 *   - gsurf-tabs-ui.c: the horizontal strip and the tree sidebar, on GTK3
 *     and GTK4 through modules/include/gsurf-chrome-compat.h.
 * This header is the contract between the core and the UI.
 */

#ifndef GSURF_TABS_PRIVATE_H
#define GSURF_TABS_PRIVATE_H

#include <gsurf/gsurf.h>
#include <gtk/gtk.h>

#include "gsurf-tab-tree.h"

G_BEGIN_DECLS

#define GSURF_TYPE_TABS_MODULE (gsurf_tabs_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfTabsModule, gsurf_tabs_module, GSURF, TABS_MODULE, GsurfModule)

/* Defaults (also documented in docs/tabs.org and data/default-config.yaml). */
#define GSURF_TABS_DEFAULT_BG_COLOR      "#181825"
#define GSURF_TABS_DEFAULT_ACTIVE_COLOR  "#313244"
#define GSURF_TABS_DEFAULT_FG_COLOR      "#cdd6f4"
/* Tabs shown in a pane other than the active one (a window layout such as
 * wmux): Catppuccin Mocha "base", between bg_color and active_color. */
#define GSURF_TABS_DEFAULT_VISIBLE_COLOR "#1e1e2e"
#define GSURF_TABS_DEFAULT_SIDEBAR_WIDTH (240)
#define GSURF_TABS_DEFAULT_INDENT        (14)
#define GSURF_TABS_DEFAULT_MAX_DEPTH     (8)
#define GSURF_TABS_FAVICON_SIZE          (16)

/* Widget names the tests (and CSS) can rely on. */
#define GSURF_TABS_STRIP_NAME        "gsurf-tab-strip"
#define GSURF_TABS_SIDEBAR_NAME      "gsurf-tab-sidebar"
#define GSURF_TABS_SIDEBAR_LIST_NAME "gsurf-tab-sidebar-list"
#define GSURF_TABS_ROW_TWISTY_NAME   "gsurf-tab-twisty"
#define GSURF_TABS_ROW_ICON_NAME     "gsurf-tab-icon"
#define GSURF_TABS_ROW_TITLE_NAME    "gsurf-tab-title"
#define GSURF_TABS_ROW_CLOSE_NAME    "gsurf-tab-close"
#define GSURF_TABS_ROW_STRIPE_NAME   "gsurf-tab-stripe"
#define GSURF_TABS_ROW_BOX_NAME      "gsurf-tab-row-box"

/* Object-data key holding a tab widget's view id (a guint64*). */
#define GSURF_TABS_ID_DATA "gsurf-tab-view-id"

/* Object-data key on the module holding its GtkCssProvider (borrowed)
 * while the UI is installed, so tests can read the stylesheet. */
#define GSURF_TABS_CSS_DATA "gsurf-tabs-css-provider"

/* Tab context menu. The action group is inserted as "tab" on the strip
 * and the sidebar; the module carries it (borrowed), the model of the
 * last menu opened (owned) and the open menu widget (borrowed, NULL when
 * none is shown) as object data for the tests. */
#define GSURF_TABS_MENU_PREFIX       "tab"
#define GSURF_TABS_MENU_ACTIONS_DATA "gsurf-tabs-menu-actions"
#define GSURF_TABS_MENU_MODEL_DATA   "gsurf-tabs-menu-model"
#define GSURF_TABS_MENU_WIDGET_DATA  "gsurf-tabs-menu-widget"
#define GSURF_TABS_MENU_NAME         "gsurf-tab-menu"

/* One sidebar row. The core never touches it; it lives in the UI. */
typedef struct _GsurfTabsRow GsurfTabsRow;

struct _GsurfTabsModule
{
	GsurfModule parent_instance;

	/* --- Configuration (configure()) --- */
	gchar   *key_new;
	gchar   *key_close;
	gchar   *key_next;
	gchar   *key_prev;
	gchar   *key_reopen;
	gchar   *key_sidebar;
	gchar   *key_collapse;
	gchar   *key_move_up;
	gchar   *key_move_down;
	gboolean position_top;        /* strip above (TRUE) or below the page */
	gboolean strip;               /* FALSE: never show the strip */
	gboolean show_single;         /* show the strip with one tab */
	gchar   *bg_color;            /* validated CSS colours */
	gchar   *active_color;
	gchar   *fg_color;
	gchar   *visible_color;       /* tabs on screen in another pane */
	gboolean sidebar;             /* initial sidebar state */
	gboolean sidebar_right;       /* sidebar_position: right */
	gint     sidebar_width;
	gboolean sidebar_hides_strip;
	gint     indent;              /* px per tree level */
	gint     max_depth;           /* indentation cap */
	GsurfTabTreeClosePolicy on_close_parent;

	/* --- Runtime state (activate() .. deactivate()) --- */
	GsurfWindow  *window;         /* weak ref: cleanup when it finalizes */
	GsurfTabTree *tree;
	gboolean      sidebar_shown;  /* toggled at runtime; starts at sidebar */
	gboolean      syncing;        /* we are reordering the window ourselves */
	GsurfContainerRegistry *registry; /* the default registry while connected */
	GQueue       *closed_uris;    /* recently closed URIs, most recent first */

	/* --- UI (gsurf-tabs-ui.c); all widgets retained until cleanup --- */
	GtkCssProvider *css;
	GtkWidget      *bar;          /* horizontal strip */
	GtkWidget      *sidebar_box;  /* the side panel */
	GtkWidget      *sidebar_list; /* vertical box of rows */
	GHashTable     *rows;         /* view id (guint64*) -> GsurfTabsRow* */
	GPtrArray      *row_order;    /* GsurfTabsRow* in the list's child order */
	GSimpleActionGroup *menu_actions; /* "tab.*" context menu actions */
	GtkWidget      *menu;         /* open context menu, or NULL */
	guint64         menu_tab;     /* the tab the context menu acts on */
};

/* --- Core, used by the UI (gsurf-tabs-module.c) --- */

/* Store and read a widget's view id. */
G_GNUC_INTERNAL void      gsurf_tabs_set_widget_id(GtkWidget *widget, guint64 id);
G_GNUC_INTERNAL guint64   gsurf_tabs_get_widget_id(GtkWidget *widget);

/* Activate, close, or collapse/expand the tab with @id. */
G_GNUC_INTERNAL void      gsurf_tabs_activate_tab(GsurfTabsModule *self, guint64 id);
G_GNUC_INTERNAL void      gsurf_tabs_close_tab(GsurfTabsModule *self, guint64 id);
G_GNUC_INTERNAL void      gsurf_tabs_toggle_collapse(GsurfTabsModule *self, guint64 id);

/* Context menu operations on the tab with @id (no-ops for a tab that is
 * gone). Reopening goes through gsurf_window_reopen_view(): @container
 * is a container id, "default" or "temporary". Splitting dispatches
 * @action (GSURF_ACTION_PANE_SPLIT_RIGHT / _DOWN) with the tab's view to
 * the module that owns the window layout. */
G_GNUC_INTERNAL void      gsurf_tabs_reload_tab(GsurfTabsModule *self, guint64 id);
G_GNUC_INTERNAL void      gsurf_tabs_duplicate_tab(GsurfTabsModule *self, guint64 id);
G_GNUC_INTERNAL void      gsurf_tabs_close_children(GsurfTabsModule *self, guint64 id);
G_GNUC_INTERNAL void      gsurf_tabs_reopen_tab(GsurfTabsModule *self, guint64 id,
                                                const gchar *container);
G_GNUC_INTERNAL void      gsurf_tabs_split_tab(GsurfTabsModule *self, guint64 id,
                                               GsurfAction action);

/* Whether the tab @view is on screen in a pane other than the active
 * one (only ever TRUE while a #GsurfWindowLayout is installed). */
G_GNUC_INTERNAL gboolean  gsurf_tabs_view_in_other_pane(GsurfTabsModule *self, GsurfView *view);

/* --- UI, used by the core (gsurf-tabs-ui.c) --- */

/* Build the strip and sidebar and pack them into the window chrome. */
G_GNUC_INTERNAL void      gsurf_tabs_ui_install(GsurfTabsModule *self);
/* Disconnect and detach every widget and the stylesheet; idempotent. */
G_GNUC_INTERNAL void      gsurf_tabs_ui_uninstall(GsurfTabsModule *self);
/* Bring the strip and every sidebar row in line with the model. */
G_GNUC_INTERNAL void      gsurf_tabs_ui_sync(GsurfTabsModule *self);
/* Refresh one tab's title, favicon and container decoration. */
G_GNUC_INTERNAL void      gsurf_tabs_ui_update_tab(GsurfTabsModule *self, guint64 id);
/* Container colours or names changed: restyle and retitle. */
G_GNUC_INTERNAL void      gsurf_tabs_ui_update_containers(GsurfTabsModule *self);
/* Show or hide the strip and sidebar per sidebar_shown and the config. */
G_GNUC_INTERNAL void      gsurf_tabs_ui_update_visibility(GsurfTabsModule *self);

G_END_DECLS

#endif /* GSURF_TABS_PRIVATE_H */
