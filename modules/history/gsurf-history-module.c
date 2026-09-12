/*
 * gsurf-history-module.c - Visited-URI history log
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfNavigationHook: appends each committed URI (and,
 * optionally, the page title) to a history file. Ports surf's history
 * patch; the file is plain text for easy dmenu/grep integration.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <errno.h>
#include <string.h>

#define GSURF_TYPE_HISTORY_MODULE (gsurf_history_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfHistoryModule, gsurf_history_module,
                     GSURF, HISTORY_MODULE, GsurfModule)

struct _GsurfHistoryModule
{
	GsurfModule parent_instance;

	gchar    *file;
	gboolean  log_titles;
};

static void gsurf_history_nav_hook_init(GsurfNavigationHookInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfHistoryModule, gsurf_history_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_NAVIGATION_HOOK,
		gsurf_history_nav_hook_init))

/* Expand a leading "~/" to the user's home directory. */
static gchar *
expand_path(const gchar *path)
{
	if (path == NULL)
		return NULL;
	if (path[0] == '~' && path[1] == '/')
		return g_build_filename(g_get_home_dir(), path + 2, NULL);
	return g_strdup(path);
}

static void
gsurf_history_after_navigate(GsurfNavigationHook *hook, GsurfView *view, const gchar *uri)
{
	GsurfHistoryModule *self = GSURF_HISTORY_MODULE(hook);
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFileOutputStream) stream = NULL;
	g_autoptr(GString) record = NULL;
	g_autofree gchar *dir = NULL;
	const gchar *title;
	const guchar *p;

	if (self->file == NULL || *self->file == '\0' || uri == NULL || *uri == '\0')
		return;
	/* Skip internal/non-web schemes. */
	if (g_ascii_strncasecmp(uri, "about:", 6) == 0 ||
	    g_ascii_strncasecmp(uri, "data:", 5) == 0)
		return;

	/* Create storage lazily, including after an external directory removal.
	 * New directories and logs are private; existing permissions are kept. */
	dir = g_path_get_dirname(self->file);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		g_warning("gsurf history: cannot create '%s': %s", dir, g_strerror(errno));
		return;
	}
	file = g_file_new_for_path(self->file);
	stream = g_file_append_to(file, G_FILE_CREATE_PRIVATE, NULL, &error);
	if (stream == NULL) {
		g_warning("gsurf history: cannot append to '%s': %s", self->file, error->message);
		return;
	}

	/* Keep exactly one tab-separated record per navigation. URI controls
	 * are percent-encoded so reopening preserves their meaning; title
	 * controls become spaces so page content cannot inject log records. */
	record = g_string_new(NULL);
	for (p = (const guchar *)uri; *p != '\0'; p++) {
		if (*p < 0x20 || *p == 0x7f)
			g_string_append_printf(record, "%%%02X", (guint)*p);
		else
			g_string_append_c(record, (gchar)*p);
	}
	title = (self->log_titles && view != NULL) ? gsurf_view_get_title(view) : NULL;
	if (title != NULL && *title != '\0') {
		g_string_append_c(record, '\t');
		for (p = (const guchar *)title; *p != '\0'; p++)
			g_string_append_c(record, (*p < 0x20 || *p == 0x7f) ? ' ' : (gchar)*p);
	}
	g_string_append_c(record, '\n');

	/* Report both write and close failures. The automatic stream cleanup
	 * also closes the descriptor when writing fails before explicit close. */
	if (!g_output_stream_write_all(G_OUTPUT_STREAM(stream), record->str,
	                              record->len, NULL, NULL, &error) ||
	    !g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, &error))
		g_warning("gsurf history: cannot save '%s': %s", self->file, error->message);
}

static void
gsurf_history_nav_hook_init(GsurfNavigationHookInterface *iface)
{
	iface->after_navigate = gsurf_history_after_navigate;
}

static const gchar *
gsurf_history_get_name(GsurfModule *module)
{
	return "history";
}

static const gchar *
gsurf_history_get_description(GsurfModule *module)
{
	return "Log visited URIs to a history file";
}

static void
gsurf_history_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfHistoryModule *self = GSURF_HISTORY_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "history");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	/* Omitted options preserve the current configuration. In particular,
	 * changing title logging must not redirect a custom history file. */
	if (yaml_mapping_has_member(m, "file")) {
		g_free(self->file);
		self->file = expand_path(yaml_mapping_get_string_member(m, "file"));
	}
	if (yaml_mapping_has_member(m, "log_titles"))
		self->log_titles = yaml_mapping_get_boolean_member(m, "log_titles");
}

static gboolean
gsurf_history_activate(GsurfModule *module)
{
	return TRUE;
}

static void
gsurf_history_module_finalize(GObject *object)
{
	GsurfHistoryModule *self = GSURF_HISTORY_MODULE(object);

	g_clear_pointer(&self->file, g_free);

	G_OBJECT_CLASS(gsurf_history_module_parent_class)->finalize(object);
}

static void
gsurf_history_module_class_init(GsurfHistoryModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_history_module_finalize;

	module_class->activate = gsurf_history_activate;
	module_class->get_name = gsurf_history_get_name;
	module_class->get_description = gsurf_history_get_description;
	module_class->configure = gsurf_history_configure;
}

static void
gsurf_history_module_init(GsurfHistoryModule *self)
{
	self->file = g_build_filename(g_get_user_data_dir(), "gsurf", "history", NULL);
	self->log_titles = TRUE;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_HISTORY_MODULE;
}
