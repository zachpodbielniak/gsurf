/*
 * gsurf-adblock-module.c - Host-based request/navigation blocking
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two-layer blocking, a feature surf itself lacks:
 *   - #GsurfNavigationHook vetoes top-level navigations whose host is in
 *     the blocklist.
 *   - #GsurfScriptInjector compiles the blocklist into a WebKit
 *     content-blocker ruleset and applies it per view, so sub-resources
 *     (scripts, images, trackers, XHR) are blocked too.
 * Blocklists load from hosts-format files and inline lists. In addition,
 * `content_filters:` loads pre-converted WebKit content-blocker JSON files
 * (e.g. EasyList/uBO lists run through abp2blocklist or `make adblock-lists`)
 * and applies each natively — this is where per-path, resource-type, and
 * element-hiding rules come from.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <string.h>

/* WebKit compiles the whole ruleset up front; cap the domain count so a
 * pathologically large hosts file can't make compilation pathological. */
#define GSURF_ADBLOCK_MAX_DOMAINS 100000

#define GSURF_TYPE_ADBLOCK_MODULE (gsurf_adblock_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfAdblockModule, gsurf_adblock_module,
                     GSURF, ADBLOCK_MODULE, GsurfModule)

/* A pre-built WebKit content-blocker ruleset loaded from a JSON file. */
typedef struct {
	gchar *id;    /* stable identifier for the WebKit filter store */
	gchar *json;  /* the raw content-blocker JSON */
} ContentFilter;

struct _GsurfAdblockModule
{
	GsurfModule parent_instance;
	GHashTable *blocked;         /* host (gchar*) -> 1 */
	GHashTable *whitelist;       /* host (gchar*) -> 1 */
	gchar      *filter_json;     /* compiled-from-hosts content-blocker ruleset */
	GPtrArray  *content_filters; /* ContentFilter* loaded from content_filters: */
};

static void
content_filter_free(gpointer data)
{
	ContentFilter *cf = data;
	g_free(cf->id);
	g_free(cf->json);
	g_free(cf);
}

static void gsurf_adblock_nav_init(GsurfNavigationHookInterface *iface);
static void gsurf_adblock_injector_init(GsurfScriptInjectorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfAdblockModule, gsurf_adblock_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_NAVIGATION_HOOK, gsurf_adblock_nav_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SCRIPT_INJECTOR, gsurf_adblock_injector_init))

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

/* Compile the blocklist into a WebKit content-blocker JSON ruleset:
 *   - one block rule whose if-domain lists every blocked host (the "*"
 *     prefix matches the host and all of its subdomains);
 *   - one ignore-previous-rules rule for whitelisted hosts (placed last so
 *     it wins). Returns %NULL when there is nothing to block. */
static gchar *
build_filter_json(GsurfAdblockModule *self)
{
	GHashTableIter it;
	gpointer key;
	GString *s;
	guint count = 0, total = g_hash_table_size(self->blocked);
	gboolean first = TRUE;

	if (total == 0)
		return NULL;

	s = g_string_new("[{\"trigger\":{\"url-filter\":\".*\",\"if-domain\":[");
	g_hash_table_iter_init(&it, self->blocked);
	while (g_hash_table_iter_next(&it, &key, NULL)) {
		if (count >= GSURF_ADBLOCK_MAX_DOMAINS)
			break;
		g_string_append_printf(s, "%s\"*%s\"", first ? "" : ",", (const gchar *)key);
		first = FALSE;
		count++;
	}
	g_string_append(s, "]},\"action\":{\"type\":\"block\"}}");

	if (g_hash_table_size(self->whitelist) > 0) {
		first = TRUE;
		g_string_append(s, ",{\"trigger\":{\"url-filter\":\".*\",\"if-domain\":[");
		g_hash_table_iter_init(&it, self->whitelist);
		while (g_hash_table_iter_next(&it, &key, NULL)) {
			g_string_append_printf(s, "%s\"*%s\"", first ? "" : ",", (const gchar *)key);
			first = FALSE;
		}
		g_string_append(s, "]},\"action\":{\"type\":\"ignore-previous-rules\"}}");
	}
	g_string_append_c(s, ']');

	if (count < total)
		g_warning("gsurf adblock: content filter capped at %u of %u domains "
			"(navigation-level blocking still covers the rest)", count, total);

	return g_string_free(s, FALSE);
}

static void
gsurf_adblock_inject(GsurfScriptInjector *injector, GsurfView *view, const gchar *uri)
{
	GsurfAdblockModule *self = GSURF_ADBLOCK_MODULE(injector);
	guint i;

	(void) uri;
	if (view == NULL)
		return;
	if (self->filter_json == NULL && self->content_filters->len == 0)
		return;
	/* Apply once per view (the inject hook also fires on URI commits). */
	if (g_object_get_data(G_OBJECT(view), "gsurf-adblock-applied") != NULL)
		return;
	g_object_set_data(G_OBJECT(view), "gsurf-adblock-applied", GINT_TO_POINTER(1));

	if (self->filter_json != NULL)
		gsurf_view_add_content_filter(view, "gsurf-adblock", self->filter_json);

	/* Apply each pre-built list as its own native WebKit content filter. */
	for (i = 0; i < self->content_filters->len; i++) {
		ContentFilter *cf = g_ptr_array_index(self->content_filters, i);
		gsurf_view_add_content_filter(view, cf->id, cf->json);
	}
}

static void
gsurf_adblock_injector_init(GsurfScriptInjectorInterface *iface)
{
	iface->inject = gsurf_adblock_inject;
}

static const gchar *gsurf_adblock_get_name(GsurfModule *m) { return "adblock"; }
static const gchar *gsurf_adblock_get_description(GsurfModule *m)
{
	return "Host-based blocking plus WebKit content filters (EasyList/uBO JSON)";
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

/* Load a pre-built WebKit content-blocker JSON file verbatim and apply it
 * natively (per-path, resource-type, element-hiding — whatever the file
 * encodes). Generate these from EasyList/uBO lists with abp2blocklist or
 * the `make adblock-lists` target. */
static void
load_content_filter(GsurfAdblockModule *self, const gchar *path)
{
	g_autofree gchar *expanded = expand_path(path);
	g_autofree gchar *base = NULL;
	g_autofree gchar *json = NULL;
	ContentFilter *cf;
	gchar *id, *p;

	if (expanded == NULL || !g_file_get_contents(expanded, &json, NULL, NULL))
		return;
	if (json == NULL || *json == '\0')
		return;

	/* Stable identifier from the file's basename (the WebKit filter store
	 * keys compiled rulesets by it, so each file needs a distinct one). */
	base = g_path_get_basename(expanded);
	if ((p = strrchr(base, '.')) != NULL)
		*p = '\0';
	id = g_strconcat("gsurf-adblock-", base, NULL);
	for (p = id; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
			*p = '-';

	cf = g_new0(ContentFilter, 1);
	cf->id = id;
	cf->json = g_steal_pointer(&json);
	g_ptr_array_add(self->content_filters, cf);
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

	/* Pre-converted WebKit content-blocker JSON files (EasyList/uBO etc.). */
	g_ptr_array_set_size(self->content_filters, 0);
	load_sequence(m, "content_filters", load_content_filter, self);

	/* Compile the WebKit content-blocker ruleset once, up front. */
	g_clear_pointer(&self->filter_json, g_free);
	self->filter_json = build_filter_json(self);

	g_message("gsurf adblock: %u blocked host(s), %u content filter(s)",
		g_hash_table_size(self->blocked), self->content_filters->len);
}

static gboolean gsurf_adblock_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_adblock_module_finalize(GObject *object)
{
	GsurfAdblockModule *self = GSURF_ADBLOCK_MODULE(object);

	g_clear_pointer(&self->blocked, g_hash_table_unref);
	g_clear_pointer(&self->whitelist, g_hash_table_unref);
	g_clear_pointer(&self->filter_json, g_free);
	g_clear_pointer(&self->content_filters, g_ptr_array_unref);

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
	self->content_filters = g_ptr_array_new_with_free_func(content_filter_free);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_ADBLOCK_MODULE;
}
