/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gsurf.h>
#include <glib/gstdio.h>
#include <unistd.h>

/* Exercise the real CLI before display initialization and user config. */
static void
test_cli(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GDir) applications = NULL;
	g_autofree gchar *self = g_file_read_link("/proc/self/exe", &error);
	g_autofree gchar *bindir = NULL;
	g_autofree gchar *binary = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *path = NULL;
	const gchar *entry;

	g_assert_no_error(error);
	bindir = g_path_get_dirname(self);
	binary = g_build_filename(bindir, "gsurf", NULL);
	root = g_dir_make_tmp("gsurf-pwa-cli-XXXXXX", &error);
	g_assert_no_error(error);
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
		G_SUBPROCESS_FLAGS_STDERR_SILENCE);
	g_subprocess_launcher_setenv(launcher, "XDG_DATA_HOME", root, TRUE);
	g_subprocess_launcher_setenv(launcher, "DISPLAY", "", TRUE);
	g_subprocess_launcher_setenv(launcher, "WAYLAND_DISPLAY", "", TRUE);
	child = g_subprocess_launcher_spawn(launcher, &error, binary, "--pwa-name", "Orphan", NULL);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_wait(child, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(g_subprocess_get_successful(child));
	g_clear_object(&child);
	child = g_subprocess_launcher_spawn(launcher, &error, binary, "--kiosk", NULL);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_wait(child, NULL, &error));
	g_assert_false(g_subprocess_get_successful(child));
	g_clear_object(&child);
	child = g_subprocess_launcher_spawn(launcher, &error, binary, "--pwa",
		"https://example.org", "https://other.example", NULL);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_wait(child, NULL, &error));
	g_assert_false(g_subprocess_get_successful(child));
	g_clear_object(&child);

	/* Root installation deliberately ignores XDG_DATA_HOME. Never touch the
	 * real system applications directory from a hermetic test. */
	if (geteuid() != 0) {
		child = g_subprocess_launcher_spawn(launcher, &error, binary, "--pwa",
			"--pwa-name", "Test App", "https://example.org", NULL);
		g_assert_no_error(error);
		g_assert_true(g_subprocess_wait_check(child, NULL, &error));
		g_assert_no_error(error);
		directory = g_build_filename(root, "applications", NULL);
		applications = g_dir_open(directory, 0, &error);
		g_assert_no_error(error);
		entry = g_dir_read_name(applications);
		g_assert_nonnull(entry);
		g_assert_true(g_str_has_suffix(entry, ".desktop"));
		path = g_build_filename(directory, entry, NULL);
		g_assert_null(g_dir_read_name(applications));
		g_assert_cmpint(g_unlink(path), ==, 0);
		g_clear_pointer(&applications, g_dir_close);
		g_assert_cmpint(g_rmdir(directory), ==, 0);
	} else {
		g_test_message("Root: user-directory installation omitted to avoid system mutation");
	}
	g_assert_cmpint(g_rmdir(root), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pwa/cli", test_cli);
	return g_test_run();
}
