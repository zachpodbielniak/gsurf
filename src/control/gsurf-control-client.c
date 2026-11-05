/*
 * gsurf-control-client.c - Find running gsurf instances and talk to them
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Instances: a scan of `$XDG_RUNTIME_DIR/gsurf` for live process
 * directories with a control socket, plus the `<name>.sock` symlinks.
 *
 * The client reads the socket itself (a byte buffer split at newlines,
 * g_socket_condition_timed_wait() for the timeouts) instead of a
 * GDataInputStream, so a call can give up after its timeout and a later
 * read still finds the rest of the line. Event notifications that arrive
 * while a call waits for its response are queued for
 * gsurf_control_client_read_event().
 */

#include "control/gsurf-control-client.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

#include "util/gsurf-runtime-dir.h"

/* ------------------------------------------------------------------ */
/* Instances                                                           */
/* ------------------------------------------------------------------ */

struct _GsurfControlInstance
{
	gint    refs;
	gint64  pid;
	gchar  *id;
	gchar  *dir;
	gchar  *socket;
	gchar  *name;
};

G_DEFINE_BOXED_TYPE(GsurfControlInstance, gsurf_control_instance, gsurf_control_instance_ref,
                    gsurf_control_instance_unref)

GsurfControlInstance *
gsurf_control_instance_ref(GsurfControlInstance *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->refs);
	return self;
}

void
gsurf_control_instance_unref(GsurfControlInstance *self)
{
	g_return_if_fail(self != NULL);
	if (!g_atomic_int_dec_and_test(&self->refs))
		return;
	g_free(self->id);
	g_free(self->dir);
	g_free(self->socket);
	g_free(self->name);
	g_free(self);
}

gint64
gsurf_control_instance_get_pid(GsurfControlInstance *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->pid;
}

const gchar *
gsurf_control_instance_get_id(GsurfControlInstance *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->id;
}

const gchar *
gsurf_control_instance_get_runtime_dir(GsurfControlInstance *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->dir;
}

const gchar *
gsurf_control_instance_get_socket_path(GsurfControlInstance *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->socket;
}

const gchar *
gsurf_control_instance_get_name(GsurfControlInstance *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->name;
}

gboolean
gsurf_control_instance_matches(GsurfControlInstance *self, const gchar *spec)
{
	gchar *end = NULL;
	gint64 pid;

	g_return_val_if_fail(self != NULL, FALSE);
	if (spec == NULL || *spec == '\0')
		return FALSE;
	if (g_strcmp0(spec, self->name) == 0 || g_strcmp0(spec, self->id) == 0)
		return TRUE;
	pid = g_ascii_strtoll(spec, &end, 10);
	return end != NULL && *end == '\0' && pid == self->pid;
}

gchar *
gsurf_control_get_base_dir(void)
{
	return g_build_filename(g_get_user_runtime_dir(), "gsurf", NULL);
}

static gint
compare_instances(gconstpointer a, gconstpointer b)
{
	const GsurfControlInstance *x = *(GsurfControlInstance *const *)a;
	const GsurfControlInstance *y = *(GsurfControlInstance *const *)b;

	if (x->pid != y->pid)
		return x->pid < y->pid ? -1 : 1;
	return g_strcmp0(x->id, y->id);
}

GPtrArray *
gsurf_control_list_instances(const gchar *base)
{
	g_autofree gchar *scan = base != NULL ? g_strdup(base) : gsurf_control_get_base_dir();
	GPtrArray *list = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gsurf_control_instance_unref);
	g_autoptr(GDir) dir = NULL;
	const gchar *entry;
	guint i;

	dir = g_dir_open(scan, 0, NULL);
	if (dir == NULL)
		return list;

	/* 1. Live process directories with a socket. */
	while ((entry = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *socket = NULL;
		GsurfControlInstance *inst;

		if (!gsurf_runtime_dir_is_process_dir_name(entry))
			continue;
		path = g_build_filename(scan, entry, NULL);
		socket = g_build_filename(path, GSURF_CONTROL_SOCKET_NAME, NULL);
		if (!g_file_test(socket, G_FILE_TEST_EXISTS) || !gsurf_runtime_dir_is_live(path))
			continue;
		inst = g_new0(GsurfControlInstance, 1);
		inst->refs = 1;
		inst->pid = g_ascii_strtoll(entry, NULL, 10);
		inst->id = g_strdup(entry);
		inst->dir = g_steal_pointer(&path);
		inst->socket = g_steal_pointer(&socket);
		g_ptr_array_add(list, inst);
	}

	/* 2. Names: "<name>.sock" symlinks pointing at one of the sockets. */
	g_dir_rewind(dir);
	while ((entry = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *link = NULL;
		g_autofree gchar *target = NULL;

		if (!g_str_has_suffix(entry, ".sock") || strlen(entry) <= strlen(".sock"))
			continue;
		link = g_build_filename(scan, entry, NULL);
		target = g_file_read_link(link, NULL);
		if (target == NULL)
			continue;
		for (i = 0; i < list->len; i++) {
			GsurfControlInstance *inst = g_ptr_array_index(list, i);

			if (inst->name == NULL && g_strcmp0(inst->socket, target) == 0)
				inst->name = g_strndup(entry, strlen(entry) - strlen(".sock"));
		}
	}
	g_ptr_array_sort(list, compare_instances);
	return list;
}

GsurfControlInstance *
gsurf_control_find_instance(const gchar *base, const gchar *spec, GError **error)
{
	g_autoptr(GPtrArray) list = gsurf_control_list_instances(base);
	guint i;

	for (i = 0; spec != NULL && i < list->len; i++)
		if (gsurf_control_instance_matches(g_ptr_array_index(list, i), spec))
			return gsurf_control_instance_ref(g_ptr_array_index(list, i));
	g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_NOT_FOUND,
		"no running gsurf instance '%s'", spec != NULL ? spec : "");
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Client                                                              */
/* ------------------------------------------------------------------ */

struct _GsurfControlClient
{
	GObject parent_instance;

	GSocketConnection *connection;
	GSocket           *socket;      /* borrowed from the connection */
	GString           *buffer;      /* bytes read past the last line */
	GQueue             events;      /* JsonNode* params of queued events */
	guint              timeout_ms;
	gint64             next_id;
	GMutex             lock;
};

G_DEFINE_FINAL_TYPE(GsurfControlClient, gsurf_control_client, G_TYPE_OBJECT)

static void
gsurf_control_client_finalize(GObject *object)
{
	GsurfControlClient *self = GSURF_CONTROL_CLIENT(object);

	gsurf_control_client_close(self);
	g_string_free(self->buffer, TRUE);
	g_mutex_clear(&self->lock);
	G_OBJECT_CLASS(gsurf_control_client_parent_class)->finalize(object);
}

static void
gsurf_control_client_class_init(GsurfControlClientClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gsurf_control_client_finalize;
}

static void
gsurf_control_client_init(GsurfControlClient *self)
{
	self->buffer = g_string_new(NULL);
	g_queue_init(&self->events);
	self->timeout_ms = 30000;
	g_mutex_init(&self->lock);
}

GsurfControlClient *
gsurf_control_client_new(void)
{
	return g_object_new(GSURF_TYPE_CONTROL_CLIENT, NULL);
}

void
gsurf_control_client_close(GsurfControlClient *self)
{
	g_return_if_fail(GSURF_IS_CONTROL_CLIENT(self));
	if (self->connection != NULL)
		g_io_stream_close(G_IO_STREAM(self->connection), NULL, NULL);
	g_clear_object(&self->connection);
	self->socket = NULL;
	g_string_truncate(self->buffer, 0);
	g_queue_clear_full(&self->events, (GDestroyNotify)json_node_unref);
}

gboolean
gsurf_control_client_connect(GsurfControlClient *self, const gchar *path, GError **error)
{
	g_autoptr(GSocketClient) client = NULL;
	g_autoptr(GSocketAddress) address = NULL;

	g_return_val_if_fail(GSURF_IS_CONTROL_CLIENT(self), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	gsurf_control_client_close(self);
	client = g_socket_client_new();
	address = g_unix_socket_address_new(path);
	self->connection = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL,
		error);
	if (self->connection == NULL)
		return FALSE;
	self->socket = g_socket_connection_get_socket(self->connection);
	return TRUE;
}

gboolean
gsurf_control_client_is_connected(GsurfControlClient *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_CLIENT(self), FALSE);
	return self->connection != NULL;
}

void
gsurf_control_client_set_timeout(GsurfControlClient *self, guint timeout_ms)
{
	g_return_if_fail(GSURF_IS_CONTROL_CLIENT(self));
	self->timeout_ms = timeout_ms;
}

guint
gsurf_control_client_get_timeout(GsurfControlClient *self)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_CLIENT(self), 0);
	return self->timeout_ms;
}

/*
 * read_line:
 * @deadline: the monotonic time to give up at; G_MAXINT64 waits forever
 *
 * The next line. FALSE without @error set on a timeout, with @error set
 * when the connection failed or closed.
 */
static gboolean
read_line(GsurfControlClient *self, gint64 deadline, gchar **line, GError **error)
{
	gchar chunk[65536];

	*line = NULL;
	for (;;) {
		gchar *newline = memchr(self->buffer->str, '\n', self->buffer->len);
		g_autoptr(GError) local = NULL;
		gint64 wait;
		gssize n;

		/* 1. A whole line in the buffer. */
		if (newline != NULL) {
			gsize length = (gsize)(newline - self->buffer->str);

			*line = g_strndup(self->buffer->str, length);
			g_string_erase(self->buffer, 0, (gssize)length + 1);
			return TRUE;
		}
		if (self->socket == NULL) {
			g_set_error_literal(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_CLOSED,
				"not connected");
			return FALSE;
		}

		/* 2. Wait for more (bounded). */
		wait = deadline == G_MAXINT64 ? -1 : MAX(deadline - g_get_monotonic_time(), 0);
		if (!g_socket_condition_timed_wait(self->socket, G_IO_IN, wait, NULL, &local)) {
			if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
				return FALSE;
			g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_CLOSED,
				"connection failed: %s", local->message);
			return FALSE;
		}

		/* 3. Read what is there; 0 bytes is the end of the stream. */
		n = g_socket_receive(self->socket, chunk, sizeof(chunk), NULL, &local);
		if (n < 0) {
			if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
				continue;
			g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_CLOSED,
				"connection failed: %s", local->message);
			return FALSE;
		}
		if (n == 0) {
			g_set_error_literal(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_CLOSED,
				"the instance closed the connection");
			return FALSE;
		}
		g_string_append_len(self->buffer, chunk, n);
	}
}

/* Queues the params of an event notification; TRUE when @node was one. */
static gboolean
queue_event(GsurfControlClient *self, JsonNode *node)
{
	JsonObject *object;

	if (!JSON_NODE_HOLDS_OBJECT(node))
		return FALSE;
	object = json_node_get_object(node);
	if (json_object_has_member(object, "id") ||
	    g_strcmp0(json_object_get_string_member_with_default(object, "method", ""),
		      "event") != 0)
		return FALSE;
	if (json_object_has_member(object, "params"))
		g_queue_push_tail(&self->events, json_object_dup_member(object, "params"));
	return TRUE;
}

/* The result of response @object, or NULL with @error from its error. */
static JsonNode *
take_result(JsonObject *object, GError **error)
{
	if (json_object_has_member(object, "error")) {
		JsonNode *err = json_object_get_member(object, "error");
		gint64 code = -32000;
		const gchar *message = "error";

		if (JSON_NODE_HOLDS_OBJECT(err)) {
			JsonObject *e = json_node_get_object(err);

			code = json_object_get_int_member_with_default(e, "code", -32000);
			message = json_object_get_string_member_with_default(e, "message", "error");
		}
		g_set_error_literal(error, GSURF_CONTROL_ERROR,
			gsurf_control_error_from_code((gint)code), message);
		return NULL;
	}
	if (json_object_has_member(object, "result"))
		return json_object_dup_member(object, "result");
	return json_node_new(JSON_NODE_NULL);
}

/* Whether response @object answers request @id. */
static gboolean
answers(JsonObject *object, gint64 id)
{
	JsonNode *node;

	if (!json_object_has_member(object, "id"))
		return FALSE;
	node = json_object_get_member(object, "id");
	return JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 &&
		json_node_get_int(node) == id;
}

JsonNode *
gsurf_control_client_call(GsurfControlClient *self, const gchar *method, JsonObject *params,
                          GError **error)
{
	g_autoptr(JsonBuilder) b = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *framed = NULL;
	g_autoptr(GError) local = NULL;
	JsonNode *result = NULL;
	gint64 deadline;
	gint64 id;

	g_return_val_if_fail(GSURF_IS_CONTROL_CLIENT(self), NULL);
	g_return_val_if_fail(method != NULL, NULL);

	g_mutex_lock(&self->lock);
	if (self->connection == NULL) {
		g_mutex_unlock(&self->lock);
		g_set_error_literal(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_CLOSED,
			"not connected");
		return NULL;
	}

	/* 1. The request line. */
	id = ++self->next_id;
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "jsonrpc");
	json_builder_add_string_value(b, "2.0");
	json_builder_set_member_name(b, "id");
	json_builder_add_int_value(b, id);
	json_builder_set_member_name(b, "method");
	json_builder_add_string_value(b, method);
	if (params != NULL) {
		JsonNode *p = json_node_new(JSON_NODE_OBJECT);

		json_node_set_object(p, params);
		json_builder_set_member_name(b, "params");
		json_builder_add_value(b, p);
	}
	json_builder_end_object(b);
	request = json_builder_get_root(b);
	text = json_to_string(request, FALSE);
	framed = g_strconcat(text, "\n", NULL);
	if (!g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(self->connection)),
			framed, strlen(framed), NULL, NULL, &local)) {
		g_mutex_unlock(&self->lock);
		g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_CLOSED,
			"cannot send: %s", local->message);
		return NULL;
	}

	/* 2. Lines until our response (events go to the queue). */
	deadline = self->timeout_ms > 0
		? g_get_monotonic_time() + (gint64)self->timeout_ms * 1000 : G_MAXINT64;
	for (;;) {
		g_autofree gchar *line = NULL;
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonNode *root;

		if (!read_line(self, deadline, &line, &local)) {
			if (local != NULL)
				g_propagate_error(error, g_steal_pointer(&local));
			else
				g_set_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_TIMEOUT,
					"no reply to %s within %u ms", method, self->timeout_ms);
			break;
		}
		if (!json_parser_load_from_data(parser, line, -1, NULL) ||
		    (root = json_parser_get_root(parser)) == NULL)
			continue;
		if (queue_event(self, root) || !JSON_NODE_HOLDS_OBJECT(root) ||
		    !answers(json_node_get_object(root), id))
			continue;
		result = take_result(json_node_get_object(root), error);
		break;
	}
	g_mutex_unlock(&self->lock);
	return result;
}

typedef struct {
	gchar      *method;
	JsonObject *params;
} CallData;

static void
call_data_free(CallData *data)
{
	g_free(data->method);
	g_clear_pointer(&data->params, json_object_unref);
	g_free(data);
}

static void
call_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
	CallData *data = task_data;
	GError *error = NULL;
	JsonNode *result;

	(void)cancellable;
	result = gsurf_control_client_call(GSURF_CONTROL_CLIENT(source), data->method,
		data->params, &error);
	if (result == NULL)
		g_task_return_error(task, error);
	else
		g_task_return_pointer(task, result, (GDestroyNotify)json_node_unref);
}

void
gsurf_control_client_call_async(GsurfControlClient *self, const gchar *method,
                                JsonObject *params, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = NULL;
	CallData *data;

	g_return_if_fail(GSURF_IS_CONTROL_CLIENT(self));
	g_return_if_fail(method != NULL);

	data = g_new0(CallData, 1);
	data->method = g_strdup(method);
	if (params != NULL) {
		g_autoptr(JsonNode) wrap = json_node_new(JSON_NODE_OBJECT);
		g_autoptr(JsonNode) copy = NULL;

		json_node_set_object(wrap, params);
		copy = json_node_copy(wrap);
		data->params = json_object_ref(json_node_get_object(copy));
	}
	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, gsurf_control_client_call_async);
	g_task_set_task_data(task, data, (GDestroyNotify)call_data_free);
	g_task_run_in_thread(task, call_thread);
}

JsonNode *
gsurf_control_client_call_finish(GsurfControlClient *self, GAsyncResult *result,
                                 GError **error)
{
	g_return_val_if_fail(GSURF_IS_CONTROL_CLIENT(self), NULL);
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}

gboolean
gsurf_control_client_subscribe(GsurfControlClient *self, const gchar *const *topics,
                               GError **error)
{
	g_autoptr(JsonObject) params = json_object_new();
	g_autoptr(JsonNode) result = NULL;
	JsonArray *array = json_array_new();
	guint i;

	g_return_val_if_fail(GSURF_IS_CONTROL_CLIENT(self), FALSE);

	for (i = 0; topics != NULL && topics[i] != NULL; i++)
		json_array_add_string_element(array, topics[i]);
	json_object_set_array_member(params, "topics", array);
	result = gsurf_control_client_call(self, "events.subscribe", params, error);
	return result != NULL;
}

JsonNode *
gsurf_control_client_read_event(GsurfControlClient *self, guint timeout_ms, GError **error)
{
	gint64 deadline;
	JsonNode *event = NULL;

	g_return_val_if_fail(GSURF_IS_CONTROL_CLIENT(self), NULL);

	g_mutex_lock(&self->lock);
	deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	while (g_queue_is_empty(&self->events)) {
		g_autofree gchar *line = NULL;
		g_autoptr(JsonParser) parser = json_parser_new();

		if (!read_line(self, deadline, &line, error))
			break;
		if (json_parser_load_from_data(parser, line, -1, NULL) &&
		    json_parser_get_root(parser) != NULL)
			queue_event(self, json_parser_get_root(parser));
	}
	event = g_queue_pop_head(&self->events);
	g_mutex_unlock(&self->lock);
	return event;
}
