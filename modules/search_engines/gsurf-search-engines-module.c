/*
 * gsurf-search-engines-module.c - Prefix-based search engines
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfUriHandler: rewrites bar input such as "g cats" into a
 * search-engine URL, and (optionally) sends bare words to the default
 * engine. Ports surf's searchengines patch.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <string.h>

#define GSURF_TYPE_SEARCH_ENGINES_MODULE (gsurf_search_engines_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfSearchEnginesModule, gsurf_search_engines_module,
                     GSURF, SEARCH_ENGINES_MODULE, GsurfModule)

struct _GsurfSearchEnginesModule
{
	GsurfModule parent_instance;

	GHashTable *engines;          /* prefix (gchar*) -> url template (gchar*) */
	gchar      *default_url;      /* template for bare words, or NULL */
	gboolean    default_is_search;
};

static void gsurf_search_engines_uri_handler_init(GsurfUriHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfSearchEnginesModule, gsurf_search_engines_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_URI_HANDLER,
		gsurf_search_engines_uri_handler_init))

/* Heuristic: does this look like a URL we should load verbatim? */
static gboolean
looks_like_url(const gchar *s)
{
	if (strstr(s, "://") != NULL)
		return TRUE;
	if (g_str_has_prefix(s, "about:") || g_str_has_prefix(s, "file:") ||
	    g_str_has_prefix(s, "data:") || g_str_has_prefix(s, "localhost"))
		return TRUE;
	/* No spaces and contains a dot -> probably a bare hostname. */
	if (strchr(s, ' ') == NULL && strchr(s, '.') != NULL)
		return TRUE;
	return FALSE;
}

/* Replace the first "%s" in @template with the URL-escaped @query. */
static gchar *
build_search_url(const gchar *template, const gchar *query)
{
	g_autofree gchar *escaped = g_uri_escape_string(query, NULL, FALSE);
	const gchar *pct = strstr(template, "%s");
	GString *out;

	if (pct == NULL)
		return g_strconcat(template, escaped, NULL);

	out = g_string_new_len(template, pct - template);
	g_string_append(out, escaped);
	g_string_append(out, pct + 2);
	return g_string_free(out, FALSE);
}

static gchar *
gsurf_search_engines_rewrite_uri(GsurfUriHandler *handler, const gchar *input)
{
	GsurfSearchEnginesModule *self = GSURF_SEARCH_ENGINES_MODULE(handler);
	GHashTableIter iter;
	gpointer key, value;

	if (input == NULL || *input == '\0')
		return NULL;

	/* Prefix match: "g cats" -> google search for "cats". */
	g_hash_table_iter_init(&iter, self->engines);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		const gchar *prefix = key;
		if (g_str_has_prefix(input, prefix))
			return build_search_url(value, input + strlen(prefix));
	}

	/* Bare words -> default engine. */
	if (self->default_is_search && self->default_url != NULL &&
	    !looks_like_url(input))
		return build_search_url(self->default_url, input);

	return NULL;
}

static void
gsurf_search_engines_uri_handler_init(GsurfUriHandlerInterface *iface)
{
	iface->rewrite_uri = gsurf_search_engines_rewrite_uri;
}

static const gchar *
gsurf_search_engines_get_name(GsurfModule *module)
{
	return "search_engines";
}

static const gchar *
gsurf_search_engines_get_description(GsurfModule *module)
{
	return "Prefix-based search engines with default-engine fallback";
}

static void
gsurf_search_engines_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfSearchEnginesModule *self = GSURF_SEARCH_ENGINES_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m, *engines;
	const gchar *default_name = NULL;

	node = gsurf_config_get_module_node(config, "search_engines");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "default_is_search"))
		self->default_is_search = yaml_mapping_get_boolean_member(m, "default_is_search");
	if (yaml_mapping_has_member(m, "default"))
		default_name = yaml_mapping_get_string_member(m, "default");

	engines = yaml_mapping_get_mapping_member(m, "engines");
	if (engines != NULL) {
		GList *names = yaml_mapping_get_members(engines), *l;
		for (l = names; l != NULL; l = l->next) {
			const gchar *name = l->data;
			YamlMapping *e = yaml_mapping_get_mapping_member(engines, name);
			const gchar *prefix, *url;

			if (e == NULL)
				continue;
			prefix = yaml_mapping_get_string_member(e, "prefix");
			url = yaml_mapping_get_string_member(e, "url");
			if (url == NULL)
				continue;

			if (prefix != NULL)
				g_hash_table_replace(self->engines,
					g_strdup(prefix), g_strdup(url));
			if (default_name != NULL && g_strcmp0(name, default_name) == 0) {
				g_free(self->default_url);
				self->default_url = g_strdup(url);
			}
		}
		g_list_free(names);
	}
}

static gboolean
gsurf_search_engines_activate(GsurfModule *module)
{
	return TRUE;
}

static void
gsurf_search_engines_module_finalize(GObject *object)
{
	GsurfSearchEnginesModule *self = GSURF_SEARCH_ENGINES_MODULE(object);

	g_clear_pointer(&self->engines, g_hash_table_unref);
	g_clear_pointer(&self->default_url, g_free);

	G_OBJECT_CLASS(gsurf_search_engines_module_parent_class)->finalize(object);
}

static void
gsurf_search_engines_module_class_init(GsurfSearchEnginesModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_search_engines_module_finalize;

	module_class->activate = gsurf_search_engines_activate;
	module_class->get_name = gsurf_search_engines_get_name;
	module_class->get_description = gsurf_search_engines_get_description;
	module_class->configure = gsurf_search_engines_configure;
}

static void
gsurf_search_engines_module_init(GsurfSearchEnginesModule *self)
{
	self->engines = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->default_url = NULL;
	self->default_is_search = TRUE;
}

/* Module entry point. */
G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_SEARCH_ENGINES_MODULE;
}
