/*
 * gsurf-webkit-content.c - Shared WebKit user-content helpers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "gsurf-webkit-content.h"
#include "core/gsurf-web-process.h"
#include <string.h>

/* --- content-blocker ruleset cache ---
 *
 * Compiling a large ruleset (EasyList-sized) takes WebKit seconds. Each
 * identifier is therefore compiled once per process and the resulting
 * #WebKitUserContentFilter is added to every view that asks for it. A
 * sidecar file stores the checksum of the JSON last compiled under an
 * identifier, so an unchanged ruleset is loaded from WebKit's on-disk store
 * on the next launch instead of being compiled again. */

typedef struct {
	gchar                   *identifier;
	gchar                   *checksum;   /* SHA-256 of the JSON */
	GBytes                  *json;       /* kept until compilation finishes */
	WebKitUserContentFilter *filter;     /* ready filter, or NULL */
	gboolean                 compiling;
	gboolean                 failed;
	GPtrArray               *pending;    /* GWeakRef* to WebKitUserContentManager */
} FilterEntry;

static GHashTable                   *filter_cache;  /* identifier -> FilterEntry* (rc) */
static WebKitUserContentFilterStore *filter_store;

static void
weak_ref_free(gpointer data)
{
	g_weak_ref_clear(data);
	g_free(data);
}

static void
filter_entry_clear(gpointer data)
{
	FilterEntry *entry = data;

	g_free(entry->identifier);
	g_free(entry->checksum);
	g_clear_pointer(&entry->json, g_bytes_unref);
	g_clear_pointer(&entry->filter, webkit_user_content_filter_unref);
	g_clear_pointer(&entry->pending, g_ptr_array_unref);
}

static void
filter_entry_unref(gpointer data)
{
	g_rc_box_release_full(data, filter_entry_clear);
}

static gchar *
filter_cache_dir(void)
{
	return g_build_filename(g_get_user_cache_dir(), "gsurf", "filters", NULL);
}

static gchar *
filter_sidecar_path(const gchar *identifier)
{
	g_autofree gchar *dir = filter_cache_dir();
	g_autofree gchar *name = g_strconcat(identifier, ".gsurf-sha256", NULL);

	return g_build_filename(dir, name, NULL);
}

/* Remember which JSON produced the compiled file now in the store. */
static void
filter_write_sidecar(FilterEntry *entry)
{
	g_autofree gchar *path = filter_sidecar_path(entry->identifier);
	g_autoptr(GError) error = NULL;

	if (!g_file_set_contents(path, entry->checksum, -1, &error))
		g_debug("gsurf: content filter '%s': cannot record checksum: %s",
			entry->identifier, error->message);
}

/* Hand a finished (or failed) compilation to every view still waiting. */
static void
filter_entry_finish(FilterEntry *entry, WebKitUserContentFilter *filter)
{
	guint i;

	entry->compiling = FALSE;
	g_clear_pointer(&entry->json, g_bytes_unref);
	if (filter == NULL) {
		entry->failed = TRUE;
		g_ptr_array_set_size(entry->pending, 0);
		return;
	}
	entry->filter = webkit_user_content_filter_ref(filter);
	for (i = 0; i < entry->pending->len; i++) {
		g_autoptr(WebKitUserContentManager) ucm = g_weak_ref_get(g_ptr_array_index(entry->pending, i));

		if (ucm != NULL)
			webkit_user_content_manager_add_filter(ucm, entry->filter);
	}
	g_ptr_array_set_size(entry->pending, 0);
}

static void
on_filter_saved(GObject *source, GAsyncResult *res, gpointer data)
{
	FilterEntry *entry = data;
	WebKitUserContentFilter *filter;
	g_autoptr(GError) error = NULL;

	filter = webkit_user_content_filter_store_save_finish(
		WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &error);
	if (filter != NULL) {
		filter_write_sidecar(entry);
	} else {
		g_warning("gsurf: content filter '%s': %s", entry->identifier,
			error != NULL ? error->message : "compilation failed");
	}
	filter_entry_finish(entry, filter);
	if (filter != NULL)
		webkit_user_content_filter_unref(filter);
	filter_entry_unref(entry);
}

static void
filter_compile(FilterEntry *entry)
{
	webkit_user_content_filter_store_save(filter_store, entry->identifier,
		entry->json, NULL, on_filter_saved, entry);
}

/* A cached compiled file whose checksum matched; fall back to compiling
 * when the store no longer has it (cache cleared, WebKit upgraded). */
static void
on_filter_loaded(GObject *source, GAsyncResult *res, gpointer data)
{
	FilterEntry *entry = data;
	WebKitUserContentFilter *filter;

	filter = webkit_user_content_filter_store_load_finish(
		WEBKIT_USER_CONTENT_FILTER_STORE(source), res, NULL);
	if (filter == NULL) {
		filter_compile(entry);
		return;
	}
	filter_entry_finish(entry, filter);
	webkit_user_content_filter_unref(filter);
	filter_entry_unref(entry);
}

void
gsurf_webkit_content_add_filter(WebKitWebView *view, const gchar *identifier,
                                const gchar *json_rules)
{
	WebKitUserContentManager *ucm;
	g_autofree gchar *checksum = NULL;
	g_autofree gchar *sidecar = NULL;
	g_autofree gchar *previous = NULL;
	FilterEntry *entry;
	GWeakRef *weak;

	g_return_if_fail(WEBKIT_IS_WEB_VIEW(view));
	g_return_if_fail(identifier != NULL && json_rules != NULL);

	ucm = webkit_web_view_get_user_content_manager(view);
	if (filter_cache == NULL) {
		g_autofree gchar *dir = filter_cache_dir();

		g_mkdir_with_parents(dir, 0755);
		filter_cache = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			filter_entry_unref);
		filter_store = webkit_user_content_filter_store_new(dir);
	}

	/* Same identifier and same rules: reuse the compiled filter, or queue
	 * behind the compilation already in flight. */
	checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, json_rules, -1);
	entry = g_hash_table_lookup(filter_cache, identifier);
	if (entry != NULL && g_str_equal(entry->checksum, checksum)) {
		if (entry->filter != NULL) {
			webkit_user_content_manager_add_filter(ucm, entry->filter);
		} else if (entry->compiling) {
			weak = g_new0(GWeakRef, 1);
			g_weak_ref_init(weak, ucm);
			g_ptr_array_add(entry->pending, weak);
		}
		return;
	}

	/* New or changed ruleset. The entry is reference counted because a
	 * replaced entry may still have a compilation in flight. */
	entry = g_rc_box_new0(FilterEntry);
	entry->identifier = g_strdup(identifier);
	entry->checksum = g_steal_pointer(&checksum);
	entry->json = g_bytes_new(json_rules, strlen(json_rules));
	entry->pending = g_ptr_array_new_with_free_func(weak_ref_free);
	entry->compiling = TRUE;
	weak = g_new0(GWeakRef, 1);
	g_weak_ref_init(weak, ucm);
	g_ptr_array_add(entry->pending, weak);
	g_hash_table_replace(filter_cache, entry->identifier, g_rc_box_acquire(entry));

	sidecar = filter_sidecar_path(identifier);
	if (g_file_get_contents(sidecar, &previous, NULL, NULL) &&
	    g_str_equal(g_strstrip(previous), entry->checksum))
		webkit_user_content_filter_store_load(filter_store, identifier, NULL,
			on_filter_loaded, entry);
	else
		filter_compile(entry);
}

/* --- web-process extensions --- */

static void
on_initialize_extensions(WebKitWebContext *context, gpointer user_data)
{
	g_autofree gchar *dir = gsurf_web_process_extension_get_directory();
	g_autoptr(GVariant) data = NULL;

	if (dir == NULL)
		return;
	data = gsurf_web_process_extension_get_user_data();
#ifdef GSURF_BACKEND_GTK4
	webkit_web_context_set_web_process_extensions_directory(context, dir);
	webkit_web_context_set_web_process_extensions_initialization_user_data(context, data);
#else
	webkit_web_context_set_web_extensions_directory(context, dir);
	webkit_web_context_set_web_extensions_initialization_user_data(context, data);
#endif
}

void
gsurf_webkit_content_attach_context(WebKitWebContext *context)
{
	g_return_if_fail(WEBKIT_IS_WEB_CONTEXT(context));

	if (g_object_get_data(G_OBJECT(context), "gsurf-web-process-attached") != NULL)
		return;
	g_object_set_data(G_OBJECT(context), "gsurf-web-process-attached", GINT_TO_POINTER(1));
#ifdef GSURF_BACKEND_GTK4
	g_signal_connect(context, "initialize-web-process-extensions",
		G_CALLBACK(on_initialize_extensions), NULL);
#else
	g_signal_connect(context, "initialize-web-extensions",
		G_CALLBACK(on_initialize_extensions), NULL);
#endif
}
