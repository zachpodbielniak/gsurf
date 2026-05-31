/*
 * test-cconfig.c - C-config compiler (crispy)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exercises the real compile-and-load pipeline: a config.c is compiled to a
 * cached .so and its gsurf_config_init() runs against the default config.
 * Skips cleanly when no compiler is available (e.g. no gcc in the env).
 */

#include <gsurf.h>
#include <glib.h>
#include <glib/gstdio.h>

static char *
write_tmp(const char *name, const char *contents)
{
	char *path = g_build_filename(g_get_tmp_dir(), name, NULL);
	g_assert_true(g_file_set_contents(path, contents, -1, NULL));
	return path;
}

/* A config that sets homepage, and — crucially — carries a *commented*
 * CRISPY_PARAMS referencing a nonexistent lib. The extractor must ignore it;
 * if it were honored, the link would fail with -lnonexistent_xyz. */
static const char *CONFIG_C =
	"/* #define CRISPY_PARAMS \"-lnonexistent_xyz\" */\n"
	"#include <gsurf/gsurf.h>\n"
	"G_MODULE_EXPORT gboolean\n"
	"gsurf_config_init(void)\n"
	"{\n"
	"    GsurfConfig *c = gsurf_config_get_default();\n"
	"    if (c == NULL) return FALSE;\n"
	"    g_free(c->homepage);\n"
	"    c->homepage = g_strdup(\"https://from-c-config\");\n"
	"    return TRUE;\n"
	"}\n";

static void
test_compile_and_load(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	GsurfConfigCompiler *cc;
	g_autofree char *path = NULL;
	gboolean ok;

	cc = gsurf_config_compiler_new(&error);
	if (cc == NULL) {
		g_test_skip("C-config compiler unavailable (no gcc?)");
		return;
	}

	gsurf_config_set_default(config);
	path = write_tmp("gsurf-test-config.c", CONFIG_C);

	ok = gsurf_config_compiler_compile_and_load(cc, path, &error);
	/* With the extractor fix, the commented CRISPY_PARAMS is ignored, so this
	 * compiles, links, loads, and runs gsurf_config_init(). */
	g_assert_no_error(error);
	g_assert_true(ok);
	g_assert_cmpstr(config->homepage, ==, "https://from-c-config");

	g_unlink(path);
	gsurf_config_set_default(NULL);
	g_object_unref(cc);
}

static void
test_compile_bad_source(void)
{
	g_autoptr(GError) error = NULL;
	GsurfConfigCompiler *cc;
	g_autofree char *path = NULL;

	cc = gsurf_config_compiler_new(&error);
	if (cc == NULL) {
		g_test_skip("C-config compiler unavailable (no gcc?)");
		return;
	}

	/* Source that does not compile must fail gracefully (FALSE + error), not
	 * crash the host. */
	path = write_tmp("gsurf-test-badconfig.c",
		"#include <gsurf/gsurf.h>\nthis is not valid C;\n");
	g_assert_false(gsurf_config_compiler_compile_and_load(cc, path, &error));

	g_unlink(path);
	g_object_unref(cc);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/cconfig/compile-and-load", test_compile_and_load);
	g_test_add_func("/gsurf/cconfig/bad-source", test_compile_bad_source);
	return g_test_run();
}
