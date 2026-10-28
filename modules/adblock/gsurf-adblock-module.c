/*
 * gsurf-adblock-module.c - uBlock Origin-style content blocking
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Layers, from cheapest to most capable:
 *   - #GsurfNavigationHook vetoes top-level navigations to hosts from
 *     hosts-format lists (`host_files:` / `block_lists:`).
 *   - #GsurfScriptInjector applies WebKit content-blocker rulesets to each
 *     view: the hosts lists, pre-built JSON (`content_filters:`), and the
 *     network + element-hiding rules compiled from uBO/ABP filter lists
 *     (`filter_lists:` / `filters:`, see gsurf-adblock-filters.c).
 *   - The adblock web-process extension (webext/) handles what WebKit's
 *     content blocker cannot: `$redirect` / `$removeparam`, scriptlets and
 *     procedural cosmetic filters. The module registers it with the
 *     filter data via gsurf_web_process_extension_register().
 *   - #GsurfRequestFilter answers redirect/removeparam queries in the UI
 *     process with the same matcher the extension uses.
 */

#define _GNU_SOURCE
#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <dlfcn.h>
#include <string.h>
#include "gsurf-adblock-filters.h"
#include "gsurf-adblock-js.h"

/* WebKit compiles the whole ruleset up front; cap the domain count so a
 * pathologically large hosts file can't make compilation pathological. */
#define GSURF_ADBLOCK_MAX_DOMAINS 100000

/* Where the web-process extension is built/installed, relative to the
 * directory holding adblock.so. */
#define GSURF_ADBLOCK_WEBEXT_SUBDIR "adblock-webext"
#define GSURF_ADBLOCK_WEBEXT_NAME   "gsurf-adblock-webext.so"

G_MODULE_EXPORT GType gsurf_module_register(void);

#define GSURF_TYPE_ADBLOCK_MODULE (gsurf_adblock_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfAdblockModule, gsurf_adblock_module,
                     GSURF, ADBLOCK_MODULE, GsurfModule)

/* A WebKit content-blocker ruleset applied to every view. */
typedef struct {
	gchar *id;    /* stable identifier for the WebKit filter store */
	gchar *json;  /* the raw content-blocker JSON */
} ContentFilter;

struct _GsurfAdblockModule
{
	GsurfModule          parent_instance;
	GHashTable          *blocked;         /* host (gchar*) -> 1 */
	GHashTable          *whitelist;       /* host (gchar*) -> 1 */
	gchar               *filter_json;     /* compiled-from-hosts content-blocker ruleset */
	GPtrArray           *content_filters; /* ContentFilter*: JSON files + compiled lists */
	GsurfAdblockMatcher *matcher;         /* redirect/removeparam rules */
	gboolean             cosmetic;
	gboolean             scriptlets;
	gboolean             redirects;
	gchar               *webext_path;     /* NULL = locate next to adblock.so */
	gboolean             webext_registered;
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
static void gsurf_adblock_request_filter_init(GsurfRequestFilterInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfAdblockModule, gsurf_adblock_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_NAVIGATION_HOOK, gsurf_adblock_nav_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SCRIPT_INJECTOR, gsurf_adblock_injector_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_REQUEST_FILTER, gsurf_adblock_request_filter_init))

/* Use one DNS spelling for files, URI hosts and whitelist entries. Reject
 * non-host syntax before it can corrupt the generated JSON domain list. */
static gchar *
normalize_host(const gchar *host)
{
	gchar *normalized;
	gchar *p;
	gsize length;

	if (host == NULL || *host == '\0')
		return NULL;
	normalized = g_hostname_to_ascii(host);
	if (normalized == NULL)
		return NULL;
	length = strlen(normalized);
	if (length > 0 && normalized[length - 1] == '.')
		normalized[--length] = '\0';
	for (p = normalized; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '.' && *p != '_') {
			g_free(normalized);
			return NULL;
		}
		*p = g_ascii_tolower(*p);
	}
	if (length == 0) {
		g_free(normalized);
		return NULL;
	}
	return normalized;
}

/* Match full domain labels, including subdomains, for both lists. */
static gboolean
host_matches(GHashTable *hosts, const gchar *host)
{
	const gchar *p;

	if (g_hash_table_contains(hosts, host))
		return TRUE;
	for (p = host; (p = strchr(p, '.')) != NULL; ) {
		p++;
		if (g_hash_table_contains(hosts, p))
			return TRUE;
	}
	return FALSE;
}

static gboolean
host_is_blocked(GsurfAdblockModule *self, const gchar *host)
{
	g_autofree gchar *normalized = normalize_host(host);

	return normalized != NULL &&
		!host_matches(self->whitelist, normalized) &&
		host_matches(self->blocked, normalized);
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

	/* Apply each ruleset as its own native WebKit content filter. The
	 * backend compiles each one once per process and shares it. */
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

/* The same redirect/removeparam decision the web-process extension makes,
 * for hosts that ask the module directly. */
static GsurfFilterVerdict
gsurf_adblock_filter_request(GsurfRequestFilter *filter, GsurfView *view, const gchar *uri,
                             gchar **redirect_uri)
{
	GsurfAdblockModule *self = GSURF_ADBLOCK_MODULE(filter);
	const gchar *page = view != NULL ? gsurf_view_get_uri(view) : NULL;
	gchar *target;
	gboolean blocked = FALSE;

	if (redirect_uri != NULL)
		*redirect_uri = NULL;
	if (self->matcher == NULL || uri == NULL)
		return GSURF_FILTER_ALLOW;
	target = gsurf_adblock_matcher_process(self->matcher, uri, page,
		gsurf_adblock_guess_type(uri), &blocked);
	if (blocked)
		return GSURF_FILTER_BLOCK;
	if (target == NULL)
		return GSURF_FILTER_ALLOW;
	if (redirect_uri != NULL)
		*redirect_uri = target;
	else
		g_free(target);
	return GSURF_FILTER_REDIRECT;
}

static void
gsurf_adblock_request_filter_init(GsurfRequestFilterInterface *iface)
{
	iface->filter_request = gsurf_adblock_filter_request;
}

static const gchar *gsurf_adblock_get_name(GsurfModule *m) { return "adblock"; }
static const gchar *gsurf_adblock_get_description(GsurfModule *m)
{
	return "uBlock Origin-style blocking: filter lists, cosmetics, scriptlets, redirects";
}

static gchar *
expand_path(const gchar *path)
{
	if (path != NULL && path[0] == '~' && path[1] == '/')
		return g_build_filename(g_get_home_dir(), path + 2, NULL);
	return g_strdup(path);
}

/* Parse bare-domain lists and hosts files. Strip inline comments before
 * splitting, skip an optional IP address, and retain every hostname alias. */
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
		gchar *comment;
		guint j;
		gboolean first = TRUE;

		comment = strchr(line, '#');
		if (comment != NULL)
			*comment = '\0';
		if (*line == '\0' || *line == '#')
			continue;
		toks = g_strsplit_set(line, " \t\r", -1);
		for (j = 0; toks[j] != NULL; j++) {
			g_autofree gchar *host = NULL;
			g_autoptr(GInetAddress) address = NULL;

			if (*toks[j] == '\0')
				continue;
			if (first) {
				first = FALSE;
				address = g_inet_address_new_from_string(toks[j]);
				if (address != NULL)
					continue;
			}
			host = normalize_host(toks[j]);
			if (host != NULL && !g_str_equal(host, "localhost"))
				g_hash_table_add(self->blocked, g_steal_pointer(&host));
		}
		g_strfreev(toks);
	}
	g_strfreev(lines);
}

/* Keep identifiers filename-safe: the WebKit filter store keys compiled
 * rulesets by them. */
static gchar *
filter_id(const gchar *suffix)
{
	gchar *id = g_strconcat("gsurf-adblock-", suffix, NULL);
	gchar *p;

	for (p = id; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
			*p = '-';
	return id;
}

static void
add_content_filter(GsurfAdblockModule *self, gchar *id, gchar *json)
{
	ContentFilter *cf = g_new0(ContentFilter, 1);

	cf->id = id;
	cf->json = json;
	g_ptr_array_add(self->content_filters, cf);
}

/* Load a pre-built WebKit content-blocker JSON file verbatim and apply it
 * natively. Kept for rulesets converted elsewhere; uBO/ABP lists can go in
 * `filter_lists:` directly. */
static void
load_content_filter(GsurfAdblockModule *self, const gchar *path)
{
	g_autofree gchar *expanded = expand_path(path);
	g_autofree gchar *base = NULL;
	g_autofree gchar *json = NULL;
	gchar *p;

	if (expanded == NULL || !g_file_get_contents(expanded, &json, NULL, NULL))
		return;
	if (json == NULL || *json == '\0')
		return;

	/* Stable identifier from the file's basename. */
	base = g_path_get_basename(expanded);
	if ((p = strrchr(base, '.')) != NULL)
		*p = '\0';
	add_content_filter(self, filter_id(base), g_steal_pointer(&json));
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
	g_autofree gchar *normalized = normalize_host(host);

	if (normalized != NULL)
		g_hash_table_add(self->whitelist, g_steal_pointer(&normalized));
}

static gboolean
mapping_bool(YamlMapping *m, const gchar *key, gboolean fallback)
{
	if (!yaml_mapping_has_member(m, key))
		return fallback;
	return yaml_mapping_get_boolean_member(m, key);
}

/* --- web-process extension --- */

/* The extension ships next to adblock.so ("adblock-webext/"), wherever
 * the module was loaded from: the build tree, an install, an AppImage. */
static gchar *
find_webext(GsurfAdblockModule *self)
{
	g_autofree gchar *dir = NULL;
	gchar *path;
	Dl_info info;

	if (self->webext_path != NULL)
		return g_file_test(self->webext_path, G_FILE_TEST_IS_REGULAR)
			? g_strdup(self->webext_path) : NULL;
	if (dladdr((void *)gsurf_module_register, &info) == 0 || info.dli_fname == NULL)
		return NULL;
	dir = g_path_get_dirname(info.dli_fname);
	path = g_build_filename(dir, GSURF_ADBLOCK_WEBEXT_SUBDIR, GSURF_ADBLOCK_WEBEXT_NAME, NULL);
	if (!g_path_is_absolute(path)) {
		g_autofree gchar *cwd = g_get_current_dir();
		gchar *absolute = g_build_filename(cwd, path, NULL);

		g_free(path);
		path = absolute;
	}
	if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
		g_free(path);
		return NULL;
	}
	return path;
}

static void
unregister_webext(GsurfAdblockModule *self)
{
	if (self->webext_registered)
		gsurf_web_process_extension_unregister("adblock");
	self->webext_registered = FALSE;
}

/* --- filter lists --- */

/* `filter_lists:` entries are a path, or { path: ..., trusted: true }.
 * Only trusted lists may use trusted-* scriptlets. */
static void
load_filter_lists(GsurfAdblockCompiler *compiler, YamlMapping *m)
{
	YamlSequence *seq = yaml_mapping_get_sequence_member(m, "filter_lists");
	guint i, n;

	if (seq == NULL)
		return;
	n = yaml_sequence_get_length(seq);
	for (i = 0; i < n; i++) {
		YamlNode *e = yaml_sequence_get_element(seq, i);
		g_autofree gchar *path = NULL;
		g_autoptr(GError) error = NULL;
		gboolean trusted = FALSE;

		if (e == NULL)
			continue;
		if (yaml_node_get_node_type(e) == YAML_NODE_SCALAR) {
			path = expand_path(yaml_node_get_string(e));
		} else if (yaml_node_get_node_type(e) == YAML_NODE_MAPPING) {
			YamlMapping *entry = yaml_node_get_mapping(e);

			path = expand_path(yaml_mapping_get_string_member(entry, "path"));
			trusted = mapping_bool(entry, "trusted", FALSE);
		}
		if (path == NULL || *path == '\0')
			continue;
		if (!gsurf_adblock_compiler_add_file(compiler, path, trusted, &error))
			g_message("gsurf adblock: filter list %s: %s", path, error->message);
	}
}

/* Compile `filter_lists:` and `filters:` into content-blocker rulesets
 * plus the request matcher and the web-process page data. */
static void
compile_filter_lists(GsurfAdblockModule *self, YamlMapping *m)
{
	g_autoptr(GsurfAdblockCompiler) compiler = NULL;
	g_autoptr(GsurfAdblockResult) result = NULL;
	g_autofree gchar *webext = NULL;
	GsurfAdblockCompileFlags flags = GSURF_ADBLOCK_COMPILE_NONE;
	GHashTableIter it;
	gpointer host;
	const gchar *inline_filters;
	guint i;

	if (!yaml_mapping_has_member(m, "filter_lists") && !yaml_mapping_has_member(m, "filters"))
		return;

	compiler = gsurf_adblock_compiler_new(gsurf_adblock_scriptlets_js, gsurf_adblock_cosmetic_js);
	load_filter_lists(compiler, m);
	/* Hand-written filters are the user's own, so they are trusted. */
	inline_filters = yaml_mapping_get_string_member(m, "filters");
	if (inline_filters != NULL)
		gsurf_adblock_compiler_add_text(compiler, inline_filters, NULL, TRUE);
	g_hash_table_iter_init(&it, self->whitelist);
	while (g_hash_table_iter_next(&it, &host, NULL))
		gsurf_adblock_compiler_add_allowed_site(compiler, host);

	webext = find_webext(self);
	if (self->cosmetic)
		flags |= GSURF_ADBLOCK_COMPILE_COSMETIC;
	if (self->scriptlets)
		flags |= GSURF_ADBLOCK_COMPILE_SCRIPTLETS;
	if (self->redirects)
		flags |= GSURF_ADBLOCK_COMPILE_REDIRECTS;
	if (webext != NULL)
		flags |= GSURF_ADBLOCK_COMPILE_WEB_PROCESS;
	else if (self->scriptlets || self->redirects)
		g_message("gsurf adblock: web-process extension not found next to adblock.so; "
			"scriptlets, procedural cosmetics and $removeparam are disabled");
	result = gsurf_adblock_compiler_compile(compiler, flags);

	for (i = 0; i < result->rulesets->len; i++) {
		GsurfAdblockRuleset *rs = g_ptr_array_index(result->rulesets, i);

		add_content_filter(self, filter_id(rs->id), g_steal_pointer(&rs->json));
	}
	gsurf_adblock_matcher_free(self->matcher);
	self->matcher = g_steal_pointer(&result->matcher);

	/* Hand the web process everything it needs, once per web process. */
	if (webext != NULL &&
	    (gsurf_adblock_matcher_get_size(self->matcher) > 0 || result->page_data != NULL)) {
		g_autoptr(GVariant) serialized = gsurf_adblock_matcher_serialize(self->matcher);
		GVariantDict dict;

		g_variant_dict_init(&dict, NULL);
		g_variant_dict_insert_value(&dict, "matcher", serialized);
		if (result->page_data != NULL)
			g_variant_dict_insert_value(&dict, "page", result->page_data);
		gsurf_web_process_extension_register("adblock", webext, g_variant_dict_end(&dict));
		self->webext_registered = TRUE;
	}

	g_message("gsurf adblock: %u network rule(s) + %u exception(s), %u generic + %u specific "
		"cosmetic, %u procedural, %u scriptlet(s), %u redirect(s), %u removeparam; "
		"%u unsupported, %u invalid",
		result->stats.network, result->stats.network_exceptions,
		result->stats.cosmetic_generic, result->stats.cosmetic_specific,
		result->stats.procedural, result->stats.scriptlets, result->stats.redirects,
		result->stats.removeparams, result->stats.unsupported, result->stats.invalid);
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

	/* Start from a clean slate so a reconfigure does not accumulate. */
	g_hash_table_remove_all(self->blocked);
	g_hash_table_remove_all(self->whitelist);
	g_ptr_array_set_size(self->content_filters, 0);
	g_clear_pointer(&self->matcher, gsurf_adblock_matcher_free);
	unregister_webext(self);

	self->cosmetic = mapping_bool(m, "cosmetic", TRUE);
	self->scriptlets = mapping_bool(m, "scriptlets", TRUE);
	self->redirects = mapping_bool(m, "redirects", TRUE);
	g_clear_pointer(&self->webext_path, g_free);
	if (yaml_mapping_has_member(m, "web_process_extension"))
		self->webext_path = expand_path(yaml_mapping_get_string_member(m, "web_process_extension"));

	load_sequence(m, "host_files", load_hosts_file, self);
	load_sequence(m, "block_lists", load_hosts_file, self);
	load_sequence(m, "whitelist", add_whitelist, self);

	/* Pre-converted WebKit content-blocker JSON files. */
	load_sequence(m, "content_filters", load_content_filter, self);

	/* uBO/ABP filter lists, compiled natively. */
	compile_filter_lists(self, m);

	/* Compile the hosts ruleset once, up front. */
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

	unregister_webext(self);
	g_clear_pointer(&self->blocked, g_hash_table_unref);
	g_clear_pointer(&self->whitelist, g_hash_table_unref);
	g_clear_pointer(&self->filter_json, g_free);
	g_clear_pointer(&self->content_filters, g_ptr_array_unref);
	g_clear_pointer(&self->matcher, gsurf_adblock_matcher_free);
	g_clear_pointer(&self->webext_path, g_free);

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
	self->cosmetic = TRUE;
	self->scriptlets = TRUE;
	self->redirects = TRUE;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_ADBLOCK_MODULE;
}
