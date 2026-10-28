/*
 * test-adblock.c - adblock module: filter-list compiler, matcher, registry
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Headless checks of the uBO/ABP filter support in the adblock module,
 * driven through its public hooks: the content-blocker rulesets it hands
 * a view (captured by a model-only view), the redirect/removeparam
 * decisions of its #GsurfRequestFilter, and the data it registers for its
 * web-process extension. Enforcement inside WebKit is covered by
 * test-adblock-webkit (make test-gui).
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* --- a view that records content filters instead of applying them --- */

typedef struct {
	GsurfView   parent;
	GHashTable *filters;   /* identifier -> JSON */
	gchar      *uri;
} TestFilterView;
typedef struct { GsurfViewClass parent; } TestFilterViewClass;
G_DEFINE_TYPE(TestFilterView, test_filter_view, GSURF_TYPE_VIEW)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(TestFilterView, g_object_unref)

static void
test_filter_view_add_content_filter(GsurfView *view, const gchar *identifier,
                                    const gchar *json_rules)
{
	TestFilterView *self = (TestFilterView *)view;

	g_hash_table_replace(self->filters, g_strdup(identifier), g_strdup(json_rules));
}

static const gchar *
test_filter_view_get_uri(GsurfView *view)
{
	return ((TestFilterView *)view)->uri;
}

static void
test_filter_view_finalize(GObject *object)
{
	TestFilterView *self = (TestFilterView *)object;

	g_hash_table_unref(self->filters);
	g_free(self->uri);
	G_OBJECT_CLASS(test_filter_view_parent_class)->finalize(object);
}

static void
test_filter_view_class_init(TestFilterViewClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = test_filter_view_finalize;
	GSURF_VIEW_CLASS(klass)->add_content_filter = test_filter_view_add_content_filter;
	GSURF_VIEW_CLASS(klass)->get_uri = test_filter_view_get_uri;
}

static void
test_filter_view_init(TestFilterView *self)
{
	self->filters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static TestFilterView *
test_filter_view_new(const gchar *uri)
{
	TestFilterView *view = g_object_new(test_filter_view_get_type(), NULL);

	view->uri = g_strdup(uri);
	return view;
}

/* --- helpers --- */

static gchar *
module_so_path(void)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);

	return g_build_filename(dir, "modules", "adblock.so", NULL);
}

/* Indent @filters as a YAML block scalar under "filters: |". */
static gchar *
yaml_with_filters(const gchar *filters, const gchar *extra)
{
	g_auto(GStrv) lines = g_strsplit(filters, "\n", -1);
	g_autofree gchar *joined = g_strjoinv("\n      ", lines);

	return g_strdup_printf("modules:\n  adblock:\n    enabled: true\n%s"
		"    filters: |\n      %s\n", extra != NULL ? extra : "", joined);
}

static GsurfModuleManager *
load_adblock(const gchar *yaml)
{
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *so = module_so_path();
	GsurfModuleManager *manager;

	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	manager = gsurf_module_manager_new();
	gsurf_module_manager_set_config(manager, config);
	g_assert_nonnull(gsurf_module_manager_load_module(manager, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(manager);
	return manager;
}

static gboolean
have_module(void)
{
	g_autofree gchar *so = module_so_path();

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("adblock.so not built");
		return FALSE;
	}
	return TRUE;
}

/* Parse a captured ruleset and return its rules array (owned by @parser). */
static JsonArray *
ruleset(TestFilterView *view, JsonParser *parser, const gchar *id)
{
	const gchar *json = g_hash_table_lookup(view->filters, id);
	g_autoptr(GError) error = NULL;

	g_assert_nonnull(json);
	g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
	g_assert_no_error(error);
	return json_node_get_array(json_parser_get_root(parser));
}

/* Index of the first rule whose url-filter equals @filter and whose action
 * type is @action, or -1. */
static gint
find_rule(JsonArray *rules, const gchar *filter, const gchar *action)
{
	guint i;

	for (i = 0; i < json_array_get_length(rules); i++) {
		JsonObject *rule = json_array_get_object_element(rules, i);
		JsonObject *trigger = json_object_get_object_member(rule, "trigger");
		JsonObject *act = json_object_get_object_member(rule, "action");

		if (g_strcmp0(json_object_get_string_member(trigger, "url-filter"), filter) == 0 &&
		    g_strcmp0(json_object_get_string_member(act, "type"), action) == 0)
			return (gint)i;
	}
	return -1;
}

static gint
find_selector(JsonArray *rules, const gchar *selector)
{
	guint i;

	for (i = 0; i < json_array_get_length(rules); i++) {
		JsonObject *act = json_object_get_object_member(
			json_array_get_object_element(rules, i), "action");

		if (g_strcmp0(json_object_get_string_member_with_default(act, "selector", NULL), selector) == 0)
			return (gint)i;
	}
	return -1;
}

static gboolean
array_has_string(JsonArray *array, const gchar *value)
{
	guint i;

	for (i = 0; array != NULL && i < json_array_get_length(array); i++)
		if (g_strcmp0(json_array_get_string_element(array, i), value) == 0)
			return TRUE;
	return FALSE;
}

static GsurfFilterVerdict
filter_request(GsurfModuleManager *manager, const gchar *page, const gchar *uri, gchar **out)
{
	g_autoptr(TestFilterView) view = test_filter_view_new(page);

	return gsurf_module_manager_dispatch_filter_request(manager, GSURF_VIEW(view), uri, out);
}

/* --- tests --- */

/* Pattern syntax, option mapping and uBO precedence in the "net" ruleset:
 * blocks, then exceptions, then $important blocks, then whole-site
 * allowances, with $badfilter and unsupported options dropping filters. */
static void
test_network_rules(void)
{
	g_autoptr(GsurfModuleManager) manager = NULL;
	g_autoptr(TestFilterView) view = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *yaml = NULL;
	JsonArray *rules;
	JsonObject *trigger;
	gint block, exception, important, site;

	if (!have_module())
		return;
	yaml = yaml_with_filters(
		"! a comment\n"
		"[Adblock Plus 2.0]\n"
		"||ads.example.com^\n"
		"/banner/*/img^$image,third-party,domain=news.example|~sports.news.example\n"
		"@@||ads.example.com/ok.js$script\n"
		"||tracker.example^$important\n"
		"||gone.example^\n"
		"||gone.example^$badfilter\n"
		"||csp.example^$csp=script-src 'none'\n"
		"0.0.0.0 hostsfile.example\n"
		"plainhost.example\n"
		"@@||allowed.example^$document\n"
		"!#if env_firefox\n"
		"||firefox-only.example^\n"
		"!#else\n"
		"||not-firefox.example^\n"
		"!#endif\n", "    whitelist: [\"mine.example\"]\n");
	manager = load_adblock(yaml);
	view = test_filter_view_new("https://news.example/");
	gsurf_module_manager_dispatch_inject_scripts(manager, GSURF_VIEW(view), NULL);
	rules = ruleset(view, parser, "gsurf-adblock-net");

	/* "||host^": scheme + optional subdomains, then the end of the host. */
	block = find_rule(rules, "^[^:]+://([^/]+\\.)?ads\\.example\\.com[:/]", "block");
	g_assert_cmpint(block, >=, 0);
	trigger = json_object_get_object_member(json_array_get_object_element(rules, block), "trigger");
	g_assert_false(json_object_has_member(trigger, "resource-type"));

	/* Types, party and page domains map onto WebKit trigger fields; the
	 * excluded subdomain becomes a following ignore rule. */
	{
		const gchar *f = "/banner/.*/img([^-.%a-zA-Z0-9_].*)?$";
		gint typed = find_rule(rules, f, "block");
		JsonObject *t;

		g_assert_cmpint(typed, >=, 0);
		t = json_object_get_object_member(json_array_get_object_element(rules, typed), "trigger");
		g_assert_true(array_has_string(json_object_get_array_member(t, "resource-type"), "image"));
		g_assert_false(array_has_string(json_object_get_array_member(t, "resource-type"), "script"));
		g_assert_true(array_has_string(json_object_get_array_member(t, "load-type"), "third-party"));
		g_assert_true(array_has_string(json_object_get_array_member(t, "if-domain"), "*news.example"));
		t = json_object_get_object_member(json_array_get_object_element(rules, typed + 1), "trigger");
		g_assert_true(array_has_string(json_object_get_array_member(t, "if-domain"), "*sports.news.example"));
	}

	/* Precedence: exceptions after blocks, $important after exceptions,
	 * site allowances (allowlist + $document) last. */
	exception = find_rule(rules, "^[^:]+://([^/]+\\.)?ads\\.example\\.com/ok\\.js", "ignore-previous-rules");
	important = find_rule(rules, "^[^:]+://([^/]+\\.)?tracker\\.example[:/]", "block");
	site = (gint)json_array_get_length(rules) - 1;
	g_assert_cmpint(exception, >, block);
	g_assert_cmpint(important, >, exception);
	trigger = json_object_get_object_member(json_array_get_object_element(rules, site), "trigger");
	g_assert_true(array_has_string(json_object_get_array_member(trigger, "if-domain"), "*allowed.example"));
	g_assert_true(array_has_string(json_object_get_array_member(trigger, "if-domain"), "*mine.example"));

	/* Hosts-file lines and bare hostnames are "||host^" filters. */
	g_assert_cmpint(find_rule(rules, "^[^:]+://([^/]+\\.)?hostsfile\\.example[:/]", "block"), >=, 0);
	g_assert_cmpint(find_rule(rules, "^[^:]+://([^/]+\\.)?plainhost\\.example[:/]", "block"), >=, 0);

	/* $badfilter, unsupported options and inactive !#if branches drop out. */
	g_assert_cmpint(find_rule(rules, "^[^:]+://([^/]+\\.)?gone\\.example[:/]", "block"), ==, -1);
	g_assert_cmpint(find_rule(rules, "^[^:]+://([^/]+\\.)?csp\\.example[:/]", "block"), ==, -1);
	g_assert_cmpint(find_rule(rules, "^[^:]+://([^/]+\\.)?firefox-only\\.example[:/]", "block"), ==, -1);
	g_assert_cmpint(find_rule(rules, "^[^:]+://([^/]+\\.)?not-firefox\\.example[:/]", "block"), >=, 0);
}

/* Generic and site-specific hiding, exceptions, and $generichide. */
static void
test_cosmetic_rules(void)
{
	g_autoptr(GsurfModuleManager) manager = NULL;
	g_autoptr(TestFilterView) view = NULL;
	g_autoptr(JsonParser) generic_parser = json_parser_new();
	g_autoptr(JsonParser) specific_parser = json_parser_new();
	g_autofree gchar *yaml = NULL;
	JsonArray *generic, *specific;
	JsonObject *trigger;
	gint i;

	if (!have_module())
		return;
	yaml = yaml_with_filters(
		"##.ad-banner\n"
		"##.sponsor\n"
		"site.example#@#.sponsor\n"
		"##.everywhere-allowed\n"
		"#@#.everywhere-allowed\n"
		"site.example,other.example##.site-ad\n"
		"other.example#@#.site-ad\n"
		"brand.*##.entity-ad\n"
		"@@||nohide.example^$generichide\n", NULL);
	manager = load_adblock(yaml);
	view = test_filter_view_new("https://site.example/");
	gsurf_module_manager_dispatch_inject_scripts(manager, GSURF_VIEW(view), NULL);
	generic = ruleset(view, generic_parser, "gsurf-adblock-cosmetic-generic");
	specific = ruleset(view, specific_parser, "gsurf-adblock-cosmetic-specific");

	g_assert_cmpint(find_selector(generic, ".ad-banner"), >=, 0);
	/* A site exception becomes unless-domain on the generic rule. */
	i = find_selector(generic, ".sponsor");
	g_assert_cmpint(i, >=, 0);
	trigger = json_object_get_object_member(json_array_get_object_element(generic, i), "trigger");
	g_assert_true(array_has_string(json_object_get_array_member(trigger, "unless-domain"), "*site.example"));
	/* A generic exception removes the selector entirely. */
	g_assert_cmpint(find_selector(generic, ".everywhere-allowed"), ==, -1);
	/* $generichide: a trailing ignore rule for that site. */
	trigger = json_object_get_object_member(json_array_get_object_element(generic,
		json_array_get_length(generic) - 1), "trigger");
	g_assert_true(array_has_string(json_object_get_array_member(trigger, "if-domain"), "*nohide.example"));

	/* The excepted domain is removed from the specific rule. */
	i = find_selector(specific, ".site-ad");
	g_assert_cmpint(i, >=, 0);
	trigger = json_object_get_object_member(json_array_get_object_element(specific, i), "trigger");
	g_assert_true(array_has_string(json_object_get_array_member(trigger, "if-domain"), "*site.example"));
	g_assert_false(array_has_string(json_object_get_array_member(trigger, "if-domain"), "*other.example"));
	/* Entities need a top-URL regex. */
	i = find_selector(specific, ".entity-ad");
	g_assert_cmpint(i, >=, 0);
	trigger = json_object_get_object_member(json_array_get_object_element(specific, i), "trigger");
	g_assert_true(json_object_has_member(trigger, "if-top-url"));
}

/* $redirect / $removeparam through the module's request filter, including
 * exceptions, priorities, page-domain limits and the allowlist. */
static void
test_request_filter(void)
{
	g_autoptr(GsurfModuleManager) manager = NULL;
	g_autofree gchar *yaml = NULL;
	gchar *out = NULL;

	if (!have_module())
		return;
	yaml = yaml_with_filters(
		"||cdn.example/ads.js$script,redirect=noop.js\n"
		"||cdn.example/ads.js$script,redirect=noop.txt:5\n"
		"||cdn.example/gpt.js$script,redirect=googletagservices_gpt.js,domain=news.example\n"
		"||cdn.example/unknown.png$image,redirect=no-such-resource\n"
		"||cdn.example/kept.js$script,redirect=noop.js\n"
		"@@||cdn.example/kept.js$redirect\n"
		"$removeparam=utm_source\n"
		"$removeparam=/^fbclid=/\n"
		"@@||keepparams.example^$removeparam\n",
		"    whitelist: [\"mine.example\"]\n");
	manager = load_adblock(yaml);

	/* The higher-priority redirect wins. */
	g_assert_cmpint(filter_request(manager, "https://news.example/", "https://cdn.example/ads.js", &out),
		==, GSURF_FILTER_REDIRECT);
	g_assert_true(g_str_has_prefix(out, "data:text/plain;base64,"));
	g_clear_pointer(&out, g_free);

	/* domain= limits a redirect to its pages. */
	g_assert_cmpint(filter_request(manager, "https://news.example/", "https://cdn.example/gpt.js", &out),
		==, GSURF_FILTER_REDIRECT);
	g_assert_true(g_str_has_prefix(out, "data:text/javascript;base64,"));
	g_clear_pointer(&out, g_free);
	g_assert_cmpint(filter_request(manager, "https://elsewhere.example/", "https://cdn.example/gpt.js", &out),
		==, GSURF_FILTER_ALLOW);
	g_assert_null(out);

	/* An unknown resource cannot be served, so the request is blocked. */
	g_assert_cmpint(filter_request(manager, "https://news.example/", "https://cdn.example/unknown.png", &out),
		==, GSURF_FILTER_BLOCK);
	/* An exception disables the redirect. */
	g_assert_cmpint(filter_request(manager, "https://news.example/", "https://cdn.example/kept.js", &out),
		==, GSURF_FILTER_ALLOW);

	/* removeparam: names and /regex/ specs; other parameters survive. */
	g_assert_cmpint(filter_request(manager, "https://news.example/",
		"https://shop.example/item?utm_source=x&id=7&fbclid=abc#top", &out), ==, GSURF_FILTER_REDIRECT);
	g_assert_cmpstr(out, ==, "https://shop.example/item?id=7#top");
	g_clear_pointer(&out, g_free);
	g_assert_cmpint(filter_request(manager, "https://news.example/",
		"https://keepparams.example/?utm_source=x", &out), ==, GSURF_FILTER_ALLOW);
	g_assert_cmpint(filter_request(manager, "https://news.example/",
		"https://shop.example/item?id=7", &out), ==, GSURF_FILTER_ALLOW);

	/* Nothing is rewritten on allowlisted sites. */
	g_assert_cmpint(filter_request(manager, "https://www.mine.example/",
		"https://cdn.example/ads.js", &out), ==, GSURF_FILTER_ALLOW);
}

/* Scriptlets and procedural cosmetics are compiled into the data the
 * module registers for its web-process extension, honouring exceptions
 * and the trusted-list gate. */
static void
test_web_process_data(void)
{
	g_autoptr(GsurfModuleManager) manager = NULL;
	g_autoptr(GVariant) all = NULL;
	g_autoptr(GVariant) adblock = NULL;
	g_autoptr(GVariant) page = NULL;
	g_autoptr(GVariant) scriptlets = NULL;
	g_autoptr(GVariant) cosmetic = NULL;
	g_autoptr(GString) calls = g_string_new(NULL);
	g_autoptr(GString) selectors = g_string_new(NULL);
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *untrusted = NULL;
	g_autofree gchar *list_yaml = NULL;
	g_autoptr(GError) error = NULL;
	GVariantDict dict;
	gsize i;

	if (!have_module())
		return;
	dir = g_dir_make_tmp("gsurf-adblock-XXXXXX", &error);
	g_assert_no_error(error);
	untrusted = g_build_filename(dir, "untrusted.txt", NULL);
	g_assert_true(g_file_set_contents(untrusted,
		"site.example##+js(trusted-set, foo, '{\"a\":1}')\n"
		"site.example##+js(aopr, fromUntrusted)\n", -1, &error));
	g_assert_no_error(error);
	list_yaml = g_strdup_printf("    filter_lists:\n      - \"%s\"\n", untrusted);
	yaml = yaml_with_filters(
		"site.example##+js(set, ads.enabled, false)\n"
		"site.example##+js(aopr, adblockDetector)\n"
		"sub.site.example#@#+js(aopr, adblockDetector)\n"
		"site.example##+js(nostif, adcheck, 1000)\n"
		"site.example#@#+js(nostif, adcheck, 1000)\n"
		"site.example##+js(no-such-scriptlet)\n"
		"site.example##.box:has-text(Sponsored)\n"
		"site.example##.panel:style(top: 0 !important)\n"
		"##.generic:has-text(x)\n", list_yaml);
	manager = load_adblock(yaml);

	all = gsurf_web_process_extension_get_user_data();
	g_variant_dict_init(&dict, all);
	adblock = g_variant_dict_lookup_value(&dict, "adblock", G_VARIANT_TYPE_VARDICT);
	g_variant_dict_clear(&dict);
	if (adblock == NULL) {
		/* Only registered when the extension was built next to the module. */
		g_test_skip("adblock web-process extension not built");
		return;
	}
	g_variant_dict_init(&dict, adblock);
	page = g_variant_dict_lookup_value(&dict, "page", G_VARIANT_TYPE_VARDICT);
	g_variant_dict_clear(&dict);
	g_assert_nonnull(page);
	g_variant_dict_init(&dict, page);
	scriptlets = g_variant_dict_lookup_value(&dict, "scriptlets", G_VARIANT_TYPE("a(asasss)"));
	cosmetic = g_variant_dict_lookup_value(&dict, "cosmetic", G_VARIANT_TYPE("a(asass)"));
	g_variant_dict_clear(&dict);
	g_assert_nonnull(scriptlets);
	g_assert_nonnull(cosmetic);

	for (i = 0; i < g_variant_n_children(scriptlets); i++) {
		g_autofree const gchar **neg = NULL;
		const gchar *call;

		g_variant_get_child(scriptlets, i, "(^a&s^a&s&s&s)", NULL, &neg, NULL, &call);
		g_string_append_printf(calls, "%s|%s\n", call,
			neg[0] != NULL ? neg[0] : "");
	}
	for (i = 0; i < g_variant_n_children(cosmetic); i++) {
		const gchar *sel;

		g_variant_get_child(cosmetic, i, "(^a&s^a&s&s)", NULL, NULL, &sel);
		g_string_append_printf(selectors, "%s\n", sel);
	}

	/* Aliases resolve to the library function, arguments become literals. */
	g_assert_nonnull(strstr(calls->str, "setConstant(\"ads.enabled\",\"false\")|\n"));
	/* A subdomain exception becomes a negated domain. */
	g_assert_nonnull(strstr(calls->str, "abortOnPropertyRead(\"adblockDetector\")|sub.site.example\n"));
	/* An exact-domain exception removes the scriptlet; unknown names and
	 * trusted-only scriptlets from untrusted lists are skipped, while
	 * ordinary scriptlets from those lists are kept. */
	g_assert_null(strstr(calls->str, "noSetTimeoutIf"));
	g_assert_null(strstr(calls->str, "trustedSetConstant"));
	g_assert_nonnull(strstr(calls->str, "abortOnPropertyRead(\"fromUntrusted\")"));

	/* Procedural and :style() filters go to the engine; generic procedural
	 * filters are refused, as uBO does. */
	g_assert_nonnull(strstr(selectors->str, ".box:has-text(Sponsored)\n"));
	g_assert_nonnull(strstr(selectors->str, ".panel:style(top: 0 !important)\n"));
	g_assert_null(strstr(selectors->str, ".generic"));

	g_unlink(untrusted);
	g_rmdir(dir);
	g_clear_object(&manager);
	gsurf_web_process_extension_unregister("adblock");
}

/* The core registry: one extension points at its own directory, several
 * get a link directory, and the init data is keyed by name. */
static void
test_web_process_registry(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("gsurf-webext-XXXXXX", &error);
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *single = NULL;
	g_autofree gchar *links = NULL;
	g_autofree gchar *link = NULL;
	g_autoptr(GVariant) data = NULL;
	g_autofree gchar *value = NULL;

	g_assert_no_error(error);
	a = g_build_filename(dir, "a.so", NULL);
	b = g_build_filename(dir, "b.so", NULL);
	g_assert_true(g_file_set_contents(a, "", 0, NULL));
	g_assert_true(g_file_set_contents(b, "", 0, NULL));

	g_assert_null(gsurf_web_process_extension_get_directory());
	gsurf_web_process_extension_register("first", a, g_variant_new_string("one"));
	single = gsurf_web_process_extension_get_directory();
	g_assert_cmpstr(single, ==, dir);

	gsurf_web_process_extension_register("second", b, NULL);
	links = gsurf_web_process_extension_get_directory();
	g_assert_cmpstr(links, !=, dir);
	link = g_build_filename(links, "second-b.so", NULL);
	g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));

	/* Extensions without data are omitted from the dictionary. */
	data = gsurf_web_process_extension_get_user_data();
	g_assert_true(g_variant_lookup(data, "first", "s", &value));
	g_assert_cmpstr(value, ==, "one");
	g_assert_false(g_variant_lookup(data, "second", "s", NULL));

	gsurf_web_process_extension_unregister("first");
	gsurf_web_process_extension_unregister("second");
	g_assert_null(gsurf_web_process_extension_get_directory());
	g_unlink(link);
	g_rmdir(links);
	g_unlink(a);
	g_unlink(b);
	g_rmdir(dir);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/gsurf/adblock/network-rules", test_network_rules);
	g_test_add_func("/gsurf/adblock/cosmetic-rules", test_cosmetic_rules);
	g_test_add_func("/gsurf/adblock/request-filter", test_request_filter);
	g_test_add_func("/gsurf/adblock/web-process-data", test_web_process_data);
	g_test_add_func("/gsurf/adblock/web-process-registry", test_web_process_registry);
	return g_test_run();
}
