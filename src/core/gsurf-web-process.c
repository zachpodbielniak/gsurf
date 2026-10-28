/*
 * gsurf-web-process.c - Web-process extension registry
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gsurf-web-process.h"
#include <glib/gstdio.h>
#include <unistd.h>

/* One registered web-process extension. */
typedef struct {
	gchar    *path;   /* absolute path of the .so */
	GVariant *data;   /* initialization payload, or NULL */
} WebProcessExtension;

static GMutex      registry_lock;
static GHashTable *registry;   /* name (gchar*) -> WebProcessExtension* */

static void
web_process_extension_free(gpointer data)
{
	WebProcessExtension *ext = data;

	g_free(ext->path);
	g_clear_pointer(&ext->data, g_variant_unref);
	g_free(ext);
}

/* Lazily create the registry; callers hold registry_lock. */
static GHashTable *
get_registry(void)
{
	if (registry == NULL)
		registry = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			web_process_extension_free);
	return registry;
}

void
gsurf_web_process_extension_register(const gchar *name, const gchar *path, GVariant *data)
{
	WebProcessExtension *ext;

	g_return_if_fail(name != NULL && *name != '\0');
	g_return_if_fail(path != NULL && g_path_is_absolute(path));

	ext = g_new0(WebProcessExtension, 1);
	ext->path = g_strdup(path);
	ext->data = data != NULL ? g_variant_ref_sink(data) : NULL;

	g_mutex_lock(&registry_lock);
	g_hash_table_replace(get_registry(), g_strdup(name), ext);
	g_mutex_unlock(&registry_lock);
}

void
gsurf_web_process_extension_unregister(const gchar *name)
{
	g_return_if_fail(name != NULL);

	g_mutex_lock(&registry_lock);
	if (registry != NULL)
		g_hash_table_remove(registry, name);
	g_mutex_unlock(&registry_lock);
}

/* Remove link directories left behind by gsurf processes that are gone, so
 * the runtime directory does not accumulate one entry per past launch. */
static void
prune_stale_link_dirs(const gchar *parent)
{
	g_autoptr(GDir) dir = g_dir_open(parent, 0, NULL);
	const gchar *name;

	if (dir == NULL)
		return;
	while ((name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *proc = NULL;
		g_autofree gchar *path = NULL;
		g_autoptr(GDir) stale = NULL;
		const gchar *entry;
		gchar *end = NULL;
		guint64 pid;

		if (!g_str_has_prefix(name, "webext-"))
			continue;
		pid = g_ascii_strtoull(name + 7, &end, 10);
		if (end == NULL || *end != '\0' || pid == 0)
			continue;
		proc = g_strdup_printf("/proc/%" G_GUINT64_FORMAT, pid);
		if (g_file_test(proc, G_FILE_TEST_EXISTS))
			continue;
		path = g_build_filename(parent, name, NULL);
		stale = g_dir_open(path, 0, NULL);
		while (stale != NULL && (entry = g_dir_read_name(stale)) != NULL) {
			g_autofree gchar *link = g_build_filename(path, entry, NULL);
			g_unlink(link);
		}
		g_rmdir(path);
	}
}

/* Rebuild the per-process link directory so it holds exactly one link per
 * registered extension. Callers hold registry_lock. */
static gchar *
build_link_directory(void)
{
	g_autofree gchar *parent = g_build_filename(g_get_user_runtime_dir(), "gsurf", NULL);
	g_autofree gchar *pid = g_strdup_printf("webext-%ld", (long)getpid());
	g_autofree gchar *dir_path = g_build_filename(parent, pid, NULL);
	g_autoptr(GDir) dir = NULL;
	GHashTableIter it;
	gpointer key, value;
	const gchar *entry;

	if (g_mkdir_with_parents(dir_path, 0700) != 0) {
		g_warning("gsurf: cannot create web-process extension directory %s", dir_path);
		return NULL;
	}
	prune_stale_link_dirs(parent);

	/* Drop links from an earlier registration set. */
	dir = g_dir_open(dir_path, 0, NULL);
	while (dir != NULL && (entry = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *link = g_build_filename(dir_path, entry, NULL);
		g_unlink(link);
	}

	g_hash_table_iter_init(&it, registry);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		WebProcessExtension *ext = value;
		g_autofree gchar *base = g_path_get_basename(ext->path);
		g_autofree gchar *link_name = g_strdup_printf("%s-%s", (const gchar *)key, base);
		g_autofree gchar *link = g_build_filename(dir_path, link_name, NULL);

		if (symlink(ext->path, link) != 0)
			g_warning("gsurf: cannot link web-process extension %s", ext->path);
	}
	return g_steal_pointer(&dir_path);
}

gchar *
gsurf_web_process_extension_get_directory(void)
{
	gchar *dir = NULL;

	g_mutex_lock(&registry_lock);
	if (registry != NULL && g_hash_table_size(registry) == 1) {
		/* A single extension: point the engine at its own directory, which
		 * also keeps it readable inside a sandboxed web process. */
		GHashTableIter it;
		gpointer value;

		g_hash_table_iter_init(&it, registry);
		if (g_hash_table_iter_next(&it, NULL, &value))
			dir = g_path_get_dirname(((WebProcessExtension *)value)->path);
	} else if (registry != NULL && g_hash_table_size(registry) > 1) {
		dir = build_link_directory();
	}
	g_mutex_unlock(&registry_lock);
	return dir;
}

GVariant *
gsurf_web_process_extension_get_user_data(void)
{
	GVariantBuilder builder;
	GHashTableIter it;
	gpointer key, value;

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_mutex_lock(&registry_lock);
	if (registry != NULL) {
		g_hash_table_iter_init(&it, registry);
		while (g_hash_table_iter_next(&it, &key, &value)) {
			WebProcessExtension *ext = value;

			if (ext->data != NULL)
				g_variant_builder_add(&builder, "{sv}", (const gchar *)key, ext->data);
		}
	}
	g_mutex_unlock(&registry_lock);
	return g_variant_ref_sink(g_variant_builder_end(&builder));
}
