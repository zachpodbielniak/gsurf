/*
 * test-control.c - The control socket without a display
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Covers the parts of the control server and client that need no web
 * engine:
 *   - JSON-RPC framing through gsurf_control_server_dispatch(): parse
 *     errors, invalid requests, unknown methods, notifications, batches
 *     (mixed, all-notification, empty), non-object params;
 *   - the method registry (add, duplicate, remove, list, describe), an
 *     asynchronous handler and a double answer;
 *   - error codes both ways;
 *   - #GsurfControlProvider: a module's methods come and go with it;
 *   - the core methods without a window and in kiosk mode, config.get,
 *     modules.list / modules.toggle;
 *   - a real client against a server on a temporary socket (from a worker
 *     thread, the server on the main loop): calls, an error, events with
 *     topic filtering, a server stop seen as a closed connection, the
 *     socket mode, the socket removed after stop, finalization;
 *   - instance discovery on a fake runtime tree (held lock, stale lock,
 *     no socket, name symlinks) and the server's name symlink;
 *   - the `control:` config section.
 */

#include <gsurf.h>
#include <control/gsurf-control.h>

#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test-util.h"

static gchar *tmp_root;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
	gboolean done;
	gchar   *line;
} Answer;

static void
on_dispatched(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Answer *a = user_data;

	a->line = gsurf_control_server_dispatch_finish(GSURF_CONTROL_SERVER(source), result);
	a->done = TRUE;
}

/* The response line for @request (NULL for notifications only). */
static gchar *
dispatch(GsurfControlServer *server, const gchar *request)
{
	Answer a = { FALSE, NULL };
	guint i;

	gsurf_control_server_dispatch(server, request, on_dispatched, &a);
	for (i = 0; i < 2000 && !a.done; i++)
		g_main_context_iteration(NULL, i > 10);
	g_assert_true(a.done);
	return a.line;
}

static JsonNode *
parse(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;

	g_assert_nonnull(text);
	g_assert_true(json_parser_load_from_data(parser, text, -1, &error));
	g_assert_no_error(error);
	return json_node_copy(json_parser_get_root(parser));
}

/* The "error.code" of a response line, or 0. */
static gint64
error_code(const gchar *line)
{
	g_autoptr(JsonNode) node = parse(line);
	JsonObject *object = json_node_get_object(node);

	if (!json_object_has_member(object, "error"))
		return 0;
	return json_object_get_int_member(json_object_get_object_member(object, "error"), "code");
}

/* The "result" of a response line (asserting there is no error). */
static JsonNode *
result_of(const gchar *line)
{
	g_autoptr(JsonNode) node = parse(line);
	JsonObject *object = json_node_get_object(node);

	if (json_object_has_member(object, "error"))
		g_error("unexpected error: %s", line);
	return json_object_dup_member(object, "result");
}

/* Calls @method with @params_json on @server; returns the response. */
static gchar *
call(GsurfControlServer *server, const gchar *method, const gchar *params_json)
{
	g_autofree gchar *request = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"%s\"%s%s}", method,
		params_json != NULL ? ",\"params\":" : "", params_json != NULL ? params_json : "");

	return dispatch(server, request);
}

#define ASSERT_HAS(text, needle) \
	G_STMT_START { \
		if (strstr((text), (needle)) == NULL) \
			g_error("'%s' not in: %s", (needle), (text)); \
	} G_STMT_END

/* ------------------------------------------------------------------ */
/* Framing                                                             */
/* ------------------------------------------------------------------ */

static void
echo_method(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonNode *copy;
	g_autoptr(JsonNode) wrap = json_node_new(JSON_NODE_OBJECT);

	(void)server;
	(void)data;
	json_node_set_object(wrap, gsurf_control_invocation_get_params(inv));
	copy = json_node_copy(wrap);
	gsurf_control_invocation_return_value(inv, copy);
}

static guint notified;

static void
note_method(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	(void)server;
	(void)data;
	notified++;
	gsurf_control_invocation_return_value(inv, NULL);
}

static void
test_framing(void)
{
	g_autoptr(GsurfControlServer) server = gsurf_control_server_new(NULL);
	gchar *line;

	g_assert_true(gsurf_control_server_add_method(server, "test.echo", "echo", "{...}",
		echo_method, NULL, NULL));
	g_assert_true(gsurf_control_server_add_method(server, "test.note", NULL, NULL,
		note_method, NULL, NULL));

	/* 1. Not JSON: a parse error with id null. */
	line = dispatch(server, "{nope");
	g_assert_cmpint(error_code(line), ==, -32700);
	ASSERT_HAS(line, "\"id\":null");
	g_free(line);

	/* 2. Not a request. */
	line = dispatch(server, "42");
	g_assert_cmpint(error_code(line), ==, -32600);
	g_free(line);
	line = dispatch(server, "{\"jsonrpc\":\"2.0\",\"id\":1}");
	g_assert_cmpint(error_code(line), ==, -32600);
	ASSERT_HAS(line, "\"id\":1");
	g_free(line);

	/* 3. Unknown method; params that are not an object. */
	line = dispatch(server, "{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"no.such\"}");
	g_assert_cmpint(error_code(line), ==, -32601);
	ASSERT_HAS(line, "\"id\":\"a\"");
	g_free(line);
	line = dispatch(server, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"test.echo\","
		"\"params\":[1,2]}");
	g_assert_cmpint(error_code(line), ==, -32602);
	g_free(line);

	/* 4. A call answers with its id and result. */
	line = dispatch(server, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"test.echo\","
		"\"params\":{\"x\":1}}");
	ASSERT_HAS(line, "\"id\":3");
	ASSERT_HAS(line, "\"result\":{\"x\":1}");
	g_free(line);

	/* 5. Notifications run but answer nothing, even unknown ones. */
	notified = 0;
	g_assert_null(dispatch(server, "{\"jsonrpc\":\"2.0\",\"method\":\"test.note\"}"));
	g_assert_null(dispatch(server, "{\"jsonrpc\":\"2.0\",\"method\":\"no.such\"}"));
	g_assert_cmpuint(notified, ==, 1);

	/* 6. Batches: one array of the calls' answers; notifications left
	 *    out; all notifications answer nothing; empty is invalid. */
	line = dispatch(server, "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"test.echo\","
		"\"params\":{\"a\":1}},{\"jsonrpc\":\"2.0\",\"method\":\"test.note\"},"
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"no.such\"},7]");
	{
		g_autoptr(JsonNode) node = parse(line);

		g_assert_true(JSON_NODE_HOLDS_ARRAY(node));
		g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), ==, 3);
	}
	ASSERT_HAS(line, "\"result\":{\"a\":1}");
	ASSERT_HAS(line, "-32601");
	ASSERT_HAS(line, "-32600");
	g_free(line);
	g_assert_null(dispatch(server, "[{\"jsonrpc\":\"2.0\",\"method\":\"test.note\"}]"));
	line = dispatch(server, "[]");
	g_assert_cmpint(error_code(line), ==, -32600);
	g_free(line);

	/* 7. events.subscribe needs a socket. */
	line = call(server, "events.subscribe", NULL);
	g_assert_cmpint(error_code(line), ==, -32002);
	g_free(line);
}

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

static guint destroyed;

static void
count_destroy(gpointer data)
{
	(void)data;
	destroyed++;
}

static gboolean
answer_later(gpointer data)
{
	GsurfControlInvocation *inv = data;
	JsonNode *node = json_node_new(JSON_NODE_VALUE);

	json_node_set_string(node, "later");
	gsurf_control_invocation_return_value(inv, node);
	g_object_unref(inv);
	return G_SOURCE_REMOVE;
}

static void
async_method(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	(void)server;
	(void)data;
	g_assert_false(gsurf_control_invocation_get_answered(inv));
	g_timeout_add(20, answer_later, g_object_ref(inv));
}

static void
twice_method(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	(void)server;
	(void)data;
	g_object_ref(inv);
	gsurf_control_invocation_return_value(inv, NULL);
	g_assert_true(gsurf_control_invocation_get_answered(inv));
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*answered twice*");
	gsurf_control_invocation_return_value(inv, NULL);
	g_test_assert_expected_messages();
	g_object_unref(inv);
}

static void
test_registry(void)
{
	g_autoptr(GsurfControlServer) server = gsurf_control_server_new(NULL);
	g_auto(GStrv) names = NULL;
	g_autoptr(JsonNode) described = NULL;
	gchar *line;
	guint i;

	/* 1. The core set is there, sorted. */
	names = gsurf_control_server_list_methods(server);
	g_assert_true(g_strv_contains((const gchar *const *)names, "methods.list"));
	g_assert_true(g_strv_contains((const gchar *const *)names, "open"));
	g_assert_true(g_strv_contains((const gchar *const *)names, "layout.command"));
	g_assert_true(g_strv_contains((const gchar *const *)names, "tree.get"));
	for (i = 1; names[i] != NULL; i++)
		g_assert_cmpint(g_strcmp0(names[i - 1], names[i]), <, 0);

	/* 2. Add, refuse a duplicate (its data freed), remove. */
	destroyed = 0;
	g_assert_true(gsurf_control_server_add_method(server, "test.x", "d", "p", echo_method,
		GINT_TO_POINTER(1), count_destroy));
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*already registered*");
	g_assert_false(gsurf_control_server_add_method(server, "test.x", NULL, NULL, echo_method,
		GINT_TO_POINTER(2), count_destroy));
	g_test_assert_expected_messages();
	g_assert_cmpuint(destroyed, ==, 1);
	g_assert_false(gsurf_control_server_add_method(server, "", NULL, NULL, echo_method,
		NULL, count_destroy));
	g_assert_cmpuint(destroyed, ==, 2);
	g_assert_true(gsurf_control_server_has_method(server, "test.x"));
	g_assert_true(gsurf_control_server_remove_method(server, "test.x"));
	g_assert_cmpuint(destroyed, ==, 3);
	g_assert_false(gsurf_control_server_has_method(server, "test.x"));
	g_assert_false(gsurf_control_server_remove_method(server, "test.x"));

	/* 3. methods.list describes them (core methods: provider null). */
	described = gsurf_control_server_describe_methods(server);
	line = call(server, "methods.list", NULL);
	ASSERT_HAS(line, "\"name\":\"page.eval\"");
	ASSERT_HAS(line, "\"provider\":null");
	g_free(line);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(described)), ==,
		g_strv_length(names));

	/* 4. An answer that comes later; a second answer is refused. */
	gsurf_control_server_add_method(server, "test.async", NULL, NULL, async_method, NULL, NULL);
	line = call(server, "test.async", NULL);
	ASSERT_HAS(line, "\"result\":\"later\"");
	g_free(line);
	gsurf_control_server_add_method(server, "test.twice", NULL, NULL, twice_method, NULL, NULL);
	line = call(server, "test.twice", NULL);
	ASSERT_HAS(line, "\"result\":null");
	g_free(line);
}

static void
test_error_codes(void)
{
	static const struct {
		GsurfControlError error;
		gint              code;
	} map[] = {
		{ GSURF_CONTROL_ERROR_FAILED, -32000 },
		{ GSURF_CONTROL_ERROR_INVALID_PARAMS, -32602 },
		{ GSURF_CONTROL_ERROR_NOT_FOUND, -32001 },
		{ GSURF_CONTROL_ERROR_UNSUPPORTED, -32002 },
		{ GSURF_CONTROL_ERROR_PERMISSION_DENIED, -32003 },
		{ GSURF_CONTROL_ERROR_METHOD_NOT_FOUND, -32601 },
		{ GSURF_CONTROL_ERROR_PARSE, -32700 },
		{ GSURF_CONTROL_ERROR_INVALID_REQUEST, -32600 },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(map); i++) {
		g_autoptr(GError) error = g_error_new_literal(GSURF_CONTROL_ERROR, map[i].error, "x");

		g_assert_cmpint(gsurf_control_error_to_code(error), ==, map[i].code);
		g_assert_cmpint(gsurf_control_error_from_code(map[i].code), ==, map[i].error);
	}
	{
		g_autoptr(GError) nf = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "x");
		g_autoptr(GError) ns = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "x");
		g_autoptr(GError) pd = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			"x");
		g_autoptr(GError) other = g_error_new_literal(G_FILE_ERROR, G_FILE_ERROR_NOENT, "x");

		g_assert_cmpint(gsurf_control_error_to_code(nf), ==, -32001);
		g_assert_cmpint(gsurf_control_error_to_code(ns), ==, -32002);
		g_assert_cmpint(gsurf_control_error_to_code(pd), ==, -32003);
		g_assert_cmpint(gsurf_control_error_to_code(other), ==, -32000);
		g_assert_cmpint(gsurf_control_error_from_code(12345), ==, GSURF_CONTROL_ERROR_FAILED);
	}
	g_assert_true(G_TYPE_IS_ENUM(GSURF_TYPE_CONTROL_ERROR));
}

/* ------------------------------------------------------------------ */
/* Providers and the core methods without a window                     */
/* ------------------------------------------------------------------ */

#define TEST_TYPE_PROVIDER_MODULE (test_provider_module_get_type())
G_DECLARE_FINAL_TYPE(TestProviderModule, test_provider_module, TEST, PROVIDER_MODULE,
                     GsurfModule)

struct _TestProviderModule
{
	GsurfModule parent_instance;
	guint       registered;
};

static void test_provider_iface_init(GsurfControlProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(TestProviderModule, test_provider_module, GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_CONTROL_PROVIDER, test_provider_iface_init))

static void
provider_hello(GsurfControlServer *server, GsurfControlInvocation *inv, gpointer data)
{
	JsonNode *node = json_node_new(JSON_NODE_VALUE);

	(void)server;
	(void)data;
	json_node_set_string(node, "hello");
	gsurf_control_invocation_return_value(inv, node);
}

static void
provider_register(GsurfControlProvider *provider, GsurfControlServer *server)
{
	TEST_PROVIDER_MODULE(provider)->registered++;
	gsurf_control_server_add_method(server, "testprov.hello", "says hello", "{}",
		provider_hello, NULL, NULL);
}

static void
test_provider_iface_init(GsurfControlProviderInterface *iface)
{
	iface->register_methods = provider_register;
}

static const gchar *
provider_get_name(GsurfModule *module)
{
	(void)module;
	return "testprov";
}

static gboolean
provider_activate(GsurfModule *module)
{
	(void)module;
	return TRUE;
}

static void
test_provider_module_class_init(TestProviderModuleClass *klass)
{
	GSURF_MODULE_CLASS(klass)->get_name = provider_get_name;
	GSURF_MODULE_CLASS(klass)->activate = provider_activate;
}

static void
test_provider_module_init(TestProviderModule *self)
{
	(void)self;
}

static void
test_provider(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	g_autoptr(GsurfControlServer) server = gsurf_control_server_new(NULL);
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	TestProviderModule *module;
	gchar *line;

	gsurf_config_set_default(config);
	module = g_object_new(TEST_TYPE_PROVIDER_MODULE, NULL);
	gsurf_module_set_enabled(GSURF_MODULE(module), TRUE);
	gsurf_module_manager_add_module(mgr, GSURF_MODULE(module));
	g_object_unref(module);
	gsurf_module_activate(GSURF_MODULE(module));

	/* 1. An inactive server registers nothing until asked. */
	g_assert_false(gsurf_control_server_has_method(server, "testprov.hello"));
	gsurf_control_server_refresh_providers(server);
	g_assert_true(gsurf_control_server_has_method(server, "testprov.hello"));
	g_assert_cmpuint(module->registered, ==, 1);
	line = call(server, "testprov.hello", NULL);
	ASSERT_HAS(line, "\"result\":\"hello\"");
	g_free(line);
	line = call(server, "methods.list", NULL);
	ASSERT_HAS(line, "\"provider\":\"testprov\"");
	g_free(line);

	/* 2. A refresh does not duplicate; modules.toggle off removes the
	 *    method, on brings it back. */
	gsurf_control_server_refresh_providers(server);
	g_assert_cmpuint(module->registered, ==, 2);
	line = call(server, "modules.toggle", "{\"name\":\"testprov\",\"enabled\":false}");
	ASSERT_HAS(line, "\"active\":false");
	g_free(line);
	g_assert_false(gsurf_control_server_has_method(server, "testprov.hello"));
	line = call(server, "modules.toggle", "{\"name\":\"testprov\"}");
	ASSERT_HAS(line, "\"active\":true");
	g_free(line);
	g_assert_true(gsurf_control_server_has_method(server, "testprov.hello"));

	/* 3. modules.list; an unknown module. */
	line = call(server, "modules.list", NULL);
	ASSERT_HAS(line, "\"name\":\"testprov\"");
	g_free(line);
	line = call(server, "modules.toggle", "{\"name\":\"nope\"}");
	g_assert_cmpint(error_code(line), ==, -32001);
	g_free(line);
	line = call(server, "modules.toggle", "{\"name\":\"testprov\",\"enabled\":\"maybe\"}");
	g_assert_cmpint(error_code(line), ==, -32602);
	g_free(line);

	gsurf_module_deactivate(GSURF_MODULE(module));
	gsurf_config_set_default(NULL);
}

static void
test_core_headless(void)
{
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfApplication) app = NULL;
	g_autoptr(GsurfControlServer) server = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	gchar *line;

	g_assert_true(gsurf_config_load_from_data(config,
		"browser:\n  homepage: https://home.example\n"
		"control:\n  enabled: false\n  name: work\n"
		"modules:\n  wmux:\n    enabled: true\n    prefix: C-a\n    panes: [1, 'two']\n",
		-1, &error));
	g_assert_no_error(error);
	gsurf_config_set_default(config);
	app = gsurf_application_new(config);
	server = gsurf_control_server_new(app);
	g_assert_true(gsurf_control_server_get_application(server) == app);

	/* 1. instance.info with no window. */
	line = call(server, "instance.info", NULL);
	result = result_of(line);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "pid"), ==,
		getpid());
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "windows"), ==,
		0);
	ASSERT_HAS(line, "\"version\":\"" GSURF_VERSION "\"");
	g_free(line);

	/* 2. Methods that need a window: not found; mode.get and the lists
	 *    work; params of the wrong type are invalid. */
	line = call(server, "views.list", NULL);
	g_assert_cmpint(error_code(line), ==, -32001);
	g_free(line);
	line = call(server, "page.title", "{\"view\":\"x\"}");
	g_assert_cmpint(error_code(line), ==, -32001);
	g_free(line);
	line = call(server, "mode.get", NULL);
	ASSERT_HAS(line, "\"mode\":\"normal\"");
	g_free(line);
	line = call(server, "mode.set", "{\"mode\":\"sideways\"}");
	g_assert_cmpint(error_code(line), ==, -32602);
	g_free(line);
	line = call(server, "mode.set", "{\"mode\":\"insert\"}");
	g_assert_cmpint(error_code(line), ==, -32002);  /* no modal module */
	g_free(line);
	line = call(server, "actions.list", NULL);
	ASSERT_HAS(line, "\"name\":\"zoom-in\"");
	g_free(line);
	line = call(server, "action.run", "{\"name\":\"no-such-action\"}");
	g_assert_cmpint(error_code(line), ==, -32001);
	g_free(line);
	line = call(server, "containers.list", NULL);
	ASSERT_HAS(line, "\"id\":\"default\"");
	g_free(line);
	line = call(server, "layout.describe", NULL);
	ASSERT_HAS(line, "\"active\":false");
	g_free(line);
	line = call(server, "open", "{\"uri\":5}");
	g_assert_cmpint(error_code(line), ==, -32001);  /* no window comes first */
	g_free(line);

	/* 3. config.get: core keys, the whole set, module sections. */
	line = call(server, "config.get", "{\"key\":\"browser.homepage\"}");
	ASSERT_HAS(line, "\"result\":\"https://home.example\"");
	g_free(line);
	line = call(server, "config.get", "{\"key\":\"control.name\"}");
	ASSERT_HAS(line, "\"result\":\"work\"");
	g_free(line);
	line = call(server, "config.get", "{\"key\":\"control.enabled\"}");
	ASSERT_HAS(line, "\"result\":false");
	g_free(line);
	line = call(server, "config.get", NULL);
	ASSERT_HAS(line, "\"tls.strict\":true");
	g_free(line);
	line = call(server, "config.get", "{\"key\":\"modules.wmux.prefix\"}");
	ASSERT_HAS(line, "\"result\":\"C-a\"");
	g_free(line);
	line = call(server, "config.get", "{\"key\":\"modules.wmux\"}");
	ASSERT_HAS(line, "\"enabled\":true");
	ASSERT_HAS(line, "\"panes\":[1,\"two\"]");
	g_free(line);
	line = call(server, "config.get", "{\"key\":\"modules.wmux.nope\"}");
	g_assert_cmpint(error_code(line), ==, -32001);
	g_free(line);
	line = call(server, "config.get", "{\"key\":\"nope\"}");
	g_assert_cmpint(error_code(line), ==, -32001);
	g_free(line);

	/* 4. Kiosk refuses opening, navigating, eval and module toggles. */
	config->kiosk = TRUE;
	line = call(server, "open", "{\"uri\":\"about:blank\"}");
	g_assert_cmpint(error_code(line), ==, -32003);
	g_free(line);
	line = call(server, "navigate", "{\"uri\":\"about:blank\"}");
	g_assert_cmpint(error_code(line), ==, -32003);
	g_free(line);
	line = call(server, "page.eval", "{\"script\":\"1\"}");
	g_assert_cmpint(error_code(line), ==, -32003);
	g_free(line);
	line = call(server, "modules.toggle", "{\"name\":\"x\"}");
	g_assert_cmpint(error_code(line), ==, -32003);
	g_free(line);
	line = call(server, "containers.reopen", "{\"container\":\"default\"}");
	g_assert_cmpint(error_code(line), ==, -32003);
	g_free(line);
	line = call(server, "layout.panes", NULL);
	g_assert_cmpint(error_code(line), ==, -32001);  /* still: no window */
	g_free(line);
	config->kiosk = FALSE;

	/* 5. The config section itself. */
	g_assert_false(config->control_enabled);
	g_assert_cmpstr(config->control_name, ==, "work");
	{
		g_autoptr(GsurfConfig) fresh = gsurf_config_new();

		g_assert_true(fresh->control_enabled);
		g_assert_null(fresh->control_name);
		g_assert_true(gsurf_config_load_from_data(fresh, "control:\n  name: ''\n", -1, NULL));
		g_assert_null(fresh->control_name);
		g_assert_true(fresh->control_enabled);
	}
	gsurf_config_set_default(NULL);
}

/* ------------------------------------------------------------------ */
/* A real client                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
	gchar    *path;
	gboolean  done;
	gchar    *failure;     /* the first failed check, or NULL */
	gboolean  subscribed;
	gboolean  emit_now;    /* main: emit the test events */
	gboolean  stop_now;    /* main: stop the server */
	gint      phase;
} ClientRun;

#define CHECK(run, cond) \
	G_STMT_START { \
		if (!(cond) && (run)->failure == NULL) \
			(run)->failure = g_strdup_printf("%s:%d: %s", __FILE__, __LINE__, #cond); \
	} G_STMT_END

static gpointer
client_thread(gpointer data)
{
	ClientRun *run = data;
	g_autoptr(GsurfControlClient) client = gsurf_control_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(JsonObject) params = json_object_new();
	const gchar *const topics[] = { "view", NULL };
	guint i;

	CHECK(run, !gsurf_control_client_is_connected(client));
	CHECK(run, gsurf_control_client_connect(client, run->path, &error));
	gsurf_control_client_set_timeout(client, 10000);
	CHECK(run, gsurf_control_client_get_timeout(client) == 10000);

	/* 1. A call, and an error that maps back to its value. */
	json_object_set_string_member(params, "word", "hi");
	result = gsurf_control_client_call(client, "test.echo", params, &error);
	CHECK(run, result != NULL && JSON_NODE_HOLDS_OBJECT(result) &&
		g_strcmp0(json_object_get_string_member(json_node_get_object(result), "word"),
			"hi") == 0);
	g_clear_pointer(&result, json_node_unref);
	result = gsurf_control_client_call(client, "no.such", NULL, &error);
	CHECK(run, result == NULL &&
		g_error_matches(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_METHOD_NOT_FOUND));
	g_clear_error(&error);

	/* 2. Events: subscribe to "view"; the main thread emits a "uri" and a
	 *    "view" event; only the view one arrives. */
	CHECK(run, gsurf_control_client_subscribe(client, topics, &error));
	run->subscribed = TRUE;
	g_atomic_int_set(&run->emit_now, TRUE);
	result = gsurf_control_client_read_event(client, 5000, &error);
	CHECK(run, result != NULL && JSON_NODE_HOLDS_OBJECT(result));
	if (result != NULL) {
		JsonObject *event = json_node_get_object(result);

		CHECK(run, g_strcmp0(json_object_get_string_member(event, "topic"), "view") == 0);
		CHECK(run, g_strcmp0(json_object_get_string_member(event, "event"), "view.test") == 0);
		CHECK(run, json_object_get_int_member(json_object_get_object_member(event, "data"),
			"n") == 2);
	}
	g_clear_pointer(&result, json_node_unref);
	/* Nothing else is waiting; a zero timeout returns at once. */
	result = gsurf_control_client_read_event(client, 0, &error);
	CHECK(run, result == NULL && error == NULL);

	/* 3. The server stops: the stream ends as "closed". */
	g_atomic_int_set(&run->stop_now, TRUE);
	for (i = 0; i < 100 && result == NULL && error == NULL; i++)
		result = gsurf_control_client_read_event(client, 100, &error);
	CHECK(run, result == NULL &&
		g_error_matches(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_CLOSED));
	g_clear_error(&error);
	result = gsurf_control_client_call(client, "test.echo", NULL, &error);
	CHECK(run, result == NULL && error != NULL);
	g_clear_error(&error);
	gsurf_control_client_close(client);
	g_atomic_int_set(&run->done, TRUE);
	return NULL;
}

static void
test_client(void)
{
	GsurfControlServer *server = gsurf_control_server_new(NULL);
	g_autofree gchar *dir = g_build_filename(tmp_root, "client", NULL);
	ClientRun run = { 0 };
	GThread *thread;
	GStatBuf st;
	guint i;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
	run.path = g_build_filename(dir, "c.sock", NULL);
	/* A stale file at the path is replaced. */
	g_assert_true(g_file_set_contents(run.path, "stale", -1, NULL));
	gsurf_control_server_add_method(server, "test.echo", NULL, NULL, echo_method, NULL, NULL);
	g_assert_false(gsurf_control_server_is_running(server));
	g_assert_true(gsurf_control_server_start(server, run.path, NULL));
	g_assert_true(gsurf_control_server_start(server, run.path, NULL));  /* no-op */
	g_assert_true(gsurf_control_server_is_running(server));
	g_assert_cmpstr(gsurf_control_server_get_socket_path(server), ==, run.path);
	g_assert_cmpint(g_lstat(run.path, &st), ==, 0);
	g_assert_true(S_ISSOCK(st.st_mode));
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);

	thread = g_thread_new("control-client", client_thread, &run);
	for (i = 0; i < 4000 && !g_atomic_int_get(&run.done); i++) {
		g_main_context_iteration(NULL, FALSE);
		if (g_atomic_int_get(&run.emit_now)) {
			JsonNode *data = json_node_new(JSON_NODE_OBJECT);
			JsonObject *object = json_object_new();

			run.emit_now = FALSE;
			gsurf_control_server_emit_event(server, "uri", "uri.test", NULL);
			json_object_set_int_member(object, "n", 2);
			json_node_take_object(data, object);
			gsurf_control_server_emit_event(server, "view", "view.test", data);
		}
		if (g_atomic_int_get(&run.stop_now)) {
			run.stop_now = FALSE;
			g_assert_cmpuint(gsurf_control_server_get_n_clients(server), ==, 1);
			gsurf_control_server_stop(server);
			g_assert_cmpuint(gsurf_control_server_get_n_clients(server), ==, 0);
		}
		g_usleep(1000);
	}
	g_thread_join(thread);
	if (run.failure != NULL)
		g_error("%s", run.failure);
	g_assert_true(run.done);

	/* The socket is gone, the server finalizes. */
	g_assert_false(g_file_test(run.path, G_FILE_TEST_EXISTS));
	g_object_add_weak_pointer(G_OBJECT(server), (gpointer *)&server);
	g_object_unref(server);
	for (i = 0; i < 20; i++)
		g_main_context_iteration(NULL, FALSE);
	g_assert_null(server);
	g_free(run.path);

	/* Starting on a directory fails. */
	server = gsurf_control_server_new(NULL);
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(gsurf_control_server_start(server, dir, &error));
		g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	}
	g_object_unref(server);
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

/* Makes a process directory @name under @base with a lock file (held
 * when @hold, returning the fd) and, when @socket, a control.sock. */
static gint
fake_instance(const gchar *base, const gchar *name, gboolean hold, gboolean socket)
{
	g_autofree gchar *dir = g_build_filename(base, name, NULL);
	g_autofree gchar *lock = g_build_filename(dir, "lock", NULL);
	g_autofree gchar *sock = g_build_filename(dir, GSURF_CONTROL_SOCKET_NAME, NULL);
	gint fd;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
	fd = g_open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	g_assert_cmpint(fd, >=, 0);
	if (hold) {
		g_assert_cmpint(flock(fd, LOCK_EX | LOCK_NB), ==, 0);
	} else {
		close(fd);
		fd = -1;
	}
	if (socket)
		g_assert_true(g_file_set_contents(sock, "", 0, NULL));
	return fd;
}

static void
fake_link(const gchar *base, const gchar *name, const gchar *instance)
{
	g_autofree gchar *link = g_build_filename(base, name, NULL);
	g_autofree gchar *target = g_build_filename(base, instance, GSURF_CONTROL_SOCKET_NAME,
		NULL);

	g_assert_cmpint(symlink(target, link), ==, 0);
}

static void
test_discovery(void)
{
	g_autofree gchar *base = g_build_filename(tmp_root, "fake-runtime", NULL);
	g_autoptr(GPtrArray) list = NULL;
	g_autoptr(GsurfControlInstance) found = NULL;
	g_autoptr(GError) error = NULL;
	GsurfControlInstance *inst;
	gint live;
	gint other;
	gint quiet;

	/* 1. A live one (named), a stale one (named), a live one without a
	 *    socket, a live unnamed one, and things that are not instances. */
	live = fake_instance(base, "123-abcdef", TRUE, TRUE);
	fake_instance(base, "456-ghijkl", FALSE, TRUE);
	quiet = fake_instance(base, "789-mnopqr", TRUE, FALSE);
	other = fake_instance(base, "1000-stuvwx", TRUE, TRUE);
	fake_link(base, "work.sock", "123-abcdef");
	fake_link(base, "dead.sock", "456-ghijkl");
	fake_link(base, "gone.sock", "999-zzzzzz");
	{
		g_autofree gchar *plain = g_build_filename(base, "not-a-process", NULL);

		g_assert_cmpint(g_mkdir_with_parents(plain, 0700), ==, 0);
	}
	g_assert_true(gsurf_runtime_dir_is_process_dir_name("123-abcdef"));
	g_assert_false(gsurf_runtime_dir_is_process_dir_name("not-a-process"));
	g_assert_false(gsurf_runtime_dir_is_live(base));

	list = gsurf_control_list_instances(base);
	g_assert_cmpuint(list->len, ==, 2);
	inst = g_ptr_array_index(list, 0);
	g_assert_cmpint(gsurf_control_instance_get_pid(inst), ==, 123);
	g_assert_cmpstr(gsurf_control_instance_get_id(inst), ==, "123-abcdef");
	g_assert_cmpstr(gsurf_control_instance_get_name(inst), ==, "work");
	g_assert_true(g_str_has_suffix(gsurf_control_instance_get_socket_path(inst),
		"/123-abcdef/" GSURF_CONTROL_SOCKET_NAME));
	g_assert_true(g_str_has_suffix(gsurf_control_instance_get_runtime_dir(inst),
		"/123-abcdef"));
	inst = g_ptr_array_index(list, 1);
	g_assert_cmpint(gsurf_control_instance_get_pid(inst), ==, 1000);
	g_assert_null(gsurf_control_instance_get_name(inst));

	/* 2. Matching by name, pid and id. */
	inst = g_ptr_array_index(list, 0);
	g_assert_true(gsurf_control_instance_matches(inst, "work"));
	g_assert_true(gsurf_control_instance_matches(inst, "123"));
	g_assert_true(gsurf_control_instance_matches(inst, "123-abcdef"));
	g_assert_false(gsurf_control_instance_matches(inst, "12"));
	g_assert_false(gsurf_control_instance_matches(inst, ""));
	found = gsurf_control_find_instance(base, "1000", &error);
	g_assert_no_error(error);
	g_assert_cmpint(gsurf_control_instance_get_pid(found), ==, 1000);
	g_clear_pointer(&found, gsurf_control_instance_unref);
	found = gsurf_control_find_instance(base, "dead", &error);
	g_assert_null(found);
	g_assert_error(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* 3. The owner goes: the instance goes with it. */
	close(live);
	g_clear_pointer(&list, g_ptr_array_unref);
	list = gsurf_control_list_instances(base);
	g_assert_cmpuint(list->len, ==, 1);
	g_assert_cmpint(gsurf_control_instance_get_pid(g_ptr_array_index(list, 0)), ==, 1000);
	close(quiet);
	close(other);

	/* 4. A missing base is no instance at all. */
	g_clear_pointer(&list, g_ptr_array_unref);
	list = gsurf_control_list_instances("/nonexistent/gsurf-test");
	g_assert_cmpuint(list->len, ==, 0);
	g_assert_true(G_TYPE_IS_BOXED(GSURF_TYPE_CONTROL_INSTANCE));
}

static void
test_name_link(void)
{
	g_autofree gchar *base = gsurf_control_get_base_dir();
	g_autofree gchar *dir = g_build_filename(tmp_root, "names", NULL);
	g_autofree gchar *path = g_build_filename(dir, "n.sock", NULL);
	g_autofree gchar *link = g_build_filename(base, "named-test.sock", NULL);
	g_autofree gchar *target = NULL;
	g_autoptr(GsurfControlServer) server = gsurf_control_server_new(NULL);
	g_autoptr(GsurfControlServer) second = gsurf_control_server_new(NULL);
	g_autofree gchar *path2 = g_build_filename(dir, "m.sock", NULL);
	gint fd;

	g_assert_true(g_str_has_prefix(base, tmp_root));
	g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);

	/* 1. Names are checked. */
	g_assert_false(gsurf_control_server_set_name(server, "bad/name"));
	g_assert_false(gsurf_control_server_set_name(server, ".hidden"));
	g_assert_false(gsurf_control_server_set_name(server, ""));
	g_assert_null(gsurf_control_server_get_name(server));
	g_assert_true(gsurf_control_server_set_name(server, "named-test"));
	g_assert_cmpstr(gsurf_control_server_get_name(server), ==, "named-test");

	/* 2. A stale link is replaced; the link goes with the server. */
	g_assert_cmpint(g_mkdir_with_parents(base, 0700), ==, 0);
	g_assert_cmpint(symlink("/nonexistent/control.sock", link), ==, 0);
	g_assert_true(gsurf_control_server_start(server, path, NULL));
	target = g_file_read_link(link, NULL);
	g_assert_cmpstr(target, ==, path);
	g_clear_pointer(&target, g_free);
	gsurf_control_server_stop(server);
	g_assert_false(g_file_test(link, G_FILE_TEST_IS_SYMLINK));

	/* 3. A live owner keeps its name. */
	fd = fake_instance(dir, "4242-livexx", TRUE, TRUE);
	{
		g_autofree gchar *live_sock = g_build_filename(dir, "4242-livexx",
			GSURF_CONTROL_SOCKET_NAME, NULL);

		g_assert_cmpint(symlink(live_sock, link), ==, 0);
		gsurf_control_server_set_name(second, "named-test");
		g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*belongs to a running*");
		g_assert_true(gsurf_control_server_start(second, path2, NULL));
		g_test_assert_expected_messages();
		target = g_file_read_link(link, NULL);
		g_assert_cmpstr(target, ==, live_sock);
		gsurf_control_server_stop(second);
		g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));
	}
	close(fd);
	g_assert_cmpint(g_unlink(link), ==, 0);

	/* 4. The default path: control.sock in this process's runtime dir,
	 *    found by discovery. */
	{
		g_autoptr(GsurfControlServer) def = gsurf_control_server_new(NULL);
		g_autoptr(GPtrArray) list = NULL;
		guint i;
		gboolean seen = FALSE;

		g_assert_true(gsurf_control_server_start(def, NULL, NULL));
		g_assert_true(g_str_has_suffix(gsurf_control_server_get_socket_path(def),
			"/" GSURF_CONTROL_SOCKET_NAME));
		list = gsurf_control_list_instances(NULL);
		for (i = 0; i < list->len; i++)
			if (gsurf_control_instance_get_pid(g_ptr_array_index(list, i)) == getpid())
				seen = TRUE;
		g_assert_true(seen);
		gsurf_control_server_stop(def);
	}
}

int
main(int argc, char *argv[])
{
	int ret;

	g_test_init(&argc, &argv, NULL);
	tmp_root = g_dir_make_tmp("gsurf-control-XXXXXX", NULL);
	g_assert_nonnull(tmp_root);
	g_setenv("XDG_RUNTIME_DIR", tmp_root, TRUE);

	g_test_add_func("/gsurf/control/framing", test_framing);
	g_test_add_func("/gsurf/control/registry", test_registry);
	g_test_add_func("/gsurf/control/error-codes", test_error_codes);
	g_test_add_func("/gsurf/control/provider", test_provider);
	g_test_add_func("/gsurf/control/core-headless", test_core_headless);
	g_test_add_func("/gsurf/control/client", test_client);
	g_test_add_func("/gsurf/control/discovery", test_discovery);
	g_test_add_func("/gsurf/control/name-link", test_name_link);

	ret = g_test_run();
	gsurf_runtime_dir_cleanup();
	gsurf_test_rmtree(tmp_root);
	g_free(tmp_root);
	return ret;
}
