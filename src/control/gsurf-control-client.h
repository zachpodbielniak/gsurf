/*
 * gsurf-control-client.h - Find running gsurf instances and talk to them
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Discovery: every gsurf process keeps a private directory
 * `$XDG_RUNTIME_DIR/gsurf/<pid>-XXXXXX` with a `lock` file it holds
 * flock()ed for its whole life (gsurf-runtime-dir.h). A directory whose
 * lock is held and that has a `control.sock` is a running instance;
 * gsurf_control_list_instances() finds them, together with their names
 * (the `<name>.sock` symlinks a named server keeps next to the
 * directories).
 *
 * #GsurfControlClient speaks the control protocol (docs/control.org):
 * synchronous calls with a timeout, an event stream after
 * gsurf_control_client_subscribe(), and an asynchronous call for
 * embedders that must not block their main loop.
 *
 * Since: 0.5
 */

#ifndef GSURF_CONTROL_CLIENT_H
#define GSURF_CONTROL_CLIENT_H

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "control/gsurf-control-server.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* Instances                                                           */
/* ------------------------------------------------------------------ */

/**
 * GsurfControlInstance:
 *
 * A running gsurf instance found by gsurf_control_list_instances(): its
 * pid, its runtime directory, its control socket and its name.
 * Reference-counted and immutable.
 *
 * Since: 0.5
 */
typedef struct _GsurfControlInstance GsurfControlInstance;

#define GSURF_TYPE_CONTROL_INSTANCE (gsurf_control_instance_get_type())

/**
 * gsurf_control_instance_get_type:
 *
 * Returns: the boxed #GType of #GsurfControlInstance
 *
 * Since: 0.5
 */
GType                 gsurf_control_instance_get_type(void) G_GNUC_CONST;

/**
 * gsurf_control_instance_ref:
 * @self: a #GsurfControlInstance
 *
 * Returns: (transfer full): @self
 *
 * Since: 0.5
 */
GsurfControlInstance *gsurf_control_instance_ref(GsurfControlInstance *self);

/**
 * gsurf_control_instance_unref:
 * @self: (transfer full): a #GsurfControlInstance
 *
 * Since: 0.5
 */
void                  gsurf_control_instance_unref(GsurfControlInstance *self);

/**
 * gsurf_control_instance_get_pid:
 * @self: a #GsurfControlInstance
 *
 * Returns: the process id in the directory name
 *
 * Since: 0.5
 */
gint64                gsurf_control_instance_get_pid(GsurfControlInstance *self);

/**
 * gsurf_control_instance_get_id:
 * @self: a #GsurfControlInstance
 *
 * Returns: (transfer none): the runtime directory's name, `<pid>-XXXXXX`
 *
 * Since: 0.5
 */
const gchar          *gsurf_control_instance_get_id(GsurfControlInstance *self);

/**
 * gsurf_control_instance_get_runtime_dir:
 * @self: a #GsurfControlInstance
 *
 * Returns: (transfer none) (type filename): the runtime directory
 *
 * Since: 0.5
 */
const gchar          *gsurf_control_instance_get_runtime_dir(GsurfControlInstance *self);

/**
 * gsurf_control_instance_get_socket_path:
 * @self: a #GsurfControlInstance
 *
 * Returns: (transfer none) (type filename): the control socket
 *
 * Since: 0.5
 */
const gchar          *gsurf_control_instance_get_socket_path(GsurfControlInstance *self);

/**
 * gsurf_control_instance_get_name:
 * @self: a #GsurfControlInstance
 *
 * Returns: (transfer none) (nullable): the instance name (`control.name`)
 *
 * Since: 0.5
 */
const gchar          *gsurf_control_instance_get_name(GsurfControlInstance *self);

/**
 * gsurf_control_instance_matches:
 * @self: a #GsurfControlInstance
 * @spec: what the user typed: a pid, a name or a directory id
 *
 * Returns: whether @spec names @self
 *
 * Since: 0.5
 */
gboolean              gsurf_control_instance_matches(GsurfControlInstance *self,
                                                     const gchar *spec);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfControlInstance, gsurf_control_instance_unref)

/**
 * gsurf_control_get_base_dir:
 *
 * Returns: (transfer full) (type filename): `$XDG_RUNTIME_DIR/gsurf`, the
 *   parent of every process runtime directory
 *
 * Since: 0.5
 */
gchar     *gsurf_control_get_base_dir(void);

/**
 * gsurf_control_list_instances:
 * @base: (nullable) (type filename): the directory to scan; %NULL is
 *   gsurf_control_get_base_dir()
 *
 * Finds the running instances: process directories whose lock is held
 * (the non-blocking flock() probe the runtime-directory sweep uses) and
 * that have a control socket. A live directory without one (an embedder,
 * or `control.enabled: false`) is skipped, as are dead ones. Names come
 * from the `<name>.sock` symlinks in @base.
 *
 * Returns: (transfer full) (element-type GsurfControlInstance): the
 *   instances, sorted by pid
 *
 * Since: 0.5
 */
GPtrArray *gsurf_control_list_instances(const gchar *base);

/**
 * gsurf_control_find_instance:
 * @base: (nullable) (type filename): as for gsurf_control_list_instances()
 * @spec: a pid, a name or a directory id
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): the instance @spec names, or
 *   %NULL with %GSURF_CONTROL_ERROR_NOT_FOUND
 *
 * Since: 0.5
 */
GsurfControlInstance *gsurf_control_find_instance(const gchar *base, const gchar *spec,
                                                  GError **error);

/* ------------------------------------------------------------------ */
/* Client                                                              */
/* ------------------------------------------------------------------ */

#define GSURF_TYPE_CONTROL_CLIENT (gsurf_control_client_get_type())

G_DECLARE_FINAL_TYPE(GsurfControlClient, gsurf_control_client, GSURF, CONTROL_CLIENT, GObject)

/**
 * gsurf_control_client_new:
 *
 * Returns: (transfer full): an unconnected client (timeout 30 seconds)
 *
 * Since: 0.5
 */
GsurfControlClient *gsurf_control_client_new(void);

/**
 * gsurf_control_client_connect:
 * @self: a #GsurfControlClient
 * @path: (type filename): the socket path
 * @error: return location for a #GError
 *
 * Connects to a control socket (closing an earlier connection).
 *
 * Returns: %TRUE when connected
 *
 * Since: 0.5
 */
gboolean    gsurf_control_client_connect(GsurfControlClient *self, const gchar *path,
                                         GError **error);

/**
 * gsurf_control_client_close:
 * @self: a #GsurfControlClient
 *
 * Closes the connection; safe to call when closed.
 *
 * Since: 0.5
 */
void        gsurf_control_client_close(GsurfControlClient *self);

/**
 * gsurf_control_client_is_connected:
 * @self: a #GsurfControlClient
 *
 * Returns: whether the client is connected
 *
 * Since: 0.5
 */
gboolean    gsurf_control_client_is_connected(GsurfControlClient *self);

/**
 * gsurf_control_client_set_timeout:
 * @self: a #GsurfControlClient
 * @timeout_ms: how long a call waits for its reply; 0 waits forever
 *
 * Since: 0.5
 */
void        gsurf_control_client_set_timeout(GsurfControlClient *self, guint timeout_ms);

/**
 * gsurf_control_client_get_timeout:
 * @self: a #GsurfControlClient
 *
 * Returns: the call timeout in milliseconds (0: none)
 *
 * Since: 0.5
 */
guint       gsurf_control_client_get_timeout(GsurfControlClient *self);

/**
 * gsurf_control_client_call:
 * @self: a #GsurfControlClient
 * @method: the method name
 * @params: (nullable): the params object
 * @error: return location for a #GError
 *
 * Sends one request and waits (blocking, up to the timeout) for its
 * response. Event notifications that arrive meanwhile are kept for
 * gsurf_control_client_read_event(). A JSON-RPC error becomes a
 * #GsurfControlError (gsurf_control_error_from_code()); a dropped
 * connection is %GSURF_CONTROL_ERROR_CLOSED, a missing reply
 * %GSURF_CONTROL_ERROR_TIMEOUT.
 *
 * Returns: (transfer full) (nullable): the result (a null node for a null
 *   result), or %NULL with @error set
 *
 * Since: 0.5
 */
JsonNode   *gsurf_control_client_call(GsurfControlClient *self, const gchar *method,
                                      JsonObject *params, GError **error);

/**
 * gsurf_control_client_call_async:
 * @self: a #GsurfControlClient
 * @method: the method name
 * @params: (nullable): the params object (copied)
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called with the result
 * @user_data: data for @callback
 *
 * gsurf_control_client_call() in a worker thread, for hosts that must
 * not block their main loop. Calls on one client run one at a time.
 *
 * Since: 0.5
 */
void        gsurf_control_client_call_async(GsurfControlClient *self, const gchar *method,
                                            JsonObject *params, GCancellable *cancellable,
                                            GAsyncReadyCallback callback, gpointer user_data);

/**
 * gsurf_control_client_call_finish:
 * @self: a #GsurfControlClient
 * @result: the #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): as gsurf_control_client_call()
 *
 * Since: 0.5
 */
JsonNode   *gsurf_control_client_call_finish(GsurfControlClient *self, GAsyncResult *result,
                                             GError **error);

/**
 * gsurf_control_client_subscribe:
 * @self: a #GsurfControlClient
 * @topics: (nullable) (array zero-terminated=1): the topics, or %NULL /
 *   empty for all of them
 * @error: return location for a #GError
 *
 * Calls `events.subscribe`; the events then arrive through
 * gsurf_control_client_read_event().
 *
 * Returns: %TRUE when subscribed
 *
 * Since: 0.5
 */
gboolean    gsurf_control_client_subscribe(GsurfControlClient *self,
                                           const gchar *const *topics, GError **error);

/**
 * gsurf_control_client_read_event:
 * @self: a #GsurfControlClient
 * @timeout_ms: how long to wait; 0 does not wait
 * @error: return location for a #GError
 *
 * Returns the next event notification's params (`{topic, event, time,
 * data}`).
 *
 * Returns: (transfer full) (nullable): the event, or %NULL: on a timeout
 *   without @error set, or with @error set when the connection failed or
 *   closed (%GSURF_CONTROL_ERROR_CLOSED)
 *
 * Since: 0.5
 */
JsonNode   *gsurf_control_client_read_event(GsurfControlClient *self, guint timeout_ms,
                                            GError **error);

G_END_DECLS

#endif /* GSURF_CONTROL_CLIENT_H */
