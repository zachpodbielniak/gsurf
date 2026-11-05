/*
 * gsurf-control-server.c - The control socket: JSON-RPC 2.0 over a Unix socket
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Parts:
 *   - errors: the GsurfControlError domain and its JSON-RPC codes;
 *   - invocations: one call, answered once (now or later);
 *   - requests: a line (one call or a batch) collecting its answers into
 *     the response line, sent to a connection or handed to a GTask
 *     (gsurf_control_server_dispatch());
 *   - connections: an async line reader, a queue of async writes, the
 *     peer credential check and the event subscription;
 *   - the registry and the built-in methods (methods.list,
 *     events.subscribe, events.unsubscribe);
 *   - events: the application's windows and views, the module manager's
 *     mode and downloads, turned into event notifications;
 *   - the server's life cycle (socket file, name symlink).
 */

#include "control/gsurf-control-server.h"
#include "control/gsurf-control-provider.h"
#include "control/gsurf-control-private.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

/* The longest request line a connection may send (16 MiB). */
#define GSURF_CONTROL_MAX_LINE (16 * 1024 * 1024)

/* JSON-RPC error codes. */
#define CODE_PARSE             (-32700)
#define CODE_INVALID_REQUEST   (-32600)
#define CODE_METHOD_NOT_FOUND  (-32601)
#define CODE_INVALID_PARAMS    (-32602)
#define CODE_FAILED            (-32000)
#define CODE_NOT_FOUND         (-32001)
#define CODE_UNSUPPORTED       (-32002)
#define CODE_PERMISSION_DENIED (-32003)
#define CODE_TIMEOUT           (-32004)
#define CODE_CLOSED            (-32005)

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

G_DEFINE_QUARK(gsurf-control-error-quark, gsurf_control_error)

GType
gsurf_control_error_get_type(void)
{
	static gsize type_id = 0;

	if (g_once_init_enter(&type_id)) {
		static const GEnumValue values[] = {
			{ GSURF_CONTROL_ERROR_FAILED, "GSURF_CONTROL_ERROR_FAILED", "failed" },
			{ GSURF_CONTROL_ERROR_INVALID_PARAMS, "GSURF_CONTROL_ERROR_INVALID_PARAMS",
			  "invalid-params" },
			{ GSURF_CONTROL_ERROR_NOT_FOUND, "GSURF_CONTROL_ERROR_NOT_FOUND", "not-found" },
			{ GSURF_CONTROL_ERROR_UNSUPPORTED, "GSURF_CONTROL_ERROR_UNSUPPORTED",
			  "unsupported" },
			{ GSURF_CONTROL_ERROR_PERMISSION_DENIED,
			  "GSURF_CONTROL_ERROR_PERMISSION_DENIED", "permission-denied" },
			{ GSURF_CONTROL_ERROR_METHOD_NOT_FOUND, "GSURF_CONTROL_ERROR_METHOD_NOT_FOUND",
			  "method-not-found" },
			{ GSURF_CONTROL_ERROR_PARSE, "GSURF_CONTROL_ERROR_PARSE", "parse" },
			{ GSURF_CONTROL_ERROR_INVALID_REQUEST, "GSURF_CONTROL_ERROR_INVALID_REQUEST",
			  "invalid-request" },
			{ GSURF_CONTROL_ERROR_TIMEOUT, "GSURF_CONTROL_ERROR_TIMEOUT", "timeout" },
			{ GSURF_CONTROL_ERROR_CLOSED, "GSURF_CONTROL_ERROR_CLOSED", "closed" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("GsurfControlError", values);

		g_once_init_leave(&type_id, id);
	}
	return (GType)type_id;
}

gint
gsurf_control_error_to_code(const GError *error)
{
	if (error == NULL)
		return CODE_FAILED;
	if (error->domain == GSURF_CONTROL_ERROR) {
		switch ((GsurfControlError)error->code) {
		case GSURF_CONTROL_ERROR_INVALID_PARAMS:    return CODE_INVALID_PARAMS;
		case GSURF_CONTROL_ERROR_NOT_FOUND:         return CODE_NOT_FOUND;
		case GSURF_CONTROL_ERROR_UNSUPPORTED:       return CODE_UNSUPPORTED;
		case GSURF_CONTROL_ERROR_PERMISSION_DENIED: return CODE_PERMISSION_DENIED;
		case GSURF_CONTROL_ERROR_METHOD_NOT_FOUND:  return CODE_METHOD_NOT_FOUND;
		case GSURF_CONTROL_ERROR_PARSE:             return CODE_PARSE;
		case GSURF_CONTROL_ERROR_INVALID_REQUEST:   return CODE_INVALID_REQUEST;
		case GSURF_CONTROL_ERROR_TIMEOUT:           return CODE_TIMEOUT;
		case GSURF_CONTROL_ERROR_CLOSED:            return CODE_CLOSED;
		case GSURF_CONTROL_ERROR_FAILED:
		default:                                    return CODE_FAILED;
		}
	}
	if (error->domain == G_IO_ERROR) {
		switch (error->code) {
		case G_IO_ERROR_NOT_FOUND:         return CODE_NOT_FOUND;
		case G_IO_ERROR_NOT_SUPPORTED:     return CODE_UNSUPPORTED;
		case G_IO_ERROR_PERMISSION_DENIED: return CODE_PERMISSION_DENIED;
		case G_IO_ERROR_INVALID_ARGUMENT:  return CODE_INVALID_PARAMS;
		default:                           return CODE_FAILED;
		}
	}
	return CODE_FAILED;
}

GsurfControlError
gsurf_control_error_from_code(gint code)
{
	switch (code) {
	case CODE_PARSE:             return GSURF_CONTROL_ERROR_PARSE;
	case CODE_INVALID_REQUEST:   return GSURF_CONTROL_ERROR_INVALID_REQUEST;
	case CODE_METHOD_NOT_FOUND:  return GSURF_CONTROL_ERROR_METHOD_NOT_FOUND;
	case CODE_INVALID_PARAMS:    return GSURF_CONTROL_ERROR_INVALID_PARAMS;
	case CODE_NOT_FOUND:         return GSURF_CONTROL_ERROR_NOT_FOUND;
	case CODE_UNSUPPORTED:       return GSURF_CONTROL_ERROR_UNSUPPORTED;
	case CODE_PERMISSION_DENIED: return GSURF_CONTROL_ERROR_PERMISSION_DENIED;
	case CODE_TIMEOUT:           return GSURF_CONTROL_ERROR_TIMEOUT;
	case CODE_CLOSED:            return GSURF_CONTROL_ERROR_CLOSED;
	default:                     return GSURF_CONTROL_ERROR_FAILED;
	}
}

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/* One registered method. */
typedef struct {
	gchar                  *name;
	gchar                  *description;
	gchar                  *params_doc;
	gchar                  *provider;   /* the providing module's name, or NULL */
	gboolean                from_provider;
	GsurfControlMethodFunc  handler;
	gpointer                user_data;
	GDestroyNotify          destroy;
} Method;

typedef struct _Conn Conn;

/* One request line: a single call or a batch, collecting its answers. */
typedef struct {
	gint      refs;
	Conn     *conn;       /* the connection to answer on, or NULL */
	GTask    *task;       /* or the dispatch() caller */
	gboolean  batch;
	JsonArray *answers;   /* response objects */
	guint     pending;    /* calls not yet answered */
	gboolean  issued;     /* every call has been started */
	gboolean  done;
} Request;

/* One client connection. */
struct _Conn {
	gint                refs;
	GsurfControlServer *server;      /* NULL once closed */
	GSocketConnection  *connection;
	GDataInputStream   *in;
	GOutputStream      *out;
	GCancellable       *cancellable;
	GQueue              writes;      /* GBytes*, the head being written */
	gboolean            writing;
	gboolean            closed;
	gboolean            subscribed;
	GHashTable         *topics;      /* subscribed topics; NULL = all */
};

struct _GsurfControlServer
{
	GObject parent_instance;

	GsurfApplication *app;            /* given to new(); weak */
	GsurfApplication *watched_app;    /* the one followed for events; weak */
	GHashTable       *methods;        /* name -> Method* */
	gboolean          registering;    /* a provider is registering */
	gchar            *registering_name;
	GSocketService   *service;
	gchar            *path;
	gchar            *name;
	gchar            *link_path;
	GPtrArray        *conns;          /* Conn* (ref) */
	GPtrArray        *windows;        /* followed GsurfWindow* (ref) */
	gint64            focused_at;
	gboolean          running;
};

struct _GsurfControlInvocation
{
	GObject parent_instance;

	GsurfControlServer *server;   /* weak */
	gchar              *method;
	JsonObject         *params;
	JsonNode           *id;       /* NULL for a notification */
	gboolean            notification;
	Request            *request;
	gboolean            answered;
};

G_DEFINE_FINAL_TYPE(GsurfControlServer, gsurf_control_server, G_TYPE_OBJECT)
G_DEFINE_FINAL_TYPE(GsurfControlInvocation, gsurf_control_invocation, G_TYPE_OBJECT)

static const gchar *const core_topics[] = {
	"view", "uri", "title", "load", "mode", "layout", "download", NULL
};

static void conn_send(Conn *conn, const gchar *line);
static void conn_close(Conn *conn);
static void conn_unref(Conn *conn);
static void request_unref(Request *request);
static void watch_window(GsurfControlServer *self, GsurfWindow *window);
static void unwatch_window(GsurfControlServer *self, GsurfWindow *window);

static void
method_free(gpointer data)
{
	Method *m = data;

	if (m->destroy != NULL)
		m->destroy(m->user_data);
	g_free(m->name);
	g_free(m->description);
	g_free(m->params_doc);
	g_free(m->provider);
	g_free(m);
}

/* One JSON value on one line. */
static gchar *
node_to_line(JsonNode *node)
{
	g_autoptr(JsonGenerator) generator = json_generator_new();

	json_generator_set_root(generator, node);
	return json_generator_to_data(generator, NULL);
}

/* {"jsonrpc":"2.0","id":@id,"error":{"code":..,"message":..}} */
static JsonNode *
error_response(JsonNode *id, gint code, const gchar *message)
{
	g_autoptr(JsonBuilder) b = json_builder_new();

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "jsonrpc");
	json_builder_add_string_value(b, "2.0");
	json_builder_set_member_name(b, "id");
	if (id != NULL)
		json_builder_add_value(b, json_node_copy(id));
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "error");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "code");
	json_builder_add_int_value(b, code);
	json_builder_set_member_name(b, "message");
	json_builder_add_string_value(b, message != NULL ? message : "error");
	json_builder_end_object(b);
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

static Request *
request_new(Conn *conn, GTask *task, gboolean batch)
{
	Request *r = g_new0(Request, 1);

	r->refs = 1;
	if (conn != NULL) {
		g_atomic_int_inc(&conn->refs);
		r->conn = conn;
	}
	r->task = task != NULL ? g_object_ref(task) : NULL;
	r->batch = batch;
	r->answers = json_array_new();
	return r;
}

static Request *
request_ref(Request *r)
{
	r->refs++;
	return r;
}

static void
request_unref(Request *r)
{
	if (--r->refs > 0)
		return;
	if (r->conn != NULL)
		conn_unref(r->conn);
	g_clear_object(&r->task);
	json_array_unref(r->answers);
	g_free(r);
}

/* Every call issued and answered: send the response line (an object, or
 * an array for a batch; nothing when every call was a notification). */
static void
request_maybe_finish(Request *r)
{
	g_autofree gchar *line = NULL;

	if (r->done || !r->issued || r->pending > 0)
		return;
	r->done = TRUE;
	if (json_array_get_length(r->answers) > 0) {
		g_autoptr(JsonNode) root = NULL;

		if (r->batch) {
			root = json_node_new(JSON_NODE_ARRAY);
			json_node_set_array(root, r->answers);
		} else {
			root = json_node_copy(json_array_get_element(r->answers, 0));
		}
		line = node_to_line(root);
	}
	if (r->conn != NULL && line != NULL)
		conn_send(r->conn, line);
	if (r->task != NULL)
		g_task_return_pointer(r->task, g_steal_pointer(&line), g_free);
}

/* An answer for a call that never reached a handler (parse error, bad
 * request, unknown method). */
static void
request_add_answer(Request *r, JsonNode *answer)
{
	json_array_add_element(r->answers, answer);
}

/* ------------------------------------------------------------------ */
/* Invocations                                                         */
/* ------------------------------------------------------------------ */

static void
gsurf_control_invocation_finalize(GObject *object)
{
	GsurfControlInvocation *self = GSURF_CONTROL_INVOCATION(object);

	if (self->server != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->server), (gpointer *)&self->server);
	g_free(self->method);
	g_clear_pointer(&self->params, json_object_unref);
	g_clear_pointer(&self->id, json_node_unref);
	g_clear_pointer(&self->request, request_unref);
	G_OBJECT_CLASS(gsurf_control_invocation_parent_class)->finalize(object);
}

static void
gsurf_control_invocation_class_init(GsurfControlInvocationClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gsurf_control_invocation_finalize;
}

static void
gsurf_control_invocation_init(GsurfControlInvocation *self)
{
	(void)self;
}

GsurfControlServer *
gsurf_control_invocation_get_server(GsurfControlInvocation *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_INVOCATION(self), NULL);
	return self->server;
}

const gchar *
gsurf_control_invocation_get_method(GsurfControlInvocation *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_INVOCATION(self), NULL);
	return self->method;
}

JsonObject *
gsurf_control_invocation_get_params(GsurfControlInvocation *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_INVOCATION(self), NULL);
	return self->params;
}

gboolean
gsurf_control_invocation_get_answered(GsurfControlInvocation *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_INVOCATION(self), TRUE);
	return self->answered;
}

/* Records @answer (consumed; NULL for a notification), completes the
 * request when it was the last call and drops the server's reference. */
static void
invocation_answer(GsurfControlInvocation *self, JsonNode *answer)
{
	Request *r = self->request;

	self->answered = TRUE;
	if (answer != NULL && !self->notification)
		request_add_answer(r, answer);
	else if (answer != NULL)
		json_node_unref(answer);
	r->pending--;
	request_maybe_finish(r);
	/* The reference the server kept while the call was open. */
	g_object_unref(self);
}

void
gsurf_control_invocation_return_value(GsurfControlInvocation *self, JsonNode *result)
{
	g_autoptr(JsonBuilder) b = NULL;

	g_return_if_fail(GSURF_IS_CONTROL_INVOCATION(self));
	if (self->answered) {
		g_critical("gsurf control: %s answered twice", self->method);
		if (result != NULL)
			json_node_unref(result);
		return;
	}
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "jsonrpc");
	json_builder_add_string_value(b, "2.0");
	json_builder_set_member_name(b, "id");
	if (self->id != NULL)
		json_builder_add_value(b, json_node_copy(self->id));
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "result");
	if (result != NULL)
		json_builder_add_value(b, result);
	else
		json_builder_add_null_value(b);
	json_builder_end_object(b);
	invocation_answer(self, json_builder_get_root(b));
}

void
gsurf_control_invocation_return_gerror(GsurfControlInvocation *self, const GError *error)
{
	g_return_if_fail(GSURF_IS_CONTROL_INVOCATION(self));
	if (self->answered) {
		g_critical("gsurf control: %s answered twice", self->method);
		return;
	}
	invocation_answer(self, error_response(self->id, gsurf_control_error_to_code(error),
		error != NULL ? error->message : "failed"));
}

void
gsurf_control_invocation_return_error_literal(GsurfControlInvocation *self,
                                              GsurfControlError code, const gchar *message)
{
	g_autoptr(GError) error = NULL;

	g_return_if_fail(GSURF_IS_CONTROL_INVOCATION(self));
	error = g_error_new_literal(GSURF_CONTROL_ERROR, code, message != NULL ? message : "failed");
	gsurf_control_invocation_return_gerror(self, error);
}

void
gsurf_control_invocation_return_error(GsurfControlInvocation *self, GsurfControlError code,
                                      const gchar *format, ...)
{
	g_autofree gchar *message = NULL;
	va_list ap;

	g_return_if_fail(GSURF_IS_CONTROL_INVOCATION(self));
	va_start(ap, format);
	message = g_strdup_vprintf(format, ap);
	va_end(ap);
	gsurf_control_invocation_return_error_literal(self, code, message);
}

/* ------------------------------------------------------------------ */
/* Calls                                                               */
/* ------------------------------------------------------------------ */

/* Runs one request object @node of @r. */
static void
handle_call(GsurfControlServer *self, Request *r, JsonNode *node)
{
	GsurfControlInvocation *inv;
	JsonObject *object;
	JsonNode *id = NULL;
	JsonNode *params;
	const gchar *method_name;
	Method *method;

	/* 1. The shape: an object with a string "method"; an "id" member
	 *    (any value) makes it a call, its absence a notification. */
	if (!JSON_NODE_HOLDS_OBJECT(node)) {
		request_add_answer(r, error_response(NULL, CODE_INVALID_REQUEST,
			"a request must be a JSON object"));
		return;
	}
	object = json_node_get_object(node);
	if (json_object_has_member(object, "id"))
		id = json_object_get_member(object, "id");
	if (!json_object_has_member(object, "method") ||
	    !JSON_NODE_HOLDS_VALUE(json_object_get_member(object, "method")) ||
	    json_node_get_value_type(json_object_get_member(object, "method")) != G_TYPE_STRING) {
		request_add_answer(r, error_response(id, CODE_INVALID_REQUEST,
			"a request needs a string 'method'"));
		return;
	}
	method_name = json_object_get_string_member(object, "method");

	/* 2. Params: an object, or nothing. */
	params = json_object_has_member(object, "params")
		? json_object_get_member(object, "params") : NULL;
	if (params != NULL && !JSON_NODE_HOLDS_OBJECT(params) && !JSON_NODE_HOLDS_NULL(params)) {
		if (id != NULL)
			request_add_answer(r, error_response(id, CODE_INVALID_PARAMS,
				"'params' must be an object (named parameters)"));
		return;
	}

	/* 3. The method. */
	method = g_hash_table_lookup(self->methods, method_name);
	if (method == NULL) {
		if (id != NULL) {
			g_autofree gchar *message = g_strdup_printf(
				"unknown method '%s' (methods.list gives them)", method_name);

			request_add_answer(r, error_response(id, CODE_METHOD_NOT_FOUND, message));
		}
		return;
	}

	/* 4. The invocation; the server's reference goes with the answer. */
	inv = g_object_new(GSURF_TYPE_CONTROL_INVOCATION, NULL);
	inv->server = self;
	g_object_add_weak_pointer(G_OBJECT(self), (gpointer *)&inv->server);
	inv->method = g_strdup(method_name);
	inv->params = params != NULL && JSON_NODE_HOLDS_OBJECT(params)
		? json_object_ref(json_node_get_object(params)) : json_object_new();
	inv->id = id != NULL ? json_node_copy(id) : NULL;
	inv->notification = id == NULL;
	inv->request = request_ref(r);
	r->pending++;
	method->handler(self, inv, method->user_data);
}

/* Runs one request line for @conn or @task. */
static void
handle_line(GsurfControlServer *self, Conn *conn, GTask *task, const gchar *line)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonNode *root;
	Request *r;

	/* 1. Not JSON: a parse error (id null). */
	if (!json_parser_load_from_data(parser, line, -1, &error) ||
	    json_parser_get_root(parser) == NULL) {
		g_autofree gchar *message = g_strdup_printf("parse error: %s",
			error != NULL ? error->message : "empty line");

		r = request_new(conn, task, FALSE);
		request_add_answer(r, error_response(NULL, CODE_PARSE, message));
		r->issued = TRUE;
		request_maybe_finish(r);
		request_unref(r);
		return;
	}
	root = json_parser_get_root(parser);

	/* 2. A batch: every element, answered in one array. */
	if (JSON_NODE_HOLDS_ARRAY(root)) {
		JsonArray *array = json_node_get_array(root);
		guint i;

		r = request_new(conn, task, json_array_get_length(array) > 0);
		if (json_array_get_length(array) == 0)
			request_add_answer(r, error_response(NULL, CODE_INVALID_REQUEST,
				"an empty batch"));
		for (i = 0; i < json_array_get_length(array); i++)
			handle_call(self, r, json_array_get_element(array, i));
	} else {
		r = request_new(conn, task, FALSE);
		handle_call(self, r, root);
	}
	r->issued = TRUE;
	request_maybe_finish(r);
	request_unref(r);
}

void
gsurf_control_server_dispatch(GsurfControlServer *self, const gchar *request,
                              GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = NULL;

	g_return_if_fail(GSURF_IS_CONTROL_SERVER(self));
	g_return_if_fail(request != NULL);

	task = g_task_new(self, NULL, callback, user_data);
	g_task_set_source_tag(task, gsurf_control_server_dispatch);
	handle_line(self, NULL, task, request);
}

gchar *
gsurf_control_server_dispatch_finish(GsurfControlServer *self, GAsyncResult *result)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), NULL);
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	return g_task_propagate_pointer(G_TASK(result), NULL);
}

/* ------------------------------------------------------------------ */
/* Connections                                                         */
/* ------------------------------------------------------------------ */

static void
conn_unref(Conn *conn)
{
	if (!g_atomic_int_dec_and_test(&conn->refs))
		return;
	g_queue_clear_full(&conn->writes, (GDestroyNotify)g_bytes_unref);
	g_clear_object(&conn->in);
	g_clear_object(&conn->cancellable);
	g_clear_object(&conn->connection);
	g_clear_pointer(&conn->topics, g_hash_table_unref);
	g_free(conn);
}

static void on_written(GObject *source, GAsyncResult *result, gpointer user_data);

/* Starts writing the head of the queue unless a write is running. */
static void
conn_flush(Conn *conn)
{
	GBytes *head;
	gsize size;
	gconstpointer data;

	if (conn->writing || conn->closed || g_queue_is_empty(&conn->writes))
		return;
	head = g_queue_peek_head(&conn->writes);
	data = g_bytes_get_data(head, &size);
	conn->writing = TRUE;
	g_atomic_int_inc(&conn->refs);
	g_output_stream_write_all_async(conn->out, data, size, G_PRIORITY_DEFAULT,
		conn->cancellable, on_written, conn);
}

static void
on_written(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Conn *conn = user_data;
	g_autoptr(GError) error = NULL;

	conn->writing = FALSE;
	if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, &error)) {
		conn_close(conn);
		conn_unref(conn);
		return;
	}
	g_bytes_unref(g_queue_pop_head(&conn->writes));
	conn_flush(conn);
	conn_unref(conn);
}

/* Queues @line (and its newline) for @conn. */
static void
conn_send(Conn *conn, const gchar *line)
{
	if (conn->closed)
		return;
	g_queue_push_tail(&conn->writes, g_bytes_new_take(g_strconcat(line, "\n", NULL),
		strlen(line) + 1));
	conn_flush(conn);
}

/* Drops @conn from its server and closes the stream. */
static void
conn_close(Conn *conn)
{
	GsurfControlServer *server = conn->server;

	if (conn->closed)
		return;
	conn->closed = TRUE;
	conn->server = NULL;
	g_cancellable_cancel(conn->cancellable);
	g_io_stream_close(G_IO_STREAM(conn->connection), NULL, NULL);
	if (server != NULL)
		g_ptr_array_remove(server->conns, conn);  /* drops the list's ref */
}

static void on_line(GObject *source, GAsyncResult *result, gpointer user_data);

static void
conn_read(Conn *conn)
{
	if (conn->closed)
		return;
	g_atomic_int_inc(&conn->refs);
	g_data_input_stream_read_line_async(conn->in, G_PRIORITY_DEFAULT, conn->cancellable,
		on_line, conn);
}

static void
on_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Conn *conn = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *line = NULL;
	gsize length = 0;

	line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, &length,
		&error);

	/* 1. End of stream, an error or a cancelled read: the client is gone. */
	if (line == NULL || conn->closed || conn->server == NULL) {
		conn_close(conn);
		conn_unref(conn);
		return;
	}

	/* 2. An oversized line ends the connection; a blank one is skipped. */
	if (length > GSURF_CONTROL_MAX_LINE) {
		g_autoptr(JsonNode) answer = error_response(NULL, CODE_INVALID_REQUEST,
			"request line too long");
		g_autofree gchar *text = node_to_line(answer);

		conn_send(conn, text);
		conn_close(conn);
		conn_unref(conn);
		return;
	}
	g_strstrip(line);
	if (*line != '\0')
		handle_line(conn->server, conn, NULL, line);
	conn_read(conn);
	conn_unref(conn);
}

/* Whether the peer of @connection runs as this user. */
static gboolean
peer_is_us(GSocketConnection *connection)
{
	g_autoptr(GCredentials) creds = NULL;
	g_autoptr(GError) error = NULL;
	uid_t uid;

	creds = g_socket_get_credentials(g_socket_connection_get_socket(connection), &error);
	if (creds == NULL)
		return FALSE;
	uid = g_credentials_get_unix_user(creds, &error);
	return uid != (uid_t)-1 && uid == getuid();
}

static gboolean
on_incoming(GSocketService *service, GSocketConnection *connection, GObject *source,
            gpointer user_data)
{
	GsurfControlServer *self = user_data;
	Conn *conn;

	(void)service;
	(void)source;
	if (!self->running)
		return TRUE;
	if (!peer_is_us(connection)) {
		g_warning("gsurf control: refused a connection from another user");
		g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
		return TRUE;
	}
	conn = g_new0(Conn, 1);
	conn->refs = 1;                         /* the server's list */
	conn->server = self;
	conn->connection = g_object_ref(connection);
	conn->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
	g_data_input_stream_set_newline_type(conn->in, G_DATA_STREAM_NEWLINE_TYPE_ANY);
	conn->out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
	conn->cancellable = g_cancellable_new();
	g_queue_init(&conn->writes);
	g_ptr_array_add(self->conns, conn);
	conn_read(conn);
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

gboolean
gsurf_control_server_add_method(GsurfControlServer *self, const gchar *name,
                                const gchar *description, const gchar *params_doc,
                                GsurfControlMethodFunc handler, gpointer user_data,
                                GDestroyNotify destroy)
{
	Method *m;

	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), FALSE);
	g_return_val_if_fail(handler != NULL, FALSE);

	if (name == NULL || *name == '\0' || g_hash_table_contains(self->methods, name)) {
		if (name != NULL && *name != '\0')
			g_warning("gsurf control: method '%s' is already registered", name);
		if (destroy != NULL)
			destroy(user_data);
		return FALSE;
	}
	m = g_new0(Method, 1);
	m->name = g_strdup(name);
	m->description = g_strdup(description);
	m->params_doc = g_strdup(params_doc);
	m->from_provider = self->registering;
	m->provider = g_strdup(self->registering ? self->registering_name : NULL);
	m->handler = handler;
	m->user_data = user_data;
	m->destroy = destroy;
	g_hash_table_insert(self->methods, m->name, m);
	return TRUE;
}

gboolean
gsurf_control_server_remove_method(GsurfControlServer *self, const gchar *name)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), FALSE);
	return name != NULL && g_hash_table_remove(self->methods, name);
}

gboolean
gsurf_control_server_has_method(GsurfControlServer *self, const gchar *name)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), FALSE);
	return name != NULL && g_hash_table_contains(self->methods, name);
}

static gint
compare_names(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

gchar **
gsurf_control_server_list_methods(GsurfControlServer *self)
{
	GPtrArray *names;
	GHashTableIter iter;
	gpointer key;

	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), NULL);

	names = g_ptr_array_new();
	g_hash_table_iter_init(&iter, self->methods);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(names, g_strdup(key));
	g_ptr_array_sort(names, compare_names);
	g_ptr_array_add(names, NULL);
	return (gchar **)g_ptr_array_free(names, FALSE);
}

JsonNode *
gsurf_control_server_describe_methods(GsurfControlServer *self)
{
	g_auto(GStrv) names = NULL;
	g_autoptr(JsonBuilder) b = NULL;
	guint i;

	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), NULL);

	names = gsurf_control_server_list_methods(self);
	b = json_builder_new();
	json_builder_begin_array(b);
	for (i = 0; names[i] != NULL; i++) {
		Method *m = g_hash_table_lookup(self->methods, names[i]);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, m->name);
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, m->description != NULL ? m->description : "");
		json_builder_set_member_name(b, "params");
		json_builder_add_string_value(b, m->params_doc != NULL ? m->params_doc : "");
		json_builder_set_member_name(b, "provider");
		if (m->provider != NULL)
			json_builder_add_string_value(b, m->provider);
		else
			json_builder_add_null_value(b);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	return json_builder_get_root(b);
}

static gboolean
method_is_provided(gpointer key, gpointer value, gpointer user_data)
{
	(void)key;
	(void)user_data;
	return ((Method *)value)->from_provider;
}

void
gsurf_control_server_refresh_providers(GsurfControlServer *self)
{
	GPtrArray *modules;
	guint i;

	g_return_if_fail(GSURF_IS_CONTROL_SERVER(self));

	/* 1. Forget every provider's methods. */
	g_hash_table_foreach_remove(self->methods, method_is_provided, NULL);

	/* 2. Ask each active provider again; what they add is theirs. */
	modules = gsurf_module_manager_get_modules(gsurf_module_manager_get_default());
	for (i = 0; modules != NULL && i < modules->len; i++) {
		GsurfModule *module = g_ptr_array_index(modules, i);

		if (!gsurf_module_is_active(module) || !GSURF_IS_CONTROL_PROVIDER(module))
			continue;
		self->registering = TRUE;
		self->registering_name = g_strdup(gsurf_module_get_name(module));
		gsurf_control_provider_register_methods(GSURF_CONTROL_PROVIDER(module), self);
		self->registering = FALSE;
		g_clear_pointer(&self->registering_name, g_free);
	}
}

/* ------------------------------------------------------------------ */
/* Built-in methods                                                    */
/* ------------------------------------------------------------------ */

static void
method_methods_list(GsurfControlServer *self, GsurfControlInvocation *inv, gpointer data)
{
	(void)data;
	gsurf_control_invocation_return_value(inv, gsurf_control_server_describe_methods(self));
}

/* The topics array of a subscription answer. */
static JsonNode *
topics_node(Conn *conn)
{
	g_autoptr(JsonBuilder) b = json_builder_new();

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "subscribed");
	json_builder_add_boolean_value(b, conn->subscribed);
	json_builder_set_member_name(b, "topics");
	json_builder_begin_array(b);
	if (conn->topics == NULL) {
		guint i;

		for (i = 0; conn->subscribed && core_topics[i] != NULL; i++)
			json_builder_add_string_value(b, core_topics[i]);
	} else {
		g_autoptr(GList) keys = g_hash_table_get_keys(conn->topics);
		GList *l;

		keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
		for (l = keys; l != NULL; l = l->next)
			json_builder_add_string_value(b, l->data);
	}
	json_builder_end_array(b);
	json_builder_set_member_name(b, "all");
	json_builder_add_boolean_value(b, conn->subscribed && conn->topics == NULL);
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

static void
method_events_subscribe(GsurfControlServer *self, GsurfControlInvocation *inv, gpointer data)
{
	Conn *conn = inv->request->conn;
	JsonObject *params = inv->params;
	JsonArray *topics = NULL;
	guint i;

	(void)self;
	(void)data;
	if (conn == NULL || conn->closed) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_UNSUPPORTED,
			"events need a socket connection");
		return;
	}

	/* 1. topics: an array of strings; absent or empty = every topic. */
	if (json_object_has_member(params, "topics") &&
	    !JSON_NODE_HOLDS_NULL(json_object_get_member(params, "topics"))) {
		if (!JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "topics"))) {
			gsurf_control_invocation_return_error_literal(inv,
				GSURF_CONTROL_ERROR_INVALID_PARAMS, "'topics' must be an array of strings");
			return;
		}
		topics = json_object_get_array_member(params, "topics");
		for (i = 0; i < json_array_get_length(topics); i++) {
			JsonNode *t = json_array_get_element(topics, i);

			if (!JSON_NODE_HOLDS_VALUE(t) || json_node_get_value_type(t) != G_TYPE_STRING) {
				gsurf_control_invocation_return_error_literal(inv,
					GSURF_CONTROL_ERROR_INVALID_PARAMS,
					"'topics' must be an array of strings");
				return;
			}
		}
	}

	/* 2. Replace the subscription. */
	conn->subscribed = TRUE;
	g_clear_pointer(&conn->topics, g_hash_table_unref);
	if (topics != NULL && json_array_get_length(topics) > 0) {
		conn->topics = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		for (i = 0; i < json_array_get_length(topics); i++)
			g_hash_table_add(conn->topics,
				g_strdup(json_array_get_string_element(topics, i)));
	}
	gsurf_control_invocation_return_value(inv, topics_node(conn));
}

static void
method_events_unsubscribe(GsurfControlServer *self, GsurfControlInvocation *inv, gpointer data)
{
	Conn *conn = inv->request->conn;

	(void)self;
	(void)data;
	if (conn == NULL || conn->closed) {
		gsurf_control_invocation_return_error_literal(inv, GSURF_CONTROL_ERROR_UNSUPPORTED,
			"events need a socket connection");
		return;
	}
	conn->subscribed = FALSE;
	g_clear_pointer(&conn->topics, g_hash_table_unref);
	gsurf_control_invocation_return_value(inv, topics_node(conn));
}

const gchar *const *
gsurf_control_server_list_topics(void)
{
	return core_topics;
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

void
gsurf_control_server_emit_event(GsurfControlServer *self, const gchar *topic,
                                const gchar *event, JsonNode *data)
{
	g_autoptr(JsonBuilder) b = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *line = NULL;
	guint i;
	gboolean any = FALSE;

	g_return_if_fail(GSURF_IS_CONTROL_SERVER(self));
	g_return_if_fail(topic != NULL && event != NULL);

	/* 1. Anyone listening? */
	for (i = 0; self->running && i < self->conns->len && !any; i++) {
		Conn *conn = g_ptr_array_index(self->conns, i);

		any = conn->subscribed &&
			(conn->topics == NULL || g_hash_table_contains(conn->topics, topic));
	}
	if (!any) {
		if (data != NULL)
			json_node_unref(data);
		return;
	}

	/* 2. One notification line for all of them. */
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "jsonrpc");
	json_builder_add_string_value(b, "2.0");
	json_builder_set_member_name(b, "method");
	json_builder_add_string_value(b, "event");
	json_builder_set_member_name(b, "params");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "topic");
	json_builder_add_string_value(b, topic);
	json_builder_set_member_name(b, "event");
	json_builder_add_string_value(b, event);
	json_builder_set_member_name(b, "time");
	json_builder_add_int_value(b, g_get_monotonic_time());
	json_builder_set_member_name(b, "data");
	if (data != NULL)
		json_builder_add_value(b, data);
	else
		json_builder_add_null_value(b);
	json_builder_end_object(b);
	json_builder_end_object(b);
	root = json_builder_get_root(b);
	line = node_to_line(root);

	/* 3. Send it (a copy of the list: a failing write may close one). */
	{
		g_autoptr(GPtrArray) targets = g_ptr_array_new();

		for (i = 0; i < self->conns->len; i++) {
			Conn *conn = g_ptr_array_index(self->conns, i);

			if (conn->subscribed &&
			    (conn->topics == NULL || g_hash_table_contains(conn->topics, topic)))
				g_ptr_array_add(targets, conn);
		}
		for (i = 0; i < targets->len; i++)
			conn_send(g_ptr_array_index(targets, i), line);
	}
}

/* {"view": id, "uri": .., "title": ..} */
static JsonNode *
view_data(GsurfView *view)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	const gchar *uri = gsurf_view_get_uri(view);
	const gchar *title = gsurf_view_get_title(view);

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "view");
	json_builder_add_int_value(b, (gint64)gsurf_view_get_id(view));
	json_builder_set_member_name(b, "uri");
	json_builder_add_string_value(b, uri != NULL ? uri : "");
	json_builder_set_member_name(b, "title");
	json_builder_add_string_value(b, title != NULL ? title : "");
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

static void
on_view_uri(GsurfView *view, const gchar *uri, gpointer user_data)
{
	(void)uri;
	gsurf_control_server_emit_event(GSURF_CONTROL_SERVER(user_data), "uri", "uri",
		view_data(view));
}

static void
on_view_title(GsurfView *view, const gchar *title, gpointer user_data)
{
	(void)title;
	gsurf_control_server_emit_event(GSURF_CONTROL_SERVER(user_data), "title", "title",
		view_data(view));
}

static void
on_view_load(GsurfView *view, GsurfLoadEvent event, gpointer user_data)
{
	JsonNode *data = view_data(view);
	GEnumClass *klass = g_type_class_ref(GSURF_TYPE_LOAD_EVENT);
	GEnumValue *value = g_enum_get_value(klass, event);

	json_object_set_string_member(json_node_get_object(data), "state",
		value != NULL ? value->value_nick : "unknown");
	g_type_class_unref(klass);
	gsurf_control_server_emit_event(GSURF_CONTROL_SERVER(user_data), "load", "load", data);
}

static void
watch_view(GsurfControlServer *self, GsurfView *view)
{
	g_signal_connect_object(view, "uri-changed", G_CALLBACK(on_view_uri), self, 0);
	g_signal_connect_object(view, "title-changed", G_CALLBACK(on_view_title), self, 0);
	g_signal_connect_object(view, "load-changed", G_CALLBACK(on_view_load), self, 0);
}

static void
unwatch_view(GsurfControlServer *self, GsurfView *view)
{
	g_signal_handlers_disconnect_by_data(view, self);
}

static void
on_view_added(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	GsurfControlServer *self = user_data;

	(void)window;
	watch_view(self, view);
	gsurf_control_server_emit_event(self, "view", "view.added", view_data(view));
}

static void
on_view_removed(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	GsurfControlServer *self = user_data;

	(void)window;
	unwatch_view(self, view);
	gsurf_control_server_emit_event(self, "view", "view.removed", view_data(view));
}

static void
on_active_view(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	GsurfControlServer *self = user_data;

	(void)window;
	if (view != NULL)
		gsurf_control_server_emit_event(self, "view", "view.active", view_data(view));
}

static void
on_view_moved(GsurfWindow *window, GsurfView *view, guint from, guint to, gpointer user_data)
{
	JsonNode *data = view_data(view);

	(void)window;
	json_object_set_int_member(json_node_get_object(data), "from", from);
	json_object_set_int_member(json_node_get_object(data), "to", to);
	gsurf_control_server_emit_event(GSURF_CONTROL_SERVER(user_data), "view", "view.moved",
		data);
}

/* {"what": @what} */
static JsonNode *
layout_data(const gchar *what)
{
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();

	json_object_set_string_member(object, "what", what);
	json_node_take_object(node, object);
	return node;
}

static void
on_visible_views(GsurfWindow *window, gpointer user_data)
{
	(void)window;
	gsurf_control_server_emit_event(GSURF_CONTROL_SERVER(user_data), "layout",
		"layout.visible", layout_data("visible"));
}

static void
on_layout_groups(GsurfWindow *window, gpointer user_data)
{
	(void)window;
	gsurf_control_server_emit_event(GSURF_CONTROL_SERVER(user_data), "layout",
		"layout.groups", layout_data("groups"));
}

static void
note_focus(GsurfControlServer *self)
{
	self->focused_at = g_get_monotonic_time();
}

static gboolean
on_key_or_button(GsurfWindow *window, guint a, guint b, gpointer user_data)
{
	(void)window;
	(void)a;
	(void)b;
	note_focus(GSURF_CONTROL_SERVER(user_data));
	return FALSE;
}

static gboolean
on_key(GsurfWindow *window, guint keyval, guint keycode, guint state, gpointer user_data)
{
	(void)keycode;
	return on_key_or_button(window, keyval, state, user_data);
}

/* The native toplevel's GtkWindow:is-active (read generically, so the
 * core links no toolkit). */
static void
on_native_active(GObject *native, GParamSpec *pspec, gpointer user_data)
{
	gboolean active = FALSE;

	(void)pspec;
	g_object_get(native, "is-active", &active, NULL);
	if (active)
		note_focus(GSURF_CONTROL_SERVER(user_data));
}

static void
watch_window(GsurfControlServer *self, GsurfWindow *window)
{
	gpointer native;
	guint i;

	if (g_ptr_array_find(self->windows, window, NULL))
		return;
	g_ptr_array_add(self->windows, g_object_ref(window));
	g_signal_connect_object(window, "view-added", G_CALLBACK(on_view_added), self, 0);
	g_signal_connect_object(window, "view-removed", G_CALLBACK(on_view_removed), self, 0);
	g_signal_connect_object(window, "active-view-changed", G_CALLBACK(on_active_view), self,
		0);
	g_signal_connect_object(window, "view-moved", G_CALLBACK(on_view_moved), self, 0);
	g_signal_connect_object(window, "visible-views-changed", G_CALLBACK(on_visible_views),
		self, 0);
	g_signal_connect_object(window, "layout-groups-changed", G_CALLBACK(on_layout_groups),
		self, 0);
	g_signal_connect_object(window, "key-press", G_CALLBACK(on_key), self, 0);
	g_signal_connect_object(window, "button-press", G_CALLBACK(on_key_or_button), self, 0);
	native = gsurf_window_get_native_widget(window);
	if (native != NULL && G_IS_OBJECT(native) &&
	    g_object_class_find_property(G_OBJECT_GET_CLASS(native), "is-active") != NULL)
		g_signal_connect_object(native, "notify::is-active", G_CALLBACK(on_native_active),
			self, 0);
	for (i = 0; i < gsurf_window_get_n_views(window); i++)
		watch_view(self, gsurf_window_get_nth_view(window, i));
}

static void
unwatch_window(GsurfControlServer *self, GsurfWindow *window)
{
	gpointer native;
	guint i;
	guint index;

	if (!g_ptr_array_find(self->windows, window, &index))
		return;
	for (i = 0; i < gsurf_window_get_n_views(window); i++)
		unwatch_view(self, gsurf_window_get_nth_view(window, i));
	native = gsurf_window_get_native_widget(window);
	if (native != NULL && G_IS_OBJECT(native))
		g_signal_handlers_disconnect_by_data(native, self);
	g_signal_handlers_disconnect_by_data(window, self);
	g_ptr_array_remove_index(self->windows, index);
}

static void
on_window_added(GsurfApplication *app, GsurfWindow *window, gpointer user_data)
{
	(void)app;
	watch_window(GSURF_CONTROL_SERVER(user_data), window);
}

static void
on_window_removed(GsurfApplication *app, GsurfWindow *window, gpointer user_data)
{
	(void)app;
	unwatch_window(GSURF_CONTROL_SERVER(user_data), window);
}

static void
on_mode_changed(GsurfModuleManager *mgr, GsurfModePolicy mode, gpointer user_data)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	GEnumClass *klass = g_type_class_ref(GSURF_TYPE_MODE_POLICY);
	GEnumValue *value = g_enum_get_value(klass, mode);

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "mode");
	json_builder_add_string_value(b, value != NULL ? value->value_nick : "normal");
	json_builder_set_member_name(b, "label");
	json_builder_add_string_value(b, gsurf_mode_policy_get_label(mode));
	json_builder_set_member_name(b, "passthrough");
	json_builder_add_boolean_value(b, gsurf_module_manager_get_input_passthrough(mgr));
	json_builder_end_object(b);
	g_type_class_unref(klass);
	gsurf_control_server_emit_event(GSURF_CONTROL_SERVER(user_data), "mode", "mode",
		json_builder_get_root(b));
}

static void
add_string_or_null(JsonBuilder *b, const gchar *name, const gchar *value)
{
	json_builder_set_member_name(b, name);
	if (value != NULL)
		json_builder_add_string_value(b, value);
	else
		json_builder_add_null_value(b);
}

static void
on_download_started(GsurfModuleManager *mgr, const gchar *uri, const gchar *suggested,
                    const gchar *destination, gpointer user_data)
{
	g_autoptr(JsonBuilder) b = json_builder_new();

	(void)mgr;
	json_builder_begin_object(b);
	add_string_or_null(b, "uri", uri);
	add_string_or_null(b, "suggested", suggested);
	add_string_or_null(b, "destination", destination);
	json_builder_end_object(b);
	gsurf_control_server_emit_event(GSURF_CONTROL_SERVER(user_data), "download",
		"download.started", json_builder_get_root(b));
}

/* Follows @app's windows (and the module manager) for events. */
static void
watch_all(GsurfControlServer *self)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfApplication *app = gsurf_control_server_get_application(self);
	GPtrArray *windows;
	guint i;

	g_signal_connect_object(mgr, "mode-changed", G_CALLBACK(on_mode_changed), self, 0);
	g_signal_connect_object(mgr, "download-started", G_CALLBACK(on_download_started), self,
		0);
	if (app == NULL)
		return;
	self->watched_app = app;
	g_object_add_weak_pointer(G_OBJECT(app), (gpointer *)&self->watched_app);
	g_signal_connect_object(app, "window-added", G_CALLBACK(on_window_added), self, 0);
	g_signal_connect_object(app, "window-removed", G_CALLBACK(on_window_removed), self, 0);
	windows = gsurf_application_get_windows(app);
	for (i = 0; windows != NULL && i < windows->len; i++)
		watch_window(self, g_ptr_array_index(windows, i));
}

static void
unwatch_all(GsurfControlServer *self)
{
	g_signal_handlers_disconnect_by_data(gsurf_module_manager_get_default(), self);
	while (self->windows->len > 0)
		unwatch_window(self, g_ptr_array_index(self->windows, self->windows->len - 1));
	if (self->watched_app != NULL) {
		g_signal_handlers_disconnect_by_data(self->watched_app, self);
		g_object_remove_weak_pointer(G_OBJECT(self->watched_app),
			(gpointer *)&self->watched_app);
		self->watched_app = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* The server                                                          */
/* ------------------------------------------------------------------ */

GsurfApplication *
gsurf_control_server_get_application(GsurfControlServer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), NULL);
	if (self->app != NULL)
		return self->app;
	return gsurf_module_manager_get_application(gsurf_module_manager_get_default());
}

GsurfWindow *
gsurf_control_server_get_window(GsurfControlServer *self)
{
	GsurfApplication *app = gsurf_control_server_get_application(self);

	return app != NULL ? gsurf_application_get_active_window(app) : NULL;
}

/* A name of letters, digits, '-', '_' and '.', not starting with '.'. */
static gboolean
name_is_valid(const gchar *name)
{
	const gchar *p;

	if (name == NULL || *name == '\0' || *name == '.' || strlen(name) > 64)
		return FALSE;
	for (p = name; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_' && *p != '.')
			return FALSE;
	return TRUE;
}

gboolean
gsurf_control_server_set_name(GsurfControlServer *self, const gchar *name)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), FALSE);
	if (name != NULL && !name_is_valid(name))
		return FALSE;
	g_free(self->name);
	self->name = g_strdup(name);
	return TRUE;
}

const gchar *
gsurf_control_server_get_name(GsurfControlServer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), NULL);
	return self->name;
}

/* Keeps `<base>/<name>.sock` pointing at our socket, unless a live
 * instance holds the name. */
static void
link_name(GsurfControlServer *self)
{
	g_autofree gchar *base = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *target = NULL;

	if (self->name == NULL)
		return;
	base = g_build_filename(g_get_user_runtime_dir(), "gsurf", NULL);
	if (g_mkdir_with_parents(base, 0700) != 0)
		return;
	file = g_strconcat(self->name, ".sock", NULL);
	link = g_build_filename(base, file, NULL);

	/* 1. Someone has the name: keep it if they are alive. */
	target = g_file_read_link(link, NULL);
	if (target != NULL && g_strcmp0(target, self->path) != 0) {
		g_autofree gchar *dir = g_path_get_dirname(target);

		if (gsurf_runtime_dir_is_live(dir) && g_file_test(target, G_FILE_TEST_EXISTS)) {
			g_warning("gsurf control: the name '%s' belongs to a running instance (%s)",
				self->name, target);
			return;
		}
	}

	/* 2. Take it. */
	if (g_unlink(link) != 0 && errno != ENOENT)
		return;
	if (symlink(self->path, link) != 0) {
		g_warning("gsurf control: cannot link %s: %s", link, g_strerror(errno));
		return;
	}
	self->link_path = g_steal_pointer(&link);
}

static void
unlink_name(GsurfControlServer *self)
{
	g_autofree gchar *target = NULL;

	if (self->link_path == NULL)
		return;
	target = g_file_read_link(self->link_path, NULL);
	if (g_strcmp0(target, self->path) == 0)
		(void)g_unlink(self->link_path);
	g_clear_pointer(&self->link_path, g_free);
}

gboolean
gsurf_control_server_start(GsurfControlServer *self, const gchar *path, GError **error)
{
	g_autoptr(GSocketAddress) address = NULL;
	g_autofree gchar *socket_path = NULL;
	GStatBuf st;

	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), FALSE);
	if (self->running)
		return TRUE;

	/* 1. The path: given, or control.sock in the private runtime dir. */
	if (path != NULL) {
		socket_path = g_strdup(path);
	} else {
		const gchar *dir = gsurf_runtime_dir_get(error);

		if (dir == NULL)
			return FALSE;
		socket_path = g_build_filename(dir, GSURF_CONTROL_SOCKET_NAME, NULL);
	}

	/* 2. A stale socket (or file) there goes; a directory does not. */
	if (g_lstat(socket_path, &st) == 0) {
		if (S_ISDIR(st.st_mode) || g_unlink(socket_path) != 0) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
				"cannot replace %s", socket_path);
			return FALSE;
		}
	}

	/* 3. Listen, owner-only. */
	self->service = g_socket_service_new();
	address = g_unix_socket_address_new(socket_path);
	if (!g_socket_listener_add_address(G_SOCKET_LISTENER(self->service), address,
			G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, error)) {
		g_clear_object(&self->service);
		return FALSE;
	}
	if (g_chmod(socket_path, 0600) != 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			"cannot restrict %s: %s", socket_path, g_strerror(errno));
		g_socket_listener_close(G_SOCKET_LISTENER(self->service));
		g_clear_object(&self->service);
		(void)g_unlink(socket_path);
		return FALSE;
	}
	g_signal_connect(self->service, "incoming", G_CALLBACK(on_incoming), self);
	self->path = g_steal_pointer(&socket_path);
	self->running = TRUE;
	self->focused_at = g_get_monotonic_time();
	g_socket_service_start(self->service);

	/* 4. The name, the providers' methods and the event sources. */
	link_name(self);
	gsurf_control_server_refresh_providers(self);
	watch_all(self);
	return TRUE;
}

void
gsurf_control_server_stop(GsurfControlServer *self)
{
	g_return_if_fail(GSURF_IS_CONTROL_SERVER(self));
	if (!self->running)
		return;
	self->running = FALSE;

	/* 1. Every connection (closing removes it from the list). */
	while (self->conns->len > 0)
		conn_close(g_ptr_array_index(self->conns, self->conns->len - 1));

	/* 2. The listener, the socket file and the name. */
	g_signal_handlers_disconnect_by_data(self->service, self);
	g_socket_service_stop(self->service);
	g_socket_listener_close(G_SOCKET_LISTENER(self->service));
	g_clear_object(&self->service);
	unlink_name(self);
	(void)g_unlink(self->path);
	g_clear_pointer(&self->path, g_free);

	/* 3. The event sources. */
	unwatch_all(self);
}

gboolean
gsurf_control_server_is_running(GsurfControlServer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), FALSE);
	return self->running;
}

const gchar *
gsurf_control_server_get_socket_path(GsurfControlServer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), NULL);
	return self->path;
}

guint
gsurf_control_server_get_n_clients(GsurfControlServer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), 0);
	return self->conns->len;
}

gint64
gsurf_control_server_get_focused_at(GsurfControlServer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_SERVER(self), 0);
	return self->focused_at;
}

static void
conn_list_free(gpointer data)
{
	conn_unref(data);
}

static void
gsurf_control_server_dispose(GObject *object)
{
	GsurfControlServer *self = GSURF_CONTROL_SERVER(object);

	gsurf_control_server_stop(self);
	if (self->app != NULL) {
		g_object_remove_weak_pointer(G_OBJECT(self->app), (gpointer *)&self->app);
		self->app = NULL;
	}
	G_OBJECT_CLASS(gsurf_control_server_parent_class)->dispose(object);
}

static void
gsurf_control_server_finalize(GObject *object)
{
	GsurfControlServer *self = GSURF_CONTROL_SERVER(object);

	g_clear_pointer(&self->methods, g_hash_table_unref);
	g_clear_pointer(&self->conns, g_ptr_array_unref);
	g_clear_pointer(&self->windows, g_ptr_array_unref);
	g_free(self->name);
	g_free(self->registering_name);
	G_OBJECT_CLASS(gsurf_control_server_parent_class)->finalize(object);
}

static void
gsurf_control_server_class_init(GsurfControlServerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = gsurf_control_server_dispose;
	object_class->finalize = gsurf_control_server_finalize;
}

static void
gsurf_control_server_init(GsurfControlServer *self)
{
	self->methods = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, method_free);
	self->conns = g_ptr_array_new_with_free_func(conn_list_free);
	self->windows = g_ptr_array_new_with_free_func(g_object_unref);
}

GsurfControlServer *
gsurf_control_server_new(GsurfApplication *app)
{
	GsurfControlServer *self;

	g_return_val_if_fail(app == NULL || GSURF_IS_APPLICATION(app), NULL);

	self = g_object_new(GSURF_TYPE_CONTROL_SERVER, NULL);
	if (app != NULL) {
		self->app = app;
		g_object_add_weak_pointer(G_OBJECT(app), (gpointer *)&self->app);
	}

	/* The built-in methods, then the core set. */
	gsurf_control_server_add_method(self, "methods.list",
		"Every method with its description, parameters and provider (null = core)",
		"{}", method_methods_list, NULL, NULL);
	gsurf_control_server_add_method(self, "events.subscribe",
		"Stream events on this connection as 'event' notifications; returns the "
		"subscription. Topics: view, uri, title, load, mode, layout, download (and any a "
		"module emits)", "{topics?: [string] (default: all)}",
		method_events_subscribe, NULL, NULL);
	gsurf_control_server_add_method(self, "events.unsubscribe",
		"Stop the event stream on this connection", "{}",
		method_events_unsubscribe, NULL, NULL);
	gsurf_control_methods_register(self);
	return self;
}
