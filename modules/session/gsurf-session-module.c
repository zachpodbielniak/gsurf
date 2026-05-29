/*
 * gsurf-session-module.c - Persist open views
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfNavigationHook: writes the active window's open view
 * URIs to a session file after each navigation. (Restore-on-startup is a
 * planned addition requiring a host startup hook.)
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_SESSION_MODULE (gsurf_session_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfSessionModule, gsurf_session_module,
                     GSURF, SESSION_MODULE, GsurfModule)

struct _GsurfSessionModule
{
	GsurfModule parent_instance;
	gchar *file;
};

static void gsurf_session_nav_init(GsurfNavigationHookInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfSessionModule, gsurf_session_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_NAVIGATION_HOOK, gsurf_session_nav_init))

static void
gsurf_session_after_navigate(GsurfNavigationHook *hook, GsurfView *view, const gchar *uri)
{
	GsurfSessionModule *self = GSURF_SESSION_MODULE(hook);
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());
	GString *out;
	guint i, n;

	if (self->file == NULL || window == NULL)
		return;

	out = g_string_new(NULL);
	n = gsurf_window_get_n_views(window);
	for (i = 0; i < n; i++) {
		GsurfView *v = gsurf_window_get_nth_view(window, i);
		const gchar *u = gsurf_view_get_uri(v);
		if (u != NULL && *u != '\0')
			g_string_append_printf(out, "%s\n", u);
	}
	g_file_set_contents(self->file, out->str, out->len, NULL);
	g_string_free(out, TRUE);
}

static void
gsurf_session_nav_init(GsurfNavigationHookInterface *iface)
{
	iface->after_navigate = gsurf_session_after_navigate;
}

static const gchar *gsurf_session_get_name(GsurfModule *m) { return "session"; }
static const gchar *gsurf_session_get_description(GsurfModule *m)
{
	return "Persist the set of open views to a session file";
}

static void
gsurf_session_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfSessionModule *self = GSURF_SESSION_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	const gchar *f;

	node = gsurf_config_get_module_node(config, "session");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);
	f = yaml_mapping_get_string_member(m, "file");
	if (f != NULL) {
		g_free(self->file);
		self->file = (f[0] == '~' && f[1] == '/')
			? g_build_filename(g_get_home_dir(), f + 2, NULL) : g_strdup(f);
		g_autofree gchar *dir = g_path_get_dirname(self->file);
		g_mkdir_with_parents(dir, 0755);
	}
}

static gboolean gsurf_session_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_session_module_finalize(GObject *object)
{
	GsurfSessionModule *self = GSURF_SESSION_MODULE(object);
	g_clear_pointer(&self->file, g_free);
	G_OBJECT_CLASS(gsurf_session_module_parent_class)->finalize(object);
}

static void
gsurf_session_module_class_init(GsurfSessionModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_session_module_finalize;
	module_class->activate = gsurf_session_activate;
	module_class->get_name = gsurf_session_get_name;
	module_class->get_description = gsurf_session_get_description;
	module_class->configure = gsurf_session_configure;
}

static void
gsurf_session_module_init(GsurfSessionModule *self)
{
	self->file = g_build_filename(g_get_user_data_dir(), "gsurf", "session", NULL);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_SESSION_MODULE;
}
