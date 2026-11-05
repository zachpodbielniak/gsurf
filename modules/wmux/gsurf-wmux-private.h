/*
 * gsurf-wmux-private.h - Shared state of the wmux module's parts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The wmux module is built from:
 *   - the toolkit-neutral core (gsurf-wmux-{layout,command,keys,bindings}),
 *     unit-tested on its own;
 *   - gsurf-wmux-module.c: the GObject, configuration, key handling, the
 *     command interpreter, actions, keybind help and the module life
 *     cycle;
 *   - gsurf-wmux-session.c: the tmux windows of the browser window (their
 *     layouts, pane -> view mapping, names, current and last window), the
 *     operations on them (split, kill, select, break, ...) and the
 *     #GsurfWindowLayout implementation (attach, add/remove/activate a
 *     view, visibility, serialize/restore);
 *   - gsurf-wmux-ui.c: the widgets (the root, pane frames in the grid, the
 *     status line with its prompt, the chooser, the pane numbers) and the
 *     stylesheet, on GTK3 and GTK4 through the chrome compat header;
 *   - gsurf-wmux-grid-gtk3.c / -gtk4.c: the pane grid container.
 *
 * This header is the contract between them.
 */

#ifndef GSURF_WMUX_PRIVATE_H
#define GSURF_WMUX_PRIVATE_H

/* <gsurf.h> resolves for the module (-Isrc) and for tests that read
 * the module struct (tests/test-wmux.c). */
#include <gsurf.h>
#include <gtk/gtk.h>

#include "gsurf-wmux-bindings.h"
#include "gsurf-wmux-command.h"
#include "gsurf-wmux-grid.h"
#include "gsurf-wmux-keys.h"
#include "gsurf-wmux-layout.h"

G_BEGIN_DECLS

#define GSURF_TYPE_WMUX_MODULE (gsurf_wmux_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfWmuxModule, gsurf_wmux_module, GSURF, WMUX_MODULE, GsurfModule)

/* Defaults (docs/wmux.org "Configuration reference" and the commented
 * block in data/default-config.yaml list the same values). */
#define GSURF_WMUX_DEFAULT_PREFIX_TIMEOUT      (0)
#define GSURF_WMUX_DEFAULT_REPEAT_TIME         (500)
#define GSURF_WMUX_DEFAULT_DISPLAY_PANES_TIME  (1000)
#define GSURF_WMUX_DEFAULT_SPLIT_RATIO         (50)
#define GSURF_WMUX_DEFAULT_MIN_PANE_PCT        (10)
#define GSURF_WMUX_DEFAULT_GAP                 (2)
#define GSURF_WMUX_DEFAULT_BORDER_WIDTH        (1)
#define GSURF_WMUX_DEFAULT_ACTIVE_BORDER_WIDTH (2)
#define GSURF_WMUX_DEFAULT_BORDER_COLOR        "#45475a"
#define GSURF_WMUX_DEFAULT_ACTIVE_BORDER_COLOR "#89b4fa"
#define GSURF_WMUX_DEFAULT_INACTIVE_OPACITY    (0.55)
#define GSURF_WMUX_DEFAULT_ACTIVE_BRIGHTEN     (0.25)
#define GSURF_WMUX_DEFAULT_STATUS_BG           "#181825"
#define GSURF_WMUX_DEFAULT_STATUS_FG           "#cdd6f4"
#define GSURF_WMUX_DEFAULT_STATUS_CURRENT_FG   "#89b4fa"
#define GSURF_WMUX_DEFAULT_WINDOW_NAME_MAX     (20)

/* The session name the status line shows ("[gsurf]"). */
#define GSURF_WMUX_SESSION_NAME "gsurf"

/* How long a status-line message (a command error) stays, in ms. */
#define GSURF_WMUX_MESSAGE_TIME (2500)

/* Widget names the tests (and user CSS) can rely on. */
#define GSURF_WMUX_ROOT_NAME           "gsurf-wmux-root"
#define GSURF_WMUX_GRID_NAME           "gsurf-wmux-grid"
#define GSURF_WMUX_STATUS_NAME         "gsurf-wmux-status"
#define GSURF_WMUX_STATUS_WINDOWS_NAME "gsurf-wmux-status-windows"
#define GSURF_WMUX_STATUS_RIGHT_NAME   "gsurf-wmux-status-right"
#define GSURF_WMUX_MESSAGE_NAME        "gsurf-wmux-message"
#define GSURF_WMUX_PROMPT_NAME         "gsurf-wmux-prompt"
#define GSURF_WMUX_PROMPT_LABEL_NAME   "gsurf-wmux-prompt-label"
#define GSURF_WMUX_PROMPT_ENTRY_NAME   "gsurf-wmux-prompt-entry"
#define GSURF_WMUX_CHOOSER_NAME        "gsurf-wmux-chooser"
#define GSURF_WMUX_PANE_NUMBER_NAME    "gsurf-wmux-pane-number"
#define GSURF_WMUX_DROP_NAME           "gsurf-wmux-drop"
#define GSURF_WMUX_DROP_ZONE_NAME      "gsurf-wmux-drop-zone"

/* CSS classes. */
#define GSURF_WMUX_PANE_CLASS           "wmux-pane"
#define GSURF_WMUX_PANE_ACTIVE_CLASS    "wmux-pane-active"
#define GSURF_WMUX_CTR_CLASS_PREFIX     "wmux-ctr-"
#define GSURF_WMUX_STATUS_CLASS         "wmux-status"
#define GSURF_WMUX_STATUS_WINDOW_CLASS  "wmux-status-window"
#define GSURF_WMUX_STATUS_CURRENT_CLASS "wmux-status-current"
#define GSURF_WMUX_CHOOSER_ROW_CLASS    "wmux-chooser-row"
#define GSURF_WMUX_CHOOSER_SEL_CLASS    "wmux-chooser-selected"
#define GSURF_WMUX_DROP_CLASS           "wmux-drop"
#define GSURF_WMUX_DROP_ZONE_CLASS      "wmux-drop-zone"

/* Object data: on the module, its GtkCssProvider (borrowed) while the UI
 * exists; on a pane frame, the id of the view it shows (a guint64*); on a
 * status-line window button and a chooser row, the window index / item
 * index plus one (GUINT_TO_POINTER). */
#define GSURF_WMUX_CSS_DATA     "gsurf-wmux-css-provider"
#define GSURF_WMUX_VIEW_ID_DATA "gsurf-wmux-view-id"
#define GSURF_WMUX_INDEX_DATA   "gsurf-wmux-index"

/**
 * GsurfWmuxWin:
 * @name: the name given by rename-window or new-window -n ("" for none)
 * @renamed: whether @name is used (automatic naming is off)
 * @layout: the panes; each pane's payload is the id of the view it
 *   shows (0 for an empty pane)
 *
 * One tmux window of the browser window.
 */
typedef struct {
	gchar           *name;
	gboolean         renamed;
	GsurfWmuxLayout *layout;
} GsurfWmuxWin;

/**
 * GsurfWmuxFrame:
 * @id: the view shown
 * @frame: the GtkBox carrying the border (owned reference)
 * @overlay: the GtkOverlay holding the web view and @number
 * @number: the display-panes number label
 * @webview: the view's widget while it is parented here (a reference)
 * @drop: the drop target over the web view, shown only while a tab is
 *   dragged (so the page keeps every other pointer event)
 * @zone: the highlight inside @drop: where a drop would put the tab
 * @zone_shown: the zone highlighted (a #GsurfWmuxDrop), or -1
 *
 * The widgets around one visible view.
 */
typedef struct {
	guint64    id;
	GtkWidget *frame;
	GtkWidget *overlay;
	GtkWidget *number;
	GtkWidget *webview;
	GtkWidget *drop;
	GtkWidget *zone;
	gint       zone_shown;
} GsurfWmuxFrame;

/* What the status-line entry is asking for. */
typedef enum {
	GSURF_WMUX_PROMPT_NONE,
	GSURF_WMUX_PROMPT_COMMAND, /* command-prompt: Enter runs */
	GSURF_WMUX_PROMPT_CONFIRM  /* confirm-before: y runs, n or Escape cancels */
} GsurfWmuxPromptKind;

/* One line of the chooser: a window (pane NONE) or a pane of it. */
typedef struct {
	guint win;
	guint pane;
} GsurfWmuxChooserItem;

struct _GsurfWmuxModule
{
	GsurfModule parent_instance;

	/* --- Configuration (configure()) --- */
	gchar             *prefix;            /* canonical key name */
	gint               prefix_timeout;    /* ms, 0 = none */
	gint               repeat_time;       /* ms */
	gint               display_panes_time;/* ms */
	GsurfWmuxBindings *prefix_table;
	GsurfWmuxBindings *root_table;
	gint               split_ratio;       /* % for a new pane */
	gboolean           keep_tab;          /* on_pane_kill: keep_tab */
	gboolean           keep_pane;         /* on_tab_close: keep_pane */
	gboolean           confirm_kill;
	gboolean           mouse;
	gboolean           remember_layout;
	gint               min_pane_pct;
	gint               gap;
	gint               border_width;
	gint               active_border_width;
	gchar             *border_color;      /* normalized CSS colours */
	gchar             *active_border_color;
	gboolean           container_colors;
	gdouble            inactive_opacity;
	gdouble            active_brighten;
	gboolean           status;
	gboolean           status_top;        /* status_position: top */
	gchar             *status_bg;
	gchar             *status_fg;
	gchar             *status_current_fg;
	gint               window_name_max;

	/* --- Runtime (activate() .. deactivate()) --- */
	GsurfWindow       *window;            /* weak: cleanup when it finalizes */
	gboolean           installed;         /* we are the window's layout */
	gboolean           attached;          /* between attach and detach */
	gboolean           refusal_logged;    /* kiosk refusal logged */
	GPtrArray         *wins;              /* GsurfWmuxWin*, the tmux windows */
	guint              current;           /* index of the current window */
	gint               last_win;          /* the previous one, or -1 */
	GsurfContainerRegistry *registry;     /* while connected */
	gint               in_activate;       /* activate_view nesting depth */
	guint64            removing;          /* the view remove_view handles */
	gchar             *visible_sig;       /* the visible views at the last sync */
	gchar             *groups_sig;        /* the groups at the last notification */
	guint64            drag_view;         /* the tab being dragged in, or 0 */

	/* Keys. */
	gboolean           prefix_active;     /* the next key is a prefix command */
	guint              prefix_timeout_id;
	gint64             repeat_until;      /* monotonic us; repeat window end */
	gboolean           send_prefix;       /* the next prefix press goes through */

	/* Border drag (grid). */
	gboolean           dragging;
	GsurfWmuxBorder    drag;

	/* --- UI (gsurf-wmux-ui.c) --- */
	GtkWidget         *root;              /* owned (ref_sink) */
	GtkWidget         *overlay;           /* grid + chooser */
	GtkWidget         *grid;
	GtkWidget         *status_box;
	GtkWidget         *status_windows;    /* box of window buttons */
	GtkWidget         *status_right;      /* "2/3 Z" */
	GtkWidget         *message;           /* a command error */
	GtkWidget         *prompt_box;
	GtkWidget         *prompt_label;
	GtkWidget         *prompt_entry;
	GtkWidget         *chooser;           /* the choose-tree panel */
	GtkWidget         *chooser_list;
	GHashTable        *frames;            /* guint64* view id -> GsurfWmuxFrame* */
	GtkCssProvider    *css;
	GsurfWmuxPromptKind prompt_kind;
	gchar             *prompt_template;   /* command-prompt template, or NULL */
	gchar             *prompt_command;    /* confirm-before's command */
	guint              message_id;        /* message timeout */
	gboolean           displaying;        /* display-panes numbers shown */
	guint              display_id;
	gboolean           chooser_open;
	GArray            *chooser_items;     /* GsurfWmuxChooserItem */
	guint              chooser_sel;
};

/* --- Module (gsurf-wmux-module.c) --- */

/* Parse and run one command line; errors go to the status line (and the
 * log). Returns whether it parsed. */
G_GNUC_INTERNAL gboolean gsurf_wmux_run_line(GsurfWmuxModule *self, const gchar *line);
/* Run a parsed command. */
G_GNUC_INTERNAL void     gsurf_wmux_run(GsurfWmuxModule *self, const GsurfWmuxCommand *cmd);
/* Set or clear the window's key grab for a pending key (prefix, chooser,
 * display-panes). */
G_GNUC_INTERNAL void     gsurf_wmux_update_grab(GsurfWmuxModule *self);

/* --- Session (gsurf-wmux-session.c) --- */

G_GNUC_INTERNAL void          gsurf_wmux_layout_iface_init(GsurfWindowLayoutInterface *iface);
G_GNUC_INTERNAL GsurfWmuxWin *gsurf_wmux_win_new(GsurfWmuxModule *self, const gchar *name);
G_GNUC_INTERNAL void          gsurf_wmux_win_free(GsurfWmuxWin *win);
/* The current tmux window (NULL when not attached). */
G_GNUC_INTERNAL GsurfWmuxWin *gsurf_wmux_current(GsurfWmuxModule *self);
/* The live view a pane of @win shows, or NULL. */
G_GNUC_INTERNAL GsurfView    *gsurf_wmux_pane_view(GsurfWmuxModule *self, GsurfWmuxWin *win,
                                                   guint pane);
/* Where view @id is shown: its window index and pane. */
G_GNUC_INTERNAL gboolean      gsurf_wmux_find_view(GsurfWmuxModule *self, guint64 id,
                                                   guint *win, guint *pane);
/* The status-line name of window @index (truncated to window_name_max). */
G_GNUC_INTERNAL gchar        *gsurf_wmux_win_display_name(GsurfWmuxModule *self, guint index);
/* Whether @view is on screen (in a pane of the current window, not hidden
 * by a zoom). */
G_GNUC_INTERNAL gboolean      gsurf_wmux_view_visible(GsurfWmuxModule *self, GsurfView *view);

/* Operations; all keep the window's active view and the widgets in step. */
G_GNUC_INTERNAL void     gsurf_wmux_select_pane(GsurfWmuxModule *self, guint pane);
G_GNUC_INTERNAL void     gsurf_wmux_select_window(GsurfWmuxModule *self, guint index);
G_GNUC_INTERNAL gboolean gsurf_wmux_split(GsurfWmuxModule *self, guint pane, GsurfWmuxSplit split,
                                          gboolean before, gboolean keep_active, guint pct,
                                          gboolean duplicate, const gchar *uri,
                                          GsurfView *existing);
G_GNUC_INTERNAL void     gsurf_wmux_kill_pane(GsurfWmuxModule *self, guint win, guint pane);
G_GNUC_INTERNAL void     gsurf_wmux_kill_window(GsurfWmuxModule *self, guint index);
G_GNUC_INTERNAL void     gsurf_wmux_new_window(GsurfWmuxModule *self, const gchar *name,
                                               const gchar *uri);
G_GNUC_INTERNAL void     gsurf_wmux_break_pane(GsurfWmuxModule *self);
G_GNUC_INTERNAL void     gsurf_wmux_rename_window(GsurfWmuxModule *self, const gchar *name);
/* Show @view (a view of the window) in place of (CENTER) or beside @target
 * (a view on screen): see wmux_show_view_at() in gsurf-wmux-session.c. */
G_GNUC_INTERNAL gboolean gsurf_wmux_show_view_at(GsurfWmuxModule *self, GsurfView *view,
                                                 GsurfView *target, GsurfWmuxDrop zone);
/* Tell the window when the groups (the tmux windows: names, current,
 * the views their panes show) changed since the last call. */
G_GNUC_INTERNAL void     gsurf_wmux_notify_groups(GsurfWmuxModule *self);
/* After a model change in the current window: make the active pane's view
 * the window's active view and focus it, and bring the widgets in line. */
G_GNUC_INTERNAL void     gsurf_wmux_show_active(GsurfWmuxModule *self);
/* Drop every view handler and the window state (detach, deactivate). */
G_GNUC_INTERNAL void     gsurf_wmux_session_clear(GsurfWmuxModule *self);

/* --- UI (gsurf-wmux-ui.c) --- */

G_GNUC_INTERNAL void      gsurf_wmux_ui_build(GsurfWmuxModule *self);
G_GNUC_INTERNAL void      gsurf_wmux_ui_free(GsurfWmuxModule *self);
/* Bring the grid, frames, classes, status line and visibility in line
 * with the model; notifies the window when the visible views changed. */
G_GNUC_INTERNAL void      gsurf_wmux_ui_sync(GsurfWmuxModule *self);
/* Drop the frame of view @id (its web view unparented, alive). */
G_GNUC_INTERNAL void      gsurf_wmux_ui_drop_frame(GsurfWmuxModule *self, guint64 id);
/* Unparent every web view and drop every frame. */
G_GNUC_INTERNAL void      gsurf_wmux_ui_clear_frames(GsurfWmuxModule *self);
G_GNUC_INTERNAL void      gsurf_wmux_ui_update_status(GsurfWmuxModule *self);
G_GNUC_INTERNAL void      gsurf_wmux_ui_update_css(GsurfWmuxModule *self);
/* The status line as plain text: "[gsurf] 0:a* 1:b-" + "  " + "2/3 Z". */
G_GNUC_INTERNAL gchar    *gsurf_wmux_ui_status_text(GsurfWmuxModule *self);
G_GNUC_INTERNAL void      gsurf_wmux_ui_message(GsurfWmuxModule *self, const gchar *text);
G_GNUC_INTERNAL void      gsurf_wmux_ui_prompt(GsurfWmuxModule *self, GsurfWmuxPromptKind kind,
                                               const gchar *prompt, const gchar *initial);
G_GNUC_INTERNAL void      gsurf_wmux_ui_prompt_close(GsurfWmuxModule *self);
G_GNUC_INTERNAL void      gsurf_wmux_ui_display_panes(GsurfWmuxModule *self, gboolean show);
/* A tab drag from chrome began (@view) or ended (NULL): show or hide the
 * drop targets over the panes. */
G_GNUC_INTERNAL void      gsurf_wmux_ui_set_drag(GsurfWmuxModule *self, GsurfView *view);
G_GNUC_INTERNAL void      gsurf_wmux_ui_chooser_open(GsurfWmuxModule *self);
G_GNUC_INTERNAL void      gsurf_wmux_ui_chooser_close(GsurfWmuxModule *self);
G_GNUC_INTERNAL void      gsurf_wmux_ui_chooser_refresh(GsurfWmuxModule *self);
/* Move the chooser selection by @delta, or act on it. */
G_GNUC_INTERNAL void      gsurf_wmux_ui_chooser_move(GsurfWmuxModule *self, gint delta);
G_GNUC_INTERNAL void      gsurf_wmux_ui_chooser_choose(GsurfWmuxModule *self);
G_GNUC_INTERNAL void      gsurf_wmux_ui_chooser_kill(GsurfWmuxModule *self);
/* The frame showing view @id, or NULL. */
G_GNUC_INTERNAL GsurfWmuxFrame *gsurf_wmux_ui_frame(GsurfWmuxModule *self, guint64 id);
/* @color mixed toward white by @amount (0..1), as a CSS colour. */
G_GNUC_INTERNAL gchar    *gsurf_wmux_brighten(const gchar *color, gdouble amount);
/* @color with its alpha multiplied by @opacity, as a CSS colour. */
G_GNUC_INTERNAL gchar    *gsurf_wmux_with_alpha(const gchar *color, gdouble opacity);

G_END_DECLS

#endif /* GSURF_WMUX_PRIVATE_H */
