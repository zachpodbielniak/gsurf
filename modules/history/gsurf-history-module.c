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
#include <stdio.h>
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
	FILE *f;
	const gchar *title;

	if (self->file == NULL || uri == NULL || *uri == '\0')
		return;
	/* Skip internal/non-web schemes. */
	if (g_str_has_prefix(uri, "about:") || g_str_has_prefix(uri, "data:"))
		return;

	f = fopen(self->file, "a");
	if (f == NULL)
		return;

	title = (self->log_titles && view != NULL) ? gsurf_view_get_title(view) : NULL;
	if (title != NULL && *title != '\0')
		fprintf(f, "%s\t%s\n", uri, title);
	else
		fprintf(f, "%s\n", uri);

	fclose(f);
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
	const gchar *file = NULL;

	node = gsurf_config_get_module_node(config, "history");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "file"))
		file = yaml_mapping_get_string_member(m, "file");
	if (yaml_mapping_has_member(m, "log_titles"))
		self->log_titles = yaml_mapping_get_boolean_member(m, "log_titles");

	g_free(self->file);
	self->file = expand_path(file ? file : "~/.local/share/gsurf/history");

	/* Ensure the parent directory exists. */
	{
		g_autofree gchar *dir = g_path_get_dirname(self->file);
		g_mkdir_with_parents(dir, 0755);
	}
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
	self->file = NULL;
	self->log_titles = TRUE;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_HISTORY_MODULE;
}
