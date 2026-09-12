/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gsurf.h>

/* Exercise `gsurf --list-modules`: names must print in alphabetical order. */
static void
test_list_modules_sorted(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) child = NULL;
	g_autofree gchar *self = g_file_read_link("/proc/self/exe", &error);
	g_autofree gchar *bindir = NULL;
	g_autofree gchar *binary = NULL;
	g_autofree gchar *stdout_buf = NULL;
	g_auto(GStrv) lines = NULL;
	gchar *previous;
	gint count;
	gint i;

	g_assert_no_error(error);
	bindir = g_path_get_dirname(self);
	binary = g_build_filename(bindir, "gsurf", NULL);
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_SILENCE);
	g_subprocess_launcher_setenv(launcher, "DISPLAY", "", TRUE);
	g_subprocess_launcher_setenv(launcher, "WAYLAND_DISPLAY", "", TRUE);
	child = g_subprocess_launcher_spawn(launcher, &error, binary,
		"--list-modules", NULL);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_communicate_utf8(child, NULL, NULL,
		&stdout_buf, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(g_subprocess_get_successful(child));
	g_assert_nonnull(stdout_buf);

	lines = g_strsplit(stdout_buf, "\n", -1);
	previous = NULL;
	count = 0;
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line;
		g_autofree gchar *stripped = NULL;
		g_auto(GStrv) fields = NULL;
		const gchar *name;

		line = lines[i];
		if (!g_str_has_prefix(line, "  "))
			continue;
		stripped = g_strdup(line);
		g_strstrip(stripped);
		fields = g_strsplit(stripped, " ", 2);
		if (fields[0] == NULL)
			continue;
		name = fields[0];
		if (g_strcmp0(name, "MODULE") == 0 ||
		    g_strcmp0(name, "------") == 0)
			continue;
		if (previous != NULL)
			g_assert_cmpstr(previous, <, name);
		g_free(previous);
		previous = g_strdup(name);
		count++;
	}
	g_free(previous);
	g_assert_cmpint(count, >=, 10);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/cli/list-modules-sorted", test_list_modules_sorted);
	return g_test_run();
}
