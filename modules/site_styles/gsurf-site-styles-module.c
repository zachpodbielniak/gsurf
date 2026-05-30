/*
 * gsurf-site-styles-module.c - Inject user stylesheets
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfScriptInjector. Applies:
 *   - a default stylesheet (config `default`, a file) to every view;
 *   - per-domain stylesheets from the `rules` sequence (each a mapping of
 *     `match` + `file`/`css`), scoped to their pattern via WebKit's
 *     user-style allow-list.
 * Ports surf's styles[] including per-URI rules.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>

#define GSURF_TYPE_SITE_STYLES_MODULE (gsurf_site_styles_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfSiteStylesModule, gsurf_site_styles_module,
                     GSURF, SITE_STYLES_MODULE, GsurfModule)

typedef struct {
	gchar *pattern;   /* WebKit URL allow-pattern, or NULL for all pages */
	gchar *css;       /* stylesheet source */
} StyleRule;

struct _GsurfSiteStylesModule
{
	GsurfModule parent_instance;
	GPtrArray  *rules;   /* StyleRule* (default entry has pattern == NULL) */
};

static void gsurf_site_styles_injector_init(GsurfScriptInjectorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfSiteStylesModule, gsurf_site_styles_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SCRIPT_INJECTOR, gsurf_site_styles_injector_init))

static void
style_rule_free(gpointer data)
{
	StyleRule *r = data;
	g_free(r->pattern);
	g_free(r->css);
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
add_rule(GsurfSiteStylesModule *self, const gchar *pattern, gchar *css /* takes ownership */)
{
	StyleRule *r;

	if (css == NULL)
		return;
	r = g_new0(StyleRule, 1);
	r->pattern = g_strdup(pattern);
	r->css = css;
	g_ptr_array_add(self->rules, r);
}

static void
add_rule_from_file(GsurfSiteStylesModule *self, const gchar *pattern, const gchar *path)
{
	g_autofree gchar *expanded = expand_path(path);
	gchar *css = NULL;

	if (expanded != NULL && g_file_get_contents(expanded, &css, NULL, NULL))
		add_rule(self, pattern, css);   /* transfers css */
}

static void
gsurf_site_styles_inject(GsurfScriptInjector *injector, GsurfView *view, const gchar *uri)
{
	GsurfSiteStylesModule *self = GSURF_SITE_STYLES_MODULE(injector);
	guint i;

	(void) uri;
	if (view == NULL)
		return;
	if (g_object_get_data(G_OBJECT(view), "gsurf-site-styles-done") != NULL)
		return;
	g_object_set_data(G_OBJECT(view), "gsurf-site-styles-done", GINT_TO_POINTER(1));

	for (i = 0; i < self->rules->len; i++) {
		StyleRule *r = g_ptr_array_index(self->rules, i);
		const gchar *allow[] = { r->pattern, NULL };
		gsurf_view_add_user_style_for(view, r->css, r->pattern != NULL ? allow : NULL);
	}
}

static void
gsurf_site_styles_injector_init(GsurfScriptInjectorInterface *iface)
{
	iface->inject = gsurf_site_styles_inject;
}

static const gchar *gsurf_site_styles_get_name(GsurfModule *m) { return "site_styles"; }
static const gchar *gsurf_site_styles_get_description(GsurfModule *m)
{
	return "Apply default and per-domain user stylesheets";
}

static void
load_rules_sequence(GsurfSiteStylesModule *self, YamlSequence *seq)
{
	guint i, n = yaml_sequence_get_length(seq);

	for (i = 0; i < n; i++) {
		YamlMapping *rm = yaml_sequence_get_mapping_element(seq, i);
		const gchar *match, *file, *css;

		if (rm == NULL || !yaml_mapping_has_member(rm, "match"))
			continue;
		match = yaml_mapping_get_string_member(rm, "match");
		if (match == NULL || *match == '\0')
			continue;

		if (yaml_mapping_has_member(rm, "file")) {
			file = yaml_mapping_get_string_member(rm, "file");
			if (file != NULL && *file != '\0')
				add_rule_from_file(self, match, file);
		} else if (yaml_mapping_has_member(rm, "css")) {
			css = yaml_mapping_get_string_member(rm, "css");
			if (css != NULL && *css != '\0')
				add_rule(self, match, g_strdup(css));
		}
	}
}

static void
gsurf_site_styles_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfSiteStylesModule *self = GSURF_SITE_STYLES_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "site_styles");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	g_ptr_array_set_size(self->rules, 0);

	if (yaml_mapping_has_member(m, "default")) {
		const gchar *path = yaml_mapping_get_string_member(m, "default");
		if (path != NULL && *path != '\0')
			add_rule_from_file(self, NULL, path);
	}
	if (yaml_mapping_has_member(m, "rules")) {
		YamlSequence *seq = yaml_mapping_get_sequence_member(m, "rules");
		if (seq != NULL)
			load_rules_sequence(self, seq);
	}
}

static gboolean gsurf_site_styles_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_site_styles_module_finalize(GObject *object)
{
	GsurfSiteStylesModule *self = GSURF_SITE_STYLES_MODULE(object);
	g_clear_pointer(&self->rules, g_ptr_array_unref);
	G_OBJECT_CLASS(gsurf_site_styles_module_parent_class)->finalize(object);
}

static void
gsurf_site_styles_module_class_init(GsurfSiteStylesModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_site_styles_module_finalize;
	module_class->activate = gsurf_site_styles_activate;
	module_class->get_name = gsurf_site_styles_get_name;
	module_class->get_description = gsurf_site_styles_get_description;
	module_class->configure = gsurf_site_styles_configure;
}

static void
gsurf_site_styles_module_init(GsurfSiteStylesModule *self)
{
	self->rules = g_ptr_array_new_with_free_func(style_rule_free);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_SITE_STYLES_MODULE;
}
