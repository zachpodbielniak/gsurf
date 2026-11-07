/*
 * test-runtime-dir.c - Per-process runtime directory and launch dir tests
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Headless tests for src/util/gsurf-runtime-dir.c: creating the private
 * process directory, sweeping the directories of dead processes (and
 * only those), recursive removal that never follows links, cleanup,
 * the chdir into the runtime directory with the launch directory kept
 * for relative paths, and temporary-container data under the runtime
 * directory being removed with the container.
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "test-util.h"

static gchar *test_home = NULL;     /* XDG_RUNTIME_DIR etc. point here */
static gchar *test_origin = NULL;   /* the working directory at start */

static gchar *
base_dir(void)
{
	return g_build_filename(g_get_user_runtime_dir(), "gsurf", NULL);
}

static void
touch(const gchar *path)
{
	g_assert_true(g_file_set_contents(path, "", 0, NULL));
}

static void
test_create(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = base_dir();
	g_autofree gchar *lock = NULL;
	g_autofree gchar *temp = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *prefix = g_strdup_printf("%ld-", (long)getpid());
	const gchar *dir;
	GStatBuf st;
	gint fd;

	dir = gsurf_runtime_dir_get(&error);
	g_assert_no_error(error);
	g_assert_nonnull(dir);
	/* Stable for the process, under $XDG_RUNTIME_DIR/gsurf, owner-only. */
	g_assert_true(gsurf_runtime_dir_get(NULL) == dir);
	g_assert_true(g_str_has_prefix(dir, base));
	name = g_path_get_basename(dir);
	g_assert_true(g_str_has_prefix(name, prefix));
	g_assert_cmpint(g_stat(dir, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);
	g_assert_cmpint(g_stat(base, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);

	/* The lock file is held: nobody else can take it. */
	lock = g_build_filename(dir, "lock", NULL);
	fd = g_open(lock, O_RDWR | O_CLOEXEC, 0);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(flock(fd, LOCK_EX | LOCK_NB), !=, 0);
	close(fd);

	/* A temporary container's directory sits below it. */
	temp = gsurf_runtime_dir_get_temp_container_dir("tmp-7");
	g_assert_nonnull(temp);
	g_assert_true(g_str_has_prefix(temp, dir));
	g_assert_true(g_str_has_suffix(temp, "/temp/tmp-7"));
}

static void
test_sweep(void)
{
	g_autofree gchar *base = base_dir();
	g_autofree gchar *dead = g_build_filename(base, "99999-aaaaaa", NULL);
	g_autofree gchar *alive = g_build_filename(base, "99998-bbbbbb", NULL);
	g_autofree gchar *young = g_build_filename(base, "99997-cccccc", NULL);
	g_autofree gchar *old = g_build_filename(base, "99996-dddddd", NULL);
	g_autofree gchar *other = g_build_filename(base, "webext-99995", NULL);
	g_autofree gchar *dead_lock = g_build_filename(dead, "lock", NULL);
	g_autofree gchar *dead_data = g_build_filename(dead, "temp", "tmp-1", "data", NULL);
	g_autofree gchar *alive_lock = g_build_filename(alive, "lock", NULL);
	const gchar *own = gsurf_runtime_dir_get(NULL);
	struct utimbuf ancient = { 1000, 1000 };
	gint fd;

	g_assert_nonnull(own);
	/* dead: a lock nobody holds, with data below it. */
	g_assert_cmpint(g_mkdir_with_parents(dead_data, 0700), ==, 0);
	touch(dead_lock);
	/* alive: its lock is held (here, by this test). */
	g_assert_cmpint(g_mkdir_with_parents(alive, 0700), ==, 0);
	fd = g_open(alive_lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(flock(fd, LOCK_EX | LOCK_NB), ==, 0);
	/* young: no lock yet (still being created), old: no lock for ages. */
	g_assert_cmpint(g_mkdir_with_parents(young, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(old, 0700), ==, 0);
	g_assert_cmpint(utime(old, &ancient), ==, 0);
	/* other: not a process directory at all. */
	g_assert_cmpint(g_mkdir_with_parents(other, 0700), ==, 0);

	g_assert_cmpuint(gsurf_runtime_dir_sweep(base), ==, 2);
	g_assert_false(g_file_test(dead, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(old, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(alive, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(young, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(other, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(own, G_FILE_TEST_IS_DIR));

	/* Once the owner lets go, the next sweep takes it. */
	close(fd);
	g_assert_cmpuint(gsurf_runtime_dir_sweep(base), ==, 1);
	g_assert_false(g_file_test(alive, G_FILE_TEST_EXISTS));

	/* A missing base is no error. */
	g_assert_cmpuint(gsurf_runtime_dir_sweep("/nonexistent/gsurf-runtime"), ==, 0);
	gsurf_test_rmtree(young);
	gsurf_test_rmtree(other);
}

static void
test_remove_tree(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *outside = g_build_filename(test_home, "outside", NULL);
	g_autofree gchar *keep = g_build_filename(outside, "keep", NULL);
	g_autofree gchar *tree = g_build_filename(test_home, "tree", NULL);
	g_autofree gchar *deep = g_build_filename(tree, "a", "b", "c", NULL);
	g_autofree gchar *file = g_build_filename(deep, "f", NULL);
	g_autofree gchar *link = g_build_filename(tree, "a", "link", NULL);

	g_assert_cmpint(g_mkdir_with_parents(outside, 0700), ==, 0);
	touch(keep);
	g_assert_cmpint(g_mkdir_with_parents(deep, 0700), ==, 0);
	touch(file);
	g_assert_cmpint(symlink(outside, link), ==, 0);

	g_assert_true(gsurf_runtime_dir_remove_tree(tree, &error));
	g_assert_no_error(error);
	g_assert_false(g_file_test(tree, G_FILE_TEST_EXISTS));
	/* The link went, what it pointed at did not. */
	g_assert_true(g_file_test(keep, G_FILE_TEST_IS_REGULAR));
	/* Missing is fine; a single file works too. */
	g_assert_true(gsurf_runtime_dir_remove_tree(tree, &error));
	g_assert_true(gsurf_runtime_dir_remove_tree(keep, &error));
	g_assert_false(g_file_test(keep, G_FILE_TEST_EXISTS));
	g_assert_no_error(error);
}

static void
test_resolve(void)
{
	g_autofree gchar *launch = gsurf_get_launch_dir();
	g_autofree gchar *rel = gsurf_resolve_path("sub/x.txt");
	g_autofree gchar *want = g_build_filename(launch, "sub", "x.txt", NULL);
	g_autofree gchar *home = gsurf_resolve_path("~/y");
	g_autofree gchar *want_home = g_build_filename(g_get_home_dir(), "y", NULL);
	g_autofree gchar *tilde = gsurf_resolve_path("~");
	g_autofree gchar *abs = gsurf_resolve_path("/etc/hosts");
	g_autofree gchar *empty = gsurf_resolve_path("");
	g_autofree gchar *expanded = gsurf_expand_home("rel");
	g_autofree gchar *want_expanded = g_build_filename(launch, "rel", NULL);

	g_assert_cmpstr(rel, ==, want);
	g_assert_cmpstr(home, ==, want_home);
	g_assert_cmpstr(tilde, ==, g_get_home_dir());
	g_assert_cmpstr(abs, ==, "/etc/hosts");
	g_assert_cmpstr(empty, ==, "");
	g_assert_null(gsurf_resolve_path(NULL));
	g_assert_cmpstr(expanded, ==, want_expanded);
}

/* After gsurf_runtime_dir_enter() the process works in the runtime
 * directory, while user paths still resolve against where it started. */
static void
test_enter(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = g_get_current_dir();
	g_autofree gchar *after = NULL;
	g_autofree gchar *launch = NULL;
	g_autofree gchar *rel = NULL;
	g_autofree gchar *want = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *want_uri = NULL;
	g_autofree gchar *page = g_build_filename(before, "page.html", NULL);
	const gchar *dir = gsurf_runtime_dir_get(NULL);

	touch(page);
	g_assert_true(gsurf_runtime_dir_enter(&error));
	g_assert_no_error(error);
	g_assert_true(gsurf_runtime_dir_enter(NULL));
	after = g_get_current_dir();
	g_assert_cmpstr(after, ==, dir);
	launch = gsurf_get_launch_dir();
	g_assert_cmpstr(launch, ==, before);
	rel = gsurf_resolve_path("x");
	want = g_build_filename(before, "x", NULL);
	g_assert_cmpstr(rel, ==, want);

	/* A relative file argument still opens the file next to the user. */
	uri = gsurf_view_normalize_uri("page.html");
	want_uri = g_filename_to_uri(page, NULL, NULL);
	g_assert_cmpstr(uri, ==, want_uri);
	g_unlink(page);
}

/* Temporary containers get their session data below the runtime dir;
 * the registry removes it with the container. Uses a fake backend that
 * creates nothing. */
static gint fake_sessions = 0;

static gpointer
fake_create(const gchar *id, const gchar *data_dir, const gchar *cache_dir)
{
	(void)id;
	/* A temporary container gets real directories (no ephemeral session). */
	g_assert_nonnull(data_dir);
	g_assert_nonnull(cache_dir);
	g_assert_true(g_file_test(data_dir, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(cache_dir, G_FILE_TEST_IS_DIR));
	fake_sessions++;
	return GINT_TO_POINTER(fake_sessions);
}

static void
fake_destroy(gpointer native)
{
	(void)native;
	fake_sessions--;
}

static const GsurfSessionBackend fake_backend = { fake_create, fake_destroy, NULL, NULL };

static void
test_temp_container_dirs(void)
{
	g_autoptr(GsurfContainerRegistry) registry =
		g_object_new(GSURF_TYPE_CONTAINER_REGISTRY, NULL);
	g_autofree gchar *data1 = NULL;
	g_autofree gchar *data2 = NULL;
	g_autofree gchar *root1 = NULL;
	g_autofree gchar *id1 = NULL;
	g_autofree gchar *pdata = NULL;
	GsurfContainer *t1;
	GsurfContainer *t2;
	GsurfContainer *p;
	const gchar *runtime = gsurf_runtime_dir_get(NULL);
	GStatBuf st;
	guint i;

	g_setenv("XDG_DATA_HOME", test_home, TRUE);
	gsurf_container_registry_set_session_backend(registry, &fake_backend);
	gsurf_container_registry_set_temp_cleanup_delay(registry, 0);

	t1 = gsurf_container_registry_new_temporary(registry, NULL);
	t2 = gsurf_container_registry_new_temporary(registry, NULL);
	p = gsurf_container_registry_define(registry, "pers", NULL, NULL, NULL);
	g_assert_null(gsurf_container_get_data_dir(t1));
	g_assert_nonnull(gsurf_container_registry_acquire(registry, gsurf_container_get_id(t1)));
	g_assert_nonnull(gsurf_container_registry_acquire(registry, gsurf_container_get_id(t2)));
	g_assert_nonnull(gsurf_container_registry_acquire(registry, "pers"));

	/* Separate, owner-only directories under the runtime dir; the
	 * persistent container stays in the XDG data dir. */
	data1 = g_strdup(gsurf_container_get_data_dir(t1));
	data2 = g_strdup(gsurf_container_get_data_dir(t2));
	pdata = g_strdup(gsurf_container_get_data_dir(p));
	g_assert_cmpstr(data1, !=, data2);
	g_assert_true(g_str_has_prefix(data1, runtime));
	g_assert_true(g_str_has_prefix(data2, runtime));
	g_assert_false(g_str_has_prefix(pdata, runtime));
	g_assert_cmpint(g_stat(data1, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);
	root1 = g_path_get_dirname(data1);
	id1 = g_strdup(gsurf_container_get_id(t1));

	/* Releasing the last view destroys t1 and its directory. */
	gsurf_container_registry_release(registry, id1);
	for (i = 0; i < 50 && gsurf_container_registry_lookup(registry, id1) != NULL; i++)
		g_main_context_iteration(NULL, FALSE);
	g_assert_null(gsurf_container_registry_lookup(registry, id1));
	g_assert_false(g_file_test(root1, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(data2, G_FILE_TEST_IS_DIR));

	/* Disposing the registry removes the rest; persistent data stays. */
	g_clear_object(&registry);
	g_assert_false(g_file_test(data2, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(pdata, G_FILE_TEST_IS_DIR));
	g_assert_cmpint(fake_sessions, ==, 0);
}

/* Last: removes the process directory; a later get() makes a new one. */
static void
test_cleanup(void)
{
	g_autofree gchar *first = g_strdup(gsurf_runtime_dir_get(NULL));
	g_autofree gchar *cwd = NULL;
	const gchar *second;

	g_assert_nonnull(first);
	gsurf_runtime_dir_cleanup();
	g_assert_false(g_file_test(first, G_FILE_TEST_EXISTS));
	/* The process left the directory before removing it. */
	cwd = g_get_current_dir();
	g_assert_cmpstr(cwd, ==, test_origin);
	second = gsurf_runtime_dir_get(NULL);
	g_assert_nonnull(second);
	g_assert_cmpstr(second, !=, first);
	gsurf_runtime_dir_cleanup();
	gsurf_runtime_dir_cleanup();
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *runtime = NULL;
	g_autofree gchar *work = NULL;
	gint result;

	test_home = g_dir_make_tmp("gsurf-runtime-dir-XXXXXX", NULL);
	runtime = g_build_filename(test_home, "runtime", NULL);
	work = g_build_filename(test_home, "work", NULL);
	g_mkdir_with_parents(runtime, 0700);
	g_mkdir_with_parents(work, 0700);
	g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
	g_assert_cmpint(g_chdir(work), ==, 0);
	test_origin = g_get_current_dir();

	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/runtime-dir/create", test_create);
	g_test_add_func("/gsurf/runtime-dir/sweep", test_sweep);
	g_test_add_func("/gsurf/runtime-dir/remove-tree", test_remove_tree);
	g_test_add_func("/gsurf/runtime-dir/resolve", test_resolve);
	g_test_add_func("/gsurf/runtime-dir/enter", test_enter);
	g_test_add_func("/gsurf/runtime-dir/temp-container-dirs", test_temp_container_dirs);
	g_test_add_func("/gsurf/runtime-dir/cleanup", test_cleanup);
	result = g_test_run();
	g_chdir("/");
	gsurf_test_rmtree(test_home);
	g_free(test_origin);
	g_free(test_home);
	return result;
}
