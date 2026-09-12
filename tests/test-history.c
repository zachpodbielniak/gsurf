/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gsurf.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

/* A model-only view supplies hostile page titles without a web process. */
typedef struct { GsurfView parent; } TestHistoryView;
typedef struct { GsurfViewClass parent; } TestHistoryViewClass;
G_DEFINE_TYPE(TestHistoryView, test_history_view, GSURF_TYPE_VIEW)

static const gchar *
test_history_title(GsurfView *view)
{
	(void)view;
	return "café\nforged\rrecord\tfield\033[31m";
}

static void
test_history_view_class_init(TestHistoryViewClass *klass)
{
	GSURF_VIEW_CLASS(klass)->get_title = test_history_title;
}

static void
test_history_view_init(TestHistoryView *self)
{
	(void)self;
}

typedef struct {
	GsurfModuleManager *manager;
	GsurfModule *module; /* borrowed from manager */
	gchar *dir;
	gchar *file;
	gchar *custom;
} HistoryFixture;

/* Every test starts with an absent XDG history directory. Configure must
 * remain side-effect free, and navigation must create storage on demand. */
static void
history_setup(HistoryFixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", &error);
	g_autofree gchar *exe_dir = NULL;
	g_autofree gchar *so = NULL;

	(void)data;
	g_assert_no_error(error);
	exe_dir = g_path_get_dirname(exe);
	so = g_build_filename(exe_dir, "modules", "history.so", NULL);
	fixture->dir = g_build_filename(g_get_user_data_dir(), "gsurf", NULL);
	fixture->file = g_build_filename(fixture->dir, "history", NULL);
	fixture->custom = g_build_filename(g_get_user_data_dir(), "custom-history", NULL);
	g_assert_false(g_file_test(fixture->dir, G_FILE_TEST_EXISTS));
	g_assert_true(gsurf_config_load_from_data(config,
		"modules:\n  history: {enabled: true}\n", -1, &error));
	g_assert_no_error(error);
	fixture->manager = gsurf_module_manager_new();
	gsurf_module_manager_set_config(fixture->manager, config);
	fixture->module = gsurf_module_manager_load_module(fixture->manager, so, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fixture->module);
	gsurf_module_manager_activate_all(fixture->manager);
}

/* Remove only this test's known files; unexpected contents fail cleanup. */
static void
history_teardown(HistoryFixture *fixture, gconstpointer data)
{
	(void)data;
	g_clear_object(&fixture->manager);
	if (g_file_test(fixture->file, G_FILE_TEST_EXISTS))
		g_assert_cmpint(g_unlink(fixture->file), ==, 0);
	if (g_file_test(fixture->custom, G_FILE_TEST_EXISTS))
		g_assert_cmpint(g_unlink(fixture->custom), ==, 0);
	if (g_file_test(fixture->dir, G_FILE_TEST_EXISTS))
		g_assert_cmpint(g_rmdir(fixture->dir), ==, 0);
	g_free(fixture->dir);
	g_free(fixture->file);
	g_free(fixture->custom);
}

/* Compare the complete log, including separators and the final newline. */
static void
assert_history(const gchar *file, const gchar *expected)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *contents = NULL;

	g_assert_true(g_file_get_contents(file, &contents, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(contents, ==, expected);
}

static void
history_records(HistoryFixture *fixture, gconstpointer data)
{
	g_autoptr(GsurfView) view = g_object_new(test_history_view_get_type(), NULL);
	GStatBuf st;

	(void)data;
	g_assert_false(g_file_test(fixture->dir, G_FILE_TEST_EXISTS));
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, view,
		"https://example.test/a\n\r\t\001\177?q=café%20");
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, NULL, "https://second.test");
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, view, "ABOUT:blank");
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, view, "DaTa:text/plain,secret");
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, view, "");
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, view, NULL);
	assert_history(fixture->file,
		"https://example.test/a%0A%0D%09%01%7F?q=café%20\tcafé forged record field [31m\n"
		"https://second.test\n");
	g_assert_cmpint(g_stat(fixture->dir, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 0077, ==, 0);
	g_assert_cmpint(g_stat(fixture->file, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 0077, ==, 0);
}

/* Updating one option must not send subsequent visits to another file. */
static void
history_reconfigure(HistoryFixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfConfig) update = gsurf_config_new();
	g_autoptr(GsurfView) view = g_object_new(test_history_view_get_type(), NULL);
	g_autofree gchar *yaml = g_strdup_printf(
		"modules:\n  history:\n    file: '%s'\n", fixture->custom);

	(void)data;
	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	gsurf_module_configure(fixture->module, config);
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, NULL, "https://first.test");
	g_assert_true(gsurf_config_load_from_data(update,
		"modules:\n  history: {log_titles: false}\n", -1, &error));
	g_assert_no_error(error);
	gsurf_module_configure(fixture->module, update);
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, view, "https://second.test");
	assert_history(fixture->custom, "https://first.test\nhttps://second.test\n");
	g_assert_false(g_file_test(fixture->file, G_FILE_TEST_EXISTS));
}

/* A failed directory creation is observable and a later visit can recover. */
static void
history_directory_failure(HistoryFixture *fixture, gconstpointer data)
{
	(void)data;
	g_assert_true(g_file_set_contents(fixture->dir, "blocker", -1, NULL));
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*history: cannot create*");
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, NULL, "https://lost.test");
	g_test_assert_expected_messages();
	g_assert_cmpint(g_unlink(fixture->dir), ==, 0);
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, NULL, "https://recovered.test");
	assert_history(fixture->file, "https://recovered.test\n");
}

/* A directory at the log path must produce an append error, not success. */
static void
history_open_failure(HistoryFixture *fixture, gconstpointer data)
{
	(void)data;
	g_assert_cmpint(g_mkdir_with_parents(fixture->file, 0700), ==, 0);
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*history: cannot append*");
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, NULL, "https://lost.test");
	g_test_assert_expected_messages();
	g_assert_cmpint(g_rmdir(fixture->file), ==, 0);
}

/* /dev/full accepts opens but fails writes, exercising the error path
 * after stream creation. Only the private test path is a new symlink. */
static void
history_write_failure(HistoryFixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) file = g_file_new_for_path(fixture->file);

	(void)data;
	if (!g_file_test("/dev/full", G_FILE_TEST_EXISTS)) {
		g_test_skip("/dev/full unavailable");
		return;
	}
	g_assert_cmpint(g_mkdir(fixture->dir, 0700), ==, 0);
	g_assert_true(g_file_make_symbolic_link(file, "/dev/full", NULL, &error));
	g_assert_no_error(error);
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*history: cannot save*");
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, NULL, "https://lost.test");
	g_test_assert_expected_messages();
	g_assert_cmpint(g_unlink(fixture->file), ==, 0);
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, NULL, "https://recovered.test");
	assert_history(fixture->file, "https://recovered.test\n");
}

/* An empty destination disables writes without creating a directory. */
static void
history_disabled_file(HistoryFixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = gsurf_config_new();

	(void)data;
	g_assert_true(gsurf_config_load_from_data(config,
		"modules:\n  history: {file: ''}\n", -1, &error));
	g_assert_no_error(error);
	gsurf_module_configure(fixture->module, config);
	gsurf_module_manager_dispatch_after_navigate(fixture->manager, NULL, "https://private.test");
	g_assert_false(g_file_test(fixture->dir, G_FILE_TEST_EXISTS));
}

int
main(int argc, char **argv)
{
	g_autofree gchar *root = g_dir_make_tmp("gsurf-history-XXXXXX", NULL);
	gint result;

	/* Set XDG before GLib caches it, keeping even default-path regressions
	 * entirely outside the user's actual browser data. */
	g_assert_nonnull(root);
	g_setenv("XDG_DATA_HOME", root, TRUE);
	g_setenv("HOME", root, TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add("/gsurf/history/records", HistoryFixture, NULL,
		history_setup, history_records, history_teardown);
	g_test_add("/gsurf/history/reconfigure", HistoryFixture, NULL,
		history_setup, history_reconfigure, history_teardown);
	g_test_add("/gsurf/history/directory-failure", HistoryFixture, NULL,
		history_setup, history_directory_failure, history_teardown);
	g_test_add("/gsurf/history/open-failure", HistoryFixture, NULL,
		history_setup, history_open_failure, history_teardown);
	g_test_add("/gsurf/history/write-failure", HistoryFixture, NULL,
		history_setup, history_write_failure, history_teardown);
	g_test_add("/gsurf/history/disabled-file", HistoryFixture, NULL,
		history_setup, history_disabled_file, history_teardown);
	result = g_test_run();
	g_assert_cmpint(g_rmdir(root), ==, 0);
	return result;
}
