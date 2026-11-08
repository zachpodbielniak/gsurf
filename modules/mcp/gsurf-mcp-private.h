/*
 * gsurf-mcp-private.h - Shared helpers of the mcp module's parts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The mcp module is built from:
 *   - gsurf-mcp-module.c: the GObject, configuration, the server life
 *     cycle, tool registration and the original page/navigation tools;
 *   - gsurf-mcp-views.c: views by stable id, the tab tree, containers,
 *     input modes and run_action (since 0.4.4);
 *   - gsurf-mcp-wmux.c: the window layout (wmux) tools: status, windows,
 *     panes, the command language and the convenience wrappers (since
 *     0.4.4).
 *
 * Every tool goes through public core API only (GsurfWindowLayout,
 * GsurfTabTreeProvider, GsurfContainerRegistry, GsurfModuleManager), so
 * the module never links against wmux, tabs or modal.
 */

#ifndef GSURF_MCP_PRIVATE_H
#define GSURF_MCP_PRIVATE_H

#include <gsurf/gsurf.h>
#include <json-glib/json-glib.h>
#include <mcp.h>

G_BEGIN_DECLS

/* The most arguments one tool takes. */
#define GSURF_MCP_TOOL_MAX_ARGS (10)

/**
 * GsurfMcpToolArg:
 * @name: the argument name; a %NULL name ends the list
 * @type: its JSON schema type: "string", "boolean", "number" or "integer"
 * @desc: what it means, for the calling model
 * @required: whether the schema lists it as required
 *
 * One argument of a tool's input schema.
 */
typedef struct {
	const gchar *name;
	const gchar *type;
	const gchar *desc;
	gboolean     required;
} GsurfMcpToolArg;

/**
 * GsurfMcpToolDef:
 * @name: the tool name, also its key under modules.mcp.tools
 * @description: what it does and returns, for the calling model
 * @args: its arguments
 * @handler: the implementation
 *
 * One tool; tables of them end with a %NULL @name.
 */
typedef struct {
	const gchar     *name;
	const gchar     *description;
	GsurfMcpToolArg  args[GSURF_MCP_TOOL_MAX_ARGS];
	McpToolHandler   handler;
} GsurfMcpToolDef;

/* The tool tables of the parts (NULL-name terminated). */
G_GNUC_INTERNAL extern const GsurfMcpToolDef gsurf_mcp_view_tools[];
G_GNUC_INTERNAL extern const GsurfMcpToolDef gsurf_mcp_wmux_tools[];

/* --- Results --- */

/* A plain text result. */
G_GNUC_INTERNAL McpToolResult *gsurf_mcp_text(const gchar *text);
/* An error result with a printf-style message. */
G_GNUC_INTERNAL McpToolResult *gsurf_mcp_error(const gchar *format, ...) G_GNUC_PRINTF(1, 2);
/* An error result from @error (or @fallback when it is NULL). */
G_GNUC_INTERNAL McpToolResult *gsurf_mcp_gerror(const GError *error, const gchar *fallback);
/* A text result holding @node serialized as JSON (the node is consumed). */
G_GNUC_INTERNAL McpToolResult *gsurf_mcp_json(JsonNode *node);

/* --- Arguments --- */

/* The string member @name of @args, or NULL (absent, null or not a string). */
G_GNUC_INTERNAL const gchar *gsurf_mcp_arg_string(JsonObject *args, const gchar *name);
/* Whether @args has a non-null member @name. */
G_GNUC_INTERNAL gboolean     gsurf_mcp_arg_has(JsonObject *args, const gchar *name);
/* The integer member @name: a JSON integer, an integral number or a string
 * of digits; FALSE with @error set when it is something else. */
G_GNUC_INTERNAL gboolean     gsurf_mcp_arg_int(JsonObject *args, const gchar *name,
                                               gint64 *out, GError **error);
/* The boolean member @name: a JSON boolean, or "true"/"false"/"1"/"0"; FALSE
 * with @error set when it is something else. @out keeps its value when the
 * member is absent. */
G_GNUC_INTERNAL gboolean     gsurf_mcp_arg_bool(JsonObject *args, const gchar *name,
                                                gboolean *out, GError **error);

/* --- Windows and views --- */

/* The active window, or NULL with @error set. */
G_GNUC_INTERNAL GsurfWindow *gsurf_mcp_window(GError **error);
/* The view with id @id in @window, or NULL with @error set. */
G_GNUC_INTERNAL GsurfView   *gsurf_mcp_view_by_id(GsurfWindow *window, gint64 id, GError **error);
/*
 * The view a tool addresses: argument @id_name (a stable view id) or,
 * when @index_name is non-NULL, argument @index_name (a position in the
 * window); with neither, the active view when @default_active, else an
 * error naming @id_name as missing.
 */
G_GNUC_INTERNAL GsurfView   *gsurf_mcp_resolve_view(GsurfWindow *window, JsonObject *args,
                                                    const gchar *id_name,
                                                    const gchar *index_name,
                                                    gboolean default_active,
                                                    GError **error);
/* The view of the active window that "view" (id) or "index" name, else the
 * active view; NULL with @error set. */
G_GNUC_INTERNAL GsurfView   *gsurf_mcp_target_view(JsonObject *args, GError **error);

/* The window's layout when it has one, else NULL with @error set ("no
 * window layout is installed; enable the wmux module"). */
G_GNUC_INTERNAL GsurfWindowLayout *gsurf_mcp_layout(GsurfWindow *window, GError **error);
/* The active module implementing GsurfTabTreeProvider, or NULL. */
G_GNUC_INTERNAL GsurfTabTreeProvider *gsurf_mcp_tree_provider(void);
/* The tree parent of @view: the provider's answer, or without one the
 * parent the opener tree gives (see docs/tabs.org, "Where the tree comes
 * from"). */
G_GNUC_INTERNAL guint64      gsurf_mcp_tree_parent(GsurfWindow *window, GsurfView *view);

/* Adds the members describing @view (at @index in @window) to the open
 * object of @b: id, index, uri, title, active, visible, opener_id,
 * parent_id, container (+ name, color, temporary). */
G_GNUC_INTERNAL void         gsurf_mcp_add_view_members(JsonBuilder *b, GsurfWindow *window,
                                                        GsurfView *view, guint index);

/* The group (wmux window) and pane index showing @view, when one does. */
G_GNUC_INTERNAL gboolean     gsurf_mcp_find_pane(GsurfWindow *window, GsurfView *view,
                                                 guint *group, guint *pane);

G_END_DECLS

#endif /* GSURF_MCP_PRIVATE_H */
