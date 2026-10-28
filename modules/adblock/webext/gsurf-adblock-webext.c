/*
 * gsurf-adblock-webext.c - adblock web-process extension
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loaded by WebKit into every web process (the adblock module registers it
 * through gsurf_web_process_extension_register()). It does the filtering
 * WebKit's content blocker cannot:
 *   - "send-request": `$redirect` serves a neutered data: stand-in,
 *     `$removeparam` strips tracking parameters (GsurfAdblockMatcher);
 *   - "window-object-cleared": before a frame's own scripts run, inject
 *     the scriptlets that apply to the frame's host into the page's main
 *     world, and start the procedural cosmetic engine in an isolated world.
 * All filter data arrives once, as the initialization #GVariant built by
 * gsurf_adblock_compiler_compile() in the UI process.
 */

#ifdef GSURF_BACKEND_GTK4
#include <webkit/webkit-web-process-extension.h>
typedef WebKitWebProcessExtension GsurfWebExtension;
#else
#include <webkit2/webkit-web-extension.h>
typedef WebKitWebExtension GsurfWebExtension;
#endif
#include <string.h>
#include "../gsurf-adblock-match.h"

/* The isolated world the cosmetic engine runs in. */
#define COSMETIC_WORLD "gsurf-adblock"

/* A filter as the web process sees it: page domains and a payload (the
 * scriptlet call, or the cosmetic selector). */
typedef struct {
	gchar **pos;      /* domains; empty = every site */
	gchar **neg;
	gchar  *fn;       /* scriptlet function (NULL for cosmetics) */
	gchar  *payload;
} PageRule;

/* Rules indexed by plain domain; entity and generic rules are scanned. */
typedef struct {
	GPtrArray  *rules;     /* PageRule*, owns them */
	GHashTable *by_domain; /* domain -> GPtrArray of PageRule* */
	GPtrArray  *scanned;   /* PageRule* without a plain positive domain */
} RuleIndex;

static GsurfAdblockMatcher *matcher;
static RuleIndex            scriptlets;
static RuleIndex            cosmetics;
static GHashTable          *library;          /* fn -> source */
static gchar               *helpers;
static gchar               *engine;
static gchar              **allowed_sites;
static gchar              **cosmetic_off_sites;
static WebKitScriptWorld   *cosmetic_world;

static void
page_rule_free(gpointer data)
{
	PageRule *r = data;

	g_strfreev(r->pos);
	g_strfreev(r->neg);
	g_free(r->fn);
	g_free(r->payload);
	g_free(r);
}

static void
rule_index_init(RuleIndex *index)
{
	index->rules = g_ptr_array_new_with_free_func(page_rule_free);
	index->by_domain = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	index->scanned = g_ptr_array_new();
}

/* File a rule under each of its plain positive domains; rules with an
 * entity ("example.*") or no positive domain are checked for every host. */
static void
rule_index_add(RuleIndex *index, PageRule *rule)
{
	gboolean scan = rule->pos[0] == NULL;
	guint i;

	g_ptr_array_add(index->rules, rule);
	for (i = 0; rule->pos[i] != NULL; i++)
		if (g_str_has_suffix(rule->pos[i], ".*"))
			scan = TRUE;
	if (scan) {
		g_ptr_array_add(index->scanned, rule);
		return;
	}
	for (i = 0; rule->pos[i] != NULL; i++) {
		GPtrArray *bucket = g_hash_table_lookup(index->by_domain, rule->pos[i]);

		if (bucket == NULL) {
			bucket = g_ptr_array_new();
			g_hash_table_insert(index->by_domain, g_strdup(rule->pos[i]), bucket);
		}
		g_ptr_array_add(bucket, rule);
	}
}

static gboolean
host_in(const gchar *host, gchar **domains)
{
	guint i;

	for (i = 0; domains != NULL && domains[i] != NULL; i++)
		if (gsurf_adblock_host_matches_domain(host, domains[i]))
			return TRUE;
	return FALSE;
}

static gboolean
rule_applies(PageRule *rule, const gchar *host)
{
	if (rule->pos[0] != NULL && !host_in(host, rule->pos))
		return FALSE;
	return !host_in(host, rule->neg);
}

/* Every rule of @index that applies to @host, in list order, deduplicated
 * by payload. */
static GPtrArray *
rule_index_lookup(RuleIndex *index, const gchar *host)
{
	GPtrArray *out = g_ptr_array_new();
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	const gchar *p;
	guint i;

	for (p = host; p != NULL; p = strchr(p, '.') != NULL ? strchr(p, '.') + 1 : NULL) {
		GPtrArray *bucket = g_hash_table_lookup(index->by_domain, p);

		for (i = 0; bucket != NULL && i < bucket->len; i++) {
			PageRule *rule = g_ptr_array_index(bucket, i);

			if (rule_applies(rule, host) && g_hash_table_add(seen, rule->payload))
				g_ptr_array_add(out, rule);
		}
	}
	for (i = 0; i < index->scanned->len; i++) {
		PageRule *rule = g_ptr_array_index(index->scanned, i);

		if (rule_applies(rule, host) && g_hash_table_add(seen, rule->payload))
			g_ptr_array_add(out, rule);
	}
	return out;
}

/* --- frame injection --- */

static gchar *
frame_host(WebKitFrame *frame)
{
	const gchar *uri = webkit_frame_get_uri(frame);
	g_autoptr(GUri) parsed = NULL;
	const gchar *host;

	if (uri == NULL || (!g_str_has_prefix(uri, "http:") && !g_str_has_prefix(uri, "https:")))
		return NULL;
	parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
	if (parsed == NULL || (host = g_uri_get_host(parsed)) == NULL || *host == '\0')
		return NULL;
	return g_ascii_strdown(host, -1);
}

static void
evaluate(WebKitFrame *frame, WebKitScriptWorld *world, const gchar *source)
{
	g_autoptr(JSCContext) context = webkit_frame_get_js_context_for_script_world(frame, world);
	g_autoptr(JSCValue) result = NULL;
	JSCException *exception;

	result = jsc_context_evaluate(context, source, -1);
	exception = jsc_context_get_exception(context);
	if (exception != NULL) {
		g_debug("gsurf adblock: %s", jsc_exception_get_message(exception));
		jsc_context_clear_exception(context);
	}
}

/* Scriptlets go into the page's own world (they must patch its globals),
 * wrapped in one closure so their helpers stay private. */
static void
inject_scriptlets(WebKitFrame *frame, WebKitScriptWorld *world, const gchar *host)
{
	g_autoptr(GPtrArray) rules = rule_index_lookup(&scriptlets, host);
	g_autoptr(GHashTable) fns = NULL;
	g_autoptr(GString) script = NULL;
	guint i;

	if (rules->len == 0)
		return;
	fns = g_hash_table_new(g_str_hash, g_str_equal);
	script = g_string_new("(function () {\n'use strict';\n");
	g_string_append(script, helpers);
	for (i = 0; i < rules->len; i++) {
		PageRule *rule = g_ptr_array_index(rules, i);
		const gchar *source = g_hash_table_lookup(library, rule->fn);

		if (source != NULL && g_hash_table_add(fns, rule->fn))
			g_string_append(script, source);
	}
	for (i = 0; i < rules->len; i++) {
		PageRule *rule = g_ptr_array_index(rules, i);

		g_string_append_printf(script, "try { %s; } catch (e) { }\n", rule->payload);
	}
	g_string_append(script, "})();\n");
	evaluate(frame, world, script->str);
}

/* Append @s as a JSON/JS string literal. */
static void
append_js_string(GString *out, const gchar *s)
{
	const guchar *p;

	g_string_append_c(out, '"');
	for (p = (const guchar *)s; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_printf(out, "\\%c", *p);
		else if (*p < 0x20)
			g_string_append_printf(out, "\\u%04x", *p);
		else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0xa8 || p[2] == 0xa9)) {
			g_string_append_printf(out, "\\u%04x", p[2] == 0xa8 ? 0x2028 : 0x2029);
			p += 2;
		} else
			g_string_append_c(out, (gchar)*p);
	}
	g_string_append_c(out, '"');
}

static void
inject_cosmetics(WebKitFrame *frame, const gchar *host)
{
	g_autoptr(GPtrArray) rules = NULL;
	g_autoptr(GString) script = NULL;
	guint i;

	if (engine == NULL || *engine == '\0' || host_in(host, cosmetic_off_sites))
		return;
	rules = rule_index_lookup(&cosmetics, host);
	if (rules->len == 0)
		return;
	script = g_string_new(engine);
	g_string_append(script, "\ngsurfAdblockCosmetic([");
	for (i = 0; i < rules->len; i++) {
		if (i > 0)
			g_string_append_c(script, ',');
		append_js_string(script, ((PageRule *)g_ptr_array_index(rules, i))->payload);
	}
	g_string_append(script, "]);\n");
	evaluate(frame, cosmetic_world, script->str);
}

/* A frame's window object was (re)created for a new document: this runs
 * before any of the document's scripts. */
static void
on_window_object_cleared(WebKitScriptWorld *world, WebKitWebPage *page, WebKitFrame *frame,
                         gpointer user_data)
{
	g_autofree gchar *host = frame_host(frame);

	if (host == NULL || host_in(host, allowed_sites))
		return;
	inject_scriptlets(frame, world, host);
	inject_cosmetics(frame, host);
}

/* --- requests --- */

static gboolean
on_send_request(WebKitWebPage *page, WebKitURIRequest *request,
                WebKitURIResponse *redirected_response, gpointer user_data)
{
	const gchar *uri = webkit_uri_request_get_uri(request);
	const gchar *page_uri = webkit_web_page_get_uri(page);
	g_autofree gchar *target = NULL;
	gboolean blocked = FALSE;
	guint type;

	/* The page's own document is never swapped for a stand-in. */
	type = g_strcmp0(uri, page_uri) == 0 ? GSURF_ADBLOCK_TYPE_DOCUMENT
	                                     : gsurf_adblock_guess_type(uri);
	target = gsurf_adblock_matcher_process(matcher, uri, page_uri, type, &blocked);
	if (blocked)
		return TRUE;   /* cancel */
	if (target != NULL) {
		if (type == GSURF_ADBLOCK_TYPE_DOCUMENT && g_str_has_prefix(target, "data:"))
			return FALSE;
		webkit_uri_request_set_uri(request, target);
	}
	return FALSE;
}

static void
on_page_created(GsurfWebExtension *extension, WebKitWebPage *page, gpointer user_data)
{
	if (matcher != NULL && gsurf_adblock_matcher_get_size(matcher) > 0)
		g_signal_connect(page, "send-request", G_CALLBACK(on_send_request), NULL);
}

/* --- initialization --- */

/* Load an a(asasss) scriptlet table or an a(asass) cosmetic table. */
static void
load_rules(RuleIndex *index, GVariant *table, gboolean with_fn)
{
	GVariantIter it;
	GVariant *child;

	rule_index_init(index);
	if (table == NULL)
		return;
	g_variant_iter_init(&it, table);
	while ((child = g_variant_iter_next_value(&it)) != NULL) {
		PageRule *rule = g_new0(PageRule, 1);

		if (with_fn)
			g_variant_get(child, "(^as^asss)", &rule->pos, &rule->neg, &rule->fn, &rule->payload);
		else
			g_variant_get(child, "(^as^ass)", &rule->pos, &rule->neg, &rule->payload);
		rule_index_add(index, rule);
		g_variant_unref(child);
	}
}

static void
load_page_data(GVariant *page)
{
	g_autoptr(GVariant) scriptlet_table = NULL;
	g_autoptr(GVariant) cosmetic_table = NULL;
	g_autoptr(GVariant) lib = NULL;
	GVariantDict dict;
	GVariantIter it;
	const gchar *fn, *source;

	library = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_variant_dict_init(&dict, page);
	scriptlet_table = g_variant_dict_lookup_value(&dict, "scriptlets", G_VARIANT_TYPE("a(asasss)"));
	cosmetic_table = g_variant_dict_lookup_value(&dict, "cosmetic", G_VARIANT_TYPE("a(asass)"));
	lib = g_variant_dict_lookup_value(&dict, "scriptlet-library", G_VARIANT_TYPE("a{ss}"));
	if (!g_variant_dict_lookup(&dict, "scriptlet-helpers", "s", &helpers))
		helpers = g_strdup("");
	if (!g_variant_dict_lookup(&dict, "cosmetic-engine", "s", &engine))
		engine = NULL;
	if (!g_variant_dict_lookup(&dict, "allowed-sites", "^as", &allowed_sites))
		allowed_sites = NULL;
	if (!g_variant_dict_lookup(&dict, "cosmetic-off-sites", "^as", &cosmetic_off_sites))
		cosmetic_off_sites = NULL;
	g_variant_dict_clear(&dict);

	if (lib != NULL) {
		g_variant_iter_init(&it, lib);
		while (g_variant_iter_next(&it, "{&s&s}", &fn, &source))
			g_hash_table_insert(library, g_strdup(fn), g_strdup(source));
	}
	load_rules(&scriptlets, scriptlet_table, TRUE);
	load_rules(&cosmetics, cosmetic_table, FALSE);
}

static void
initialize(GsurfWebExtension *extension, GVariant *user_data)
{
	g_autoptr(GVariant) mine = NULL;
	g_autoptr(GVariant) matcher_data = NULL;
	g_autoptr(GVariant) page = NULL;
	GVariantDict dict;

	if (user_data == NULL || !g_variant_is_of_type(user_data, G_VARIANT_TYPE_VARDICT))
		return;
	g_variant_dict_init(&dict, user_data);
	mine = g_variant_dict_lookup_value(&dict, "adblock", G_VARIANT_TYPE_VARDICT);
	g_variant_dict_clear(&dict);
	if (mine == NULL)
		return;

	g_variant_dict_init(&dict, mine);
	matcher_data = g_variant_dict_lookup_value(&dict, "matcher", NULL);
	page = g_variant_dict_lookup_value(&dict, "page", G_VARIANT_TYPE_VARDICT);
	g_variant_dict_clear(&dict);

	matcher = gsurf_adblock_matcher_new_from_variant(matcher_data);
	g_signal_connect(extension, "page-created", G_CALLBACK(on_page_created), NULL);

	if (page != NULL) {
		load_page_data(page);
		cosmetic_world = webkit_script_world_new_with_name(COSMETIC_WORLD);
		if (scriptlets.rules->len > 0 || cosmetics.rules->len > 0)
			g_signal_connect(webkit_script_world_get_default(), "window-object-cleared",
				G_CALLBACK(on_window_object_cleared), NULL);
	}
}

#ifdef GSURF_BACKEND_GTK4
G_MODULE_EXPORT void
webkit_web_process_extension_initialize_with_user_data(WebKitWebProcessExtension *extension,
                                                       const GVariant *user_data)
{
	initialize(extension, (GVariant *)user_data);
}
#else
G_MODULE_EXPORT void
webkit_web_extension_initialize_with_user_data(WebKitWebExtension *extension,
                                               const GVariant *user_data)
{
	initialize(extension, (GVariant *)user_data);
}
#endif
