/*
 * gsurf-enums.h - GSURF Enumerations and Flags
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * All enums/flags used across the GSURF library. Those used in GObject
 * properties, signals, or introspection are registered as GTypes in
 * gsurf-enums.c (gsurf_*_get_type()).
 */

#ifndef GSURF_ENUMS_H
#define GSURF_ENUMS_H

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * GsurfBackendType:
 * @GSURF_BACKEND_GTK3_WEBKIT2: GTK3 + WebKit2GTK 4.1
 * @GSURF_BACKEND_GTK4_WEBKIT6: GTK4 + WebKitGTK 6.0
 * @GSURF_BACKEND_LRG: libregnum/raylib backend (no GTK windowing); the page is
 *   rendered into a GL texture composited by libregnum. The web engine is chosen
 *   at build time (offscreen WebKitGTK when a GTK backend is also linked, or WPE
 *   WebKit for a GTK-free build) — see #GsurfLrgRenderMode and gsurf-backend.h.
 *
 * The windowing/web-engine backend. Unlike the two GTK backends (which are
 * mutually exclusive at build time), %GSURF_BACKEND_LRG is *additive*: a single
 * libgsurf may contain a GTK backend and the LRG backend together and pick
 * between them at runtime (e.g. cmacs uses GTK for pgtk frames and LRG for
 * @samp{--lrg} frames).
 */
typedef enum {
	GSURF_BACKEND_GTK3_WEBKIT2,
	GSURF_BACKEND_GTK4_WEBKIT6,
	GSURF_BACKEND_LRG
} GsurfBackendType;

/**
 * GsurfLrgRenderMode:
 * @GSURF_LRG_RENDER_MODE_2D: flat 2D page (a textured quad); the default.
 * @GSURF_LRG_RENDER_MODE_3D: every tab as a panel in a 3D scene (builds with
 *   LRG_3D=1; docs/lrg-3d.org).
 * @GSURF_LRG_RENDER_MODE_3DVR: stereo 3D for VR headsets. Reserved.
 *
 * The render mode of the libregnum (%GSURF_BACKEND_LRG) backend, mirroring
 * cmacs's @code{LrgRenderMode}. Parsed from the @samp{--lrg[=MODE]} command line
 * by gsurf_lrg_render_mode_from_string().
 */
typedef enum {
	GSURF_LRG_RENDER_MODE_2D,
	GSURF_LRG_RENDER_MODE_3D,
	GSURF_LRG_RENDER_MODE_3DVR
} GsurfLrgRenderMode;

/**
 * GsurfKeyMod:
 * @GSURF_MOD_NONE: no modifier
 * @GSURF_MOD_SHIFT: Shift
 * @GSURF_MOD_CTRL: Control
 * @GSURF_MOD_ALT: Alt / Mod1
 * @GSURF_MOD_SUPER: Super / Logo
 *
 * Keyboard modifier flags (backend-agnostic; translated from GDK state).
 */
typedef enum /*< flags >*/ {
	GSURF_MOD_NONE  = 0,
	GSURF_MOD_SHIFT = 1 << 0,
	GSURF_MOD_CTRL  = 1 << 1,
	GSURF_MOD_ALT   = 1 << 2,
	GSURF_MOD_SUPER = 1 << 3
} GsurfKeyMod;

/**
 * GsurfMousebutton:
 * @GSURF_BUTTON_PRIMARY: left button
 * @GSURF_BUTTON_MIDDLE: middle button
 * @GSURF_BUTTON_SECONDARY: right button
 * @GSURF_BUTTON_BACK: thumb back (button 8)
 * @GSURF_BUTTON_FORWARD: thumb forward (button 9)
 *
 * Pointer buttons of interest.
 */
typedef enum {
	GSURF_BUTTON_PRIMARY   = 1,
	GSURF_BUTTON_MIDDLE    = 2,
	GSURF_BUTTON_SECONDARY = 3,
	GSURF_BUTTON_BACK      = 8,
	GSURF_BUTTON_FORWARD   = 9
} GsurfMousebutton;

/**
 * GsurfModePolicy:
 * @GSURF_MODE_NORMAL: vim-style normal mode (keys are commands)
 * @GSURF_MODE_INSERT: insert mode (keys pass to the page)
 * @GSURF_MODE_FOLLOW: link-hint follow mode
 * @GSURF_MODE_FIND: incremental find mode
 * @GSURF_MODE_PASSTHROUGH: forward all keys to the page (incl. Escape)
 *
 * Modal input states for the vim-style keymap.
 */
typedef enum {
	GSURF_MODE_NORMAL,
	GSURF_MODE_INSERT,
	GSURF_MODE_FOLLOW,
	GSURF_MODE_FIND,
	GSURF_MODE_PASSTHROUGH
} GsurfModePolicy;

/**
 * GsurfChromeEdge:
 * @GSURF_CHROME_EDGE_LEFT: beside the view area, on the left
 * @GSURF_CHROME_EDGE_RIGHT: beside the view area, on the right
 *
 * Where gsurf_window_add_side_widget() packs a side panel (e.g. the
 * tabs module's tree sidebar).
 */
typedef enum {
	GSURF_CHROME_EDGE_LEFT,
	GSURF_CHROME_EDGE_RIGHT
} GsurfChromeEdge;

/**
 * GsurfLayoutDropZone:
 * @GSURF_LAYOUT_DROP_CENTER: the middle of the target: show the view in
 *   the target's place
 * @GSURF_LAYOUT_DROP_LEFT: the left edge band: split the target, the
 *   view on the left
 * @GSURF_LAYOUT_DROP_RIGHT: the right edge band: split, the view on the
 *   right
 * @GSURF_LAYOUT_DROP_TOP: the top edge band: split, the view above
 * @GSURF_LAYOUT_DROP_BOTTOM: the bottom edge band: split, the view below
 *
 * Where a view dropped on another view's place should go
 * (gsurf_window_layout_show_view_at()).
 *
 * Since: 0.4.2
 */
typedef enum {
	GSURF_LAYOUT_DROP_CENTER,
	GSURF_LAYOUT_DROP_LEFT,
	GSURF_LAYOUT_DROP_RIGHT,
	GSURF_LAYOUT_DROP_TOP,
	GSURF_LAYOUT_DROP_BOTTOM
} GsurfLayoutDropZone;

/**
 * GsurfTabTreePosition:
 * @GSURF_TAB_TREE_POSITION_BEFORE: the moved tab becomes the target's
 *   previous sibling (same parent), right before it
 * @GSURF_TAB_TREE_POSITION_AFTER: the moved tab becomes the target's next
 *   sibling, after the target's whole subtree
 * @GSURF_TAB_TREE_POSITION_CHILD: the moved tab becomes the target's last
 *   child
 *
 * Where gsurf_tab_tree_provider_move_view() puts a tab (with its subtree)
 * relative to another tab; the same three places a sidebar drop offers.
 *
 * Since: 0.4.4
 */
typedef enum {
	GSURF_TAB_TREE_POSITION_BEFORE,
	GSURF_TAB_TREE_POSITION_AFTER,
	GSURF_TAB_TREE_POSITION_CHILD
} GsurfTabTreePosition;

/**
 * GsurfLoadEvent:
 * @GSURF_LOAD_STARTED: navigation started
 * @GSURF_LOAD_REDIRECTED: a redirect occurred
 * @GSURF_LOAD_COMMITTED: content began to be received
 * @GSURF_LOAD_FINISHED: load completed
 *
 * Mirrors WebKitLoadEvent; emitted by GsurfView::load-changed.
 */
typedef enum {
	GSURF_LOAD_STARTED,
	GSURF_LOAD_REDIRECTED,
	GSURF_LOAD_COMMITTED,
	GSURF_LOAD_FINISHED
} GsurfLoadEvent;

/**
 * GsurfPolicyDecision:
 * @GSURF_POLICY_USE: proceed with the navigation/resource
 * @GSURF_POLICY_IGNORE: block it
 * @GSURF_POLICY_DOWNLOAD: treat as a download
 *
 * Result of a navigation/response policy decision.
 */
typedef enum {
	GSURF_POLICY_USE,
	GSURF_POLICY_IGNORE,
	GSURF_POLICY_DOWNLOAD
} GsurfPolicyDecision;

/**
 * GsurfFilterVerdict:
 * @GSURF_FILTER_ALLOW: allow the request
 * @GSURF_FILTER_BLOCK: block the request
 * @GSURF_FILTER_REDIRECT: redirect to a different URI
 *
 * Result returned by #GsurfRequestFilter implementations (adblock).
 */
typedef enum {
	GSURF_FILTER_ALLOW,
	GSURF_FILTER_BLOCK,
	GSURF_FILTER_REDIRECT
} GsurfFilterVerdict;

/**
 * GsurfTlsDecision:
 * @GSURF_TLS_REJECT: reject the connection (default)
 * @GSURF_TLS_PROCEED: accept despite the certificate error
 * @GSURF_TLS_PROMPT: ask the user
 *
 * Result returned by #GsurfCertHandler implementations.
 */
typedef enum {
	GSURF_TLS_REJECT,
	GSURF_TLS_PROCEED,
	GSURF_TLS_PROMPT
} GsurfTlsDecision;

/**
 * GsurfPermissionVerdict:
 * @GSURF_PERMISSION_DENY: deny the request
 * @GSURF_PERMISSION_ALLOW: allow the request
 * @GSURF_PERMISSION_PROMPT: ask the user / pass through
 *
 * Result returned by #GsurfPermissionHandler implementations.
 */
typedef enum {
	GSURF_PERMISSION_DENY,
	GSURF_PERMISSION_ALLOW,
	GSURF_PERMISSION_PROMPT
} GsurfPermissionVerdict;

/**
 * GsurfModuleState:
 * @GSURF_MODULE_STATE_LOADED: loaded but not yet activated
 * @GSURF_MODULE_STATE_ACTIVE: activated
 * @GSURF_MODULE_STATE_INACTIVE: deactivated
 * @GSURF_MODULE_STATE_ERROR: failed to load/activate
 *
 * Lifecycle state of a loaded module.
 */
typedef enum {
	GSURF_MODULE_STATE_LOADED,
	GSURF_MODULE_STATE_ACTIVE,
	GSURF_MODULE_STATE_INACTIVE,
	GSURF_MODULE_STATE_ERROR
} GsurfModuleState;

/**
 * GsurfModulePriority:
 * @GSURF_PRIORITY_HIGHEST: runs first during hook dispatch
 * @GSURF_PRIORITY_HIGH: high priority
 * @GSURF_PRIORITY_DEFAULT: default priority
 * @GSURF_PRIORITY_LOW: low priority
 * @GSURF_PRIORITY_LOWEST: runs last (e.g. the built-in keymap handler)
 *
 * Hook-dispatch priority; lower numeric value runs first.
 */
typedef enum {
	GSURF_PRIORITY_HIGHEST = 0,
	GSURF_PRIORITY_HIGH    = 25,
	GSURF_PRIORITY_DEFAULT = 50,
	GSURF_PRIORITY_LOW     = 75,
	GSURF_PRIORITY_LOWEST  = 100
} GsurfModulePriority;

/**
 * GsurfHookPoint:
 *
 * Dispatch points at which the module manager invokes modules. Used for
 * explicit hook registration and introspection of what a module handles.
 */
typedef enum {
	/* Input */
	GSURF_HOOK_KEY_PRESS,
	GSURF_HOOK_KEY_RELEASE,
	GSURF_HOOK_BUTTON_PRESS,
	GSURF_HOOK_BUTTON_RELEASE,
	GSURF_HOOK_SCROLL,
	/* Navigation */
	GSURF_HOOK_URI_REWRITE,
	GSURF_HOOK_PRE_NAVIGATE,
	GSURF_HOOK_POST_NAVIGATE,
	GSURF_HOOK_DECIDE_POLICY,
	/* Load */
	GSURF_HOOK_LOAD_STARTED,
	GSURF_HOOK_LOAD_COMMITTED,
	GSURF_HOOK_LOAD_FINISHED,
	GSURF_HOOK_LOAD_FAILED,
	GSURF_HOOK_TLS_ERROR,
	/* Resource */
	GSURF_HOOK_RESOURCE_REQUEST,
	GSURF_HOOK_RESOURCE_RESPONSE,
	/* Content */
	GSURF_HOOK_INJECT_SCRIPT,
	GSURF_HOOK_INJECT_STYLE,
	GSURF_HOOK_CONTEXT_MENU,
	/* Download */
	GSURF_HOOK_DOWNLOAD_REQUESTED,
	GSURF_HOOK_DOWNLOAD_FINISHED,
	/* Permission */
	GSURF_HOOK_PERMISSION_REQUEST,
	/* UI */
	GSURF_HOOK_TITLE_CHANGE,
	GSURF_HOOK_STATUS_CHANGE,
	GSURF_HOOK_PROGRESS_CHANGE,
	GSURF_HOOK_FAVICON_CHANGE,
	GSURF_HOOK_RENDER_OVERLAY,
	/* Lifecycle */
	GSURF_HOOK_VIEW_CREATE,
	GSURF_HOOK_VIEW_DESTROY,
	GSURF_HOOK_WINDOW_CREATE,
	GSURF_HOOK_FULLSCREEN,
	GSURF_HOOK_WEB_PROCESS_CRASHED,
	/* sentinel */
	GSURF_HOOK_LAST
} GsurfHookPoint;

/**
 * GsurfAction:
 *
 * Browser actions that can be bound to keys/mouse buttons. Resolved from
 * config strings by gsurf_action_from_string().
 */
typedef enum {
	GSURF_ACTION_NONE = 0,
	/* Navigation */
	GSURF_ACTION_BACK,
	GSURF_ACTION_FORWARD,
	GSURF_ACTION_RELOAD,
	GSURF_ACTION_RELOAD_NOCACHE,
	GSURF_ACTION_STOP,
	GSURF_ACTION_HOME,
	GSURF_ACTION_OPEN_PROMPT,
	GSURF_ACTION_OPEN_NEW_VIEW,
	/* Scrolling */
	GSURF_ACTION_SCROLL_UP,
	GSURF_ACTION_SCROLL_DOWN,
	GSURF_ACTION_SCROLL_LEFT,
	GSURF_ACTION_SCROLL_RIGHT,
	GSURF_ACTION_SCROLL_TOP,
	GSURF_ACTION_SCROLL_BOTTOM,
	GSURF_ACTION_PAGE_UP,
	GSURF_ACTION_PAGE_DOWN,
	GSURF_ACTION_HALF_PAGE_UP,
	GSURF_ACTION_HALF_PAGE_DOWN,
	/* Zoom */
	GSURF_ACTION_ZOOM_IN,
	GSURF_ACTION_ZOOM_OUT,
	GSURF_ACTION_ZOOM_RESET,
	/* Find */
	GSURF_ACTION_FIND,
	GSURF_ACTION_FIND_NEXT,
	GSURF_ACTION_FIND_PREV,
	/* Clipboard */
	GSURF_ACTION_COPY_URL,
	GSURF_ACTION_PASTE_URL,
	/* Modal */
	GSURF_ACTION_ENTER_NORMAL_MODE,
	GSURF_ACTION_ENTER_INSERT_MODE,
	GSURF_ACTION_FOLLOW_HINTS,
	GSURF_ACTION_FOLLOW_HINTS_NEW_VIEW,
	/* Tabs/views */
	GSURF_ACTION_TAB_NEW,
	GSURF_ACTION_TAB_CLOSE,
	GSURF_ACTION_TAB_NEXT,
	GSURF_ACTION_TAB_PREV,
	GSURF_ACTION_TAB_REOPEN,
	/* Window */
	GSURF_ACTION_TOGGLE_FULLSCREEN,
	GSURF_ACTION_QUIT,
	/* Toggles (the setting toggled is carried in the keybind arg) */
	GSURF_ACTION_TOGGLE_SETTING,
	/* Module-defined action (dispatched by name in arg) */
	GSURF_ACTION_MODULE,
	/* Overlay listing currently active keybindings (core + modules) */
	GSURF_ACTION_SHOW_KEYBINDS,
	/* Tree sidebar (tabs module) */
	GSURF_ACTION_TAB_SIDEBAR_TOGGLE,
	GSURF_ACTION_TAB_TREE_COLLAPSE,
	GSURF_ACTION_TAB_MOVE_UP,
	GSURF_ACTION_TAB_MOVE_DOWN,
	/* Containers (containers module) */
	GSURF_ACTION_CONTAINER_NEW_TEMP,
	GSURF_ACTION_CONTAINER_REOPEN_NEXT,
	/* Dark mode (dark_mode module) */
	GSURF_ACTION_DARK_MODE_TOGGLE,
	GSURF_ACTION_DARK_MODE_TOGGLE_SITE,
	/* Form autofill (form_autofill module) */
	GSURF_ACTION_FORM_AUTOFILL,
	/* Panes (a GsurfWindowLayout module such as wmux): split the pane
	 * showing the view (or the active pane) */
	GSURF_ACTION_PANE_SPLIT_RIGHT,
	GSURF_ACTION_PANE_SPLIT_DOWN,
	/* Open the hovered link in a new tab, a child of the current one,
	 * without leaving the current one (since 0.4.2) */
	GSURF_ACTION_OPEN_NEW_VIEW_BACKGROUND,
	/* The --lrg=3d tab space (since 0.5.1); no-ops in other windows */
	GSURF_ACTION_LRG_CYCLE_ARRANGEMENT,
	GSURF_ACTION_LRG_CYCLE_ENVIRONMENT,
	GSURF_ACTION_LRG_READING,
	GSURF_ACTION_LRG_HISTORY,
	GSURF_ACTION_LRG_FOCUS,
	GSURF_ACTION_LRG_PIN,
	GSURF_ACTION_LRG_UNPIN_ALL,
	GSURF_ACTION_LRG_CAMERA_RESET,
	GSURF_ACTION_LRG_OVERVIEW,
	GSURF_ACTION_LRG_ORBIT_LEFT,
	GSURF_ACTION_LRG_ORBIT_RIGHT,
	GSURF_ACTION_LRG_ORBIT_UP,
	GSURF_ACTION_LRG_ORBIT_DOWN,
	GSURF_ACTION_LRG_ZOOM_IN,
	GSURF_ACTION_LRG_ZOOM_OUT,
	GSURF_ACTION_LAST
} GsurfAction;

/* GType registration for introspection / signals / properties */
GType gsurf_backend_type_get_type(void) G_GNUC_CONST;
GType gsurf_lrg_render_mode_get_type(void) G_GNUC_CONST;
GType gsurf_key_mod_get_type(void) G_GNUC_CONST;
GType gsurf_mode_policy_get_type(void) G_GNUC_CONST;
GType gsurf_load_event_get_type(void) G_GNUC_CONST;
GType gsurf_chrome_edge_get_type(void) G_GNUC_CONST;
GType gsurf_layout_drop_zone_get_type(void) G_GNUC_CONST;
GType gsurf_tab_tree_position_get_type(void) G_GNUC_CONST;
GType gsurf_policy_decision_get_type(void) G_GNUC_CONST;
GType gsurf_filter_verdict_get_type(void) G_GNUC_CONST;
GType gsurf_tls_decision_get_type(void) G_GNUC_CONST;
GType gsurf_permission_verdict_get_type(void) G_GNUC_CONST;
GType gsurf_module_state_get_type(void) G_GNUC_CONST;
GType gsurf_hook_point_get_type(void) G_GNUC_CONST;
GType gsurf_action_get_type(void) G_GNUC_CONST;

#define GSURF_TYPE_BACKEND_TYPE       (gsurf_backend_type_get_type())
#define GSURF_TYPE_LRG_RENDER_MODE    (gsurf_lrg_render_mode_get_type())
#define GSURF_TYPE_KEY_MOD            (gsurf_key_mod_get_type())
#define GSURF_TYPE_MODE_POLICY        (gsurf_mode_policy_get_type())
#define GSURF_TYPE_LOAD_EVENT         (gsurf_load_event_get_type())
#define GSURF_TYPE_CHROME_EDGE        (gsurf_chrome_edge_get_type())
#define GSURF_TYPE_LAYOUT_DROP_ZONE   (gsurf_layout_drop_zone_get_type())
#define GSURF_TYPE_TAB_TREE_POSITION  (gsurf_tab_tree_position_get_type())
#define GSURF_TYPE_POLICY_DECISION    (gsurf_policy_decision_get_type())
#define GSURF_TYPE_FILTER_VERDICT     (gsurf_filter_verdict_get_type())
#define GSURF_TYPE_TLS_DECISION       (gsurf_tls_decision_get_type())
#define GSURF_TYPE_PERMISSION_VERDICT (gsurf_permission_verdict_get_type())
#define GSURF_TYPE_MODULE_STATE       (gsurf_module_state_get_type())
#define GSURF_TYPE_HOOK_POINT         (gsurf_hook_point_get_type())
#define GSURF_TYPE_ACTION             (gsurf_action_get_type())

/**
 * gsurf_mode_policy_get_label:
 * @mode: a #GsurfModePolicy
 *
 * The mode's name as a mode indicator shows it: "NORMAL", "INSERT",
 * "FOLLOW", "FIND" or "PASSTHROUGH".
 *
 * Returns: (transfer none) (nullable): the label, or %NULL for an
 *   unknown value
 *
 * Since: 0.4.2
 */
const gchar *gsurf_mode_policy_get_label(GsurfModePolicy mode);

/**
 * gsurf_action_from_string:
 * @str: an action name (e.g. "reload", "scroll-down"; underscores accepted)
 *
 * Resolves a configuration action string to a #GsurfAction. Accepts both
 * '-' and '_' separators.
 *
 * Returns: the matching #GsurfAction, or %GSURF_ACTION_NONE if unknown
 */
GsurfAction gsurf_action_from_string(const gchar *str);

/**
 * gsurf_action_to_string:
 * @action: a #GsurfAction
 *
 * Returns: (transfer none): the canonical action nick, or %NULL
 */
const gchar *gsurf_action_to_string(GsurfAction action);

/**
 * gsurf_action_get_description:
 * @action: a #GsurfAction
 *
 * Returns: (transfer none): a short human-readable description, or %NULL
 */
const gchar *gsurf_action_get_description(GsurfAction action);

/**
 * gsurf_lrg_render_mode_from_string:
 * @str: a render-mode name (@samp{2d}, @samp{3d}, @samp{3dvr}; case-insensitive)
 * @out_mode: (out): location for the parsed #GsurfLrgRenderMode
 *
 * Parses the @samp{--lrg[=MODE]} argument. A %NULL or empty @str selects
 * %GSURF_LRG_RENDER_MODE_2D (the default), matching @samp{emacs --lrg}. cmacs
 * accepts a @samp{MODE:ARRANGEMENT:ENVIRONMENT} form for 3D; only the leading
 * @samp{MODE} token is interpreted here and the rest is ignored.
 *
 * Returns: %TRUE if @str named a known mode (so @out_mode is set); %FALSE for an
 *   unrecognised mode (@out_mode is left at %GSURF_LRG_RENDER_MODE_2D).
 */
gboolean gsurf_lrg_render_mode_from_string(const gchar *str,
                                           GsurfLrgRenderMode *out_mode);

/**
 * gsurf_lrg_render_mode_to_string:
 * @mode: a #GsurfLrgRenderMode
 *
 * Returns: (transfer none): the canonical mode nick (@samp{2d}/@samp{3d}/@samp{3dvr}),
 *   or %NULL
 */
const gchar *gsurf_lrg_render_mode_to_string(GsurfLrgRenderMode mode);

/**
 * gsurf_lrg_render_mode_is_implemented:
 * @mode: a #GsurfLrgRenderMode
 *
 * Returns: %TRUE if @mode is implemented in this build: 2D always, 3D in
 *   builds with the libregnum scene (LRG_3D=1).  3D-VR is reserved.
 */
gboolean gsurf_lrg_render_mode_is_implemented(GsurfLrgRenderMode mode);

/**
 * gsurf_lrg_spec_parse:
 * @spec: (nullable): the @samp{--lrg} argument, @samp{MODE[:A][:B]}
 * @out_mode: (out): the render mode
 * @out_arrangement: (out) (transfer full) (nullable): the arrangement id, or
 *   %NULL when the spec names none
 * @out_environment: (out) (transfer full) (nullable): the environment id, or
 *   %NULL when the spec names none
 *
 * Parses the full cmacs-style spec: after MODE, each @samp{:}- (or
 * @samp{,}-) separated token is an environment if it names a built-in
 * environment (void, workshop, cockpit, sky, studio) and an arrangement
 * otherwise, so @samp{3d:grid:sky}, @samp{3d:sky} and @samp{3d:sky:grid} all
 * work.  Empty tokens are skipped.  Arrangement ids are not validated here
 * (the scene falls back to its default for an unknown one).
 *
 * Returns: %TRUE if MODE is known (see gsurf_lrg_render_mode_from_string())
 */
gboolean gsurf_lrg_spec_parse(const gchar *spec, GsurfLrgRenderMode *out_mode,
                              gchar **out_arrangement, gchar **out_environment);

G_END_DECLS

#endif /* GSURF_ENUMS_H */
