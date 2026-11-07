/*
 * test-gsurfctl.c - The gsurfctl binary
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Runs the built gsurfctl (the sibling of this test binary, like
 * test-pwa-cli) against:
 *   - nothing: --help, --license, --completion, help --names (which the
 *     /gsurf skill's tests rely on), usage errors (exit 2), no instance
 *     (exit 3);
 *   - control servers in this process (the main loop answers while the
 *     child runs): instance selection (only one, -i by pid and name,
 *     $GSURF_INSTANCE, the most recently focused of two, -S, -a), -j
 *     output, call with JSON and K=V, a failed call (exit 1), kiosk
 *     refusals (exit 4), -q, watch;
 *   - with GSURF_TEST_GUI=1, a real gsurf started under the display:
 *     open, tabs, tree, action, wmux, panes, mode, watch, -a, and exit 3
 *     once it has quit.
 */

#include <gsurf.h>
#include <control/gsurf-control.h>

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "test-util.h"

static gchar *tmp_root;
static gchar *ctl_path;      /* the gsurfctl next to this test */
static gchar *gsurf_rel;     /* the gsurf next to this test, as argv[0] gave it */
static gchar *orig_runtime;  /* the session's XDG_RUNTIME_DIR (the display's sockets) */

/* ------------------------------------------------------------------ */
/* Running gsurfctl                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
	gboolean     done;
	gchar       *out;
	gchar       *err;
} Proc;

static void
on_communicated(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Proc *p = user_data;

	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &p->out, &p->err,
		NULL);
	p->done = TRUE;
}

/* Waits (bounded) for @child while the main loop runs, calling @tick
 * (if any) every 10 ms; returns its exit status. */
static gint
finish(GSubprocess *child, gchar **out, gchar **err, void (*tick)(guint, gpointer),
       gpointer data)
{
	Proc p = { FALSE, NULL, NULL };
	guint i;

	g_subprocess_communicate_utf8_async(child, NULL, NULL, on_communicated, &p);
	for (i = 0; i < 3000 && !p.done; i++) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		if (tick != NULL)
			tick(i, data);
		g_usleep(10000);
	}
	g_assert_true(p.done);
	g_assert_true(g_subprocess_get_if_exited(child));
	if (out != NULL)
		*out = g_steal_pointer(&p.out);
	if (err != NULL)
		*err = g_steal_pointer(&p.err);
	g_free(p.out);
	g_free(p.err);
	return g_subprocess_get_exit_status(child);
}

static GSubprocess *
spawn_ctl(const gchar *const *args)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GError) error = NULL;
	GSubprocess *child;
	guint i;

	g_ptr_array_add(argv, ctl_path);
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);
	child = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	return child;
}

/*
 * ctl:
 * @expect: the exit status expected
 * @out: (out) (optional): stdout
 * @...: the arguments, NULL-terminated
 */
static void
ctl(gint expect, gchar **out, ...)
{
	g_autoptr(GPtrArray) args = g_ptr_array_new();
	g_autoptr(GSubprocess) child = NULL;
	g_autofree gchar *stdout_text = NULL;
	g_autofree gchar *stderr_text = NULL;
	const gchar *arg;
	va_list ap;
	gint status;

	va_start(ap, out);
	while ((arg = va_arg(ap, const gchar *)) != NULL)
		g_ptr_array_add(args, (gpointer)arg);
	va_end(ap);
	g_ptr_array_add(args, NULL);
	child = spawn_ctl((const gchar *const *)args->pdata);
	status = finish(child, &stdout_text, &stderr_text, NULL, NULL);
	if (status != expect) {
		g_autofree gchar *line = g_strjoinv(" ", (gchar **)args->pdata);

		g_error("gsurfctl %s: exit %d, expected %d\nstdout: %s\nstderr: %s", line, status,
			expect, stdout_text, stderr_text);
	}
	if (out != NULL)
		*out = g_steal_pointer(&stdout_text);
}

#define ASSERT_HAS(text, needle) \
	G_STMT_START { \
		if (strstr((text), (needle)) == NULL) \
			g_error("'%s' not in: %s", (needle), (text)); \
	} G_STMT_END

static JsonNode *
parse(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(json_parser_load_from_data(parser, text, -1, &error));
	g_assert_no_error(error);
	return json_node_copy(json_parser_get_root(parser));
}

/* ------------------------------------------------------------------ */
/* Without an instance                                                 */
/* ------------------------------------------------------------------ */

static void
test_offline(void)
{
	static const gchar *const names[] = {
		"list", "info", "open", "nav", "back", "forward", "reload", "stop", "tabs", "tab",
		"close", "tree", "move", "collapse", "containers", "reopen", "title", "uri", "text",
		"eval", "screenshot", "3d", "3d-arrange", "3d-env", "3d-reading", "3d-history",
		"3d-focus", "3d-pin", "3d-unpin-all", "3d-camera", "3d-shot",
		"actions", "action", "mode", "panes", "windows", "wmux",
		"select-window", "rename-window", "config", "modules", "module", "methods", "watch",
		"call", "help", NULL
	};
	gchar *out = NULL;
	guint i;

	/* 1. Help, license, version, completion. */
	ctl(0, &out, "--help", NULL);
	ASSERT_HAS(out, "Examples:");
	ASSERT_HAS(out, "gsurfctl wmux 'split-window -h https://github.com'");
	ASSERT_HAS(out, "Exit status:");
	g_free(out);
	ctl(0, &out, "--license", NULL);
	ASSERT_HAS(out, "GNU Affero General Public License");
	g_free(out);
	ctl(0, &out, "-V", NULL);
	ASSERT_HAS(out, "gsurfctl " GSURF_VERSION);
	g_free(out);
	ctl(0, &out, "--completion", "bash", NULL);
	ASSERT_HAS(out, "complete -F _gsurfctl gsurfctl");
	ASSERT_HAS(out, "gsurfctl help --names");
	{
		g_autofree gchar *script = g_build_filename(tmp_root, "completion.bash", NULL);
		g_autofree gchar *bash = g_find_program_in_path("bash");

		g_assert_true(g_file_set_contents(script, out, -1, NULL));
		if (bash != NULL) {
			g_autoptr(GSubprocess) check = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, NULL,
				bash, "-n", script, NULL);

			g_assert_nonnull(check);
			g_assert_true(g_subprocess_wait_check(check, NULL, NULL));
		}
	}
	g_free(out);
	ctl(2, NULL, "--completion", "zsh", NULL);

	/* 2. help --names: exactly the commands, one per line, offline. */
	ctl(0, &out, "help", "--names", NULL);
	{
		g_auto(GStrv) lines = g_strsplit(g_strchomp(out), "\n", -1);

		g_assert_cmpuint(g_strv_length(lines), ==, g_strv_length((gchar **)names));
		for (i = 0; names[i] != NULL; i++)
			g_assert_true(g_strv_contains((const gchar *const *)lines, names[i]));
	}
	g_free(out);
	ctl(0, &out, "help", "wmux", NULL);
	ASSERT_HAS(out, "usage: gsurfctl wmux");
	ASSERT_HAS(out, "layout.command");
	g_free(out);

	/* 3. Usage errors. */
	ctl(2, NULL, NULL);
	ctl(2, NULL, "frobnicate", NULL);
	ctl(2, NULL, "help", "frobnicate", NULL);
	ctl(2, NULL, "open", "a", "b", NULL);
	ctl(2, NULL, "nav", NULL);
	ctl(2, NULL, "--bogus-option", "tabs", NULL);

	/* 4. Nothing is running. */
	ctl(0, &out, "list", NULL);
	g_free(out);
	ctl(0, &out, "-j", "list", NULL);
	g_assert_cmpstr(g_strstrip(out), ==, "[]");
	g_free(out);
	ctl(3, NULL, "tabs", NULL);
	ctl(3, NULL, "-i", "nope", "tabs", NULL);
	ctl(3, NULL, "tabs", "-i", "nope", NULL);
	ctl(3, NULL, "-a", "tabs", NULL);
	ctl(3, NULL, "-S", "/nonexistent/gsurf.sock", "tabs", NULL);
}

/* ------------------------------------------------------------------ */
/* Against in-process servers                                          */
/* ------------------------------------------------------------------ */

static GsurfControlServer *emitter;

static void
emit_tick(guint i, gpointer data)
{
	(void)data;
	/* Events now and then; a SIGTERM after a while ends the watch. */
	if (i % 10 == 5) {
		JsonNode *node = json_node_new(JSON_NODE_OBJECT);
		JsonObject *object = json_object_new();

		json_object_set_int_member(object, "view", 42);
		json_node_take_object(node, object);
		gsurf_control_server_emit_event(emitter, "view", "view.test", node);
		gsurf_control_server_emit_event(emitter, "uri", "uri.test", NULL);
	}
}

static void
test_servers(void)
{
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfControlServer) server = NULL;
	g_autoptr(GsurfControlServer) second = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *pid = g_strdup_printf("%d", (gint)getpid());
	g_autofree gchar *base = gsurf_control_get_base_dir();
	g_autofree gchar *fake_dir = NULL;
	g_autofree gchar *fake_sock = NULL;
	gchar *out = NULL;
	gint fd;

	config->control_name = g_strdup("ctltest");
	gsurf_config_set_default(config);

	/* 1. One instance: picked without -i; by pid and by name. */
	server = gsurf_control_server_new(NULL);
	g_assert_true(gsurf_control_server_set_name(server, "ctltest"));
	g_assert_true(gsurf_control_server_start(server, NULL, NULL));
	ctl(0, &out, "list", NULL);
	ASSERT_HAS(out, pid);
	ASSERT_HAS(out, "ctltest");
	g_free(out);
	ctl(0, &out, "list", "--names", NULL);
	ASSERT_HAS(out, "ctltest\n");
	g_free(out);
	ctl(0, &out, "-j", "list", NULL);
	node = parse(out);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), ==, 1);
	g_clear_pointer(&node, json_node_unref);
	g_free(out);
	ctl(0, &out, "info", "-j", NULL);
	node = parse(out);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node), "pid"), ==,
		getpid());
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(node), "name"), ==,
		"ctltest");
	g_clear_pointer(&node, json_node_unref);
	g_free(out);
	ctl(0, NULL, "-i", pid, "info", NULL);
	ctl(0, NULL, "-i", "ctltest", "info", NULL);
	ctl(0, NULL, "-S", gsurf_control_server_get_socket_path(server), "info", NULL);
	g_setenv("GSURF_INSTANCE", "ctltest", TRUE);
	ctl(0, NULL, "info", NULL);
	g_setenv("GSURF_INSTANCE", "nope", TRUE);
	ctl(3, NULL, "info", NULL);
	g_unsetenv("GSURF_INSTANCE");

	/* 2. Output and raw calls. */
	ctl(0, &out, "-q", "info", NULL);
	g_assert_cmpstr(out, ==, "");
	g_free(out);
	ctl(0, &out, "methods", "--names", NULL);
	ASSERT_HAS(out, "page.eval\n");
	ASSERT_HAS(out, "layout.command\n");
	g_free(out);
	ctl(0, &out, "config", "control.name", NULL);
	g_assert_cmpstr(g_strstrip(out), ==, "ctltest");
	g_free(out);
	ctl(0, &out, "-j", "config", "control.enabled", NULL);
	g_assert_cmpstr(g_strstrip(out), ==, "true");
	g_free(out);
	ctl(0, &out, "call", "config.get", "key=control.name", NULL);
	g_assert_cmpstr(g_strstrip(out), ==, "ctltest");
	g_free(out);
	ctl(0, &out, "-j", "call", "config.get", "{\"key\":\"tls.strict\"}", NULL);
	g_assert_cmpstr(g_strstrip(out), ==, "true");
	g_free(out);
	ctl(2, NULL, "call", "config.get", "{bad", NULL);
	ctl(2, NULL, "call", "config.get", "plain", NULL);
	ctl(1, NULL, "call", "no.such", NULL);
	ctl(1, NULL, "config", "no.such.key", NULL);
	ctl(0, &out, "-j", "mode", NULL);
	ASSERT_HAS(out, "\"mode\":\"normal\"");
	g_free(out);

	/* 3. Failures: no window (1), usage after connecting (2), kiosk (4). */
	ctl(1, &out, "-j", "tabs", NULL);
	ASSERT_HAS(out, "\"error\"");
	ASSERT_HAS(out, "no window");
	g_free(out);
	ctl(2, NULL, "move", "3", NULL);
	ctl(2, NULL, "tab", "x", NULL);
	ctl(2, NULL, "collapse", "3", "sideways", NULL);
	config->kiosk = TRUE;
	ctl(4, NULL, "open", "https://example.org", NULL);
	ctl(4, NULL, "eval", "1+1", NULL);
	ctl(4, NULL, "module", "wmux", "off", NULL);
	config->kiosk = FALSE;

	/* 4. watch: events of the asked topic until a signal ends it. */
	emitter = server;
	{
		const gchar *const args[] = { "watch", "view", NULL };
		g_autoptr(GSubprocess) child = spawn_ctl(args);
		g_autofree gchar *text = NULL;
		guint i;

		/* Let it subscribe and see a few events, then stop it. */
		for (i = 0; i < 150; i++) {
			while (g_main_context_iteration(NULL, FALSE))
				;
			emit_tick(i, NULL);
			g_usleep(10000);
		}
		g_subprocess_send_signal(child, SIGTERM);
		g_assert_cmpint(finish(child, &text, NULL, emit_tick, NULL), ==, 0);
		ASSERT_HAS(text, "[view] view.test view: 42");
		g_assert_null(strstr(text, "uri.test"));
	}

	/* 5. Two instances: the most recently focused wins; -a reaches both. */
	fake_dir = g_build_filename(base, "999999-zzzzzz", NULL);
	g_assert_cmpint(g_mkdir_with_parents(fake_dir, 0700), ==, 0);
	{
		g_autofree gchar *lock = g_build_filename(fake_dir, "lock", NULL);

		fd = g_open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		g_assert_cmpint(fd, >=, 0);
		g_assert_cmpint(flock(fd, LOCK_EX | LOCK_NB), ==, 0);
	}
	fake_sock = g_build_filename(fake_dir, GSURF_CONTROL_SOCKET_NAME, NULL);
	g_usleep(2000);
	second = gsurf_control_server_new(NULL);
	g_assert_true(gsurf_control_server_set_name(second, "second"));
	g_assert_true(gsurf_control_server_start(second, fake_sock, NULL));
	g_assert_cmpint(gsurf_control_server_get_focused_at(second), >,
		gsurf_control_server_get_focused_at(server));
	ctl(0, &out, "-j", "info", NULL);
	node = parse(out);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(node), "name"), ==,
		"second");
	g_clear_pointer(&node, json_node_unref);
	g_free(out);
	ctl(0, &out, "-i", "ctltest", "-j", "info", NULL);
	ASSERT_HAS(out, "\"name\":\"ctltest\"");
	g_free(out);
	ctl(0, &out, "-a", "-j", "config", "control.name", NULL);
	ASSERT_HAS(out, "{\"instance\":\"ctltest\",\"result\":\"ctltest\"}");
	ASSERT_HAS(out, "{\"instance\":\"second\",\"result\":\"ctltest\"}");
	g_free(out);
	ctl(1, NULL, "-a", "tabs", NULL);

	gsurf_control_server_stop(second);
	close(fd);
	gsurf_control_server_stop(server);
	ctl(3, NULL, "info", NULL);
	gsurf_config_set_default(NULL);
}

/* ------------------------------------------------------------------ */
/* A real gsurf                                                        */
/* ------------------------------------------------------------------ */

static void
test_real_gsurf(void)
{
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) browser = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *config = g_build_filename(tmp_root, "gui.yaml", NULL);
	g_autofree gchar *runtime = g_build_filename(tmp_root, "gui-runtime", NULL);
	g_autofree gchar *wayland = NULL;
	const gchar *display = g_getenv("WAYLAND_DISPLAY");
	gchar *out = NULL;
	guint i;
	gint status = -1;

	if (g_getenv("GSURF_TEST_GUI") == NULL) {
		g_test_skip("set GSURF_TEST_GUI=1 (make test-gui) to run gsurf");
		return;
	}
	if (!g_file_test(gsurf_rel, G_FILE_TEST_IS_EXECUTABLE)) {
		g_test_skip("gsurf is not built");
		return;
	}

	/* 1. A config with wmux, tabs and modal, and a private runtime dir
	 *    (an absolute Wayland display survives the move). */
	g_assert_true(g_file_set_contents(config,
		"browser:\n  homepage: about:blank\n  new_view_uri: about:blank\n"
		"control:\n  name: guitest\n"
		"modules:\n  wmux:\n    enabled: true\n  tabs:\n    enabled: true\n"
		"  modal:\n    enabled: true\n  session:\n    enabled: false\n", -1, NULL));
	g_assert_cmpint(g_mkdir_with_parents(runtime, 0700), ==, 0);
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
		G_SUBPROCESS_FLAGS_STDERR_SILENCE);
	if (display != NULL && !g_path_is_absolute(display) && orig_runtime != NULL) {
		wayland = g_build_filename(orig_runtime, display, NULL);
		g_subprocess_launcher_setenv(launcher, "WAYLAND_DISPLAY", wayland, TRUE);
	}
	g_subprocess_launcher_setenv(launcher, "XDG_RUNTIME_DIR", runtime, TRUE);
	browser = g_subprocess_launcher_spawn(launcher, &error, gsurf_rel, "--no-c-config",
		"-c", config, "about:blank", NULL);
	g_assert_no_error(error);

	/* 2. gsurfctl finds it by name once its socket is up. */
	g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
	for (i = 0; i < 200 && status != 0; i++) {
		g_autoptr(GSubprocess) probe = NULL;
		const gchar *const args[] = { "-i", "guitest", "info", NULL };

		probe = spawn_ctl(args);
		status = finish(probe, NULL, NULL, NULL, NULL);
		if (status != 0)
			g_usleep(100000);
	}
	g_assert_cmpint(status, ==, 0);

	/* 3. Tabs, the tree, actions. */
	ctl(0, &out, "open", "about:blank", "--bg", NULL);
	g_free(out);
	ctl(0, &out, "-j", "tabs", NULL);
	node = parse(out);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), ==, 2);
	g_clear_pointer(&node, json_node_unref);
	g_free(out);
	ctl(0, &out, "-j", "tree", NULL);
	ASSERT_HAS(out, "\"has_tree_provider\":true");
	g_free(out);
	ctl(0, NULL, "action", "zoom-in", NULL);
	ctl(0, &out, "eval", "6*7", NULL);
	g_assert_cmpstr(g_strstrip(out), ==, "42");
	g_free(out);

	/* 4. wmux. */
	ctl(0, NULL, "wmux", "split-window -h", NULL);
	ctl(0, &out, "-j", "panes", NULL);
	node = parse(out);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
		json_node_get_object(node), "panes")), ==, 2);
	g_clear_pointer(&node, json_node_unref);
	g_free(out);
	ctl(0, NULL, "wmux", "--", "split-window", "-v", NULL);
	ctl(1, NULL, "wmux", "select-window -t 9", NULL);
	ctl(0, &out, "windows", NULL);
	ASSERT_HAS(out, "window 0");
	g_free(out);

	/* 5. Modes. */
	ctl(0, &out, "mode", "insert", NULL);
	g_assert_cmpstr(g_strstrip(out), ==, "insert");
	g_free(out);
	ctl(0, &out, "mode", NULL);
	g_assert_cmpstr(g_strstrip(out), ==, "insert");
	g_free(out);
	ctl(0, NULL, "mode", "normal", NULL);

	/* 6. watch sees a tab open; -a reaches it. */
	{
		const gchar *const args[] = { "-j", "watch", "view", NULL };
		g_autoptr(GSubprocess) child = spawn_ctl(args);
		g_autofree gchar *text = NULL;

		g_usleep(500000);
		ctl(0, NULL, "-q", "open", "about:blank", NULL);
		g_usleep(500000);
		g_subprocess_send_signal(child, SIGTERM);
		g_assert_cmpint(finish(child, &text, NULL, NULL, NULL), ==, 0);
		ASSERT_HAS(text, "\"event\":\"view.added\"");
	}
	ctl(0, &out, "-a", "uri", NULL);
	ASSERT_HAS(out, "[guitest]");
	g_free(out);

	/* 7. It quits: the instance is gone. */
	g_subprocess_send_signal(browser, SIGTERM);
	g_assert_true(g_subprocess_wait(browser, NULL, NULL));
	ctl(3, NULL, "-i", "guitest", "tabs", NULL);
	g_setenv("XDG_RUNTIME_DIR", tmp_root, TRUE);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *self = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *argv_dir = NULL;
	int ret;

	g_test_init(&argc, &argv, NULL);
	self = g_file_read_link("/proc/self/exe", NULL);
	g_assert_nonnull(self);
	dir = g_path_get_dirname(self);
	ctl_path = g_build_filename(dir, "gsurfctl", NULL);
	/* gsurf by the path this test was started with: WebKitGTK 6's sandbox
	 * wants the relative form make uses. */
	argv_dir = g_path_get_dirname(argv[0]);
	gsurf_rel = g_build_filename(argv_dir, "gsurf", NULL);
	tmp_root = g_dir_make_tmp("gsurf-gsurfctl-XXXXXX", NULL);
	g_assert_nonnull(tmp_root);
	g_unsetenv("GSURF_INSTANCE");
	g_setenv("XDG_DATA_HOME", tmp_root, TRUE);
	g_setenv("XDG_CACHE_HOME", tmp_root, TRUE);
	g_setenv("XDG_CONFIG_HOME", tmp_root, TRUE);

	/* A private runtime dir, so no instance of the user's is ever seen;
	 * the session's is kept to reach the display from gsurf. */
	orig_runtime = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	g_setenv("XDG_RUNTIME_DIR", tmp_root, TRUE);

	g_test_add_func("/gsurf/gsurfctl/offline", test_offline);
	g_test_add_func("/gsurf/gsurfctl/servers", test_servers);
	g_test_add_func("/gsurf/gsurfctl/real-gsurf", test_real_gsurf);

	ret = g_test_run();
	gsurf_runtime_dir_cleanup();
	gsurf_test_rmtree(tmp_root);
	g_free(tmp_root);
	g_free(ctl_path);
	g_free(gsurf_rel);
	g_free(orig_runtime);
	return ret;
}
