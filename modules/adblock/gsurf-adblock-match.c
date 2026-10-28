/*
 * gsurf-adblock-match.c - Request-time filter matching (redirect/removeparam)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Rules are indexed by one literal token each (the uBO approach): a URL is
 * split into its alphanumeric tokens and only the rules filed under one of
 * those tokens, plus the few untokenizable rules, run their regex. That
 * keeps per-request cost flat no matter how many rules the lists define.
 */

#include "gsurf-adblock-match.h"
#include <libsoup/soup.h>
#include <string.h>

/* Serialized rule: regex, token, match_case, exception, action, arg,
 * if_domains, unless_domains, party, types, priority. */
#define RULE_VARIANT_TYPE "(ssbbusasasuui)"
#define MATCHER_VARIANT_TYPE "(a" RULE_VARIANT_TYPE "as)"

typedef struct {
	gchar             *regex_source;
	GRegex            *regex;
	gchar             *token;
	gboolean           match_case;
	gboolean           exception;
	GsurfAdblockAction action;
	gchar             *arg;
	gchar            **if_domains;
	gchar            **unless_domains;
	GsurfAdblockParty  party;
	guint              types;
	gint               priority;
} MatchRule;

struct _GsurfAdblockMatcher
{
	GPtrArray  *rules;          /* MatchRule*, owns them */
	GHashTable *by_token;       /* token -> GPtrArray of MatchRule* (borrowed) */
	GPtrArray  *untokenized;    /* MatchRule* (borrowed) */
	GPtrArray  *allowed_sites;  /* gchar* */
};

static void
match_rule_free(gpointer data)
{
	MatchRule *rule = data;

	g_free(rule->regex_source);
	g_clear_pointer(&rule->regex, g_regex_unref);
	g_free(rule->token);
	g_free(rule->arg);
	g_strfreev(rule->if_domains);
	g_strfreev(rule->unless_domains);
	g_free(rule);
}

GsurfAdblockMatcher *
gsurf_adblock_matcher_new(void)
{
	GsurfAdblockMatcher *self = g_new0(GsurfAdblockMatcher, 1);

	self->rules = g_ptr_array_new_with_free_func(match_rule_free);
	self->by_token = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	self->untokenized = g_ptr_array_new();
	self->allowed_sites = g_ptr_array_new_with_free_func(g_free);
	return self;
}

void
gsurf_adblock_matcher_free(GsurfAdblockMatcher *self)
{
	if (self == NULL)
		return;
	g_ptr_array_unref(self->untokenized);
	g_hash_table_unref(self->by_token);
	g_ptr_array_unref(self->rules);
	g_ptr_array_unref(self->allowed_sites);
	g_free(self);
}

/* Copy a domain list, treating an empty one as "no restriction". */
static gchar **
copy_domains(const gchar *const *domains)
{
	if (domains == NULL || domains[0] == NULL)
		return NULL;
	return g_strdupv((gchar **)domains);
}

gboolean
gsurf_adblock_matcher_add(GsurfAdblockMatcher *self, const gchar *regex, const gchar *token,
                          gboolean match_case, gboolean exception, GsurfAdblockAction action,
                          const gchar *arg, const gchar *const *if_domains,
                          const gchar *const *unless_domains, GsurfAdblockParty party,
                          guint types, gint priority, GError **error)
{
	MatchRule *rule;
	GRegex *compiled;
	GPtrArray *bucket;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(regex != NULL, FALSE);

	compiled = g_regex_new(regex,
		G_REGEX_OPTIMIZE | (match_case ? 0 : G_REGEX_CASELESS),
		G_REGEX_MATCH_DEFAULT, error);
	if (compiled == NULL)
		return FALSE;

	rule = g_new0(MatchRule, 1);
	rule->regex_source = g_strdup(regex);
	rule->regex = compiled;
	rule->token = g_strdup(token != NULL ? token : "");
	rule->match_case = match_case;
	rule->exception = exception;
	rule->action = action;
	rule->arg = g_strdup(arg != NULL ? arg : "");
	rule->if_domains = copy_domains(if_domains);
	rule->unless_domains = copy_domains(unless_domains);
	rule->party = party;
	rule->types = types;
	rule->priority = priority;
	g_ptr_array_add(self->rules, rule);

	/* Index under the rule's token, or check it for every request. */
	if (*rule->token == '\0') {
		g_ptr_array_add(self->untokenized, rule);
		return TRUE;
	}
	bucket = g_hash_table_lookup(self->by_token, rule->token);
	if (bucket == NULL) {
		bucket = g_ptr_array_new();
		g_hash_table_insert(self->by_token, g_strdup(rule->token), bucket);
	}
	g_ptr_array_add(bucket, rule);
	return TRUE;
}

void
gsurf_adblock_matcher_add_allowed_site(GsurfAdblockMatcher *self, const gchar *host)
{
	g_return_if_fail(self != NULL);
	if (host != NULL && *host != '\0')
		g_ptr_array_add(self->allowed_sites, g_ascii_strdown(host, -1));
}

guint
gsurf_adblock_matcher_get_size(GsurfAdblockMatcher *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->rules->len;
}

/* --- serialization --- */

static const gchar *const empty_strv[] = { NULL };

GVariant *
gsurf_adblock_matcher_serialize(GsurfAdblockMatcher *self)
{
	GVariantBuilder rules, sites;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	g_variant_builder_init(&rules, G_VARIANT_TYPE("a" RULE_VARIANT_TYPE));
	for (i = 0; i < self->rules->len; i++) {
		MatchRule *r = g_ptr_array_index(self->rules, i);

		g_variant_builder_add(&rules, "(ssbbus^as^asuui)", r->regex_source, r->token,
			r->match_case, r->exception, (guint32)r->action, r->arg,
			r->if_domains != NULL ? (const gchar *const *)r->if_domains : empty_strv,
			r->unless_domains != NULL ? (const gchar *const *)r->unless_domains : empty_strv,
			(guint32)r->party, (guint32)r->types, (gint32)r->priority);
	}
	g_variant_builder_init(&sites, G_VARIANT_TYPE_STRING_ARRAY);
	for (i = 0; i < self->allowed_sites->len; i++)
		g_variant_builder_add(&sites, "s", (const gchar *)g_ptr_array_index(self->allowed_sites, i));

	return g_variant_ref_sink(g_variant_new("(" "a" RULE_VARIANT_TYPE "as)", &rules, &sites));
}

GsurfAdblockMatcher *
gsurf_adblock_matcher_new_from_variant(GVariant *variant)
{
	GsurfAdblockMatcher *self;
	g_autoptr(GVariantIter) rules = NULL;
	g_autoptr(GVariantIter) sites = NULL;
	const gchar *regex, *token, *arg, *site;
	const gchar **if_domains, **unless_domains;
	gboolean match_case, exception;
	guint32 action, party, types;
	gint32 priority;

	if (variant == NULL || !g_variant_is_of_type(variant, G_VARIANT_TYPE(MATCHER_VARIANT_TYPE)))
		return NULL;

	self = gsurf_adblock_matcher_new();
	g_variant_get(variant, "(a" RULE_VARIANT_TYPE "as)", &rules, &sites);
	while (g_variant_iter_next(rules, "(&s&sbbu&s^a&s^a&suui)", &regex, &token, &match_case,
	                           &exception, &action, &arg, &if_domains, &unless_domains,
	                           &party, &types, &priority)) {
		g_autoptr(GError) error = NULL;

		if (!gsurf_adblock_matcher_add(self, regex, token, match_case, exception,
		                               (GsurfAdblockAction)action, arg, if_domains,
		                               unless_domains, (GsurfAdblockParty)party,
		                               types, priority, &error))
			g_debug("gsurf adblock: dropped rule /%s/: %s", regex, error->message);
		g_free(if_domains);
		g_free(unless_domains);
	}
	while (g_variant_iter_next(sites, "&s", &site))
		gsurf_adblock_matcher_add_allowed_site(self, site);
	return self;
}

/* --- domain helpers --- */

gboolean
gsurf_adblock_host_matches_domain(const gchar *host, const gchar *domain)
{
	gsize host_len, domain_len;
	const gchar *p;

	if (host == NULL || domain == NULL || *host == '\0' || *domain == '\0')
		return FALSE;

	/* Entity "example.*": "example." at a label boundary, followed by a
	 * public suffix (example.com, example.co.uk, cdn.example.de). */
	domain_len = strlen(domain);
	if (domain_len > 2 && g_str_has_suffix(domain, ".*")) {
		gsize stem = domain_len - 1;   /* keep the trailing '.' */

		for (p = host; p != NULL; p = strchr(p, '.'), p = p != NULL ? p + 1 : NULL) {
			if (strncmp(p, domain, stem) == 0 && p[stem] != '\0' &&
			    soup_tld_domain_is_public_suffix(p + stem))
				return TRUE;
		}
		return FALSE;
	}

	/* Plain domain: an exact match or a subdomain. */
	host_len = strlen(host);
	if (host_len < domain_len)
		return FALSE;
	if (host_len == domain_len)
		return g_ascii_strcasecmp(host, domain) == 0;
	return host[host_len - domain_len - 1] == '.' &&
		g_ascii_strcasecmp(host + host_len - domain_len, domain) == 0;
}

static gboolean
host_in_list(const gchar *host, gchar **domains)
{
	guint i;

	for (i = 0; domains != NULL && domains[i] != NULL; i++)
		if (gsurf_adblock_host_matches_domain(host, domains[i]))
			return TRUE;
	return FALSE;
}

/* The registrable domain ("eTLD+1"), falling back to the host itself for
 * IP addresses, localhost and other names without a public suffix. */
static gchar *
base_domain(const gchar *host)
{
	const gchar *base;

	if (host == NULL)
		return NULL;
	base = soup_tld_get_base_domain(host, NULL);
	return g_strdup(base != NULL ? base : host);
}

static gchar *
url_host(const gchar *url)
{
	g_autoptr(GUri) uri = NULL;
	const gchar *host;

	if (url == NULL)
		return NULL;
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL || (host = g_uri_get_host(uri)) == NULL || *host == '\0')
		return NULL;
	return g_ascii_strdown(host, -1);
}

/* --- matching --- */

typedef struct {
	const gchar *url;
	const gchar *page_host;   /* NULL when unknown */
	gboolean     third_party;
	gboolean     party_known;
	guint        type;
} RequestInfo;

/* Everything but the URL regex: type, party and page-domain conditions. */
static gboolean
rule_conditions_match(MatchRule *rule, RequestInfo *req)
{
	if (rule->types != 0 && req->type != 0 && (rule->types & req->type) == 0)
		return FALSE;
	/* A typed rule cannot be confirmed for a request of unknown type; the
	 * only exception is a removeparam rule, whose worst case is a cleaner
	 * URL rather than a broken resource. */
	if (rule->types != 0 && req->type == 0 && rule->action == GSURF_ADBLOCK_ACTION_REDIRECT)
		return FALSE;
	if (rule->party != GSURF_ADBLOCK_PARTY_ANY) {
		if (!req->party_known)
			return FALSE;
		if ((rule->party == GSURF_ADBLOCK_PARTY_THIRD) != req->third_party)
			return FALSE;
	}
	if (rule->if_domains != NULL &&
	    (req->page_host == NULL || !host_in_list(req->page_host, rule->if_domains)))
		return FALSE;
	if (rule->unless_domains != NULL && req->page_host != NULL &&
	    host_in_list(req->page_host, rule->unless_domains))
		return FALSE;
	return TRUE;
}

static void
collect_matches(GPtrArray *candidates, RequestInfo *req, GPtrArray *out)
{
	guint i;

	for (i = 0; candidates != NULL && i < candidates->len; i++) {
		MatchRule *rule = g_ptr_array_index(candidates, i);

		if (g_ptr_array_find(out, rule, NULL))
			continue;
		if (!rule_conditions_match(rule, req))
			continue;
		if (g_regex_match(rule->regex, req->url, 0, NULL))
			g_ptr_array_add(out, rule);
	}
}

/* Every rule whose token occurs in the URL, plus the untokenized ones. */
static GPtrArray *
find_matching_rules(GsurfAdblockMatcher *self, RequestInfo *req)
{
	GPtrArray *out = g_ptr_array_new();
	g_autofree gchar *lower = g_ascii_strdown(req->url, -1);
	const gchar *p = lower;

	collect_matches(self->untokenized, req, out);
	while (*p != '\0') {
		const gchar *start;
		g_autofree gchar *token = NULL;

		while (*p != '\0' && !g_ascii_isalnum(*p))
			p++;
		start = p;
		while (g_ascii_isalnum(*p))
			p++;
		if (p == start)
			continue;
		token = g_strndup(start, p - start);
		collect_matches(g_hash_table_lookup(self->by_token, token), req, out);
	}
	return out;
}

/* Is the redirect/removeparam @arg disabled by a matching exception? */
static gboolean
is_excepted(GPtrArray *matches, GsurfAdblockAction action, const gchar *arg)
{
	guint i;

	for (i = 0; i < matches->len; i++) {
		MatchRule *rule = g_ptr_array_index(matches, i);

		if (rule->exception && rule->action == action &&
		    (*rule->arg == '\0' || g_str_equal(rule->arg, arg)))
			return TRUE;
	}
	return FALSE;
}

/* Does one query parameter fall under a removeparam spec? An empty spec
 * removes every parameter; "/re/" is tested against "name=value"; "~spec"
 * inverts the test. */
static gboolean
param_matches_spec(const gchar *param, const gchar *spec)
{
	gboolean invert = FALSE;
	gboolean hit;
	gsize len;

	if (*spec == '\0')
		return TRUE;
	if (*spec == '~') {
		invert = TRUE;
		spec++;
	}
	len = strlen(spec);
	if (len >= 2 && spec[0] == '/' && spec[len - 1] == '/') {
		g_autofree gchar *pattern = g_strndup(spec + 1, len - 2);
		g_autoptr(GRegex) re = g_regex_new(pattern, G_REGEX_DEFAULT, G_REGEX_MATCH_DEFAULT, NULL);

		hit = re != NULL && g_regex_match(re, param, 0, NULL);
	} else {
		const gchar *eq = strchr(param, '=');
		gsize name_len = eq != NULL ? (gsize)(eq - param) : strlen(param);

		hit = name_len == len && strncmp(param, spec, len) == 0;
	}
	return invert ? !hit : hit;
}

/* Rebuild @url without the parameters any of @specs removes. */
static gchar *
remove_params(const gchar *url, GPtrArray *specs)
{
	const gchar *query = strchr(url, '?');
	const gchar *fragment;
	g_autofree gchar *base = NULL;
	g_autofree gchar *params = NULL;
	gchar **parts;
	GString *out;
	gboolean changed = FALSE, first = TRUE;
	guint i, j;

	if (query == NULL)
		return NULL;
	fragment = strchr(query, '#');
	base = g_strndup(url, query - url);
	params = fragment != NULL ? g_strndup(query + 1, fragment - query - 1) : g_strdup(query + 1);

	out = g_string_new(base);
	parts = g_strsplit(params, "&", -1);
	for (i = 0; parts[i] != NULL; i++) {
		gboolean drop = FALSE;

		if (*parts[i] == '\0')
			continue;
		for (j = 0; j < specs->len && !drop; j++)
			drop = param_matches_spec(parts[i], g_ptr_array_index(specs, j));
		if (drop) {
			changed = TRUE;
			continue;
		}
		g_string_append_c(out, first ? '?' : '&');
		g_string_append(out, parts[i]);
		first = FALSE;
	}
	g_strfreev(parts);
	if (fragment != NULL)
		g_string_append(out, fragment);
	if (!changed) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

gchar *
gsurf_adblock_matcher_process(GsurfAdblockMatcher *self, const gchar *url,
                              const gchar *page_url, guint type, gboolean *blocked)
{
	g_autofree gchar *page_host = NULL;
	g_autofree gchar *req_host = NULL;
	g_autoptr(GPtrArray) matches = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	MatchRule *redirect = NULL;
	RequestInfo req = { 0 };
	guint i;

	if (blocked != NULL)
		*blocked = FALSE;
	g_return_val_if_fail(self != NULL, NULL);
	if (url == NULL || self->rules->len == 0)
		return NULL;
	if (!g_str_has_prefix(url, "http:") && !g_str_has_prefix(url, "https:") &&
	    !g_str_has_prefix(url, "ws:") && !g_str_has_prefix(url, "wss:"))
		return NULL;

	/* Nothing is rewritten on allowlisted sites. */
	page_host = url_host(page_url);
	req_host = url_host(url);
	for (i = 0; page_host != NULL && i < self->allowed_sites->len; i++)
		if (gsurf_adblock_host_matches_domain(page_host, g_ptr_array_index(self->allowed_sites, i)))
			return NULL;

	req.url = url;
	req.page_host = page_host;
	req.type = type;
	if (page_host != NULL && req_host != NULL) {
		g_autofree gchar *page_base = base_domain(page_host);
		g_autofree gchar *req_base = base_domain(req_host);

		req.party_known = TRUE;
		req.third_party = g_ascii_strcasecmp(page_base, req_base) != 0;
	}
	matches = find_matching_rules(self, &req);

	/* The highest-priority redirect that no exception disables wins. */
	for (i = 0; i < matches->len; i++) {
		MatchRule *rule = g_ptr_array_index(matches, i);

		if (rule->exception || rule->action != GSURF_ADBLOCK_ACTION_REDIRECT)
			continue;
		if (is_excepted(matches, GSURF_ADBLOCK_ACTION_REDIRECT, rule->arg))
			continue;
		if (redirect == NULL || rule->priority > redirect->priority)
			redirect = rule;
	}
	if (redirect != NULL) {
		gchar *target = gsurf_adblock_resource_get_uri(redirect->arg);

		if (target == NULL && blocked != NULL)
			*blocked = TRUE;
		return target;
	}

	/* Otherwise strip every parameter a non-excepted rule removes. */
	specs = g_ptr_array_new();
	for (i = 0; i < matches->len; i++) {
		MatchRule *rule = g_ptr_array_index(matches, i);

		if (rule->exception || rule->action != GSURF_ADBLOCK_ACTION_REMOVEPARAM)
			continue;
		if (is_excepted(matches, GSURF_ADBLOCK_ACTION_REMOVEPARAM, rule->arg))
			continue;
		g_ptr_array_add(specs, rule->arg);
	}
	if (specs->len == 0)
		return NULL;
	return remove_params(url, specs);
}

/* --- request type inference --- */

guint
gsurf_adblock_guess_type(const gchar *url)
{
	static const struct {
		const gchar *ext;
		guint        type;
	} table[] = {
		{ "js", GSURF_ADBLOCK_TYPE_SCRIPT }, { "mjs", GSURF_ADBLOCK_TYPE_SCRIPT },
		{ "css", GSURF_ADBLOCK_TYPE_STYLESHEET },
		{ "png", GSURF_ADBLOCK_TYPE_IMAGE }, { "gif", GSURF_ADBLOCK_TYPE_IMAGE },
		{ "jpg", GSURF_ADBLOCK_TYPE_IMAGE }, { "jpeg", GSURF_ADBLOCK_TYPE_IMAGE },
		{ "webp", GSURF_ADBLOCK_TYPE_IMAGE }, { "avif", GSURF_ADBLOCK_TYPE_IMAGE },
		{ "svg", GSURF_ADBLOCK_TYPE_IMAGE }, { "ico", GSURF_ADBLOCK_TYPE_IMAGE },
		{ "woff", GSURF_ADBLOCK_TYPE_FONT }, { "woff2", GSURF_ADBLOCK_TYPE_FONT },
		{ "ttf", GSURF_ADBLOCK_TYPE_FONT }, { "otf", GSURF_ADBLOCK_TYPE_FONT },
		{ "mp4", GSURF_ADBLOCK_TYPE_MEDIA }, { "webm", GSURF_ADBLOCK_TYPE_MEDIA },
		{ "mp3", GSURF_ADBLOCK_TYPE_MEDIA }, { "m3u8", GSURF_ADBLOCK_TYPE_MEDIA },
		{ "ogg", GSURF_ADBLOCK_TYPE_MEDIA },
		{ "html", GSURF_ADBLOCK_TYPE_SUBDOCUMENT }, { "htm", GSURF_ADBLOCK_TYPE_SUBDOCUMENT },
		{ "json", GSURF_ADBLOCK_TYPE_XHR }, { "xml", GSURF_ADBLOCK_TYPE_XHR },
	};
	g_autoptr(GUri) uri = NULL;
	const gchar *path, *dot, *slash;
	guint i;

	if (url == NULL)
		return 0;
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL || (path = g_uri_get_path(uri)) == NULL)
		return 0;
	slash = strrchr(path, '/');
	dot = strrchr(path, '.');
	if (dot == NULL || (slash != NULL && dot < slash))
		return 0;
	for (i = 0; i < G_N_ELEMENTS(table); i++)
		if (g_ascii_strcasecmp(dot + 1, table[i].ext) == 0)
			return table[i].type;
	return 0;
}

/* --- redirect resources ---
 *
 * Neutered stand-ins for uBO's redirect resource names. Script surrogates
 * expose the API surface pages commonly call so a blocked ad/analytics
 * library does not take the page's own code down with it. */

#define GPT_SURROGATE \
	"(function(){var n=function(){},s={},p={},k=['addEventListener','removeEventListener'," \
	"'setTargeting','clearTargeting','defineSizeMapping','addService','setCollapseEmptyDiv'," \
	"'setClickUrl','setForceSafeFrame','setSafeFrameConfig','set','updateTargetingFromMap'," \
	"'setRequestNonPersonalizedAds','setPrivacySettings','setTagForChildDirectedTreatment'," \
	"'setPublisherProvidedId','setLocation'];k.forEach(function(m){s[m]=function(){return s};" \
	"p[m]=function(){return p}});s.getTargeting=function(){return[]};s.getSlotElementId=" \
	"function(){return''};s.getAdUnitPath=function(){return''};s.getSizes=function(){return[]};" \
	"s.get=function(){return null};s.getResponseInformation=function(){return null};" \
	"p.getTargeting=function(){return[]};p.getTargetingKeys=function(){return[]};" \
	"p.getSlots=function(){return[]};p.get=function(){return null};p.enableSingleRequest=" \
	"function(){return true};['collapseEmptyDivs','disableInitialLoad','enableLazyLoad'," \
	"'refresh','clear','setCentering','updateCorrelator','display','setVideoContent']" \
	".forEach(function(m){p[m]=n});var g=window.googletag||{},c=g.cmd||[];g.apiReady=true;" \
	"g.pubadsReady=true;g.pubads=function(){return p};g.companionAds=function(){return{" \
	"setRefreshUnfilledSlots:n,addEventListener:n}};g.content=function(){return{setContent:n," \
	"addEventListener:n}};g.defineSlot=function(){return s};g.defineOutOfPageSlot=function(){" \
	"return s};g.destroySlots=function(){return true};g.display=n;g.enableServices=n;" \
	"g.getVersion=function(){return''};g.sizeMapping=function(){var m={addSize:function(){" \
	"return m},build:function(){return[]}};return m};g.setAdIframeTitle=n;g.openConsole=n;" \
	"g.disablePublisherConsole=n;g.setConfig=n;g.getConfig=function(){return{}};" \
	"g.secureSignalProviders=[];g.cmd={push:function(f){try{f()}catch(e){}return 1}};" \
	"window.googletag=g;for(var i=0;i<c.length;i++){try{c[i]()}catch(e){}}})();"

#define GA_SURROGATE \
	"(function(){var n=function(){},t={get:n,set:n,send:n},ga=function(){var a=arguments," \
	"o=a[a.length-1];if(o&&typeof o==='object'&&typeof o.hitCallback==='function'){try{" \
	"o.hitCallback()}catch(e){}}else if(typeof o==='function'){try{o(t)}catch(e){}}};" \
	"ga.create=function(){return t};ga.getByName=function(){return t};ga.getAll=function(){" \
	"return[t]};ga.remove=n;ga.loaded=true;var k=window.GoogleAnalyticsObject||'ga'," \
	"q=window[k]&&window[k].q;window[k]=ga;if(Array.isArray(q)){q.forEach(function(a){" \
	"ga.apply(null,a)})}})();"

#define GTM_SURROGATE \
	"(function(){var cb=function(o){if(o&&typeof o==='object'&&typeof o.eventCallback===" \
	"'function'){setTimeout(o.eventCallback,1)}},d=window.dataLayer;if(Array.isArray(d)){" \
	"var push=d.push;d.push=function(o){cb(o);return push.apply(this,arguments)};d.forEach(cb)}" \
	"window.google_tag_manager=window.google_tag_manager||{};})();"

#define ADSBYGOOGLE_SURROGATE \
	"(function(){window.adsbygoogle={loaded:true,push:function(){},length:0};" \
	"document.querySelectorAll('ins.adsbygoogle').forEach(function(e){" \
	"e.setAttribute('data-adsbygoogle-status','done')})})();"

#define APSTAG_SURROGATE \
	"(function(){var n=function(){},cb=function(){var f=arguments[arguments.length-1];" \
	"if(typeof f==='function'){try{f([])}catch(e){}}};window.apstag={_Q:[],init:n," \
	"fetchBids:cb,setDisplayBids:n,targetingKeys:function(){return[]},renderImp:n,dpa:n," \
	"rpa:n,upa:n,thirdPartyData:{}}})();"

#define NOBAB_SURROGATE \
	"(function(){var F=function(){},P=F.prototype;P.check=function(){return this};" \
	"P.clearEvent=function(){};P.emitEvent=function(){return this};P.on=function(d,f){" \
	"if(!d){try{f()}catch(e){}}return this};P.onDetected=function(){return this};" \
	"P.onNotDetected=function(f){try{f()}catch(e){}return this};P.setOption=function(){" \
	"return this};P.options={set:function(){return this},get:function(){}};var o=new F();" \
	"window.FuckAdBlock=window.BlockAdBlock=window.SniffAdBlock=F;window.fuckAdBlock=" \
	"window.blockAdBlock=window.sniffAdBlock=o})();"

typedef struct {
	const gchar *names;     /* '|'-separated name and aliases */
	const gchar *mime;
	const gchar *body;      /* text, or base64 when @base64 */
	gboolean     base64;
} Resource;

static const Resource resources[] = {
	{ "noop.js|noopjs|abp-resource:blank-js", "text/javascript", "(function(){})();", FALSE },
	{ "noop.txt|nooptext|abp-resource:blank-text|empty", "text/plain", "", FALSE },
	{ "noop.css|noopcss|abp-resource:blank-css", "text/css", "", FALSE },
	{ "noop.html|noopframe|abp-resource:blank-html", "text/html", "<!DOCTYPE html>", FALSE },
	{ "noop.json|noopjson", "application/json", "{}", FALSE },
	{ "1x1.gif|1x1-transparent.gif|abp-resource:1x1-transparent-gif", "image/gif",
	  "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", TRUE },
	{ "2x2.png|2x2-transparent.png|abp-resource:2x2-transparent-png", "image/png",
	  "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAC0lEQVR42mNgQAcAABIAAeRVjecAAAAASUVORK5CYII=", TRUE },
	{ "3x2.png|3x2-transparent.png|abp-resource:3x2-transparent-png", "image/png",
	  "iVBORw0KGgoAAAANSUhEUgAAAAMAAAACCAYAAACddGYaAAAAC0lEQVR42mNgwAUAABoAAS+Yl6YAAAAASUVORK5CYII=", TRUE },
	{ "32x32.png|32x32-transparent.png|abp-resource:32x32-transparent-png", "image/png",
	  "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAGklEQVR42u3BAQEAAACCIP+vbkhAAQAAAO8GECAAAcm1w7EAAAAASUVORK5CYII=", TRUE },
	{ "noop-0.1s.mp3|noopmp3-0.1s|abp-resource:blank-mp3", "audio/mpeg", "", FALSE },
	{ "noop-0.5s.mp3", "audio/mpeg", "", FALSE },
	{ "noop-1s.mp4|noopmp4-1s|abp-resource:blank-mp4", "video/mp4", "", FALSE },
	{ "noop-vast2.xml|noopvast-2.0", "text/xml",
	  "<?xml version=\"1.0\" encoding=\"UTF-8\"?><VAST version=\"2.0\"></VAST>", FALSE },
	{ "noop-vast3.xml|noopvast-3.0", "text/xml",
	  "<?xml version=\"1.0\" encoding=\"UTF-8\"?><VAST version=\"3.0\"></VAST>", FALSE },
	{ "noop-vast4.xml|noopvast-4.0", "text/xml",
	  "<?xml version=\"1.0\" encoding=\"UTF-8\"?><VAST version=\"4.0\"></VAST>", FALSE },
	{ "noop-vmap1.xml|noop-vmap1.0.xml|noopvmap-1.0", "text/xml",
	  "<?xml version=\"1.0\" encoding=\"UTF-8\"?><vmap:VMAP xmlns:vmap=\"http://www.iab.net/videosuite/vmap\" version=\"1.0\"></vmap:VMAP>", FALSE },
	{ "googletagservices_gpt.js|googletagservices.com/gpt.js", "text/javascript", GPT_SURROGATE, FALSE },
	{ "google-analytics_analytics.js|google-analytics.com/analytics.js|google-analytics_ga.js|google-analytics.com/ga.js",
	  "text/javascript", GA_SURROGATE, FALSE },
	{ "googletagmanager_gtm.js|googletagmanager.com/gtm.js", "text/javascript", GTM_SURROGATE, FALSE },
	{ "googlesyndication_adsbygoogle.js|googlesyndication.com/adsbygoogle.js",
	  "text/javascript", ADSBYGOOGLE_SURROGATE, FALSE },
	{ "amazon_apstag.js|amazon-adsystem.com/aax2/amzn_ads.js", "text/javascript", APSTAG_SURROGATE, FALSE },
	{ "scorecardresearch_beacon.js|scorecardresearch.com/beacon.js", "text/javascript",
	  "(function(){window.COMSCORE={beacon:function(){},purge:function(){return[]}}})();", FALSE },
	{ "nobab.js|bab-defuser.js|nofab.js|fuckadblock.js-3.2.0|fuckadblock.js", "text/javascript",
	  NOBAB_SURROGATE, FALSE },
};

static const Resource *
resource_find(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(resources); i++) {
		g_auto(GStrv) names = g_strsplit(resources[i].names, "|", -1);

		if (g_strv_contains((const gchar *const *)names, name))
			return &resources[i];
	}
	return NULL;
}

gchar *
gsurf_adblock_resource_get_uri(const gchar *name)
{
	const Resource *res;
	g_autofree gchar *encoded = NULL;

	if (name == NULL || *name == '\0')
		return NULL;
	res = resource_find(name);
	/* Unknown script surrogates degrade to an empty script, which is what
	 * a page expecting a missing library would see anyway. */
	if (res == NULL && g_str_has_suffix(name, ".js"))
		res = resource_find("noop.js");
	if (res == NULL)
		return NULL;
	if (res->base64)
		return g_strdup_printf("data:%s;base64,%s", res->mime, res->body);
	encoded = g_base64_encode((const guchar *)res->body, strlen(res->body));
	return g_strdup_printf("data:%s;base64,%s", res->mime, encoded);
}
