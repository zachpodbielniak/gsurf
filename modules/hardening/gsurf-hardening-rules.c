/*
 * gsurf-hardening-rules.c - Pure decision logic of the hardening module
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Compiled into hardening.so, into the web-process extension and into
 * tests/test-hardening.c. GLib/GIO only; see gsurf-hardening-rules.h.
 */

#include "gsurf-hardening-rules.h"
#include <gio/gio.h>
#include <string.h>

/* --- names --- */

typedef struct {
	const gchar *name;
	guint        feature;
} FeatureName;

/* The `skip:` vocabulary, in the order the docs list the options. */
static const FeatureName feature_names[] = {
	{ "https_only",   GSURF_HARDENING_HTTPS_ONLY },
	{ "strip_params", GSURF_HARDENING_STRIP_PARAMS },
	{ "referer",      GSURF_HARDENING_REFERER },
	{ "gpc",          GSURF_HARDENING_GPC },
	{ "dnt",          GSURF_HARDENING_DNT },
	{ "no_prefetch",  GSURF_HARDENING_NO_PREFETCH },
	{ "webrtc",       GSURF_HARDENING_WEBRTC },
};

/* Options that exist but act on a whole session, never on one site. */
static const gchar *const session_wide_names[] = { "third_party_cookies", "itp", NULL };

guint
gsurf_hardening_feature_from_name(const gchar *name)
{
	guint i;

	if (name == NULL)
		return 0;
	if (g_ascii_strcasecmp(name, "all") == 0)
		return GSURF_HARDENING_ALL;
	for (i = 0; i < G_N_ELEMENTS(feature_names); i++)
		if (g_ascii_strcasecmp(name, feature_names[i].name) == 0)
			return feature_names[i].feature;
	return 0;
}

const gchar *
gsurf_hardening_feature_to_name(guint feature)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(feature_names); i++)
		if (feature_names[i].feature == feature)
			return feature_names[i].name;
	return NULL;
}

gboolean
gsurf_hardening_referer_from_name(const gchar *name, GsurfHardeningReferer *out)
{
	GsurfHardeningReferer policy;

	if (name == NULL)
		return FALSE;
	if (g_ascii_strcasecmp(name, "off") == 0)
		policy = GSURF_HARDENING_REFERER_OFF;
	else if (g_ascii_strcasecmp(name, "trim_cross_origin") == 0)
		policy = GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN;
	else if (g_ascii_strcasecmp(name, "strip_cross_origin") == 0)
		policy = GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN;
	else
		return FALSE;
	if (out != NULL)
		*out = policy;
	return TRUE;
}

const gchar *
gsurf_hardening_referer_to_name(GsurfHardeningReferer policy)
{
	switch (policy) {
	case GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN:  return "trim_cross_origin";
	case GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN: return "strip_cross_origin";
	case GSURF_HARDENING_REFERER_OFF:
	default:                                         return "off";
	}
}

/* --- URI helpers --- */

/* Parse leniently: keep %-escapes as they are and do not insist on a
 * DNS-valid host, since request URIs come straight from pages. */
static GUri *
parse_uri(const gchar *uri)
{
	if (uri == NULL || *uri == '\0')
		return NULL;
	return g_uri_parse(uri, G_URI_FLAGS_ENCODED | G_URI_FLAGS_NON_DNS, NULL);
}

/* The port a URI really uses: its explicit port, else the scheme's. */
static gint
effective_port(GUri *uri)
{
	const gchar *scheme = g_uri_get_scheme(uri);
	gint port = g_uri_get_port(uri);

	if (port > 0)
		return port;
	if (g_ascii_strcasecmp(scheme, "http") == 0 || g_ascii_strcasecmp(scheme, "ws") == 0)
		return 80;
	if (g_ascii_strcasecmp(scheme, "https") == 0 || g_ascii_strcasecmp(scheme, "wss") == 0)
		return 443;
	return -1;
}

/* Lowercase host without a trailing dot, or NULL. */
static gchar *
normalized_host(GUri *uri)
{
	const gchar *host = g_uri_get_host(uri);
	gchar *out;
	gsize len;

	if (host == NULL || *host == '\0')
		return NULL;
	out = g_ascii_strdown(host, -1);
	len = strlen(out);
	if (len > 1 && out[len - 1] == '.')
		out[len - 1] = '\0';
	return out;
}

/* --- local hosts --- */

/* IP literals: unspecified, loopback, link-local and private ranges are
 * local; so are CGNAT (100.64/10) and IPv6 unique-local (fc00::/7). */
static gboolean
address_is_local(GInetAddress *address)
{
	const guint8 *bytes = g_inet_address_to_bytes(address);

	if (g_inet_address_get_is_any(address) ||
	    g_inet_address_get_is_loopback(address) ||
	    g_inet_address_get_is_link_local(address) ||
	    g_inet_address_get_is_site_local(address))
		return TRUE;
	if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4)
		return bytes[0] == 100 && (bytes[1] & 0xc0) == 64;
	return (bytes[0] & 0xfe) == 0xfc;
}

gboolean
gsurf_hardening_host_is_local(const gchar *host)
{
	static const gchar *const suffixes[] = {
		".localhost", ".local", ".lan", ".internal", ".home.arpa", NULL
	};
	g_autofree gchar *lower = NULL;
	g_autoptr(GInetAddress) address = NULL;
	gsize len;
	guint i;

	if (host == NULL || *host == '\0')
		return TRUE;

	/* One spelling: lowercase, no brackets, no trailing dot. */
	if (host[0] == '[') {
		len = strlen(host);
		lower = g_ascii_strdown(host + 1, len > 2 && host[len - 1] == ']' ? (gssize)(len - 2) : -1);
	} else {
		lower = g_ascii_strdown(host, -1);
	}
	len = strlen(lower);
	if (len > 1 && lower[len - 1] == '.')
		lower[--len] = '\0';

	/* IP literals are decided by their address range alone. */
	address = g_inet_address_new_from_string(lower);
	if (address != NULL)
		return address_is_local(address);

	/* Names: localhost, single labels ("router") and local suffixes. */
	if (g_str_equal(lower, "localhost") || strchr(lower, '.') == NULL)
		return TRUE;
	for (i = 0; suffixes[i] != NULL; i++)
		if (g_str_has_suffix(lower, suffixes[i]))
			return TRUE;
	return FALSE;
}

/* --- https-only --- */

gchar *
gsurf_hardening_upgrade_uri(const gchar *uri, gboolean upgrade_local)
{
	g_autoptr(GUri) parsed = parse_uri(uri);
	g_autoptr(GUri) upgraded = NULL;
	g_autofree gchar *host = NULL;
	gint port;

	/* Only plain http with a host is ever upgraded. */
	if (parsed == NULL || g_ascii_strcasecmp(g_uri_get_scheme(parsed), "http") != 0)
		return NULL;
	host = normalized_host(parsed);
	if (host == NULL)
		return NULL;
	if (!upgrade_local && gsurf_hardening_host_is_local(host))
		return NULL;

	/* Same URI with scheme https; :80 means "the default port" and goes,
	 * any other explicit port is kept (Firefox's HTTPS-Only does the same). */
	port = g_uri_get_port(parsed);
	if (port == 80)
		port = -1;
	upgraded = g_uri_build(g_uri_get_flags(parsed), "https", g_uri_get_userinfo(parsed),
		g_uri_get_host(parsed), port, g_uri_get_path(parsed), g_uri_get_query(parsed),
		g_uri_get_fragment(parsed));
	return g_uri_to_string(upgraded);
}

gboolean
gsurf_hardening_is_downgrade(const gchar *from, const gchar *to)
{
	g_autoptr(GUri) a = parse_uri(from);
	g_autofree gchar *upgraded = NULL;
	g_autofree gchar *normalized = NULL;

	/* Only an https response can be downgraded. */
	if (a == NULL || g_ascii_strcasecmp(g_uri_get_scheme(a), "https") != 0)
		return FALSE;

	/* A loop is a redirect whose upgrade lands exactly where it came
	 * from; any other http target is simply upgraded and followed. */
	upgraded = gsurf_hardening_upgrade_uri(to, TRUE);
	if (upgraded == NULL)
		return FALSE;
	normalized = g_uri_to_string(a);
	return g_strcmp0(upgraded, normalized) == 0;
}

/* --- query-parameter stripping --- */

/* Does the parameter @component ("name=value", still encoded) carry a
 * name that matches one of @globs? */
static gboolean
param_matches(const gchar *component, const gchar * const *globs)
{
	const gchar *eq = strchr(component, '=');
	const gchar *end = eq != NULL ? eq : component + strlen(component);
	g_autofree gchar *decoded = g_uri_unescape_segment(component, end, NULL);
	g_autofree gchar *name = NULL;
	guint i;

	/* An invalid escape is matched as written. */
	name = g_ascii_strdown(decoded != NULL ? decoded : component,
		decoded != NULL ? -1 : (gssize)(end - component));
	for (i = 0; globs[i] != NULL; i++) {
		g_autofree gchar *glob = g_ascii_strdown(globs[i], -1);

		if (*glob != '\0' && g_pattern_match_simple(glob, name))
			return TRUE;
	}
	return FALSE;
}

gchar *
gsurf_hardening_strip_params(const gchar *uri, const gchar * const *globs)
{
	const gchar *hash, *query_end, *question;
	g_autofree gchar *query = NULL;
	g_auto(GStrv) components = NULL;
	g_autoptr(GString) kept = NULL;
	GString *out;
	gboolean removed = FALSE;
	guint i, n_kept = 0;

	if (uri == NULL || globs == NULL || globs[0] == NULL)
		return NULL;

	/* Work on the raw string so everything outside the removed
	 * parameters stays byte-for-byte identical. The query runs from the
	 * first '?' to the fragment's '#'. */
	hash = strchr(uri, '#');
	query_end = hash != NULL ? hash : uri + strlen(uri);
	question = memchr(uri, '?', (gsize)(query_end - uri));
	if (question == NULL)
		return NULL;
	query = g_strndup(question + 1, (gsize)(query_end - question - 1));

	/* Keep the components that do not match, in their original order. */
	components = g_strsplit(query, "&", -1);
	kept = g_string_new(NULL);
	for (i = 0; components[i] != NULL; i++) {
		if (*components[i] != '\0' && param_matches(components[i], globs)) {
			removed = TRUE;
			continue;
		}
		if (n_kept++ > 0)
			g_string_append_c(kept, '&');
		g_string_append(kept, components[i]);
	}
	if (!removed)
		return NULL;

	out = g_string_new_len(uri, question - uri);
	if (kept->len > 0) {
		g_string_append_c(out, '?');
		g_string_append_len(out, kept->str, (gssize)kept->len);
	}
	if (hash != NULL)
		g_string_append(out, hash);
	return g_string_free(out, FALSE);
}

/* --- Referer --- */

gboolean
gsurf_hardening_same_origin(const gchar *a, const gchar *b)
{
	g_autoptr(GUri) ua = parse_uri(a);
	g_autoptr(GUri) ub = parse_uri(b);
	g_autofree gchar *ha = NULL;
	g_autofree gchar *hb = NULL;

	if (ua == NULL || ub == NULL)
		return FALSE;
	if (g_ascii_strcasecmp(g_uri_get_scheme(ua), g_uri_get_scheme(ub)) != 0)
		return FALSE;
	ha = normalized_host(ua);
	hb = normalized_host(ub);
	if (ha == NULL || g_strcmp0(ha, hb) != 0)
		return FALSE;
	return effective_port(ua) == effective_port(ub);
}

gchar *
gsurf_hardening_origin_of(const gchar *uri)
{
	g_autoptr(GUri) parsed = parse_uri(uri);
	g_autofree gchar *scheme = NULL;
	g_autofree gchar *host = NULL;
	GString *out;
	gint port;

	if (parsed == NULL || (host = normalized_host(parsed)) == NULL)
		return NULL;
	scheme = g_ascii_strdown(g_uri_get_scheme(parsed), -1);
	out = g_string_new(scheme);
	g_string_append(out, "://");
	if (strchr(host, ':') != NULL)
		g_string_append_printf(out, "[%s]", host);
	else
		g_string_append(out, host);

	/* Omit the port when it is the scheme's default. */
	port = g_uri_get_port(parsed);
	if (port > 0 && !((port == 80 && g_str_equal(scheme, "http")) ||
	                  (port == 443 && g_str_equal(scheme, "https"))))
		g_string_append_printf(out, ":%d", port);
	g_string_append_c(out, '/');
	return g_string_free(out, FALSE);
}

GsurfHardeningRefererAction
gsurf_hardening_referer_rewrite(GsurfHardeningReferer policy, const gchar *referer,
                                const gchar *target, gchar **replacement)
{
	g_autofree gchar *origin = NULL;

	if (replacement != NULL)
		*replacement = NULL;

	/* Nothing to do: policy off, no header, or a same-origin request. */
	if (policy == GSURF_HARDENING_REFERER_OFF || referer == NULL || *referer == '\0')
		return GSURF_HARDENING_REFERER_KEEP;
	if (gsurf_hardening_same_origin(referer, target))
		return GSURF_HARDENING_REFERER_KEEP;

	/* Cross-origin from here on. */
	if (policy == GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN)
		return GSURF_HARDENING_REFERER_REMOVE;
	origin = gsurf_hardening_origin_of(referer);
	if (origin == NULL)
		return GSURF_HARDENING_REFERER_REMOVE;
	if (g_str_equal(origin, referer))
		return GSURF_HARDENING_REFERER_KEEP;
	if (replacement != NULL)
		*replacement = g_steal_pointer(&origin);
	return GSURF_HARDENING_REFERER_REPLACE;
}

/* --- speculative requests --- */

gboolean
gsurf_hardening_is_speculative(const gchar *purpose, const gchar *sec_purpose,
                               const gchar *ping_to, const gchar *content_type)
{
	g_autofree gchar *p = purpose != NULL ? g_ascii_strdown(purpose, -1) : NULL;
	g_autofree gchar *sp = sec_purpose != NULL ? g_ascii_strdown(sec_purpose, -1) : NULL;
	g_autofree gchar *ct = content_type != NULL ? g_ascii_strdown(content_type, -1) : NULL;

	if (p != NULL && strstr(p, "prefetch") != NULL)
		return TRUE;
	if (sp != NULL && g_str_has_prefix(g_strstrip(sp), "prefetch"))
		return TRUE;
	if (ping_to != NULL)
		return TRUE;
	return ct != NULL && g_str_has_prefix(g_strstrip(ct), "text/ping");
}

/* --- exemptions --- */

typedef struct {
	GRegex *regex;
	guint   mask;
} ExemptRule;

struct _GsurfHardeningExemptions {
	GArray *rules;   /* ExemptRule */
};

/* Sort one `skip:` name into the mask or one of the report lists. */
static guint
skip_name(const gchar *name, GPtrArray *unknown, GPtrArray *session_wide)
{
	guint feature = gsurf_hardening_feature_from_name(name);

	if (feature != 0)
		return feature;
	if (g_strv_contains(session_wide_names, name)) {
		if (session_wide != NULL)
			g_ptr_array_add(session_wide, g_strdup(name));
	} else if (unknown != NULL) {
		g_ptr_array_add(unknown, g_strdup(name));
	}
	return 0;
}

guint
gsurf_hardening_skip_mask(GVariant *skip, GPtrArray *unknown, GPtrArray *session_wide)
{
	GVariantIter it;
	const gchar *name;
	guint mask = 0;

	/* No `skip:` at all: the rule is a full site exception. */
	if (skip == NULL)
		return GSURF_HARDENING_ALL;

	if (g_variant_is_of_type(skip, G_VARIANT_TYPE_STRING)) {
		/* `skip: https_only` (one name) or `skip:` (YAML null → ""). */
		name = g_variant_get_string(skip, NULL);
		return *name != '\0' ? skip_name(name, unknown, session_wide) : 0;
	}
	if (g_variant_is_of_type(skip, G_VARIANT_TYPE_STRING_ARRAY)) {
		g_variant_iter_init(&it, skip);
		while (g_variant_iter_next(&it, "&s", &name))
			mask |= skip_name(name, unknown, session_wide);
		return mask;
	}

	/* A number, a boolean or a nested structure is not a name list. */
	if (unknown != NULL)
		g_ptr_array_add(unknown, g_variant_print(skip, FALSE));
	return 0;
}

static void
exempt_rule_clear(gpointer data)
{
	ExemptRule *rule = data;

	g_clear_pointer(&rule->regex, g_regex_unref);
}

GsurfHardeningExemptions *
gsurf_hardening_exemptions_new(GVariant *rules)
{
	GsurfHardeningExemptions *self = g_new0(GsurfHardeningExemptions, 1);
	GVariantIter it;
	const gchar *pattern;
	GVariant *data;

	self->rules = g_array_new(FALSE, TRUE, sizeof(ExemptRule));
	g_array_set_clear_func(self->rules, exempt_rule_clear);
	if (rules == NULL || !g_variant_is_of_type(rules, G_VARIANT_TYPE("a(sa{sv})")))
		return self;

	/* Same regex flags as GsurfHostRules: caseless, unanchored. */
	g_variant_iter_init(&it, rules);
	while (g_variant_iter_next(&it, "(&s@a{sv})", &pattern, &data)) {
		g_autoptr(GVariant) skip = g_variant_lookup_value(data, "skip", NULL);
		ExemptRule rule = { 0 };

		rule.regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
		if (rule.regex != NULL) {
			rule.mask = gsurf_hardening_skip_mask(skip, NULL, NULL);
			g_array_append_val(self->rules, rule);
		}
		g_variant_unref(data);
	}
	return self;
}

void
gsurf_hardening_exemptions_free(GsurfHardeningExemptions *self)
{
	if (self == NULL)
		return;
	g_array_unref(self->rules);
	g_free(self);
}

guint
gsurf_hardening_exemptions_get_size(GsurfHardeningExemptions *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->rules->len;
}

guint
gsurf_hardening_exemptions_lookup(GsurfHardeningExemptions *self, const gchar *host)
{
	guint i, mask = 0;

	g_return_val_if_fail(self != NULL, 0);
	if (host == NULL || *host == '\0')
		return 0;

	/* Every matching rule contributes: the union is what gets skipped. */
	for (i = 0; i < self->rules->len; i++) {
		ExemptRule *rule = &g_array_index(self->rules, ExemptRule, i);

		if (g_regex_match(rule->regex, host, 0, NULL))
			mask |= rule->mask;
	}
	return mask;
}

/* --- policy --- */

GsurfHardeningPolicy *
gsurf_hardening_policy_new(void)
{
	GsurfHardeningPolicy *self = g_new0(GsurfHardeningPolicy, 1);

	self->strip_params = g_new0(gchar *, 1);
	self->referer = GSURF_HARDENING_REFERER_OFF;
	self->exempt = gsurf_hardening_exemptions_new(NULL);
	return self;
}

void
gsurf_hardening_policy_free(GsurfHardeningPolicy *self)
{
	if (self == NULL)
		return;
	g_strfreev(self->strip_params);
	g_clear_pointer(&self->exempt_rules, g_variant_unref);
	gsurf_hardening_exemptions_free(self->exempt);
	g_free(self);
}

void
gsurf_hardening_policy_set_exemptions(GsurfHardeningPolicy *self, GVariant *rules)
{
	g_return_if_fail(self != NULL);

	if (rules != NULL)
		g_variant_ref_sink(rules);
	g_clear_pointer(&self->exempt_rules, g_variant_unref);
	self->exempt_rules = rules;
	gsurf_hardening_exemptions_free(self->exempt);
	self->exempt = gsurf_hardening_exemptions_new(rules);
}

GVariant *
gsurf_hardening_policy_to_variant(GsurfHardeningPolicy *self)
{
	GVariantDict dict;

	g_return_val_if_fail(self != NULL, NULL);

	g_variant_dict_init(&dict, NULL);
	g_variant_dict_insert(&dict, "https-only", "b", self->https_only);
	g_variant_dict_insert(&dict, "https-only-local", "b", self->https_only_local);
	g_variant_dict_insert_value(&dict, "strip-params",
		g_variant_new_strv((const gchar * const *)self->strip_params, -1));
	g_variant_dict_insert(&dict, "referer", "s", gsurf_hardening_referer_to_name(self->referer));
	g_variant_dict_insert(&dict, "gpc", "b", self->gpc);
	g_variant_dict_insert(&dict, "dnt", "b", self->dnt);
	g_variant_dict_insert(&dict, "no-prefetch", "b", self->no_prefetch);
	g_variant_dict_insert(&dict, "webrtc-off", "b", self->webrtc_off);
	g_variant_dict_insert_value(&dict, "exempt", self->exempt_rules != NULL
		? self->exempt_rules
		: g_variant_new_array(G_VARIANT_TYPE("(sa{sv})"), NULL, 0));
	return g_variant_dict_end(&dict);
}

GsurfHardeningPolicy *
gsurf_hardening_policy_new_from_variant(GVariant *dict)
{
	GsurfHardeningPolicy *self = gsurf_hardening_policy_new();
	g_autoptr(GVariant) exempt = NULL;
	g_auto(GStrv) params = NULL;
	const gchar *referer = NULL;

	if (dict == NULL || !g_variant_is_of_type(dict, G_VARIANT_TYPE_VARDICT))
		return self;

	/* Each lookup leaves the default (off) on a missing or mistyped key. */
	g_variant_lookup(dict, "https-only", "b", &self->https_only);
	g_variant_lookup(dict, "https-only-local", "b", &self->https_only_local);
	g_variant_lookup(dict, "gpc", "b", &self->gpc);
	g_variant_lookup(dict, "dnt", "b", &self->dnt);
	g_variant_lookup(dict, "no-prefetch", "b", &self->no_prefetch);
	g_variant_lookup(dict, "webrtc-off", "b", &self->webrtc_off);
	if (g_variant_lookup(dict, "referer", "&s", &referer))
		gsurf_hardening_referer_from_name(referer, &self->referer);
	if (g_variant_lookup(dict, "strip-params", "^as", &params)) {
		g_strfreev(self->strip_params);
		self->strip_params = g_steal_pointer(&params);
	}
	exempt = g_variant_lookup_value(dict, "exempt", G_VARIANT_TYPE("a(sa{sv})"));
	if (exempt != NULL)
		gsurf_hardening_policy_set_exemptions(self, g_steal_pointer(&exempt));
	return self;
}

guint
gsurf_hardening_policy_get_features(GsurfHardeningPolicy *self)
{
	guint features = 0;

	g_return_val_if_fail(self != NULL, 0);
	if (self->https_only)
		features |= GSURF_HARDENING_HTTPS_ONLY;
	if (self->strip_params != NULL && self->strip_params[0] != NULL)
		features |= GSURF_HARDENING_STRIP_PARAMS;
	if (self->referer != GSURF_HARDENING_REFERER_OFF)
		features |= GSURF_HARDENING_REFERER;
	if (self->gpc)
		features |= GSURF_HARDENING_GPC;
	if (self->dnt)
		features |= GSURF_HARDENING_DNT;
	if (self->no_prefetch)
		features |= GSURF_HARDENING_NO_PREFETCH;
	if (self->webrtc_off)
		features |= GSURF_HARDENING_WEBRTC;
	return features;
}

guint
gsurf_hardening_policy_features_for_host(GsurfHardeningPolicy *self, const gchar *top_host)
{
	g_return_val_if_fail(self != NULL, 0);
	return gsurf_hardening_policy_get_features(self) &
		~gsurf_hardening_exemptions_lookup(self->exempt, top_host);
}
