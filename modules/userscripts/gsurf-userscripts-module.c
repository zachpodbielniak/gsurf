/*
 * gsurf-userscripts-module.c - Inject user scripts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfScriptInjector. Injects:
 *   - a global script (config `global`) into every view;
 *   - every *.js file in a scripts directory (config `dir`) into every view;
 *   - per-domain scripts from the `rules` mapping (URL-pattern -> file),
 *     each scoped to its pattern via WebKit's user-script allow-list.
 * Ports surf's script.js behavior plus per-site scoping.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_USERSCRIPTS_MODULE (gsurf_userscripts_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfUserscriptsModule, gsurf_userscripts_module,
                     GSURF, USERSCRIPTS_MODULE, GsurfModule)

typedef struct {
	gchar *pattern;   /* WebKit URL allow-pattern, or NULL for all pages */
	gchar *source;    /* JavaScript source */
} ScriptRule;

struct _GsurfUserscriptsModule
{
	GsurfModule parent_instance;
	gboolean    at_end;
	GPtrArray  *rules;    /* ScriptRule* (global entries have pattern == NULL) */
};

static void gsurf_userscripts_injector_init(GsurfScriptInjectorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfUserscriptsModule, gsurf_userscripts_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SCRIPT_INJECTOR, gsurf_userscripts_injector_init))

static void
script_rule_free(gpointer data)
{
	ScriptRule *r = data;
	g_free(r->pattern);
	g_free(r->source);
	g_free(r);
}

static gchar *
expand_path(const gchar *path)
{
	if (path != NULL && path[0] == '~' && path[1] == '/')
		return g_build_filename(g_get_home_dir(), path + 2, NULL);
	return g_strdup(path);
}

static void
add_rule_from_file(GsurfUserscriptsModule *self, const gchar *pattern, const gchar *path)
{
	g_autofree gchar *expanded = expand_path(path);
	g_autofree gchar *source = NULL;

	if (expanded == NULL || !g_file_get_contents(expanded, &source, NULL, NULL))
		return;

	ScriptRule *r = g_new0(ScriptRule, 1);
	r->pattern = g_strdup(pattern);   /* NULL = all pages */
	r->source = g_steal_pointer(&source);
	g_ptr_array_add(self->rules, r);
}

static void
gsurf_userscripts_inject(GsurfScriptInjector *injector, GsurfView *view, const gchar *uri)
{
	GsurfUserscriptsModule *self = GSURF_USERSCRIPTS_MODULE(injector);
	guint i;

	(void) uri;
	if (view == NULL)
		return;
	/* Inject once per view (the hook also fires on URI commits). */
	if (g_object_get_data(G_OBJECT(view), "gsurf-userscripts-done") != NULL)
		return;
	g_object_set_data(G_OBJECT(view), "gsurf-userscripts-done", GINT_TO_POINTER(1));

	for (i = 0; i < self->rules->len; i++) {
		ScriptRule *r = g_ptr_array_index(self->rules, i);
		const gchar *allow[] = { r->pattern, NULL };
		gsurf_view_add_user_script_for(view, r->source, self->at_end,
			r->pattern != NULL ? allow : NULL);
	}
}

static void
gsurf_userscripts_injector_init(GsurfScriptInjectorInterface *iface)
{
	iface->inject = gsurf_userscripts_inject;
}

static const gchar *gsurf_userscripts_get_name(GsurfModule *m) { return "userscripts"; }
static const gchar *gsurf_userscripts_get_description(GsurfModule *m)
{
	return "Inject global, directory, and per-domain user JavaScript";
}

static void
load_dir(GsurfUserscriptsModule *self, const gchar *path)
{
	g_autofree gchar *expanded = expand_path(path);
	g_autoptr(GDir) dir = NULL;
	const gchar *name;

	if (expanded == NULL)
		return;
	dir = g_dir_open(expanded, 0, NULL);
	if (dir == NULL)
		return;
	while ((name = g_dir_read_name(dir)) != NULL) {
		if (g_str_has_suffix(name, ".js")) {
			g_autofree gchar *full = g_build_filename(expanded, name, NULL);
			add_rule_from_file(self, NULL, full);
		}
	}
}

static void
gsurf_userscripts_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfUserscriptsModule *self = GSURF_USERSCRIPTS_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "userscripts");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "inject_time"))
		self->at_end = g_strcmp0(yaml_mapping_get_string_member(m, "inject_time"), "start") != 0;

	/* Reset any previously-loaded scripts. */
	g_ptr_array_set_size(self->rules, 0);

	if (yaml_mapping_has_member(m, "global")) {
		const gchar *path = yaml_mapping_get_string_member(m, "global");
		if (path != NULL && *path != '\0')
			add_rule_from_file(self, NULL, path);
	}
	if (yaml_mapping_has_member(m, "dir")) {
		const gchar *path = yaml_mapping_get_string_member(m, "dir");
		if (path != NULL && *path != '\0')
			load_dir(self, path);
	}
	if (yaml_mapping_has_member(m, "rules")) {
		YamlNode *rn = yaml_mapping_get_member(m, "rules");
		if (rn != NULL && yaml_node_get_node_type(rn) == YAML_NODE_MAPPING) {
			YamlMapping *rm = yaml_node_get_mapping(rn);
			GList *keys = yaml_mapping_get_members(rm), *l;
			for (l = keys; l != NULL; l = l->next) {
				const gchar *pattern = l->data;
				const gchar *path = yaml_mapping_get_string_member(rm, pattern);
				if (path != NULL && *path != '\0')
					add_rule_from_file(self, pattern, path);
			}
			g_list_free(keys);
		}
	}
}

static gboolean gsurf_userscripts_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_userscripts_module_finalize(GObject *object)
{
	GsurfUserscriptsModule *self = GSURF_USERSCRIPTS_MODULE(object);
	g_clear_pointer(&self->rules, g_ptr_array_unref);
	G_OBJECT_CLASS(gsurf_userscripts_module_parent_class)->finalize(object);
}

static void
gsurf_userscripts_module_class_init(GsurfUserscriptsModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_userscripts_module_finalize;
	module_class->activate = gsurf_userscripts_activate;
	module_class->get_name = gsurf_userscripts_get_name;
	module_class->get_description = gsurf_userscripts_get_description;
	module_class->configure = gsurf_userscripts_configure;
}

static void
gsurf_userscripts_module_init(GsurfUserscriptsModule *self)
{
	self->at_end = TRUE;
	self->rules = g_ptr_array_new_with_free_func(script_rule_free);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_USERSCRIPTS_MODULE;
}
