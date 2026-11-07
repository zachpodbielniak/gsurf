/*
 * test-skill-examples.c - the /gsurf agent skill's examples against the code
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * skills/gsurf cannot drift from what gsurf actually accepts:
 *
 *   - every ```yaml block of SKILL.md and the references, and the whole of
 *     templates/config.yaml.template, loads through the real config loader
 *     and configures every built module without a single GLib warning; its
 *     root keys are core sections, its module names are real modules, and
 *     every keybinds:/mousebinds: value is a real action (the loader skips
 *     unknown actions silently, so this is checked here);
 *   - templates/config.c.template compiles, loads and runs through the real
 *     C-config compiler (crispy), and its embedded modules YAML configures
 *     every module cleanly; every complete C program in the references
 *     (one with a main() or a gsurf_config_init()) passes gcc -fsyntax-only;
 *   - templates/module.c.template and module.mk.template build a .so the
 *     way `make modules' does, with zero compiler warnings, and the module
 *     manager loads it and it behaves as its comment says;
 *   - every gsurfctl command the skill names exists (`gsurfctl help
 *     --names'); skipped while the gsurfctl binary is not built.
 *
 * Headless. Warnings are captured instead of aborting, so a failure names
 * the file and line of the offending block.
 */

#include <gsurf.h>
#include <yaml-glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "test-util.h"

/* Warnings and criticals logged since the last check_clean(). */
static GString *captured = NULL;

/* The handler g_test_init() installed; still prints every message. */
static GLogFunc previous_handler = NULL;

/*
 * capture_handler:
 *
 * Default log handler that records every warning and critical (with its
 * domain) in @captured, then hands the message to the test framework's
 * handler, which prints it.
 */
static void
capture_handler(
	const gchar		*log_domain,
	GLogLevelFlags		log_level,
	const gchar		*message,
	gpointer		user_data
){
	if ((log_level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING)) != 0)
		g_string_append_printf(captured, "[%s] %s\n", log_domain != NULL ? log_domain : "", message);
	previous_handler(log_domain, log_level, message, NULL);
}

/* Fail the current test with everything captured, naming @what. */
static void
check_clean(
	const gchar	*what
){
	if (captured->len > 0) {
		g_test_fail_printf("%s logged:\n%s", what, captured->str);
		g_string_truncate(captured, 0);
	}
}

/* --- paths next to the build tree (build/<mode>/test-*) --- */

/* (transfer full): the directory of this test binary (OUTDIR). */
static gchar *
out_dir(
	void
){
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);

	g_assert_nonnull(exe);
	return g_path_get_dirname(exe);
}

/* (transfer full): a path under the repository root. */
static gchar *
repo_path(
	const gchar	*relative
){
	g_autofree gchar *dir = out_dir();

	return g_build_filename(dir, "..", "..", relative, NULL);
}

static gchar *
read_file(
	const gchar	*path
){
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read %s: %s", path, error->message);
	return text;
}

/* (transfer full) (element-type filename): SKILL.md and every reference. */
static GPtrArray *
skill_markdown_files(
	void
){
	g_autofree gchar *skill = repo_path("skills/gsurf");
	g_autofree gchar *refs = g_build_filename(skill, "references", NULL);
	GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GDir) dir = g_dir_open(refs, 0, NULL);
	const gchar *name;

	g_ptr_array_add(files, g_build_filename(skill, "SKILL.md", NULL));
	g_assert_nonnull(dir);
	while ((name = g_dir_read_name(dir)) != NULL)
		if (g_str_has_suffix(name, ".md"))
			g_ptr_array_add(files, g_build_filename(refs, name, NULL));
	return files;
}

/*
 * Code blocks: (transfer full) (element-type utf8) of "<file>:<line>\n<code>"
 * for every fenced block of @lang in @path; the fence's indentation is
 * removed from each line.
 */
static GPtrArray *
fenced_blocks(
	const gchar	*path,
	const gchar	*lang
){
	g_autofree gchar *text = read_file(path);
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	g_autofree gchar *open = g_strconcat("```", lang, NULL);
	GPtrArray *blocks = g_ptr_array_new_with_free_func(g_free);
	GString *block = NULL;
	gsize indent = 0;
	guint i;

	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];
		const gchar *stripped = line + strspn(line, " ");

		if (block == NULL) {
			if (g_strcmp0(stripped, open) == 0) {
				indent = (gsize)(stripped - line);
				block = g_string_new(NULL);
				g_string_append_printf(block, "%s:%u\n", path, i + 2);
			}
			continue;
		}
		if (g_strcmp0(stripped, "```") == 0) {
			g_ptr_array_add(blocks, g_string_free(block, FALSE));
			block = NULL;
			continue;
		}
		g_string_append_printf(block, "%s\n", strlen(line) >= indent ? line + indent : "");
	}
	g_assert_null(block);
	return blocks;
}

/* --- the module manager with every built module --- */

static GsurfModuleManager *
load_all_modules(
	void
){
	g_autofree gchar *dir_path = NULL;
	g_autofree gchar *outdir = out_dir();
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GDir) dir = NULL;
	GsurfModuleManager *mgr;
	const gchar *name;

	dir_path = g_build_filename(outdir, "modules", NULL);
	dir = g_dir_open(dir_path, 0, NULL);
	if (dir == NULL)
		return NULL;
	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	while ((name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *so = NULL;
		g_autoptr(GError) error = NULL;

		if (!g_str_has_suffix(name, ".so"))
			continue;
		so = g_build_filename(dir_path, name, NULL);
		g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
		g_assert_no_error(error);
	}
	check_clean("loading the built modules");
	return mgr;
}

/* Configure every module of @mgr from @config; warnings fail as @what. */
static void
configure_all(
	GsurfModuleManager	*mgr,
	GsurfConfig		*config,
	const gchar		*what
){
	GPtrArray *modules = gsurf_module_manager_get_modules(mgr);
	guint i;

	for (i = 0; i < modules->len; i++)
		gsurf_module_configure(g_ptr_array_index(modules, i), config);
	check_clean(what);
}

/* --- YAML blocks --- */

static const gchar *const core_sections[] = {
	"browser", "user_agent", "webkit", "tls", "storage", "network", "window",
	"keybinds", "mousebinds", "modules", "ignore_yaml", "control", NULL
};

/* Root keys, module names and bound actions of one YAML document. */
static void
check_yaml_names(
	GsurfModuleManager	*mgr,
	const gchar		*yaml,
	const gchar		*what
){
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GList) roots = NULL;
	YamlMapping *root;
	YamlMapping *section;
	GList *l;

	g_assert_true(yaml_parser_load_from_data(parser, yaml, -1, &error));
	g_assert_no_error(error);
	root = yaml_node_get_mapping(yaml_parser_get_root(parser));
	g_assert_nonnull(root);
	roots = yaml_mapping_get_members(root);
	for (l = roots; l != NULL; l = l->next)
		if (!g_strv_contains(core_sections, l->data))
			g_test_fail_printf("%s: '%s' is not a section gsurf reads", what, (const gchar *)l->data);

	section = yaml_mapping_get_mapping_member(root, "modules");
	if (section != NULL) {
		g_autoptr(GList) names = yaml_mapping_get_members(section);

		for (l = names; l != NULL; l = l->next)
			if (gsurf_module_manager_get_module(mgr, l->data) == NULL &&
			    g_strcmp0(l->data, "mcp") != 0)   /* built only with MCP=1 */
				g_test_fail_printf("%s: no module named '%s'", what, (const gchar *)l->data);
	}
	for (guint b = 0; b < 2; b++) {
		g_autoptr(GList) keys = NULL;

		section = yaml_mapping_get_mapping_member(root, b == 0 ? "keybinds" : "mousebinds");
		if (section == NULL)
			continue;
		keys = yaml_mapping_get_members(section);
		for (l = keys; l != NULL; l = l->next) {
			const gchar *action = yaml_mapping_get_string_member(section, l->data);

			/* "none" / "" unbind a default; anything else must exist. */
			if (action != NULL && (*action == '\0' || g_ascii_strcasecmp(action, "none") == 0))
				continue;
			if (gsurf_action_from_string(action) == GSURF_ACTION_NONE)
				g_test_fail_printf("%s: '%s' is bound to unknown action '%s'",
					what, (const gchar *)l->data, action);
		}
	}
}

/* Load @yaml over the built-in defaults and configure every module. */
static void
check_yaml(
	GsurfModuleManager	*mgr,
	const gchar		*yaml,
	const gchar		*what
){
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *defaults_path = repo_path("data/default-config.yaml");
	g_autofree gchar *defaults = read_file(defaults_path);

	g_assert_true(gsurf_config_load_from_data(config, defaults, -1, &error));
	if (!gsurf_config_load_from_data(config, yaml, -1, &error)) {
		g_test_fail_printf("%s does not load: %s", what, error ? error->message : "?");
		return;
	}
	check_yaml_names(mgr, yaml, what);
	configure_all(mgr, config, what);
}

static void
test_yaml_blocks(
	void
){
	g_autoptr(GsurfModuleManager) mgr = load_all_modules();
	g_autoptr(GPtrArray) files = skill_markdown_files();
	g_autofree gchar *template_path = repo_path("skills/gsurf/templates/config.yaml.template");
	g_autofree gchar *template_yaml = read_file(template_path);
	guint n = 0;
	guint i;
	guint j;

	if (mgr == NULL) {
		g_test_skip("modules are not built");
		return;
	}
	for (i = 0; i < files->len; i++) {
		g_autoptr(GPtrArray) blocks = fenced_blocks(g_ptr_array_index(files, i), "yaml");

		for (j = 0; j < blocks->len; j++) {
			const gchar *block = g_ptr_array_index(blocks, j);
			const gchar *body = strchr(block, '\n') + 1;
			g_autofree gchar *what = g_strndup(block, (gsize)(body - 1 - block));

			check_yaml(mgr, body, what);
			n++;
		}
	}
	check_yaml(mgr, template_yaml, "templates/config.yaml.template");
	g_test_message("%u yaml blocks", n);
	g_assert_cmpuint(n, >=, 10);
}

/* --- C --- */

/* The C template through the real crispy compiler, loaded and run. */
static void
test_c_template(
	void
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autofree gchar *defaults_path = repo_path("data/default-config.yaml");
	g_autofree gchar *defaults = read_file(defaults_path);
	g_autofree gchar *template_path = repo_path("skills/gsurf/templates/config.c.template");
	g_autofree gchar *tmp = g_dir_make_tmp("gsurf-skill-XXXXXX", NULL);
	g_autofree gchar *source = g_build_filename(tmp, "config.c", NULL);
	g_autofree gchar *text = read_file(template_path);
	GsurfConfigCompiler *cc;

	cc = gsurf_config_compiler_new(&error);
	if (cc == NULL) {
		g_test_skip("C-config compiler unavailable (no gcc?)");
		gsurf_test_rmtree(tmp);
		return;
	}
	g_assert_true(gsurf_config_load_from_data(config, defaults, -1, NULL));
	gsurf_config_set_default(config);
	g_assert_true(g_file_set_contents(source, text, -1, NULL));
	g_assert_true(gsurf_config_compiler_compile_and_load(cc, source, &error));
	g_assert_no_error(error);
	check_clean("compiling templates/config.c.template");

	/* What the template sets. */
	g_assert_cmpstr(config->homepage, ==, "about:blank");
	g_assert_cmpint(config->window_width, ==, 1280);
	g_assert_false(config->settings->webgl);
	g_assert_cmpint(gsurf_config_get_keybind_action(config, "Ctrl+Shift+l"), ==, GSURF_ACTION_OPEN_PROMPT);
	g_assert_nonnull(gsurf_config_get_module_node(config, "modal"));
	g_assert_nonnull(gsurf_config_get_module_node(config, "containers"));

	mgr = load_all_modules();
	if (mgr != NULL)
		configure_all(mgr, config, "templates/config.c.template modules");
	gsurf_config_set_default(NULL);
	g_object_unref(cc);
	gsurf_test_rmtree(tmp);
}

/* gcc with the module flags; TRUE when it exits 0 with no warning. */
static gboolean
run_compiler(
	gchar		**argv,
	gchar		**output
){
	gint status = 0;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;

	if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &status, &error))
		g_error("cannot run %s: %s", argv[0], error->message);
	*output = g_strconcat(out, err, NULL);
	return g_spawn_check_wait_status(status, NULL) && strstr(*output, "warning:") == NULL;
}

/* (transfer full) (nullable): the module flags `make' wrote next to us. */
static gchar *
module_cflags(
	void
){
	g_autofree gchar *outdir = out_dir();
	g_autofree gchar *path = g_build_filename(outdir, "module-cflags.txt", NULL);
	gchar *flags = NULL;

	if (!g_file_get_contents(path, &flags, NULL, NULL))
		return NULL;
	return g_strstrip(flags);
}

/* Every complete C program in the references passes -fsyntax-only. */
static void
test_c_snippets(
	void
){
	g_autoptr(GPtrArray) files = skill_markdown_files();
	g_autofree gchar *flags = module_cflags();
	g_autofree gchar *tmp = g_dir_make_tmp("gsurf-skill-XXXXXX", NULL);
	g_auto(GStrv) flag_argv = NULL;
	guint n = 0;
	guint i;
	guint j;

	if (flags == NULL || !g_shell_parse_argv(flags, NULL, &flag_argv, NULL)) {
		g_test_skip("module-cflags.txt missing (built by make test)");
		gsurf_test_rmtree(tmp);
		return;
	}
	for (i = 0; i < files->len; i++) {
		g_autoptr(GPtrArray) blocks = fenced_blocks(g_ptr_array_index(files, i), "c");

		for (j = 0; j < blocks->len; j++) {
			const gchar *block = g_ptr_array_index(blocks, j);
			const gchar *body = strchr(block, '\n') + 1;
			g_autofree gchar *what = g_strndup(block, (gsize)(body - 1 - block));
			g_autofree gchar *source = g_build_filename(tmp, "snippet.c", NULL);
			g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
			g_auto(GStrv) argv = NULL;
			g_autofree gchar *output = NULL;

			if (strstr(body, "\nmain(") == NULL && strstr(body, "\ngsurf_config_init(") == NULL)
				continue;   /* a fragment, not a program */
			g_assert_true(g_file_set_contents(source, body, -1, NULL));
			g_strv_builder_add_many(builder, "gcc", "-fsyntax-only", NULL);
			g_strv_builder_addv(builder, (const char **)flag_argv);
			g_strv_builder_add(builder, source);
			argv = g_strv_builder_end(builder);
			if (!run_compiler(argv, &output))
				g_test_fail_printf("%s does not compile cleanly:\n%s", what, output);
			n++;
		}
	}
	g_test_message("%u C programs", n);
	g_assert_cmpuint(n, >=, 1);
	gsurf_test_rmtree(tmp);
}

/* The module template, built like a module and loaded by the manager. */
static void
test_module_template(
	void
){
	g_autofree gchar *flags = module_cflags();
	g_autofree gchar *outdir = out_dir();
	g_autofree gchar *tmp = g_dir_make_tmp("gsurf-skill-XXXXXX", NULL);
	g_autofree gchar *mod_dir = g_build_filename(tmp, "quicklinks", NULL);
	g_autofree gchar *so_dir = g_build_filename(tmp, "out", NULL);
	g_autofree gchar *so = g_build_filename(so_dir, "quicklinks.so", NULL);
	g_autofree gchar *c_path = repo_path("skills/gsurf/templates/module.c.template");
	g_autofree gchar *mk_path = repo_path("skills/gsurf/templates/module.mk.template");
	g_autofree gchar *c_text = read_file(c_path);
	g_autofree gchar *mk_text = read_file(mk_path);
	g_autofree gchar *c_dst = g_build_filename(mod_dir, "gsurf-quicklinks-module.c", NULL);
	g_autofree gchar *mk_dst = g_build_filename(mod_dir, "Makefile", NULL);
	g_autofree gchar *arg_out = g_strconcat("OUTDIR=", so_dir, NULL);
	g_autofree gchar *arg_lib = g_strconcat("LIBDIR=", outdir, NULL);
	g_autofree gchar *arg_cflags = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *rewritten = NULL;
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GError) error = NULL;
	gchar *argv[] = { "env", "-u", "MAKEFLAGS", "-u", "MAKELEVEL", "-u", "MFLAGS",
		"make", "-s", "-C", NULL, NULL, NULL, NULL, "LDFLAGS=-shared -fPIC", NULL };
	GsurfModule *module;

	if (flags == NULL) {
		g_test_skip("module-cflags.txt missing (built by make test)");
		gsurf_test_rmtree(tmp);
		return;
	}
	arg_cflags = g_strconcat("CFLAGS=", flags, NULL);
	g_assert_cmpint(g_mkdir_with_parents(mod_dir, 0700), ==, 0);
	g_assert_true(g_file_set_contents(c_dst, c_text, -1, NULL));
	g_assert_true(g_file_set_contents(mk_dst, mk_text, -1, NULL));
	argv[10] = mod_dir;
	argv[11] = arg_out;
	argv[12] = arg_lib;
	argv[13] = arg_cflags;
	if (!run_compiler(argv, &output))
		g_test_fail_printf("the module template does not build cleanly:\n%s", output);
	g_assert_true(g_file_test(so, G_FILE_TEST_EXISTS));

	/* Loaded and configured as its header comment says. */
	g_assert_true(gsurf_config_load_from_data(config,
		"modules:\n  quicklinks:\n    enabled: true\n"
		"    links: { gl: \"https://gitlab.com\" }\n", -1, NULL));
	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	module = gsurf_module_manager_load_module(mgr, so, &error);
	g_assert_no_error(error);
	g_assert_nonnull(module);
	g_assert_cmpstr(gsurf_module_get_name(module), ==, "quicklinks");
	g_assert_true(gsurf_module_get_enabled(module));
	g_assert_true(GSURF_IS_URI_HANDLER(module));
	rewritten = gsurf_uri_handler_rewrite_uri(GSURF_URI_HANDLER(module), " gl ");
	g_assert_cmpstr(rewritten, ==, "https://gitlab.com");
	g_clear_pointer(&rewritten, g_free);
	g_assert_null(gsurf_uri_handler_rewrite_uri(GSURF_URI_HANDLER(module), "nope"));
	check_clean("the module template");

	/* A bad entry warns and is skipped; a reconfigure replaces the links. */
	g_assert_true(gsurf_config_load_from_data(config,
		"modules:\n  quicklinks:\n    links: { bad: \"no scheme\", w: \"https://w.example/\" }\n",
		-1, NULL));
	gsurf_module_configure(module, config);
	g_assert_nonnull(strstr(captured->str, "quicklinks.links.bad"));
	g_string_truncate(captured, 0);
	g_assert_null(gsurf_uri_handler_rewrite_uri(GSURF_URI_HANDLER(module), "gl"));
	rewritten = gsurf_uri_handler_rewrite_uri(GSURF_URI_HANDLER(module), "w");
	g_assert_cmpstr(rewritten, ==, "https://w.example/");

	g_clear_object(&mgr);
	gsurf_test_rmtree(tmp);
}

/* --- gsurfctl commands --- */

/* Options of gsurfctl that take a value (the next word is not a command). */
static const gchar *const value_options[] = {
	"-i", "--instance", "-S", "--socket", "-T", "--timeout", "--completion",
	"--container", "--child-of", "--parent", "--before", "--after", NULL
};

/*
 * Add to @names the command of every "gsurfctl ..." in @code (one line of
 * code): the first word after the options. Placeholders (CMD, <x>, $var),
 * options-only invocations and "gsurfctl" used as a plain word are skipped.
 */
static void
collect_commands(
	const gchar	*code,
	GHashTable	*names
){
	const gchar *p = code;

	while ((p = strstr(p, "gsurfctl")) != NULL) {
		g_auto(GStrv) words = NULL;
		gboolean skip_value = FALSE;
		const gchar *end;
		guint i;

		/* A whole word, followed by a space, and not inside a quoted
		 * string (a message such as 'gsurfctl is not on PATH'). */
		if ((p > code && (g_ascii_isalnum(p[-1]) || strchr("-_/'\"", p[-1]) != NULL)) ||
		    p[8] != ' ') {
			p += 8;
			continue;
		}
		p += 9;
		end = p + strcspn(p, "|;&)`\n");
		{
			g_autofree gchar *rest = g_strndup(p, (gsize)(end - p));

			words = g_strsplit_set(rest, " \t", -1);
		}
		for (i = 0; words[i] != NULL; i++) {
			const gchar *w = words[i];

			if (*w == '\0')
				continue;
			if (skip_value) {
				skip_value = FALSE;
				continue;
			}
			if (w[0] == '-') {
				skip_value = g_strv_contains(value_options, w);
				continue;
			}
			if (g_regex_match_simple("^[a-z][a-z.-]*$", w, 0, 0))
				g_hash_table_add(names, g_strdup(w));
			break;
		}
	}
}

/* (transfer full): every gsurfctl command named in the skill. */
static GHashTable *
skill_commands(
	void
){
	GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GPtrArray) files = skill_markdown_files();
	g_autofree gchar *skill = repo_path("skills/gsurf");
	g_autofree gchar *script = g_build_filename(skill, "templates", "gsurfctl-session.sh.template", NULL);
	guint i;

	g_ptr_array_add(files, g_strdup(script));
	for (i = 0; i < files->len; i++) {
		const gchar *path = g_ptr_array_index(files, i);
		g_autofree gchar *text = read_file(path);
		g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
		gboolean fenced = g_str_has_suffix(path, ".template");
		gboolean in_commands = FALSE;
		guint j;

		for (j = 0; lines[j] != NULL; j++) {
			const gchar *line = lines[j];
			const gchar *stripped = line + strspn(line, " ");

			if (!g_str_has_suffix(path, ".template") && g_str_has_prefix(stripped, "```")) {
				fenced = !fenced;
				continue;
			}
			if (g_str_has_prefix(line, "## "))
				in_commands = g_str_has_suffix(path, "gsurfctl.md") && strcmp(line, "## Commands") == 0;
			if (fenced) {
				collect_commands(line, names);
			} else {
				/* Inline code spans; in the command table of gsurfctl.md the
				 * spans of the first column are commands without the
				 * "gsurfctl" prefix. */
				g_auto(GStrv) parts = g_strsplit(line, "`", -1);
				g_auto(GStrv) cells = g_strsplit(line, "|", 3);
				guint k;

				for (k = 1; parts[k] != NULL; k += 2) {
					collect_commands(parts[k], names);
					if (parts[k + 1] == NULL)
						break;
				}
				if (in_commands && g_str_has_prefix(line, "| `") && cells[1] != NULL) {
					g_auto(GStrv) first = g_strsplit(cells[1], "`", -1);

					for (k = 1; first[k] != NULL; k += 2) {
						g_autofree gchar *call = g_strconcat("gsurfctl ", first[k], NULL);

						collect_commands(call, names);
						if (first[k + 1] == NULL)
							break;
					}
				}
			}
		}
	}
	return names;
}

static void
test_gsurfctl_commands(
	void
){
	g_autofree gchar *outdir = out_dir();
	g_autofree gchar *ctl = g_build_filename(outdir, "gsurfctl", NULL);
	g_autoptr(GHashTable) named = skill_commands();
	g_autoptr(GHashTable) known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_auto(GStrv) lines = NULL;
	gchar *argv[] = { ctl, "help", "--names", NULL };
	GHashTableIter iter;
	gpointer key;
	gint status = 0;
	guint i;

	/* The extractor itself: commands the skill certainly names. */
	g_assert_true(g_hash_table_contains(named, "tabs"));
	g_assert_true(g_hash_table_contains(named, "wmux"));
	g_assert_true(g_hash_table_contains(named, "open"));
	g_assert_false(g_hash_table_contains(named, "bash"));   /* --completion bash */
	{
		GList *keys = g_list_sort(g_hash_table_get_keys(named), (GCompareFunc)g_strcmp0);
		GString *list = g_string_new(NULL);
		GList *l;

		for (l = keys; l != NULL; l = l->next)
			g_string_append_printf(list, " %s", (const gchar *)l->data);
		g_test_message("the skill names %u gsurfctl commands:%s", g_hash_table_size(named), list->str);
		g_string_free(list, TRUE);
		g_list_free(keys);
	}

	if (!g_file_test(ctl, G_FILE_TEST_IS_EXECUTABLE)) {
		g_test_skip("gsurfctl is not built");
		return;
	}
	g_assert_true(g_spawn_sync(NULL, argv, NULL, 0, NULL, NULL, &out, NULL, &status, &error));
	g_assert_no_error(error);
	g_assert_true(g_spawn_check_wait_status(status, NULL));
	lines = g_strsplit(out, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_strstrip(lines[i]);
		if (*lines[i] != '\0')
			g_hash_table_add(known, g_strdup(lines[i]));
	}
	g_assert_cmpuint(g_hash_table_size(known), >=, 20);
	g_hash_table_iter_init(&iter, named);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		if (!g_hash_table_contains(known, key))
			g_test_fail_printf("the skill names 'gsurfctl %s', which gsurfctl does not have",
				(const gchar *)key);
}

int
main(
	int	argc,
	char	**argv
){
	gint result;

	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	/* ccache cannot use the isolated cache directory; compile directly. */
	g_setenv("CCACHE_DISABLE", "1", TRUE);
	/* Record warnings instead of aborting, so each failure names its block. */
	captured = g_string_new(NULL);
	g_log_set_always_fatal(G_LOG_FATAL_MASK);
	previous_handler = g_log_set_default_handler(capture_handler, NULL);

	g_test_add_func("/gsurf/skill/yaml-blocks", test_yaml_blocks);
	g_test_add_func("/gsurf/skill/c-template", test_c_template);
	g_test_add_func("/gsurf/skill/c-snippets", test_c_snippets);
	g_test_add_func("/gsurf/skill/module-template", test_module_template);
	g_test_add_func("/gsurf/skill/gsurfctl-commands", test_gsurfctl_commands);
	result = g_test_run();
	g_string_free(captured, TRUE);
	return result;
}
