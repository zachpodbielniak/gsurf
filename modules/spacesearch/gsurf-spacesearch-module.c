/*
 * gsurf-spacesearch-module.c - Leading-space forces a search
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfUriHandler: bar input beginning with a space is sent
 * to a search engine even if it would otherwise look like a URL. Ports
 * surf's spacesearch patch.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <string.h>

#define GSURF_TYPE_SPACESEARCH_MODULE (gsurf_spacesearch_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfSpacesearchModule, gsurf_spacesearch_module,
                     GSURF, SPACESEARCH_MODULE, GsurfModule)

struct _GsurfSpacesearchModule
{
	GsurfModule parent_instance;
	gchar *url;   /* search URL template with %s */
};

static void gsurf_spacesearch_uri_init(GsurfUriHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfSpacesearchModule, gsurf_spacesearch_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_URI_HANDLER, gsurf_spacesearch_uri_init))

static gchar *
gsurf_spacesearch_rewrite_uri(GsurfUriHandler *handler, const gchar *input)
{
	GsurfSpacesearchModule *self = GSURF_SPACESEARCH_MODULE(handler);
	g_autofree gchar *trimmed = NULL;
	g_autofree gchar *escaped = NULL;
	const gchar *pct;
	GString *out;

	if (input == NULL || input[0] != ' ' || self->url == NULL)
		return NULL;

	trimmed = g_strdup(input);
	g_strstrip(trimmed);
	if (*trimmed == '\0')
		return NULL;
	escaped = g_uri_escape_string(trimmed, NULL, FALSE);

	pct = strstr(self->url, "%s");
	if (pct == NULL)
		return g_strconcat(self->url, escaped, NULL);

	out = g_string_new_len(self->url, pct - self->url);
	g_string_append(out, escaped);
	g_string_append(out, pct + 2);
	return g_string_free(out, FALSE);
}

static void
gsurf_spacesearch_uri_init(GsurfUriHandlerInterface *iface)
{
	iface->rewrite_uri = gsurf_spacesearch_rewrite_uri;
}

static const gchar *gsurf_spacesearch_get_name(GsurfModule *m) { return "spacesearch"; }
static const gchar *gsurf_spacesearch_get_description(GsurfModule *m)
{
	return "Leading space in the bar forces a search";
}

static void
gsurf_spacesearch_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfSpacesearchModule *self = GSURF_SPACESEARCH_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "spacesearch");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "url")) {
		g_free(self->url);
		self->url = g_strdup(yaml_mapping_get_string_member(m, "url"));
	}
}

static gboolean gsurf_spacesearch_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_spacesearch_module_finalize(GObject *object)
{
	GsurfSpacesearchModule *self = GSURF_SPACESEARCH_MODULE(object);

	g_clear_pointer(&self->url, g_free);

	G_OBJECT_CLASS(gsurf_spacesearch_module_parent_class)->finalize(object);
}

static void
gsurf_spacesearch_module_class_init(GsurfSpacesearchModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_spacesearch_module_finalize;
	module_class->activate = gsurf_spacesearch_activate;
	module_class->get_name = gsurf_spacesearch_get_name;
	module_class->get_description = gsurf_spacesearch_get_description;
	module_class->configure = gsurf_spacesearch_configure;
}

static void
gsurf_spacesearch_module_init(GsurfSpacesearchModule *self)
{
	self->url = g_strdup("https://duckduckgo.com/?q=%s");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_SPACESEARCH_MODULE;
}
