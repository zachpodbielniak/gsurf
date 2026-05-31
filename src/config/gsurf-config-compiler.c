/*
 * gsurf-config-compiler.c - C configuration compiler (via crispy)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "config/gsurf-config-compiler.h"

#include <crispy.h>

#include <gio/gio.h>
#include <gmodule.h>
#include <string.h>

typedef gboolean (*GsurfConfigInitFunc)(void);

struct _GsurfConfigCompiler
{
	GObject parent_instance;

	CrispyGccCompiler *compiler;
};

G_DEFINE_FINAL_TYPE(GsurfConfigCompiler, gsurf_config_compiler, G_TYPE_OBJECT)

/* Extract the optional `#define CRISPY_PARAMS "..."` value, if present.
 *
 * Only an actual preprocessor define is honored — a commented-out example
 * (e.g. the `/* #define CRISPY_PARAMS "..." *​/` line in the shipped config
 * templates) is ignored. The line, after leading whitespace, must begin
 * with `#define`; that excludes lines that open with a comment. */
static gchar *
extract_crispy_params(const gchar *source)
{
	g_auto(GStrv) lines = g_strsplit(source, "\n", -1);
	guint i;

	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];
		const gchar *p, *start, *end;

		while (*line == ' ' || *line == '\t')
			line++;
		if (!g_str_has_prefix(line, "#define"))
			continue;
		p = strstr(line, "CRISPY_PARAMS");
		if (p == NULL)
			continue;
		start = strchr(p, '"');
		if (start == NULL)
			continue;
		start++;
		end = strchr(start, '"');
		if (end == NULL)
			continue;
		return g_strndup(start, end - start);
	}
	return NULL;
}

gchar *
gsurf_config_compiler_find_default_path(void)
{
	const gchar *env;
	gchar *path;

	env = g_getenv("GSURF_CONFIG_C");
	if (env != NULL && g_file_test(env, G_FILE_TEST_EXISTS))
		return g_strdup(env);

	path = g_build_filename(g_get_user_config_dir(), "gsurf", "config.c", NULL);
	if (g_file_test(path, G_FILE_TEST_EXISTS))
		return path;
	g_free(path);

	path = g_build_filename(GSURF_SYSCONFDIR, "gsurf", "config.c", NULL);
	if (g_file_test(path, G_FILE_TEST_EXISTS))
		return path;
	g_free(path);

	return NULL;
}

gboolean
gsurf_config_compiler_compile_and_load(GsurfConfigCompiler *self,
                                       const gchar *source_path,
                                       GError **error)
{
	g_autofree gchar *source = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *cache_dir = NULL;
	g_autofree gchar *out_so = NULL;
	g_autofree gchar *params = NULL;
	g_autofree gchar *extra_flags = NULL;
	GModule *gmod;
	gpointer sym = NULL;
	GsurfConfigInitFunc init_fn;

	g_return_val_if_fail(GSURF_IS_CONFIG_COMPILER(self), FALSE);
	g_return_val_if_fail(source_path != NULL, FALSE);

	if (!g_file_get_contents(source_path, &source, NULL, error))
		return FALSE;

	/* Content-hash cache: recompile only when the source changes. */
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, source, -1);
	cache_dir = g_build_filename(g_get_user_cache_dir(), "gsurf", "cconfig", NULL);
	g_mkdir_with_parents(cache_dir, 0755);
	out_so = g_build_filename(cache_dir, hash, NULL);
	{
		gchar *tmp = g_strconcat(out_so, ".so", NULL);
		g_free(out_so);
		out_so = tmp;
	}

	if (!g_file_test(out_so, G_FILE_TEST_EXISTS)) {
		params = extract_crispy_params(source);
		/* Resolve <gsurf/gsurf.h> and the headers' internal quote
		 * includes against the in-tree development headers. */
		extra_flags = g_strdup_printf("-I%s -I%s/gsurf%s%s",
			GSURF_DEV_INCLUDE_DIR, GSURF_DEV_INCLUDE_DIR,
			params ? " " : "", params ? params : "");

		if (!crispy_compiler_compile_shared(CRISPY_COMPILER(self->compiler),
				source_path, out_so, extra_flags, error))
			return FALSE;
	}

	/* Open globally so the config can resolve gsurf_* from the running
	 * process; keep it resident so its symbols stay valid. */
	gmod = g_module_open(out_so, G_MODULE_BIND_LAZY);
	if (gmod == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"failed to load compiled config: %s", g_module_error());
		return FALSE;
	}

	if (!g_module_symbol(gmod, "gsurf_config_init", &sym) || sym == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"compiled config has no gsurf_config_init symbol");
		g_module_close(gmod);
		return FALSE;
	}

	g_module_make_resident(gmod);
	init_fn = (GsurfConfigInitFunc)sym;
	return init_fn();
}

static void
gsurf_config_compiler_finalize(GObject *object)
{
	GsurfConfigCompiler *self = GSURF_CONFIG_COMPILER(object);

	g_clear_object(&self->compiler);

	G_OBJECT_CLASS(gsurf_config_compiler_parent_class)->finalize(object);
}

static void
gsurf_config_compiler_class_init(GsurfConfigCompilerClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gsurf_config_compiler_finalize;
}

static void
gsurf_config_compiler_init(GsurfConfigCompiler *self)
{
}

GsurfConfigCompiler *
gsurf_config_compiler_new(GError **error)
{
	GsurfConfigCompiler *self;
	CrispyGccCompiler *compiler;

	compiler = crispy_gcc_compiler_new(error);
	if (compiler == NULL)
		return NULL;

	self = g_object_new(GSURF_TYPE_CONFIG_COMPILER, NULL);
	self->compiler = compiler;
	return self;
}
