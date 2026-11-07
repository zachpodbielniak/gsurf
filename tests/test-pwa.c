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

/* Find the entry with @id in a listing. */
static GsurfPwaEntry *
entry_by_id(GPtrArray *entries, const gchar *id)
{
	guint i;

	for (i = 0; i < entries->len; i++) {
		GsurfPwaEntry *e = g_ptr_array_index(entries, i);

		if (g_strcmp0(e->id, id) == 0)
			return e;
	}
	return NULL;
}

static void
test_list_find_uninstall(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *root = g_dir_make_tmp("gsurf-pwa-list-XXXXXX", &error);
	g_autofree gchar *user = NULL;
	g_autofree gchar *system = NULL;
	g_autofree gchar *mail_user = NULL;
	g_autofree gchar *mail_system = NULL;
	g_autofree gchar *dash_a = NULL;
	g_autofree gchar *dash_b = NULL;
	g_autofree gchar *legacy = NULL;
	g_autofree gchar *intl = NULL;
	g_autofree gchar *stray = NULL;
	g_autofree gchar *mail_hash = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *data = NULL;
	g_autoptr(GKeyFile) desktop = g_key_file_new();
	g_autoptr(GPtrArray) entries = NULL;
	GsurfPwaEntry *e;
	gboolean trashed = TRUE;
	const gchar *nasty = "https://example.org/app%20one?q=$test&quote=\"x\"&tick=`id`#hash";
	guint i;

	g_assert_no_error(error);
	user = g_build_filename(root, "user", NULL);
	system = g_build_filename(root, "system", NULL);

	/* An empty (missing) pair of directories lists nothing. */
	entries = gsurf_pwa_list(user, system);
	g_assert_cmpuint(entries->len, ==, 0);
	g_clear_pointer(&entries, g_ptr_array_unref);

	/* The same URL per user and system-wide, two apps sharing a name, a
	 * launcher written before X-Gsurf-PWA-URI, and a non-Latin name. */
	mail_user = gsurf_pwa_install("https://mail.example.org", "My Mail", "/bin/true",
		user, &error);
	g_assert_no_error(error);
	mail_system = gsurf_pwa_install("https://mail.example.org", "My Mail", "/bin/true",
		system, &error);
	g_assert_no_error(error);
	dash_a = gsurf_pwa_install("https://dash.example.org/a", "Dash", "/bin/true", user,
		&error);
	g_assert_no_error(error);
	dash_b = gsurf_pwa_install("https://dash.example.org/b", "Dash", "/bin/true", user,
		&error);
	g_assert_no_error(error);
	legacy = gsurf_pwa_install(nasty, NULL, "/bin/true", user, &error);
	g_assert_no_error(error);
	g_assert_true(g_key_file_load_from_file(desktop, legacy, G_KEY_FILE_NONE, &error));
	g_assert_true(g_key_file_remove_key(desktop, "Desktop Entry", "X-Gsurf-PWA-URI",
		&error));
	data = g_key_file_to_data(desktop, NULL, NULL);
	g_assert_true(g_file_set_contents(legacy, data, -1, &error));
	g_assert_no_error(error);
	intl = gsurf_pwa_install("https://example.jp", "日本", "/bin/true", user, &error);
	g_assert_no_error(error);
	/* Other desktop files are not ours. */
	stray = g_build_filename(user, "gsurf-pwa-short.desktop", NULL);
	g_assert_true(g_file_set_contents(stray, "[Desktop Entry]\nName=x\n", -1, &error));
	g_assert_no_error(error);

	entries = gsurf_pwa_list(user, system);
	g_assert_cmpuint(entries->len, ==, 6);

	/* Ids: unique, sorted, slugged; hash suffix for a shared slug, plus the
	 * scope for one URL installed twice. */
	for (i = 1; i < entries->len; i++)
		g_assert_cmpint(g_strcmp0(
			((GsurfPwaEntry *)g_ptr_array_index(entries, i - 1))->id,
			((GsurfPwaEntry *)g_ptr_array_index(entries, i))->id), <, 0);
	mail_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
		"https://mail.example.org", -1);
	id = g_strdup_printf("my-mail-%.8s-user", mail_hash);
	e = entry_by_id(entries, id);
	g_assert_nonnull(e);
	g_assert_false(e->system);
	g_assert_cmpstr(e->path, ==, mail_user);
	g_clear_pointer(&id, g_free);
	id = g_strdup_printf("my-mail-%.8s-system", mail_hash);
	e = entry_by_id(entries, id);
	g_assert_nonnull(e);
	g_assert_true(e->system);
	g_assert_cmpstr(e->uri, ==, "https://mail.example.org");

	/* The legacy launcher's URL comes back intact from its Exec line. */
	e = entry_by_id(entries, "example-org");
	g_assert_nonnull(e);
	g_assert_cmpstr(e->name, ==, "example.org");
	g_assert_cmpstr(e->uri, ==, nasty);
	g_assert_cmpstr(e->path, ==, legacy);
	g_assert_nonnull(entry_by_id(entries, "pwa"));

	/* Find: id, name in any case, URL; ambiguous and unknown queries fail. */
	g_assert_true(gsurf_pwa_find(entries, "example-org", &error) == e);
	g_assert_true(gsurf_pwa_find(entries, "EXAMPLE.ORG", &error) == e);
	g_assert_true(gsurf_pwa_find(entries, nasty, &error) == e);
	g_assert_no_error(error);
	g_assert_null(gsurf_pwa_find(entries, "dash", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	g_assert_nonnull(strstr(error->message, "dash-"));
	g_clear_error(&error);
	g_assert_null(gsurf_pwa_find(entries, "https://mail.example.org", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	g_clear_error(&error);
	g_assert_null(gsurf_pwa_find(entries, "nope", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_null(gsurf_pwa_find(entries, "", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Uninstall: the launcher is gone and the listing shrinks. */
	g_assert_true(gsurf_pwa_uninstall(e, &trashed, &error));
	g_assert_no_error(error);
	g_assert_false(g_file_test(legacy, G_FILE_TEST_EXISTS));
	g_clear_pointer(&entries, g_ptr_array_unref);
	entries = gsurf_pwa_list(user, system);
	g_assert_cmpuint(entries->len, ==, 5);
	g_assert_null(entry_by_id(entries, "example-org"));
	/* With one Dash left... */
	e = gsurf_pwa_find(entries, "https://dash.example.org/a", &error);
	g_assert_nonnull(e);
	g_assert_true(gsurf_pwa_uninstall(e, NULL, &error));
	g_clear_pointer(&entries, g_ptr_array_unref);
	entries = gsurf_pwa_list(user, system);
	/* ...its slug no longer clashes and loses the hash. */
	e = entry_by_id(entries, "dash");
	g_assert_nonnull(e);
	g_assert_cmpstr(e->path, ==, dash_b);

	/* Tidy up (whatever went to a trash is already out of the tree). */
	for (i = 0; i < entries->len; i++)
		g_unlink(((GsurfPwaEntry *)g_ptr_array_index(entries, i))->path);
	g_unlink(stray);
	g_rmdir(user);
	g_rmdir(system);
	g_assert_cmpint(g_rmdir(root), ==, 0);
}

/* A pinned container reaches gsurf as `--container NAME` before `--`, through
 * the same GLib launcher desktop environments use, and lists back. */
static void
test_container(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDesktopAppInfo) app = NULL;
	g_autoptr(GAppLaunchContext) context = g_app_launch_context_new();
	g_autoptr(GKeyFile) desktop = g_key_file_new();
	g_autoptr(GPtrArray) entries = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("gsurf-pwa-container-XXXXXX", &error);
	g_autofree gchar *executable = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *data = NULL;
	const gchar *container = "Work $tuff \"x\" 50%";
	const gchar *uri = "https://mail.example.org/";
	GsurfPwaEntry *e;
	gint64 deadline;

	g_assert_no_error(error);
	executable = g_build_filename(directory, "gsurf", NULL);
	output = g_build_filename(directory, "arguments", NULL);
	g_assert_true(g_file_set_contents(executable,
		"#!/bin/bash\nprintf '%s\\n' \"$@\" > \"$GSURF_TEST_OUTPUT\"\n", -1, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(executable, 0700), ==, 0);

	/* An empty container is refused like an empty name. */
	g_assert_null(gsurf_pwa_install_full(uri, NULL, "", executable, directory, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	path = gsurf_pwa_install_full(uri, "Mail", container, executable, directory, &error);
	g_assert_no_error(error);
	app = g_desktop_app_info_new_from_filename(path);
	g_assert_nonnull(app);
	g_app_launch_context_setenv(context, "GSURF_TEST_OUTPUT", output);
	g_assert_true(g_app_info_launch(G_APP_INFO(app), NULL, context, &error));
	g_assert_no_error(error);
	expected = g_strdup_printf("--kiosk\n--container\n%s\n--\n%s\n", container, uri);
	deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
	do {
		g_clear_pointer(&contents, g_free);
		if (g_file_get_contents(output, &contents, NULL, NULL) &&
		    g_strcmp0(contents, expected) == 0)
			break;
		g_usleep(10000);
	} while (g_get_monotonic_time() < deadline);
	g_assert_cmpstr(contents, ==, expected);

	/* Listed with its container; without the key it comes from Exec. */
	entries = gsurf_pwa_list(directory, NULL);
	g_assert_cmpuint(entries->len, ==, 1);
	e = g_ptr_array_index(entries, 0);
	g_assert_cmpstr(e->container, ==, container);
	g_assert_cmpstr(e->uri, ==, uri);
	g_clear_pointer(&entries, g_ptr_array_unref);
	g_assert_true(g_key_file_load_from_file(desktop, path, G_KEY_FILE_NONE, &error));
	g_assert_true(g_key_file_remove_key(desktop, "Desktop Entry", "X-Gsurf-PWA-Container",
		&error));
	g_assert_true(g_key_file_remove_key(desktop, "Desktop Entry", "X-Gsurf-PWA-URI",
		&error));
	data = g_key_file_to_data(desktop, NULL, NULL);
	g_assert_true(g_file_set_contents(path, data, -1, &error));
	g_assert_no_error(error);
	entries = gsurf_pwa_list(directory, NULL);
	e = g_ptr_array_index(entries, 0);
	g_assert_cmpstr(e->container, ==, container);
	g_assert_cmpstr(e->uri, ==, uri);
	g_clear_pointer(&entries, g_ptr_array_unref);

	/* Reinstalling the URL without a container unpins it. */
	g_free(gsurf_pwa_install_full(uri, "Mail", NULL, executable, directory, &error));
	g_assert_no_error(error);
	entries = gsurf_pwa_list(directory, NULL);
	g_assert_cmpuint(entries->len, ==, 1);
	g_assert_null(((GsurfPwaEntry *)g_ptr_array_index(entries, 0))->container);

	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(g_unlink(executable), ==, 0);
	g_assert_cmpint(g_unlink(output), ==, 0);
	g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pwa/install-launch", test_install_and_launch);
	g_test_add_func("/pwa/invalid-input", test_invalid_input);
	g_test_add_func("/pwa/list-find-uninstall", test_list_find_uninstall);
	g_test_add_func("/pwa/container", test_container);
	return g_test_run();
}
