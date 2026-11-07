/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

/* Run gsurf with @argv (NULL-terminated) and return its stdout; *ok gets
 * whether it exited successfully. */
static gchar *
run_capture(GSubprocessLauncher *launcher, gboolean *ok, const gchar *const *argv)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSubprocess) child = NULL;
	gchar *out = NULL;

	child = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_communicate_utf8(child, NULL, NULL, &out, NULL, &error));
	g_assert_no_error(error);
	*ok = g_subprocess_get_successful(child);
	return out;
}

/* Delete a directory tree (the test's private trash). */
static void
remove_tree(const gchar *path)
{
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
	const gchar *name;

	if (dir != NULL) {
		while ((name = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *child = g_build_filename(path, name, NULL);

			if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
			    !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
				remove_tree(child);
			else
				g_unlink(child);
		}
	}
	g_rmdir(path);
}

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
	/* Plain local GIO: no gvfs daemon writing metadata into XDG_DATA_HOME
	 * when --uninstall-pwa trashes a launcher. */
	g_subprocess_launcher_setenv(launcher, "GIO_USE_VFS", "local", TRUE);
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
		g_clear_pointer(&applications, g_dir_close);

		/* --list-pwa shows it under an id; --uninstall-pwa takes that id. */
		{
			const gchar *list[] = { binary, "--list-pwa", NULL };
			const gchar *uninstall[] = { binary, "--uninstall-pwa", "test-app", NULL };
			const gchar *again[] = { binary, "--uninstall-pwa", "test-app", NULL };
			const gchar *with_uri[] = { binary, "--list-pwa", "https://example.org", NULL };
			const gchar *with_pwa[] = { binary, "--pwa", "--uninstall-pwa", "x",
				"https://example.org", NULL };
			const gchar *orphan_container[] = { binary, "--pwa-container", "work",
				"https://example.org", NULL };
			const gchar *container_pwa[] = { binary, "--pwa", "--container", "work",
				"https://example.org", NULL };
			const gchar *pinned[] = { binary, "--pwa", "--pwa-name", "Pinned",
				"--pwa-container", "work", "https://pinned.example.org", NULL };
			g_autofree gchar *out = NULL;
			g_autofree gchar *trash = g_build_filename(root, "Trash", NULL);
			gboolean ok = FALSE;

			g_subprocess_launcher_set_flags(launcher,
				G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
			out = run_capture(launcher, &ok, list);
			g_assert_true(ok);
			g_assert_nonnull(strstr(out, "test-app"));
			g_assert_nonnull(strstr(out, "Test App"));
			g_assert_nonnull(strstr(out, "https://example.org"));
			g_clear_pointer(&out, g_free);
			out = run_capture(launcher, &ok, uninstall);
			g_assert_true(ok);
			g_assert_nonnull(strstr(out, "Uninstalled Test App (test-app)"));
			g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
			g_clear_pointer(&out, g_free);
			out = run_capture(launcher, &ok, list);
			g_assert_true(ok);
			g_assert_nonnull(strstr(out, "No gsurf PWAs installed"));
			g_clear_pointer(&out, g_free);
			out = run_capture(launcher, &ok, again);
			g_assert_false(ok);
			g_clear_pointer(&out, g_free);
			out = run_capture(launcher, &ok, with_uri);
			g_assert_false(ok);
			g_clear_pointer(&out, g_free);
			out = run_capture(launcher, &ok, with_pwa);
			g_assert_false(ok);
			g_clear_pointer(&out, g_free);
			/* --pwa-container needs --pwa; --container is not for --pwa. */
			out = run_capture(launcher, &ok, orphan_container);
			g_assert_false(ok);
			g_clear_pointer(&out, g_free);
			out = run_capture(launcher, &ok, container_pwa);
			g_assert_false(ok);
			g_clear_pointer(&out, g_free);
			/* A pinned app lists its container; the others show '-'. */
			out = run_capture(launcher, &ok, pinned);
			g_assert_true(ok);
			g_clear_pointer(&out, g_free);
			out = run_capture(launcher, &ok, list);
			g_assert_true(ok);
			g_assert_nonnull(strstr(out, "CONTAINER"));
			g_assert_nonnull(strstr(out, "pinned  Pinned  work"));
			g_clear_pointer(&out, g_free);
			{
				const gchar *drop[] = { binary, "--uninstall-pwa", "pinned", NULL };

				out = run_capture(launcher, &ok, drop);
				g_assert_true(ok);
			}
			/* XDG_DATA_HOME points into the test tree, so the trash does. */
			remove_tree(trash);
		}
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
