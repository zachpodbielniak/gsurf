/*
 * gsurf-adblock-module.c - Host-based request/navigation blocking
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfNavigationHook: blocks navigations whose host is in a
 * blocklist (loaded from hosts-format files and an inline list). This is
 * the navigation-level layer; sub-resource filtering via WebKit content
 * filters is a planned addition. A feature surf itself lacks.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <string.h>

#define GSURF_TYPE_ADBLOCK_MODULE (gsurf_adblock_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfAdblockModule, gsurf_adblock_module,
                     GSURF, ADBLOCK_MODULE, GsurfModule)

struct _GsurfAdblockModule
{
	GsurfModule parent_instance;
	GHashTable *blocked;     /* host (gchar*) -> 1 */
	GHashTable *whitelist;   /* host (gchar*) -> 1 */
};

static void gsurf_adblock_nav_init(GsurfNavigationHookInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfAdblockModule, gsurf_adblock_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_NAVIGATION_HOOK, gsurf_adblock_nav_init))

static gboolean
host_is_blocked(GsurfAdblockModule *self, const gchar *host)
{
	const gchar *p;

	if (host == NULL)
		return FALSE;
	if (g_hash_table_contains(self->whitelist, host))
		return FALSE;

	/* Exact host, then each parent domain (sub.ads.example -> ads.example
	 * -> example). */
	if (g_hash_table_contains(self->blocked, host))
		return TRUE;
	for (p = host; (p = strchr(p, '.')) != NULL; ) {
		p++;
		if (g_hash_table_contains(self->blocked, p))
			return TRUE;
	}
	return FALSE;
}

static GsurfPolicyDecision
gsurf_adblock_before_navigate(GsurfNavigationHook *hook, GsurfView *view, const gchar *uri)
{
	GsurfAdblockModule *self = GSURF_ADBLOCK_MODULE(hook);
	g_autoptr(GUri) parsed = NULL;
	const gchar *host;

	if (uri == NULL)
		return GSURF_POLICY_USE;

	parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
	if (parsed == NULL)
		return GSURF_POLICY_USE;

	host = g_uri_get_host(parsed);
	if (host_is_blocked(self, host)) {
		g_message("gsurf adblock: blocked %s", host);
		return GSURF_POLICY_IGNORE;
	}
	return GSURF_POLICY_USE;
}

static void
gsurf_adblock_nav_init(GsurfNavigationHookInterface *iface)
{
	iface->before_navigate = gsurf_adblock_before_navigate;
}

static const gchar *gsurf_adblock_get_name(GsurfModule *m) { return "adblock"; }
static const gchar *gsurf_adblock_get_description(GsurfModule *m)
{
	return "Host-based navigation blocking (hosts files + inline list)";
}

static gchar *
expand_path(const gchar *path)
{
	if (path != NULL && path[0] == '~' && path[1] == '/')
		return g_build_filename(g_get_home_dir(), path + 2, NULL);
	return g_strdup(path);
}

/* Parse a hosts-format file: ignore comments/blank lines; for each line
 * take the last whitespace-separated token as the host (so both
 * "0.0.0.0 ads.example" and "ads.example" work). */
static void
load_hosts_file(GsurfAdblockModule *self, const gchar *path)
{
	g_autofree gchar *expanded = expand_path(path);
	g_autofree gchar *contents = NULL;
	gchar **lines;
	guint i;

	if (expanded == NULL ||
	    !g_file_get_contents(expanded, &contents, NULL, NULL))
		return;

	lines = g_strsplit(contents, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		gchar *line = g_strstrip(lines[i]);
		gchar **toks;
		const gchar *host;

		if (*line == '\0' || *line == '#')
			continue;
		toks = g_strsplit_set(line, " \t", -1);
		host = toks[0];
		for (guint j = 0; toks[j] != NULL; j++)
			if (*toks[j] != '\0')
				host = toks[j];
		if (host != NULL && *host != '\0' &&
		    g_strcmp0(host, "localhost") != 0)
			g_hash_table_add(self->blocked, g_strdup(host));
		g_strfreev(toks);
	}
	g_strfreev(lines);
}

static void
load_sequence(YamlMapping *m, const gchar *key, void (*fn)(GsurfAdblockModule *, const gchar *),
              GsurfAdblockModule *self)
{
	YamlSequence *seq = yaml_mapping_get_sequence_member(m, key);
	guint i, n;

	if (seq == NULL)
		return;
	n = yaml_sequence_get_length(seq);
	for (i = 0; i < n; i++) {
		YamlNode *e = yaml_sequence_get_element(seq, i);
		if (e != NULL && yaml_node_get_node_type(e) == YAML_NODE_SCALAR)
			fn(self, yaml_node_get_string(e));
	}
}

static void
add_whitelist(GsurfAdblockModule *self, const gchar *host)
{
	if (host != NULL && *host != '\0')
		g_hash_table_add(self->whitelist, g_strdup(host));
}

static void
gsurf_adblock_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfAdblockModule *self = GSURF_ADBLOCK_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "adblock");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	load_sequence(m, "host_files", load_hosts_file, self);
	load_sequence(m, "block_lists", load_hosts_file, self);
	load_sequence(m, "whitelist", add_whitelist, self);

	g_message("gsurf adblock: %u blocked host(s)", g_hash_table_size(self->blocked));
}

static gboolean gsurf_adblock_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_adblock_module_finalize(GObject *object)
{
	GsurfAdblockModule *self = GSURF_ADBLOCK_MODULE(object);

	g_clear_pointer(&self->blocked, g_hash_table_unref);
	g_clear_pointer(&self->whitelist, g_hash_table_unref);

	G_OBJECT_CLASS(gsurf_adblock_module_parent_class)->finalize(object);
}

static void
gsurf_adblock_module_class_init(GsurfAdblockModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_adblock_module_finalize;
	module_class->activate = gsurf_adblock_activate;
	module_class->get_name = gsurf_adblock_get_name;
	module_class->get_description = gsurf_adblock_get_description;
	module_class->configure = gsurf_adblock_configure;
}

static void
gsurf_adblock_module_init(GsurfAdblockModule *self)
{
	self->blocked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->whitelist = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_ADBLOCK_MODULE;
}
