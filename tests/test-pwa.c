/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gsurf.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static void
test_install_and_launch(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GKeyFile) desktop = g_key_file_new();
	g_autoptr(GDesktopAppInfo) app = NULL;
	g_autoptr(GAppLaunchContext) context = g_app_launch_context_new();
	g_autofree gchar *directory = g_dir_make_tmp("gsurf-pwa-XXXXXX", &error);
	g_autofree gchar *executable = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *replacement = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *victim = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *expected = NULL;
	gint64 deadline;
	const gchar *uri = "https://example.org/app%20one?q=$test&quote=\"x\"&tick=`id`#hash";

	g_assert_no_error(error);
	executable = g_build_filename(directory, "gsurf space $ ` \" \\", NULL);
	output = g_build_filename(directory, "arguments", NULL);
	/* The recorder receives launch arguments directly; nothing evaluates URI
	 * text as code. Its path also stresses desktop Exec escaping. */
	g_assert_true(g_file_set_contents(executable,
		"#!/bin/bash\nprintf '%s\\n' \"$@\" > \"$GSURF_TEST_OUTPUT\"\n", -1, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(executable, 0700), ==, 0);
	path = gsurf_pwa_install(uri, NULL, executable, directory, &error);
	g_assert_no_error(error);
	g_assert_nonnull(path);
	g_assert_true(g_key_file_load_from_file(desktop, path, G_KEY_FILE_NONE, &error));
	g_assert_no_error(error);
	name = g_key_file_get_string(desktop, "Desktop Entry", "Name", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(name, ==, "example.org");

	/* Parse AND execute through the same GLib API desktop launchers use. */
	app = g_desktop_app_info_new_from_filename(path);
	g_assert_nonnull(app);
	g_app_launch_context_setenv(context, "GSURF_TEST_OUTPUT", output);
	g_assert_true(g_app_info_launch(G_APP_INFO(app), NULL, context, &error));
	g_assert_no_error(error);
	expected = g_strdup_printf("--kiosk\n--\n%s\n", uri);
	deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
	do {
		g_clear_pointer(&contents, g_free);
		if (g_file_get_contents(output, &contents, NULL, NULL) &&
		    g_strcmp0(contents, expected) == 0)
			break;
		g_usleep(10000);
	} while (g_get_monotonic_time() < deadline);
	g_assert_cmpstr(contents, ==, expected);

	/* Replacement must replace a symlink, preserving its unrelated target. */
	victim = g_build_filename(directory, "untouched", NULL);
	g_assert_true(g_file_set_contents(victim, "preserved", -1, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(symlink(victim, path), ==, 0);

	/* Names cannot escape the destination; rename replaces the same app. */
	replacement = gsurf_pwa_install(uri, "../Mail ü", executable, directory, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(replacement, ==, path);
	g_assert_false(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
	g_clear_pointer(&contents, g_free);
	g_assert_true(g_file_get_contents(victim, &contents, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(contents, ==, "preserved");
	g_assert_true(g_key_file_load_from_file(desktop, path, G_KEY_FILE_NONE, &error));
	g_assert_no_error(error);
	g_clear_pointer(&name, g_free);
	name = g_key_file_get_string(desktop, "Desktop Entry", "Name", &error);
	g_assert_cmpstr(name, ==, "../Mail ü");
	other = gsurf_pwa_install("https://example.org/other", NULL,
		executable, directory, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(path, !=, other);
	g_assert_cmpint(g_unlink(victim), ==, 0);
	g_assert_cmpint(g_unlink(other), ==, 0);
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(g_unlink(executable), ==, 0);
	g_assert_cmpint(g_unlink(output), ==, 0);
	g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void
test_invalid_input(void)
{
	const gchar *invalid[] = { "", "example.org", "file:///tmp/x", "javascript:alert(1)",
		"https://", "https://user:password@example.org", "https://example.org/a b",
		"https://example.org/\nExec=bad", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("gsurf-pwa-invalid-XXXXXX", &error);
	g_autofree gchar *destination = NULL;
	guint i;

	g_assert_no_error(error);
	destination = g_build_filename(directory, "applications", NULL);
	for (i = 0; invalid[i] != NULL; i++) {
		g_assert_null(gsurf_pwa_install(invalid[i], NULL, "/bin/true", destination, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
	g_assert_null(gsurf_pwa_install("https://example.org", "", "/bin/true", destination, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_null(gsurf_pwa_install("https://example.org", NULL, "/tmp/gsurf%", destination, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(g_file_test(destination, G_FILE_TEST_EXISTS));

	/* A file in place of the directory gives a useful propagated I/O error. */
	g_assert_true(g_file_set_contents(destination, "occupied", -1, &error));
	g_assert_no_error(error);
	g_assert_null(gsurf_pwa_install("https://example.org", NULL, "/bin/true", destination, &error));
	g_assert_nonnull(error);
	g_assert_cmpint(g_unlink(destination), ==, 0);
	g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pwa/install-launch", test_install_and_launch);
	g_test_add_func("/pwa/invalid-input", test_invalid_input);
	return g_test_run();
}
