/*
 * test-wmux-docs.c - docs/wmux.org and the default config against wmux.so
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The wmux documentation cannot drift from the code:
 *
 *   - every #+begin_src yaml block of docs/wmux.org (the quick start, the
 *     cookbook) configures the real wmux.so without a single warning (an
 *     unknown key, a bad value, a binding that does not parse would warn;
 *     GLib tests fail on any warning);
 *   - the commented wmux: block of data/default-config.yaml does too, and
 *     lists exactly the keys of the "Configuration reference" table;
 *   - every example line of the "Command reference" parses as a binding
 *     of the real module.
 *
 * Headless: only configure() runs.
 */

#include <gsurf.h>
#include <string.h>

/* --- files next to the build tree (build/<mode>/test-*) --- */

static gchar *
read_repo_file(const gchar *dir_name, const gchar *file_name)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);
	g_autofree gchar *path = g_build_filename(dir, "..", "..", dir_name, file_name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read %s: %s", path, error->message);
	return text;
}

static gchar *
module_so_path(void)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);

	return g_build_filename(dir, "modules", "wmux.so", NULL);
}

/* A manager with wmux.so loaded (configured from an empty config), or
 * NULL when the module is not built. */
static GsurfModuleManager *
load_module(GsurfModule **module)
{
	g_autofree gchar *so = module_so_path();
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	GsurfModuleManager *mgr;

	if (!g_file_test(so, G_FILE_TEST_EXISTS))
		return NULL;
	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	*module = gsurf_module_manager_load_module(mgr, so, &error);
	g_assert_no_error(error);
	g_assert_nonnull(*module);
	return mgr;
}

/* Configure @module from @yaml; a warning anywhere fails the test. */
static void
configure_from(GsurfModule *module, const gchar *yaml, const gchar *what)
{
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GError) error = NULL;

	if (!gsurf_config_load_from_data(config, yaml, -1, &error))
		g_error("%s is not valid YAML: %s\n%s", what, error->message, yaml);
	g_test_message("configuring from %s", what);
	gsurf_module_configure(module, config);
}

/* --- docs/wmux.org --- */

/* Every yaml block of docs/wmux.org. */
static void
test_doc_yaml_blocks(void)
{
	g_autofree gchar *doc = read_repo_file("docs", "wmux.org");
	g_auto(GStrv) lines = g_strsplit(doc, "\n", -1);
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GString) block = NULL;
	GsurfModule *module = NULL;
	guint blocks = 0;
	guint start = 0;
	guint i;

	mgr = load_module(&module);
	if (mgr == NULL) {
		g_test_skip("wmux.so not built");
		return;
	}
	for (i = 0; lines[i] != NULL; i++) {
		if (strcmp(lines[i], "#+begin_src yaml") == 0) {
			g_assert_null(block);
			block = g_string_new(NULL);
			start = i + 1;
		} else if (strcmp(lines[i], "#+end_src") == 0 && block != NULL) {
			g_autofree gchar *what = g_strdup_printf("docs/wmux.org line %u", start);
			g_autofree gchar *text = g_string_free(g_steal_pointer(&block), FALSE);

			/* Every block is a complete config with a wmux section. */
			g_assert_true(g_str_has_prefix(text, "modules:\n  wmux:\n"));
			configure_from(module, text, what);
			blocks++;
		} else if (block != NULL) {
			g_string_append_printf(block, "%s\n", lines[i]);
		}
	}
	g_assert_null(block);
	/* The quick start and the cookbook. */
	g_assert_cmpuint(blocks, >=, 13);
}

/* The keys of the "Configuration reference" table, in order. */
static GPtrArray *
reference_keys(gchar **lines)
{
	GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);
	gboolean in_ref = FALSE;
	guint i;

	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];
		const gchar *open;
		const gchar *close;

		if (g_str_has_prefix(line, "* ")) {
			in_ref = strcmp(line, "* Configuration reference") == 0;
			continue;
		}
		if (!in_ref || !g_str_has_prefix(line, "| ="))
			continue;
		open = line + 3;
		close = strchr(open, '=');
		g_assert_nonnull(close);
		g_ptr_array_add(keys, g_strndup(open, (gsize)(close - open)));
	}
	return keys;
}

/* The wmux: block of data/default-config.yaml: from "  wmux:" to the
 * next line indented by two (another module, or a section comment). */
static gchar *
default_block(void)
{
	g_autofree gchar *text = read_repo_file("data", "default-config.yaml");
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	GString *out = NULL;
	guint i;

	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];

		if (out == NULL) {
			if (strcmp(line, "  wmux:") == 0)
				out = g_string_new("modules:\n  wmux:\n");
			continue;
		}
		if (g_str_has_prefix(line, "  ") && line[2] != ' ')
			break;
		g_string_append_printf(out, "%s\n", line);
	}
	g_assert_nonnull(out);
	return g_string_free(out, FALSE);
}

static void
test_default_config_block(void)
{
	g_autofree gchar *doc = read_repo_file("docs", "wmux.org");
	g_auto(GStrv) doc_lines = g_strsplit(doc, "\n", -1);
	g_autoptr(GPtrArray) documented = reference_keys(doc_lines);
	g_autofree gchar *block = default_block();
	g_auto(GStrv) lines = g_strsplit(block, "\n", -1);
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GPtrArray) listed = g_ptr_array_new_with_free_func(g_free);
	GsurfModule *module = NULL;
	guint commented_bindings = 0;
	guint i;

	mgr = load_module(&module);
	if (mgr == NULL) {
		g_test_skip("wmux.so not built");
		return;
	}

	/* 1. It configures without a warning, and stays disabled. */
	configure_from(module, block, "data/default-config.yaml wmux block");
	g_assert_nonnull(strstr(block, "    enabled: false\n"));

	/* 2. Its keys are the reference table's, in the same order. */
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];
		const gchar *colon;

		if (g_str_has_prefix(line, "    #   \""))
			commented_bindings++;
		if (!g_str_has_prefix(line, "    ") || line[4] == ' ' || line[4] == '#')
			continue;
		colon = strchr(line + 4, ':');
		g_assert_nonnull(colon);
		g_ptr_array_add(listed, g_strndup(line + 4, (gsize)(colon - line - 4)));
	}
	g_assert_cmpuint(listed->len, ==, documented->len);
	for (i = 0; i < listed->len; i++)
		g_assert_cmpstr(g_ptr_array_index(listed, i), ==, g_ptr_array_index(documented, i));
	g_assert_cmpuint(listed->len, ==, 34);

	/* 3. Its two commented binding examples are valid bindings. */
	g_assert_cmpuint(commented_bindings, ==, 2);
	for (i = 0; lines[i] != NULL; i++) {
		g_autofree gchar *binding = NULL;
		g_autofree gchar *yaml = NULL;
		gchar *comment;

		if (!g_str_has_prefix(lines[i], "    #   \""))
			continue;
		binding = g_strdup(lines[i] + 8);
		comment = strstr(binding, "  #");
		if (comment != NULL)
			*comment = '\0';
		yaml = g_strdup_printf("modules:\n  wmux:\n    bindings:\n      %s\n", binding);
		configure_from(module, yaml, "a commented binding of the default config");
	}
}

/* Every example of the "Command reference" as a binding of the module. */
static void
test_command_examples(void)
{
	g_autofree gchar *doc = read_repo_file("docs", "wmux.org");
	g_auto(GStrv) lines = g_strsplit(doc, "\n", -1);
	g_autoptr(GsurfModuleManager) mgr = NULL;
	GsurfModule *module = NULL;
	gboolean in_ref = FALSE;
	gboolean in_example = FALSE;
	guint n = 0;
	guint i;

	mgr = load_module(&module);
	if (mgr == NULL) {
		g_test_skip("wmux.so not built");
		return;
	}
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];

		if (g_str_has_prefix(line, "* ")) {
			in_ref = strcmp(line, "* Command reference") == 0;
			continue;
		}
		if (!in_ref)
			continue;
		if (strcmp(line, "#+begin_example") == 0) {
			in_example = TRUE;
		} else if (strcmp(line, "#+end_example") == 0) {
			in_example = FALSE;
		} else if (in_example && *line != '\0') {
			g_auto(GStrv) parts = g_strsplit(line, "'", -1);
			g_autofree gchar *quoted = g_strjoinv("''", parts);
			g_autofree gchar *yaml = g_strdup_printf(
				"modules:\n  wmux:\n    bindings:\n      F1: '%s'\n", quoted);

			configure_from(module, yaml, line);
			n++;
		}
	}
	g_assert_false(in_example);
	g_assert_cmpuint(n, >=, 50);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/wmux/docs/yaml-blocks", test_doc_yaml_blocks);
	g_test_add_func("/wmux/docs/default-config-block", test_default_config_block);
	g_test_add_func("/wmux/docs/command-examples", test_command_examples);
	return g_test_run();
}
