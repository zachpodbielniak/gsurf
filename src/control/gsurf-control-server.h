/*
 * gsurf-control-server.h - The control socket: JSON-RPC 2.0 over a Unix socket
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #GsurfControlServer listens on `<runtime dir>/control.sock` (the
 * process's private `$XDG_RUNTIME_DIR/gsurf/<pid>-XXXXXX`, see
 * gsurf-runtime-dir.h) and answers newline-delimited JSON-RPC 2.0
 * requests: one JSON value per line, a request object or a batch array.
 * The socket is mode 0600 inside the 0700 directory, and a connection
 * whose peer runs as another user is closed at once.
 *
 * Methods live in a registry (gsurf_control_server_add_method()). The
 * server registers the core methods (instance.info, open, views.*,
 * page.*, tree.*, layout.*, ...; docs/control.org lists them) itself;
 * modules add their own through #GsurfControlProvider. A handler answers
 * through its #GsurfControlInvocation, at once or later (page.eval waits
 * for the page), exactly once.
 *
 * `events.subscribe` turns a connection into an event stream as well:
 * every gsurf_control_server_emit_event() whose topic it asked for
 * arrives as a JSON-RPC notification line
 * `{"jsonrpc":"2.0","method":"event","params":{"topic":..,"event":..,
 * "time":..,"data":..}}`. The server emits the core topics itself
 * (view, uri, title, load, mode, layout, download).
 *
 * The library never opens a socket on its own: the gsurf binary starts
 * a server, and an embedder does so with gsurf_control_server_new() and
 * gsurf_control_server_start() when it wants one.
 *
 * Since: 0.5
 */

#ifndef GSURF_CONTROL_SERVER_H
#define GSURF_CONTROL_SERVER_H

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "core/gsurf-application.h"

G_BEGIN_DECLS

/**
 * GSURF_CONTROL_SOCKET_NAME:
 *
 * The control socket's file name inside the process runtime directory.
 *
 * Since: 0.5
 */
#define GSURF_CONTROL_SOCKET_NAME "control.sock"

/**
 * GSURF_CONTROL_ERROR:
 *
 * The error domain of #GsurfControlError.
 *
 * Since: 0.5
 */
#define GSURF_CONTROL_ERROR (gsurf_control_error_quark())

/**
 * GsurfControlError:
 * @GSURF_CONTROL_ERROR_FAILED: the method failed (JSON-RPC code -32000)
 * @GSURF_CONTROL_ERROR_INVALID_PARAMS: a parameter is missing or of the
 *   wrong type (-32602)
 * @GSURF_CONTROL_ERROR_NOT_FOUND: the view, pane, window, container,
 *   module or key does not exist (-32001)
 * @GSURF_CONTROL_ERROR_UNSUPPORTED: the method needs something this
 *   instance lacks: no window layout (wmux), no tab tree, no modal module
 *   (-32002)
 * @GSURF_CONTROL_ERROR_PERMISSION_DENIED: kiosk mode refuses it (-32003)
 * @GSURF_CONTROL_ERROR_METHOD_NOT_FOUND: no such method (-32601)
 * @GSURF_CONTROL_ERROR_PARSE: the line is not JSON (-32700)
 * @GSURF_CONTROL_ERROR_INVALID_REQUEST: the JSON is not a request (-32600)
 * @GSURF_CONTROL_ERROR_TIMEOUT: no reply in time (client side)
 * @GSURF_CONTROL_ERROR_CLOSED: the connection closed (client side)
 *
 * Errors of the control protocol. Each maps to a JSON-RPC error code
 * (gsurf_control_error_to_code()); a client turns a code back into the
 * matching value (gsurf_control_error_from_code()).
 *
 * Since: 0.5
 */
typedef enum {
	GSURF_CONTROL_ERROR_FAILED,
	GSURF_CONTROL_ERROR_INVALID_PARAMS,
	GSURF_CONTROL_ERROR_NOT_FOUND,
	GSURF_CONTROL_ERROR_UNSUPPORTED,
	GSURF_CONTROL_ERROR_PERMISSION_DENIED,
	GSURF_CONTROL_ERROR_METHOD_NOT_FOUND,
	GSURF_CONTROL_ERROR_PARSE,
	GSURF_CONTROL_ERROR_INVALID_REQUEST,
	GSURF_CONTROL_ERROR_TIMEOUT,
	GSURF_CONTROL_ERROR_CLOSED
} GsurfControlError;

/**
 * gsurf_control_error_quark:
 *
 * Returns: the #GsurfControlError domain
 *
 * Since: 0.5
 */
GQuark gsurf_control_error_quark(void);

/**
 * gsurf_control_error_get_type:
 *
 * Returns: the #GType of #GsurfControlError
 *
 * Since: 0.5
 */
GType  gsurf_control_error_get_type(void) G_GNUC_CONST;
#define GSURF_TYPE_CONTROL_ERROR (gsurf_control_error_get_type())

/**
 * gsurf_control_error_to_code:
 * @error: an error
 *
 * The JSON-RPC error code for @error: a #GsurfControlError maps to its
 * own code; %G_IO_ERROR_NOT_FOUND, %G_IO_ERROR_NOT_SUPPORTED,
 * %G_IO_ERROR_PERMISSION_DENIED and %G_IO_ERROR_INVALID_ARGUMENT map to
 * the not-found, unsupported, permission-denied and invalid-params codes;
 * anything else is -32000.
 *
 * Returns: the code
 *
 * Since: 0.5
 */
gint   gsurf_control_error_to_code(const GError *error);

/**
 * gsurf_control_error_from_code:
 * @code: a JSON-RPC error code
 *
 * Returns: the #GsurfControlError for @code; unknown codes are
 *   %GSURF_CONTROL_ERROR_FAILED
 *
 * Since: 0.5
 */
GsurfControlError gsurf_control_error_from_code(gint code);

#define GSURF_TYPE_CONTROL_SERVER (gsurf_control_server_get_type())

G_DECLARE_FINAL_TYPE(GsurfControlServer, gsurf_control_server, GSURF, CONTROL_SERVER, GObject)

#define GSURF_TYPE_CONTROL_INVOCATION (gsurf_control_invocation_get_type())

G_DECLARE_FINAL_TYPE(GsurfControlInvocation, gsurf_control_invocation, GSURF,
                     CONTROL_INVOCATION, GObject)

/**
 * GsurfControlMethodFunc:
 * @server: the server
 * @invocation: (transfer none): the call; answer it exactly once with
 *   gsurf_control_invocation_return_value() or one of the
 *   gsurf_control_invocation_return_error() family, now or later (the
 *   server keeps it alive until then)
 * @user_data: (closure): the data given to gsurf_control_server_add_method()
 *
 * Implements one control method.
 *
 * Since: 0.5
 */
typedef void (*GsurfControlMethodFunc) (GsurfControlServer *server,
                                        GsurfControlInvocation *invocation,
                                        gpointer user_data);

/**
 * gsurf_control_server_new:
 * @app: (nullable): the application whose windows the methods act on and
 *   the events follow; %NULL uses the default module manager's
 *   application (gsurf_module_manager_get_application()) at start
 *
 * Creates a server with the core methods registered. It does not listen
 * until gsurf_control_server_start().
 *
 * Returns: (transfer full): a new #GsurfControlServer
 *
 * Since: 0.5
 */
GsurfControlServer *gsurf_control_server_new(GsurfApplication *app);

/**
 * gsurf_control_server_get_application:
 * @self: a #GsurfControlServer
 *
 * Returns: (transfer none) (nullable): the application the methods act
 *   on: the one given to gsurf_control_server_new(), else the default
 *   module manager's
 *
 * Since: 0.5
 */
GsurfApplication *gsurf_control_server_get_application(GsurfControlServer *self);

/**
 * gsurf_control_server_set_name:
 * @self: a #GsurfControlServer
 * @name: (nullable): an instance name (letters, digits, `-`, `_` and `.`),
 *   or %NULL for none
 *
 * Names the instance: while it runs, gsurf_control_server_start() keeps
 * a `<name>.sock` symlink to the socket in `$XDG_RUNTIME_DIR/gsurf`, so
 * clients can pick it by name (`gsurfctl -i NAME`). Takes effect at the
 * next start. A name a live instance already holds is not taken.
 *
 * Returns: %FALSE when @name is not a valid name (nothing changes)
 *
 * Since: 0.5
 */
gboolean     gsurf_control_server_set_name(GsurfControlServer *self, const gchar *name);

/**
 * gsurf_control_server_get_name:
 * @self: a #GsurfControlServer
 *
 * Returns: (transfer none) (nullable): the instance name
 *
 * Since: 0.5
 */
const gchar *gsurf_control_server_get_name(GsurfControlServer *self);

/**
 * gsurf_control_server_start:
 * @self: a #GsurfControlServer
 * @path: (nullable) (type filename): the socket path; %NULL is
 *   #GSURF_CONTROL_SOCKET_NAME in the process runtime directory
 *   (gsurf_runtime_dir_get())
 * @error: return location for a #GError
 *
 * Starts listening (a stale file at @path is replaced), registers the
 * methods of the active #GsurfControlProvider modules and begins
 * following the application's windows for events. Starting a running
 * server does nothing.
 *
 * Returns: %TRUE when the server listens
 *
 * Since: 0.5
 */
gboolean     gsurf_control_server_start(GsurfControlServer *self, const gchar *path,
                                        GError **error);

/**
 * gsurf_control_server_stop:
 * @self: a #GsurfControlServer
 *
 * Closes every connection, stops listening and removes the socket file
 * and the name symlink. Calls still being answered are dropped. Safe to
 * call on a stopped server; disposing a running server stops it.
 *
 * Since: 0.5
 */
void         gsurf_control_server_stop(GsurfControlServer *self);

/**
 * gsurf_control_server_is_running:
 * @self: a #GsurfControlServer
 *
 * Returns: whether the server listens
 *
 * Since: 0.5
 */
gboolean     gsurf_control_server_is_running(GsurfControlServer *self);

/**
 * gsurf_control_server_get_socket_path:
 * @self: a #GsurfControlServer
 *
 * Returns: (transfer none) (nullable) (type filename): the socket path
 *   while running
 *
 * Since: 0.5
 */
const gchar *gsurf_control_server_get_socket_path(GsurfControlServer *self);

/**
 * gsurf_control_server_get_n_clients:
 * @self: a #GsurfControlServer
 *
 * Returns: the number of open connections
 *
 * Since: 0.5
 */
guint        gsurf_control_server_get_n_clients(GsurfControlServer *self);

/**
 * gsurf_control_server_get_focused_at:
 * @self: a #GsurfControlServer
 *
 * The monotonic time (g_get_monotonic_time(), comparable between
 * processes) at which a window of this instance last gained the focus or
 * saw a key or button press; the start time until then. Clients use it
 * to pick the most recently used instance.
 *
 * Returns: the time in microseconds
 *
 * Since: 0.5
 */
gint64       gsurf_control_server_get_focused_at(GsurfControlServer *self);

/**
 * gsurf_control_server_add_method:
 * @self: a #GsurfControlServer
 * @name: the method name, e.g. `tabs.sidebar`
 * @description: (nullable): one line on what it does and returns
 * @params_doc: (nullable): its parameters, e.g. `{view?: int, uri: string}`
 * @handler: (scope notified) (closure user_data) (destroy destroy): the
 *   implementation
 * @user_data: data for @handler
 * @destroy: (nullable): frees @user_data when the method is removed
 *
 * Registers a method. A method registered while a #GsurfControlProvider
 * registers its methods belongs to that provider and goes away when the
 * providers are refreshed (gsurf_control_server_refresh_providers()).
 *
 * Returns: %FALSE (and @destroy is called) when @name is empty or taken
 *
 * Since: 0.5
 */
gboolean     gsurf_control_server_add_method(GsurfControlServer *self, const gchar *name,
                                             const gchar *description,
                                             const gchar *params_doc,
                                             GsurfControlMethodFunc handler,
                                             gpointer user_data,
                                             GDestroyNotify destroy);

/**
 * gsurf_control_server_remove_method:
 * @self: a #GsurfControlServer
 * @name: a method name
 *
 * Returns: whether a method was removed
 *
 * Since: 0.5
 */
gboolean     gsurf_control_server_remove_method(GsurfControlServer *self, const gchar *name);

/**
 * gsurf_control_server_has_method:
 * @self: a #GsurfControlServer
 * @name: a method name
 *
 * Returns: whether @name is registered
 *
 * Since: 0.5
 */
gboolean     gsurf_control_server_has_method(GsurfControlServer *self, const gchar *name);

/**
 * gsurf_control_server_list_methods:
 * @self: a #GsurfControlServer
 *
 * Returns: (transfer full) (array zero-terminated=1): the method names,
 *   sorted
 *
 * Since: 0.5
 */
gchar      **gsurf_control_server_list_methods(GsurfControlServer *self);

/**
 * gsurf_control_server_describe_methods:
 * @self: a #GsurfControlServer
 *
 * The answer of `methods.list`: an array of `{name, description, params,
 * provider}` objects sorted by name (`provider` is the providing module's
 * name, or null for a core method).
 *
 * Returns: (transfer full): a JSON array node
 *
 * Since: 0.5
 */
JsonNode    *gsurf_control_server_describe_methods(GsurfControlServer *self);

/**
 * gsurf_control_server_refresh_providers:
 * @self: a #GsurfControlServer
 *
 * Removes every provider's methods and asks each active module that
 * implements #GsurfControlProvider (in the default module manager) to
 * register its methods again. The server does this at start and after
 * `modules.toggle`; call it after activating or deactivating modules
 * yourself.
 *
 * Since: 0.5
 */
void         gsurf_control_server_refresh_providers(GsurfControlServer *self);

/**
 * gsurf_control_server_emit_event:
 * @self: a #GsurfControlServer
 * @topic: the topic subscribers choose by, e.g. `view`
 * @event: the event name, e.g. `view.added`
 * @data: (transfer full) (nullable): the event's data, or %NULL for null
 *
 * Sends the event to every connection subscribed to @topic (or to all
 * topics). Does nothing while the server is stopped.
 *
 * Since: 0.5
 */
void         gsurf_control_server_emit_event(GsurfControlServer *self, const gchar *topic,
                                             const gchar *event, JsonNode *data);

/**
 * gsurf_control_server_list_topics:
 *
 * Returns: (transfer none) (array zero-terminated=1): the topics the
 *   server emits by itself
 *
 * Since: 0.5
 */
const gchar *const *gsurf_control_server_list_topics(void);

/**
 * gsurf_control_server_dispatch:
 * @self: a #GsurfControlServer
 * @request: one JSON-RPC request line (a request object or a batch)
 * @callback: (scope async) (closure user_data): called with the response
 *   line once every call answered, or %NULL when nothing needs an answer
 *   (notifications)
 * @user_data: data for @callback
 *
 * Runs @request through the registry as if it came over the socket
 * (without one: `events.subscribe` fails), for in-process callers and
 * tests. @callback may run before this returns.
 *
 * Since: 0.5
 */
void         gsurf_control_server_dispatch(GsurfControlServer *self, const gchar *request,
                                           GAsyncReadyCallback callback, gpointer user_data);

/**
 * gsurf_control_server_dispatch_finish:
 * @self: a #GsurfControlServer
 * @result: the result given to the callback
 *
 * Returns: (transfer full) (nullable): the response line (without the
 *   newline), or %NULL when the request held only notifications
 *
 * Since: 0.5
 */
gchar       *gsurf_control_server_dispatch_finish(GsurfControlServer *self,
                                                  GAsyncResult *result);

/**
 * gsurf_control_invocation_get_server:
 * @self: a #GsurfControlInvocation
 *
 * Returns: (transfer none): the server running the call
 *
 * Since: 0.5
 */
GsurfControlServer *gsurf_control_invocation_get_server(GsurfControlInvocation *self);

/**
 * gsurf_control_invocation_get_method:
 * @self: a #GsurfControlInvocation
 *
 * Returns: (transfer none): the method name
 *
 * Since: 0.5
 */
const gchar *gsurf_control_invocation_get_method(GsurfControlInvocation *self);

/**
 * gsurf_control_invocation_get_params:
 * @self: a #GsurfControlInvocation
 *
 * The parameters; a call without them has an
 * empty object.
 *
 * Returns: (transfer none): the params object
 *
 * Since: 0.5
 */
JsonObject  *gsurf_control_invocation_get_params(GsurfControlInvocation *self);

/**
 * gsurf_control_invocation_get_answered:
 * @self: a #GsurfControlInvocation
 *
 * Returns: whether the call has been answered
 *
 * Since: 0.5
 */
gboolean     gsurf_control_invocation_get_answered(GsurfControlInvocation *self);

/**
 * gsurf_control_invocation_return_value:
 * @self: a #GsurfControlInvocation
 * @result: (transfer full) (nullable): the result, or %NULL for null
 *
 * Answers the call with @result. A second answer is ignored (with a
 * critical).
 *
 * Since: 0.5
 */
void         gsurf_control_invocation_return_value(GsurfControlInvocation *self,
                                                   JsonNode *result);

/**
 * gsurf_control_invocation_return_gerror:
 * @self: a #GsurfControlInvocation
 * @error: the error; its code becomes the JSON-RPC code
 *   (gsurf_control_error_to_code())
 *
 * Answers the call with an error.
 *
 * Since: 0.5
 */
void         gsurf_control_invocation_return_gerror(GsurfControlInvocation *self,
                                                    const GError *error);

/**
 * gsurf_control_invocation_return_error:
 * @self: a #GsurfControlInvocation
 * @code: the error
 * @format: printf()-style format of the message
 * @...: its arguments
 *
 * Answers the call with an error.
 *
 * Since: 0.5
 */
void         gsurf_control_invocation_return_error(GsurfControlInvocation *self,
                                                   GsurfControlError code,
                                                   const gchar *format, ...)
	G_GNUC_PRINTF(3, 4);

/**
 * gsurf_control_invocation_return_error_literal:
 * @self: a #GsurfControlInvocation
 * @code: the error
 * @message: the message
 *
 * Answers the call with an error.
 *
 * Since: 0.5
 */
void         gsurf_control_invocation_return_error_literal(GsurfControlInvocation *self,
                                                           GsurfControlError code,
                                                           const gchar *message);

G_END_DECLS

#endif /* GSURF_CONTROL_SERVER_H */
