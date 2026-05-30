/*
 * gsurf-session-module.c - Persist and restore open views
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfNavigationHook (save the open view URIs after each
 * navigation) and #GsurfInputHandler (a manual save key). On activate,
 * if `auto_restore` is set and no URI was given on the command line, the
 * saved session is reopened into the window. Restoring marks startup as
 * "handled" so the host does not also load the homepage.
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
	gchar    *file;
	gchar    *key_save;
	gboolean  auto_restore;
};

static void gsurf_session_nav_init(GsurfNavigationHookInterface *iface);
static void gsurf_session_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfSessionModule, gsurf_session_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_NAVIGATION_HOOK, gsurf_session_nav_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_session_input_init))

static void
session_save(GsurfSessionModule *self)
{
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
		if (u != NULL && *u != '\0' && g_strcmp0(u, "about:blank") != 0)
			g_string_append_printf(out, "%s\n", u);
	}
	g_file_set_contents(self->file, out->str, out->len, NULL);
	g_string_free(out, TRUE);
}

static void
gsurf_session_after_navigate(GsurfNavigationHook *hook, GsurfView *view, const gchar *uri)
{
	session_save(GSURF_SESSION_MODULE(hook));
}

static void
gsurf_session_nav_init(GsurfNavigationHookInterface *iface)
{
	iface->after_navigate = gsurf_session_after_navigate;
}

static gboolean
gsurf_session_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                               guint keyval, guint keycode, guint state, GsurfModePolicy mode)
{
	GsurfSessionModule *self = GSURF_SESSION_MODULE(handler);

	if (self->key_save != NULL && gsurf_keys_match(keyval, state, self->key_save)) {
		session_save(self);
		return TRUE;
	}
	return FALSE;
}

static void
gsurf_session_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_session_handle_key_event;
}

static const gchar *gsurf_session_get_name(GsurfModule *m) { return "session"; }
static const gchar *gsurf_session_get_description(GsurfModule *m)
{
	return "Persist and restore the set of open views";
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

	if (yaml_mapping_has_member(m, "file")) {
		f = yaml_mapping_get_string_member(m, "file");
		if (f != NULL) {
			g_free(self->file);
			self->file = (f[0] == '~' && f[1] == '/')
				? g_build_filename(g_get_home_dir(), f + 2, NULL) : g_strdup(f);
			g_autofree gchar *dir = g_path_get_dirname(self->file);
			g_mkdir_with_parents(dir, 0755);
		}
	}
	if (yaml_mapping_has_member(m, "auto_restore"))
		self->auto_restore = yaml_mapping_get_boolean_member(m, "auto_restore");
	if (yaml_mapping_has_member(m, "key_save")) {
		g_free(self->key_save);
		self->key_save = g_strdup(yaml_mapping_get_string_member(m, "key_save"));
	}
}

/* Reopen saved URIs: the first goes into the existing initial view, the
 * rest into fresh tabs. Returns the number of URIs restored. */
static guint
session_restore(GsurfSessionModule *self, GsurfWindow *window)
{
	GsurfConfig *config = gsurf_config_get_default();
	g_autofree gchar *contents = NULL;
	g_auto(GStrv) lines = NULL;
	guint i, restored = 0;

	if (self->file == NULL ||
	    !g_file_get_contents(self->file, &contents, NULL, NULL))
		return 0;

	lines = g_strsplit(contents, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *uri = g_strstrip(lines[i]);
		GsurfView *view;

		if (*uri == '\0')
			continue;

		if (restored == 0) {
			view = gsurf_window_get_active_view(window);
		} else {
			view = gsurf_view_new();
			if (config != NULL && config->settings != NULL)
				gsurf_view_apply_settings(view, config->settings);
			gsurf_window_add_view(window, view);
			g_object_unref(view);
		}
		if (view != NULL)
			gsurf_view_load_uri(view, uri);
		restored++;
	}
	return restored;
}

static gboolean
gsurf_session_activate(GsurfModule *module)
{
	GsurfSessionModule *self = GSURF_SESSION_MODULE(module);
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfApplication *app = gsurf_module_manager_get_application(mgr);
	GsurfWindow *window = gsurf_module_manager_get_active_window(mgr);
	const gchar *cli_uri;

	if (!self->auto_restore || window == NULL || app == NULL)
		return TRUE;

	/* A URI on the command line takes precedence over the saved session. */
	cli_uri = g_object_get_data(G_OBJECT(app), "gsurf-cli-uri");
	if (cli_uri != NULL)
		return TRUE;

	if (session_restore(self, window) > 0)
		g_object_set_data(G_OBJECT(app), "gsurf-startup-handled", GINT_TO_POINTER(1));

	return TRUE;
}

static void
gsurf_session_module_finalize(GObject *object)
{
	GsurfSessionModule *self = GSURF_SESSION_MODULE(object);
	g_clear_pointer(&self->file, g_free);
	g_clear_pointer(&self->key_save, g_free);
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
	self->key_save = g_strdup("Ctrl+Shift+s");
	self->auto_restore = FALSE;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_SESSION_MODULE;
}
