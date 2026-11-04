/*
 * test-container-registry.c - Containers, view identity/opener, window order
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Headless: views and windows are minimal GsurfView/GsurfWindow
 * subclasses and native sessions come from a fake GsurfSessionBackend,
 * so the core placement, lifetime and policy rules are tested without a
 * display or WebKit. test-containers-webkit covers real WebKit sessions.
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "test-util.h"

/* --- headless view / window --- */

typedef struct { GsurfView parent; } TestView;
typedef struct { GsurfViewClass parent; } TestViewClass;
typedef struct { GsurfWindow parent; } TestWindow;
typedef struct { GsurfWindowClass parent; } TestWindowClass;

GType test_view_get_type(void);
GType test_window_get_type(void);
G_DEFINE_TYPE(TestView, test_view, GSURF_TYPE_VIEW)
G_DEFINE_TYPE(TestWindow, test_window, GSURF_TYPE_WINDOW)

static void test_view_class_init(TestViewClass *klass) { (void)klass; }
static void test_view_init(TestView *self) { (void)self; }
static void test_window_class_init(TestWindowClass *klass) { (void)klass; }
static void test_window_init(TestWindow *self) { (void)self; }

/* Create a headless view with the base-class construct properties. */
static GsurfView *
new_view(GsurfView *opener, const gchar *container, const gchar *uri, gboolean related)
{
	return g_object_new(test_view_get_type(),
		"opener", opener,
		"container-id", container,
		"initial-uri", uri,
		"related", related,
		NULL);
}

/* --- fake session backend --- */

typedef struct {
	guint     created;
	guint     destroyed;
	guint     policies;
	gchar    *last_id;
	gchar    *last_data_dir;
	gboolean  last_itp;
	gint      last_cookie_policy;
	gboolean  default_policied;
} FakeBackend;

static FakeBackend fake;

static gpointer
fake_create(const gchar *id, const gchar *data_dir, const gchar *cache_dir)
{
	(void)cache_dir;
	fake.created++;
	g_free(fake.last_id);
	fake.last_id = g_strdup(id);
	g_free(fake.last_data_dir);
	fake.last_data_dir = g_strdup(data_dir);
	/* Any non-NULL token stands for a native session. */
	return g_strdup(id);
}

static void
fake_destroy(gpointer native)
{
	fake.destroyed++;
	g_free(native);
}

static void
fake_apply_policy(gpointer native, gboolean itp, gint cookie_policy)
{
	fake.policies++;
	fake.last_itp = itp;
	fake.last_cookie_policy = cookie_policy;
	if (native == NULL)
		fake.default_policied = TRUE;
}

static const GsurfSessionBackend fake_backend = {
	fake_create, fake_destroy, fake_apply_policy
};

static void
fake_reset(void)
{
	g_clear_pointer(&fake.last_id, g_free);
	g_clear_pointer(&fake.last_data_dir, g_free);
	memset(&fake, 0, sizeof(fake));
}

/* Each test leaves the shared default registry without a resolver or
 * backend so the next one starts clean. */
static GsurfContainerRegistry *
registry_for_test(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();

	gsurf_container_registry_set_resolver(registry, NULL, NULL, NULL);
	gsurf_container_registry_set_session_backend(registry, NULL);
	gsurf_container_registry_set_temp_cleanup_delay(registry, 0);
	fake_reset();
	return registry;
}

/* Spin the default main context until @done or ~2s pass. */
static void
spin_until(gboolean *done)
{
	gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

	while (!*done && g_get_monotonic_time() < deadline)
		g_main_context_iteration(NULL, FALSE);
}

static void
on_removed(GsurfContainerRegistry *registry, GsurfContainer *container, gpointer data)
{
	(void)registry;
	(void)container;
	*(gboolean *)data = TRUE;
}

/* ===== ids ===== */

static void
test_validate_id(void)
{
	static const gchar *const good[] = { "media", "Work_2", "a-b", "x" };
	static const gchar *const bad[] = {
		"", "has space", "../escape", "slash/inside", "dot.name",
		"temporary", "tmp-3", "default", "ünïcode",
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", /* 65 */
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(good); i++)
		g_assert_true(gsurf_container_validate_id(good[i], NULL));
	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autoptr(GError) error = NULL;

		g_assert_false(gsurf_container_validate_id(bad[i], &error));
		g_assert_error(error, GSURF_CONTAINER_ERROR, GSURF_CONTAINER_ERROR_INVALID_ID);
	}
	g_assert_false(gsurf_container_validate_id(NULL, NULL));
}

/* ===== persistent containers ===== */

static void
test_define_undefine(void)
{
	GsurfContainerRegistry *registry = registry_for_test();
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) list = NULL;
	GsurfContainer *media;
	GsurfContainer *again;
	gboolean removed = FALSE;
	gulong handler;

	media = gsurf_container_registry_define(registry, "def-media", "Media", "#f9e2af", &error);
	g_assert_no_error(error);
	g_assert_nonnull(media);
	g_assert_cmpstr(gsurf_container_get_id(media), ==, "def-media");
	g_assert_cmpstr(gsurf_container_get_name(media), ==, "Media");
	g_assert_cmpstr(gsurf_container_get_color(media), ==, "#f9e2af");
	g_assert_false(gsurf_container_get_temporary(media));
	g_assert_true(gsurf_container_registry_lookup(registry, "def-media") == media);
	g_assert_null(gsurf_container_registry_lookup(registry, NULL));

	/* Redefinition updates the look and keeps the object. */
	again = gsurf_container_registry_define(registry, "def-media", NULL, NULL, &error);
	g_assert_true(again == media);
	g_assert_cmpstr(gsurf_container_get_name(media), ==, "def-media");
	g_assert_null(gsurf_container_get_color(media));

	/* Invalid ids are refused. */
	g_assert_null(gsurf_container_registry_define(registry, "bad id", NULL, NULL, &error));
	g_assert_error(error, GSURF_CONTAINER_ERROR, GSURF_CONTAINER_ERROR_INVALID_ID);
	g_clear_error(&error);

	list = gsurf_container_registry_list(registry);
	g_assert_true(g_ptr_array_find(list, media, NULL));

	/* A container in use cannot be removed; an unknown one is reported. */
	{
		g_autoptr(GsurfView) view = new_view(NULL, "def-media", NULL, FALSE);

		g_assert_cmpstr(gsurf_view_get_container_id(view), ==, "def-media");
		g_assert_cmpuint(gsurf_container_get_n_views(media), ==, 1);
		g_assert_false(gsurf_container_registry_undefine(registry, "def-media", &error));
		g_assert_error(error, GSURF_CONTAINER_ERROR, GSURF_CONTAINER_ERROR_IN_USE);
		g_clear_error(&error);
	}
	g_assert_cmpuint(gsurf_container_get_n_views(media), ==, 0);

	handler = g_signal_connect(registry, "container-removed", G_CALLBACK(on_removed), &removed);
	g_assert_true(gsurf_container_registry_undefine(registry, "def-media", &error));
	g_assert_no_error(error);
	g_assert_true(removed);
	g_signal_handler_disconnect(registry, handler);
	g_assert_null(gsurf_container_registry_lookup(registry, "def-media"));
	g_assert_false(gsurf_container_registry_undefine(registry, "def-media", &error));
	g_assert_error(error, GSURF_CONTAINER_ERROR, GSURF_CONTAINER_ERROR_NOT_FOUND);
}

/* ===== temporary containers ===== */

static void
test_temporary_lifetime(void)
{
	GsurfContainerRegistry *registry = registry_for_test();
	g_autofree gchar *id = NULL;
	GsurfView *view;
	gboolean removed = FALSE;
	gulong handler;

	gsurf_container_registry_set_session_backend(registry, &fake_backend);
	handler = g_signal_connect(registry, "container-removed", G_CALLBACK(on_removed), &removed);

	/* "temporary" creates a fresh tmp-N container for the view. */
	view = new_view(NULL, GSURF_CONTAINER_TEMPORARY, NULL, FALSE);
	id = g_strdup(gsurf_view_get_container_id(view));
	g_assert_true(g_str_has_prefix(id, "tmp-"));
	g_assert_true(gsurf_container_get_temporary(gsurf_container_registry_lookup(registry, id)));
	/* Ephemeral: the backend gets no data directory. */
	g_assert_cmpuint(fake.created, ==, 1);
	g_assert_null(fake.last_data_dir);
	g_assert_true(gsurf_view_get_session(view) != NULL);

	/* A second view in the same temporary container shares the session. */
	{
		g_autoptr(GsurfView) sibling = new_view(NULL, id, NULL, FALSE);

		g_assert_cmpstr(gsurf_view_get_container_id(sibling), ==, id);
		g_assert_true(gsurf_view_get_session(sibling) == gsurf_view_get_session(view));
		g_assert_cmpuint(fake.created, ==, 1);
	}
	/* One view still open: the idle cleanup must not remove it. */
	g_main_context_iteration(NULL, FALSE);
	g_assert_false(removed);

	g_object_unref(view);
	spin_until(&removed);
	g_assert_true(removed);
	g_assert_cmpuint(fake.destroyed, ==, 1);
	g_assert_null(gsurf_container_registry_lookup(registry, id));
	g_signal_handler_disconnect(registry, handler);
}

/* A temporary container nobody ever uses is still cleaned up, and a view
 * acquiring it before the delay cancels the cleanup. */
static void
test_temporary_unused_and_reacquired(void)
{
	GsurfContainerRegistry *registry = registry_for_test();
	GsurfContainer *unused;
	GsurfContainer *kept;
	g_autofree gchar *unused_id = NULL;
	g_autofree gchar *kept_id = NULL;
	gboolean removed = FALSE;
	gulong handler;

	unused = gsurf_container_registry_new_temporary(registry, "#89b4fa");
	unused_id = g_strdup(gsurf_container_get_id(unused));
	g_assert_cmpstr(gsurf_container_get_color(unused), ==, "#89b4fa");
	handler = g_signal_connect(registry, "container-removed", G_CALLBACK(on_removed), &removed);
	spin_until(&removed);
	g_assert_null(gsurf_container_registry_lookup(registry, unused_id));
	g_signal_handler_disconnect(registry, handler);

	kept = gsurf_container_registry_new_temporary(registry, NULL);
	kept_id = g_strdup(gsurf_container_get_id(kept));
	g_assert_cmpstr(kept_id, !=, unused_id);
	{
		g_autoptr(GsurfView) view = new_view(NULL, kept_id, NULL, FALSE);
		guint i;

		for (i = 0; i < 20; i++)
			g_main_context_iteration(NULL, FALSE);
		g_assert_true(gsurf_container_registry_lookup(registry, kept_id) == kept);
		g_assert_cmpuint(gsurf_container_get_n_views(kept), ==, 1);
	}
	removed = FALSE;
	handler = g_signal_connect(registry, "container-removed", G_CALLBACK(on_removed), &removed);
	spin_until(&removed);
	g_signal_handler_disconnect(registry, handler);
}

/* ===== resolution ===== */

typedef struct {
	guint      calls;
	GsurfView *last_opener;
	gchar     *last_requested;
	gchar     *last_uri;
	gchar     *answer;
	gboolean   freed;
} ResolverState;

static gchar *
test_resolver(GsurfContainerRegistry *registry, GsurfView *opener,
              const gchar *requested, const gchar *uri, gpointer user_data)
{
	ResolverState *state = user_data;

	(void)registry;
	state->calls++;
	state->last_opener = opener;
	g_free(state->last_requested);
	state->last_requested = g_strdup(requested);
	g_free(state->last_uri);
	state->last_uri = g_strdup(uri);
	return g_strdup(state->answer);
}

static void
resolver_state_freed(gpointer data)
{
	((ResolverState *)data)->freed = TRUE;
}

static void
test_resolve(void)
{
	GsurfContainerRegistry *registry = registry_for_test();
	ResolverState state = { 0 };
	ResolverState replaced = { 0 };
	g_autoptr(GsurfView) opener = NULL;

	g_assert_nonnull(gsurf_container_registry_define(registry, "res-work", NULL, NULL, NULL));

	/* No resolver: the request is honoured; unknown ids fall back. */
	{
		g_autoptr(GsurfView) plain = new_view(NULL, NULL, NULL, FALSE);
		g_autoptr(GsurfView) work = new_view(NULL, "res-work", NULL, FALSE);
		g_autoptr(GsurfView) unknown = NULL;

		g_assert_null(gsurf_view_get_container_id(plain));
		g_assert_null(gsurf_view_get_session(plain));
		g_assert_cmpstr(gsurf_view_get_container_id(work), ==, "res-work");
		g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*unknown container 'nope'*");
		unknown = new_view(NULL, "nope", NULL, FALSE);
		g_test_assert_expected_messages();
		g_assert_null(gsurf_view_get_container_id(unknown));
	}

	/* "default" asks for the default container explicitly, silently. */
	{
		g_autoptr(GsurfView) dflt = new_view(NULL, GSURF_CONTAINER_DEFAULT, NULL, FALSE);

		g_assert_null(gsurf_view_get_container_id(dflt));
		g_assert_null(gsurf_view_get_session(dflt));
	}

	/* A resolver sees the opener, request and URI hint and decides. */
	gsurf_container_registry_set_resolver(registry, test_resolver, &replaced,
		resolver_state_freed);
	gsurf_container_registry_set_resolver(registry, test_resolver, &state,
		resolver_state_freed);
	g_assert_true(replaced.freed);
	opener = new_view(NULL, NULL, NULL, FALSE);
	state.answer = "res-work";
	{
		g_autoptr(GsurfView) child = new_view(opener, "ignored", "https://example.com/", FALSE);

		g_assert_cmpuint(state.calls, >=, 1);
		g_assert_true(state.last_opener == opener);
		g_assert_cmpstr(state.last_requested, ==, "ignored");
		g_assert_cmpstr(state.last_uri, ==, "https://example.com/");
		g_assert_cmpstr(gsurf_view_get_container_id(child), ==, "res-work");
	}
	/* A resolver may name the default container with the token too. */
	state.answer = GSURF_CONTAINER_DEFAULT;
	{
		g_autoptr(GsurfView) dflt = new_view(opener, NULL, NULL, FALSE);

		g_assert_null(gsurf_view_get_container_id(dflt));
	}
	state.answer = "res-work";

	/* A related popup always shares its opener's container: the resolver
	 * is not even consulted. */
	{
		g_autoptr(GsurfView) work = new_view(NULL, NULL, NULL, FALSE);
		guint calls;
		g_autoptr(GsurfView) popup = NULL;

		g_assert_cmpstr(gsurf_view_get_container_id(work), ==, "res-work");
		state.answer = NULL;
		calls = state.calls;
		popup = new_view(work, NULL, NULL, TRUE);
		g_assert_cmpuint(state.calls, ==, calls);
		g_assert_cmpstr(gsurf_view_get_container_id(popup), ==, "res-work");
		g_assert_true(gsurf_view_get_related(popup));
	}

	/* "related" without an opener is meaningless and is dropped. */
	{
		g_autoptr(GsurfView) orphan = new_view(NULL, NULL, NULL, TRUE);

		g_assert_false(gsurf_view_get_related(orphan));
	}

	gsurf_container_registry_set_resolver(registry, NULL, NULL, NULL);
	g_assert_true(state.freed);
	g_free(state.last_requested);
	g_free(state.last_uri);
	g_assert_true(gsurf_container_registry_undefine(registry, "res-work", NULL));
}

/* ===== sessions and policy ===== */

static void
on_session_created(GsurfContainerRegistry *registry, GsurfContainer *container,
                   gpointer native, gpointer data)
{
	(void)registry;
	(void)container;
	g_assert_nonnull(native);
	(*(guint *)data)++;
}

static void
test_sessions_and_policy(void)
{
	GsurfContainerRegistry *registry = registry_for_test();
	g_autofree gchar *home = g_dir_make_tmp("gsurf-containers-XXXXXX", NULL);
	g_autofree gchar *data_home = g_build_filename(home, "data", NULL);
	g_autofree gchar *cache_home = g_build_filename(home, "cache", NULL);
	g_autofree gchar *expected = NULL;
	guint sessions = 0;
	gulong handler;
	GStatBuf st;

	g_setenv("XDG_DATA_HOME", data_home, TRUE);
	g_setenv("XDG_CACHE_HOME", cache_home, TRUE);
	g_assert_nonnull(gsurf_container_registry_define(registry, "pol-bank", NULL, NULL, NULL));

	/* A policy set before the backend reaches the default session once
	 * the backend is installed. */
	gsurf_container_registry_set_policy(registry, TRUE, 2);
	g_assert_cmpuint(fake.policies, ==, 0);
	gsurf_container_registry_set_session_backend(registry, &fake_backend);
	g_assert_true(fake.default_policied);

	handler = g_signal_connect(registry, "session-created", G_CALLBACK(on_session_created), &sessions);
	{
		g_autoptr(GsurfView) a = new_view(NULL, "pol-bank", NULL, FALSE);
		g_autoptr(GsurfView) b = new_view(NULL, "pol-bank", NULL, FALSE);

		/* Created once, on first use, with private directories. */
		g_assert_cmpuint(fake.created, ==, 1);
		g_assert_cmpuint(sessions, ==, 1);
		g_assert_cmpstr(fake.last_id, ==, "pol-bank");
		expected = gsurf_container_registry_get_data_dir("pol-bank");
		g_assert_cmpstr(fake.last_data_dir, ==, expected);
		g_assert_true(g_str_has_prefix(expected, data_home));
		g_assert_cmpint(g_stat(expected, &st), ==, 0);
		g_assert_cmpint(st.st_mode & 0777, ==, 0700);
		/* The new session got the policy too. */
		g_assert_true(fake.last_itp);
		g_assert_cmpint(fake.last_cookie_policy, ==, 2);
		g_assert_true(gsurf_view_get_session(a) == gsurf_view_get_session(b));

		/* A later policy change reaches default + live sessions. */
		fake.policies = 0;
		gsurf_container_registry_set_policy(registry, FALSE, 7);
		g_assert_cmpuint(fake.policies, ==, 2);
		g_assert_false(fake.last_itp);
		g_assert_cmpint(fake.last_cookie_policy, ==, 2);  /* clamped */
	}
	/* Persistent containers keep their session with no views open. */
	g_assert_cmpuint(fake.destroyed, ==, 0);
	g_signal_handler_disconnect(registry, handler);
	g_assert_true(gsurf_container_registry_undefine(registry, "pol-bank", NULL));
	g_assert_cmpuint(fake.destroyed, ==, 1);

	gsurf_container_registry_set_policy(registry, FALSE, -1);
	gsurf_container_registry_set_session_backend(registry, NULL);
	g_assert_true(gsurf_test_rmtree(home));
}

/* ===== view identity and opener ===== */

static void
test_view_identity(void)
{
	GsurfView *parent;
	GsurfView *child;
	guint64 parent_id;

	registry_for_test();
	parent = new_view(NULL, NULL, "https://example.org/", FALSE);
	child = new_view(parent, NULL, NULL, FALSE);
	parent_id = gsurf_view_get_id(parent);

	g_assert_cmpuint(parent_id, >, 0);
	g_assert_cmpuint(gsurf_view_get_id(child), >, parent_id);
	g_assert_true(gsurf_view_lookup(parent_id) == parent);
	g_assert_null(gsurf_view_lookup(0));
	g_assert_cmpstr(gsurf_view_get_initial_uri(parent), ==, "https://example.org/");
	g_assert_null(gsurf_view_get_opener(parent));
	g_assert_cmpuint(gsurf_view_get_opener_id(parent), ==, 0);
	g_assert_true(gsurf_view_get_opener(child) == parent);
	g_assert_cmpuint(gsurf_view_get_opener_id(child), ==, parent_id);
	{
		guint64 id = 0;

		g_object_get(child, "id", &id, NULL);
		g_assert_cmpuint(id, ==, gsurf_view_get_id(child));
	}

	/* The opener is weak; its id survives for the tab tree. */
	g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&parent);
	g_object_unref(parent);
	g_assert_null(parent);
	g_assert_null(gsurf_view_get_opener(child));
	g_assert_cmpuint(gsurf_view_get_opener_id(child), ==, parent_id);
	g_assert_null(gsurf_view_lookup(parent_id));

	g_object_add_weak_pointer(G_OBJECT(child), (gpointer *)&child);
	g_object_unref(child);
	g_assert_null(child);
}

/* ===== script messages and world fallbacks ===== */

static void
on_script_message(GsurfView *view, const gchar *name, const gchar *json, gpointer data)
{
	GString *log = data;

	(void)view;
	g_string_append_printf(log, "%s=%s;", name, json != NULL ? json : "null");
}

static void
on_js_done(GObject *source, GAsyncResult *result, gpointer data)
{
	GError **error = data;
	gchar *value = gsurf_view_run_javascript_in_world_finish(GSURF_VIEW(source), result, error);

	g_assert_null(value);
}

static void
test_script_messages(void)
{
	g_autoptr(GsurfView) view = NULL;
	g_autoptr(GString) log = g_string_new(NULL);
	g_autoptr(GError) error = NULL;

	registry_for_test();
	view = new_view(NULL, NULL, NULL, FALSE);

	/* Detailed connections only see their own handler's messages. */
	g_signal_connect(view, "script-message::autofill", G_CALLBACK(on_script_message), log);
	gsurf_view_emit_script_message(view, "autofill", "{\"a\":1}");
	gsurf_view_emit_script_message(view, "other", "2");
	gsurf_view_emit_script_message(view, "autofill", NULL);
	g_assert_cmpstr(log->str, ==, "autofill={\"a\":1};autofill=null;");

	/* A backend without world support: registration and injection are
	 * no-ops, evaluation reports NOT_SUPPORTED instead of hanging. */
	gsurf_view_register_script_message(view, "autofill", "gsurf-autofill");
	gsurf_view_add_user_script_in_world(view, "1", FALSE, NULL, "gsurf-autofill");
	gsurf_view_run_javascript_in_world_async(view, "1", "gsurf-autofill", NULL, on_js_done, &error);
	while (error == NULL)
		g_main_context_iteration(NULL, TRUE);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
	g_assert_null(gsurf_view_get_favicon(view));
}

/* ===== window order ===== */

static void
on_view_moved(GsurfWindow *window, GsurfView *view, guint from, guint to, gpointer data)
{
	GString *log = data;

	(void)window;
	g_string_append_printf(log, "%" G_GUINT64_FORMAT ":%u->%u;", gsurf_view_get_id(view), from, to);
}

static void
test_window_order(void)
{
	g_autoptr(GsurfWindow) window = g_object_new(test_window_get_type(), NULL);
	g_autoptr(GString) log = g_string_new(NULL);
	g_autoptr(GString) expect = g_string_new(NULL);
	GsurfView *v[4];
	guint i;

	registry_for_test();
	for (i = 0; i < G_N_ELEMENTS(v); i++)
		v[i] = new_view(NULL, NULL, NULL, FALSE);

	/* insert_view places at the index; past-the-end appends; the first
	 * view becomes active as with add_view. */
	gsurf_window_insert_view(window, v[0], 5);
	gsurf_window_insert_view(window, v[1], 0);
	gsurf_window_insert_view(window, v[2], 1);
	gsurf_window_add_view(window, v[3]);
	gsurf_window_insert_view(window, v[3], 0);  /* already owned: no-op */
	g_assert_cmpuint(gsurf_window_get_n_views(window), ==, 4);
	g_assert_true(gsurf_window_get_nth_view(window, 0) == v[1]);
	g_assert_true(gsurf_window_get_nth_view(window, 1) == v[2]);
	g_assert_true(gsurf_window_get_nth_view(window, 2) == v[0]);
	g_assert_true(gsurf_window_get_nth_view(window, 3) == v[3]);
	g_assert_true(gsurf_window_get_active_view(window) == v[0]);
	g_assert_cmpint(gsurf_window_get_view_index(window, v[3]), ==, 3);

	g_signal_connect(window, "view-moved", G_CALLBACK(on_view_moved), log);
	gsurf_window_move_view(window, v[3], 0);
	gsurf_window_move_view(window, v[3], 0);          /* no change: no signal */
	gsurf_window_move_view(window, v[1], G_MAXUINT);  /* clamps to last */
	g_string_append_printf(expect, "%" G_GUINT64_FORMAT ":3->0;%" G_GUINT64_FORMAT ":1->3;",
		gsurf_view_get_id(v[3]), gsurf_view_get_id(v[1]));
	g_assert_cmpstr(log->str, ==, expect->str);
	g_assert_true(gsurf_window_get_nth_view(window, 0) == v[3]);
	g_assert_true(gsurf_window_get_nth_view(window, 3) == v[1]);

	/* Unowned views are ignored. */
	{
		g_autoptr(GsurfView) stranger = new_view(NULL, NULL, NULL, FALSE);

		gsurf_window_move_view(window, stranger, 0);
		g_assert_cmpint(gsurf_window_get_view_index(window, stranger), ==, -1);
	}
	/* Headless windows have no chrome: a side widget is a no-op. */
	gsurf_window_add_side_widget(window, NULL, GSURF_CHROME_EDGE_LEFT);

	/* The window holds the references; drop ours. */
	for (i = 0; i < G_N_ELEMENTS(v); i++)
		g_object_unref(v[i]);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/containers/validate-id", test_validate_id);
	g_test_add_func("/gsurf/containers/define-undefine", test_define_undefine);
	g_test_add_func("/gsurf/containers/temporary-lifetime", test_temporary_lifetime);
	g_test_add_func("/gsurf/containers/temporary-unused", test_temporary_unused_and_reacquired);
	g_test_add_func("/gsurf/containers/resolve", test_resolve);
	g_test_add_func("/gsurf/containers/sessions-and-policy", test_sessions_and_policy);
	g_test_add_func("/gsurf/view/identity-and-opener", test_view_identity);
	g_test_add_func("/gsurf/view/script-messages", test_script_messages);
	g_test_add_func("/gsurf/window/order", test_window_order);
	return g_test_run();
}
