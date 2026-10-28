/*
 * gsurf-adblock-filters.c - uBlock Origin / Adblock Plus filter compiler
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The compiler works in two passes. Parsing (add_text/add_file) classifies
 * every line and stores it in an intermediate form; exceptions are only
 * recorded. compile() then applies `$badfilter` and the cosmetic/scriptlet
 * exceptions and renders each category for its enforcement point (see the
 * header). Doing exceptions at compile time keeps the result independent
 * of the order lists were added in.
 */

#include "gsurf-adblock-filters.h"
#include <string.h>

/* `!#include` nesting limit (uBO's lists nest one level deep). */
#define MAX_INCLUDE_DEPTH 4

/* uBO resource-type names that make up "every sub-resource": the default
 * type set of a filter without type options. Documents are added back as
 * frames only, so a generic pattern never blocks a top-level navigation. */
#define DEFAULT_TYPES (GSURF_ADBLOCK_TYPE_ALL & ~(GSURF_ADBLOCK_TYPE_DOCUMENT | GSURF_ADBLOCK_TYPE_POPUP))

/* Bits for `@@...$generichide` / `$specifichide` / `$elemhide`. */
#define COSMETIC_OFF_GENERIC  (1u << 0)
#define COSMETIC_OFF_SPECIFIC (1u << 1)

typedef enum {
	RUNTIME_NONE,
	RUNTIME_REDIRECT,
	RUNTIME_REMOVEPARAM
} RuntimeKind;

typedef enum {
	COSMETIC_HIDE,        /* plain selector: native element hiding */
	COSMETIC_PROCEDURAL   /* needs the JS engine (procedural, :style, :remove) */
} CosmeticKind;

/* Page domains a filter applies to. Entities ("example.*") sit in the same
 * arrays as plain domains and are told apart by their ".*" suffix. */
typedef struct {
	GPtrArray *pos;
	GPtrArray *neg;
} DomainSet;

typedef struct {
	gchar            *key;           /* filter text minus options order noise, for $badfilter */
	gchar            *regex;         /* WebKit url-filter (NULL for runtime-only regexes) */
	gchar            *pcre;          /* PCRE for the request matcher */
	gchar            *token;         /* matcher index token */
	gchar            *host;          /* host of a pure "||host^" filter */
	gboolean          exception;
	gboolean          important;
	gboolean          match_case;
	guint             types;         /* explicit type mask, 0 = default */
	GsurfAdblockParty party;
	DomainSet         domains;
	gboolean          doc_exception; /* @@...$document: the whole site */
	guint             cosmetic_off;  /* COSMETIC_OFF_* for @@ filters */
	RuntimeKind       runtime;
	gchar            *runtime_arg;
	gint              priority;
} NetRule;

typedef struct {
	gchar        *selector;
	CosmeticKind  kind;
	DomainSet     domains;
} CosmeticRule;

typedef struct {
	gchar     *fn;      /* JS function implementing the scriptlet */
	gchar     *call;    /* the complete JS call expression */
	gchar     *key;     /* canonical name + arguments, for exceptions */
	DomainSet  domains;
} ScriptletRule;

struct _GsurfAdblockCompiler
{
	GPtrArray  *net;                  /* NetRule* */
	GHashTable *badfilters;           /* key -> 1 */
	GPtrArray  *cosmetic;             /* CosmeticRule* */
	GHashTable *cosmetic_exceptions;  /* selector -> GPtrArray of domains ("" = everywhere) */
	GPtrArray  *scriptlets;           /* ScriptletRule* */
	GHashTable *scriptlet_exceptions; /* key ("" = all) -> GPtrArray of domains */
	GHashTable *allowed_sites;        /* host -> 1 */

	/* Scriptlet library, parsed from js/scriptlets.js. */
	gchar      *scriptlet_helpers;
	GHashTable *scriptlet_aliases;    /* name/alias -> fn */
	GHashTable *scriptlet_sources;    /* fn -> source */
	GHashTable *scriptlet_trusted;    /* fn -> 1 for trusted-only scriptlets */
	gchar      *cosmetic_engine;

	GsurfAdblockStats stats;
	guint       include_depth;
};

/* --- small helpers --- */

static void
domain_set_init(DomainSet *set)
{
	set->pos = g_ptr_array_new_with_free_func(g_free);
	set->neg = g_ptr_array_new_with_free_func(g_free);
}

static void
domain_set_clear(DomainSet *set)
{
	g_clear_pointer(&set->pos, g_ptr_array_unref);
	g_clear_pointer(&set->neg, g_ptr_array_unref);
}

static gboolean
is_entity(const gchar *domain)
{
	return g_str_has_suffix(domain, ".*");
}

static gboolean
has_entity(GPtrArray *domains)
{
	guint i;

	for (i = 0; i < domains->len; i++)
		if (is_entity(g_ptr_array_index(domains, i)))
			return TRUE;
	return FALSE;
}

static void
net_rule_free(gpointer data)
{
	NetRule *r = data;

	g_free(r->key);
	g_free(r->regex);
	g_free(r->pcre);
	g_free(r->token);
	g_free(r->host);
	g_free(r->runtime_arg);
	domain_set_clear(&r->domains);
	g_free(r);
}

static void
cosmetic_rule_free(gpointer data)
{
	CosmeticRule *r = data;

	g_free(r->selector);
	domain_set_clear(&r->domains);
	g_free(r);
}

static void
scriptlet_rule_free(gpointer data)
{
	ScriptletRule *r = data;

	g_free(r->fn);
	g_free(r->call);
	g_free(r->key);
	domain_set_clear(&r->domains);
	g_free(r);
}

/* Append @s as a JSON string literal (also valid as a JS string literal). */
static void
append_json_string(GString *out, const gchar *s)
{
	const guchar *p;

	g_string_append_c(out, '"');
	for (p = (const guchar *)s; *p != '\0'; p++) {
		switch (*p) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\n': g_string_append(out, "\\n"); break;
		case '\r': g_string_append(out, "\\r"); break;
		case '\t': g_string_append(out, "\\t"); break;
		default:
			if (*p < 0x20)
				g_string_append_printf(out, "\\u%04x", *p);
			/* U+2028/U+2029 are valid JSON but end a JS statement. */
			else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0xa8 || p[2] == 0xa9)) {
				g_string_append_printf(out, "\\u%04x", p[2] == 0xa8 ? 0x2028 : 0x2029);
				p += 2;
			} else
				g_string_append_c(out, (gchar)*p);
		}
	}
	g_string_append_c(out, '"');
}

/* Escape every regex metacharacter in @s. */
static void
append_regex_literal(GString *out, const gchar *s, gsize len)
{
	gsize i;

	for (i = 0; i < len; i++) {
		if (strchr(".+?*()[]{}\\^$|", s[i]) != NULL)
			g_string_append_c(out, '\\');
		g_string_append_c(out, s[i]);
	}
}

/* Normalize one filter domain: lowercase ASCII (IDN to punycode), no
 * trailing dot. Entities keep their ".*". Returns NULL when invalid. */
static gchar *
normalize_domain(const gchar *domain)
{
	g_autofree gchar *stem = NULL;
	gchar *ascii, *p;
	gboolean entity;
	gsize len;

	if (domain == NULL || *domain == '\0')
		return NULL;
	entity = is_entity(domain);
	stem = entity ? g_strndup(domain, strlen(domain) - 2) : g_strdup(domain);
	if (*stem == '\0' || *stem == '.' || strchr(stem, '*') != NULL)
		return NULL;
	ascii = g_hostname_to_ascii(stem);
	if (ascii == NULL)
		return NULL;
	len = strlen(ascii);
	if (len > 0 && ascii[len - 1] == '.')
		ascii[--len] = '\0';
	for (p = ascii; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '.' && *p != '_') {
			g_free(ascii);
			return NULL;
		}
		*p = g_ascii_tolower(*p);
	}
	if (len == 0) {
		g_free(ascii);
		return NULL;
	}
	if (entity) {
		gchar *with = g_strconcat(ascii, ".*", NULL);

		g_free(ascii);
		return with;
	}
	return ascii;
}

/* Parse "a.com|~b.a.com|c.*" (or comma-separated). Returns FALSE when the
 * list names domains but none of its positive entries is usable, so the
 * filter must be dropped rather than widened to every site. */
static gboolean
parse_domain_list(const gchar *list, gchar separator, DomainSet *set)
{
	gchar sep[2] = { separator, '\0' };
	g_auto(GStrv) parts = g_strsplit(list, sep, -1);
	gboolean wanted_pos = FALSE;
	guint i;

	for (i = 0; parts[i] != NULL; i++) {
		gchar *item = g_strstrip(parts[i]);
		gboolean negated = (*item == '~');
		gchar *domain;

		if (*item == '\0')
			continue;
		if (negated)
			item++;
		else
			wanted_pos = TRUE;
		/* uBO also accepts /regex/ domains; WebKit cannot express them. */
		if (*item == '/')
			return FALSE;
		domain = normalize_domain(item);
		if (domain == NULL)
			continue;
		g_ptr_array_add(negated ? set->neg : set->pos, domain);
	}
	return !wanted_pos || set->pos->len > 0;
}

/* --- ABP network pattern -> regex --- */

/* Tokens too common to narrow a lookup; a rule indexed by one is checked
 * for nearly every request. */
static gboolean
is_bad_token(const gchar *token, gsize len)
{
	static const gchar *const bad[] = {
		"com", "http", "https", "www", "net", "org", "js", "html", "php", "ads", NULL
	};
	guint i;

	if (len < 2)
		return TRUE;
	for (i = 0; bad[i] != NULL; i++)
		if (strlen(bad[i]) == len && g_ascii_strncasecmp(token, bad[i], len) == 0)
			return TRUE;
	return FALSE;
}

/* Pick the longest alphanumeric run that every matching URL must contain
 * as a whole token: it may not touch a '*' wildcard or an unanchored
 * pattern edge (which could continue into more alphanumerics). */
static gchar *
pick_token(const gchar *body, gboolean start_anchored, gboolean end_anchored)
{
	const gchar *p = body, *best = NULL;
	gsize best_len = 0;
	gboolean best_bad = TRUE;

	while (*p != '\0') {
		const gchar *start;
		gsize len;
		gboolean left_ok, right_ok, bad;

		if (!g_ascii_isalnum(*p)) {
			p++;
			continue;
		}
		start = p;
		while (g_ascii_isalnum(*p))
			p++;
		len = p - start;
		left_ok = start == body ? start_anchored : start[-1] != '*';
		right_ok = *p == '\0' ? end_anchored : *p != '*';
		if (!left_ok || !right_ok)
			continue;
		bad = is_bad_token(start, len);
		if (best == NULL || (best_bad && !bad) || (bad == best_bad && len > best_len)) {
			best = start;
			best_len = len;
			best_bad = bad;
		}
	}
	return best != NULL ? g_ascii_strdown(best, best_len) : g_strdup("");
}

gchar *
gsurf_adblock_pattern_to_regex(const gchar *pattern, gchar **token)
{
	g_autofree gchar *body = NULL;
	GString *out;
	gboolean host_anchor = FALSE, start_anchor = FALSE, end_anchor = FALSE;
	gboolean in_host;
	const gchar *p;
	gsize len;

	if (token != NULL)
		*token = NULL;
	if (pattern == NULL)
		return NULL;

	/* Strip the anchors and the wildcards that mean nothing at the edges. */
	if (g_str_has_prefix(pattern, "||")) {
		host_anchor = TRUE;
		pattern += 2;
	} else if (*pattern == '|') {
		start_anchor = TRUE;
		pattern++;
	}
	body = g_strdup(pattern);
	len = strlen(body);
	if (len > 0 && body[len - 1] == '|') {
		end_anchor = TRUE;
		body[--len] = '\0';
	}
	while (*body == '*' && !host_anchor && !start_anchor)
		memmove(body, body + 1, len--);
	while (len > 0 && body[len - 1] == '*' && !end_anchor)
		body[--len] = '\0';

	for (p = body; *p != '\0'; p++)
		if ((guchar)*p >= 0x80 || (guchar)*p < 0x20)
			return NULL;

	if (token != NULL)
		*token = pick_token(body, host_anchor || start_anchor,
			end_anchor || (len > 0 && body[len - 1] == '^'));

	out = g_string_new(NULL);
	if (host_anchor)
		g_string_append(out, "^[^:]+://([^/]+\\.)?");
	else if (start_anchor)
		g_string_append_c(out, '^');
	if (len == 0 && !host_anchor && !start_anchor && !end_anchor) {
		g_string_append(out, ".*");
		return g_string_free(out, FALSE);
	}

	/* While a "||" pattern is still inside the hostname, a separator can
	 * only be the ':' or '/' that ends the authority. */
	in_host = host_anchor;
	for (p = body; *p != '\0'; p++) {
		switch (*p) {
		case '*':
			in_host = FALSE;
			while (p[1] == '*')
				p++;
			g_string_append(out, ".*");
			break;
		case '^':
			if (in_host)
				g_string_append(out, "[:/]");
			else if (p[1] == '\0' && !end_anchor)
				g_string_append(out, "([^-.%a-zA-Z0-9_].*)?$");
			else
				g_string_append(out, "[^-.%a-zA-Z0-9_]");
			in_host = FALSE;
			break;
		default:
			if (!g_ascii_isalnum(*p) && *p != '.' && *p != '-')
				in_host = FALSE;
			append_regex_literal(out, p, 1);
		}
	}
	if (end_anchor)
		g_string_append_c(out, '$');
	return g_string_free(out, FALSE);
}

/* Translate a /regex/ filter into WebKit's subset, or fail. Shorthand
 * classes become explicit ones; alternation, counted repetition, groups
 * with modifiers and backreferences have no equivalent. */
static gchar *
regex_to_webkit(const gchar *re)
{
	GString *out = g_string_new(NULL);
	gboolean in_class = FALSE;
	gsize i, len = strlen(re);

	for (i = 0; i < len; i++) {
		gchar c = re[i];

		if ((guchar)c >= 0x80)
			goto fail;
		if (c == '\\') {
			gchar n = re[++i];

			if (n == '\0')
				goto fail;
			if (n == 'd')
				g_string_append(out, in_class ? "0-9" : "[0-9]");
			else if (n == 'w')
				g_string_append(out, in_class ? "a-zA-Z0-9_" : "[a-zA-Z0-9_]");
			else if (g_ascii_isalnum(n))
				goto fail;
			else {
				g_string_append_c(out, '\\');
				g_string_append_c(out, n);
			}
			continue;
		}
		if (in_class) {
			if (c == ']')
				in_class = FALSE;
			g_string_append_c(out, c);
			continue;
		}
		switch (c) {
		case '[':
			in_class = TRUE;
			break;
		case '|': case '{': case '}':
			goto fail;
		case '(':
			if (re[i + 1] == '?')
				goto fail;
			break;
		case '^':
			if (i != 0)
				goto fail;
			break;
		case '$':
			if (i != len - 1)
				goto fail;
			break;
		case '?': case '*': case '+':
			/* lazy/possessive quantifiers */
			if (re[i + 1] == '?' || re[i + 1] == '+')
				goto fail;
			break;
		}
		g_string_append_c(out, c);
	}
	if (in_class)
		goto fail;
	return g_string_free(out, FALSE);
fail:
	g_string_free(out, TRUE);
	return NULL;
}

/* --- network filters --- */

/* Map one resource-type option name to its bit. */
static guint
type_from_option(const gchar *name)
{
	static const struct { const gchar *name; guint type; } table[] = {
		{ "script", GSURF_ADBLOCK_TYPE_SCRIPT },
		{ "image", GSURF_ADBLOCK_TYPE_IMAGE },
		{ "stylesheet", GSURF_ADBLOCK_TYPE_STYLESHEET }, { "css", GSURF_ADBLOCK_TYPE_STYLESHEET },
		{ "font", GSURF_ADBLOCK_TYPE_FONT },
		{ "media", GSURF_ADBLOCK_TYPE_MEDIA },
		{ "xmlhttprequest", GSURF_ADBLOCK_TYPE_XHR }, { "xhr", GSURF_ADBLOCK_TYPE_XHR },
		{ "subdocument", GSURF_ADBLOCK_TYPE_SUBDOCUMENT }, { "frame", GSURF_ADBLOCK_TYPE_SUBDOCUMENT },
		{ "document", GSURF_ADBLOCK_TYPE_DOCUMENT }, { "doc", GSURF_ADBLOCK_TYPE_DOCUMENT },
		{ "websocket", GSURF_ADBLOCK_TYPE_WEBSOCKET },
		{ "ping", GSURF_ADBLOCK_TYPE_PING }, { "beacon", GSURF_ADBLOCK_TYPE_PING },
		{ "other", GSURF_ADBLOCK_TYPE_OTHER },
		{ "popup", GSURF_ADBLOCK_TYPE_POPUP }, { "popunder", GSURF_ADBLOCK_TYPE_POPUP },
		{ "object", GSURF_ADBLOCK_TYPE_OBJECT }, { "object-subrequest", GSURF_ADBLOCK_TYPE_OBJECT },
		{ "all", GSURF_ADBLOCK_TYPE_ALL },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(table); i++)
		if (g_str_equal(name, table[i].name))
			return table[i].type;
	return 0;
}

/* Split options on ',' (a '\,' is a literal comma inside a value). */
static GPtrArray *
split_options(const gchar *options)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	GString *cur = g_string_new(NULL);
	const gchar *p;

	for (p = options; *p != '\0'; p++) {
		if (*p == '\\' && p[1] == ',') {
			g_string_append_c(cur, ',');
			p++;
		} else if (*p == ',') {
			g_ptr_array_add(out, g_strdup(g_strstrip(cur->str)));
			g_string_truncate(cur, 0);
		} else {
			g_string_append_c(cur, *p);
		}
	}
	g_ptr_array_add(out, g_strdup(g_strstrip(cur->str)));
	g_string_free(cur, TRUE);
	return out;
}

/* A filter that is just a hostname ("ads.example.com") is uBO shorthand
 * for "||ads.example.com^". */
static gboolean
is_plain_hostname(const gchar *line)
{
	const gchar *p;
	gboolean dot = FALSE, alpha_tld = FALSE;

	if (*line == '\0' || *line == '.' || *line == '-')
		return FALSE;
	for (p = line; *p != '\0'; p++) {
		if (*p == '.') {
			if (p[1] == '\0' || p[1] == '.' || p[1] == '-' || p[-1] == '-')
				return FALSE;
			dot = TRUE;
			alpha_tld = TRUE;
		} else if (!g_ascii_isalnum(*p) && *p != '-') {
			return FALSE;
		} else if (g_ascii_isdigit(*p) && dot) {
			alpha_tld = alpha_tld && strchr(p, '.') != NULL;
		}
	}
	return dot && alpha_tld && p[-1] != '-';
}

/* Where do the options begin? At the last '$' ("$removeparam=x" has an
 * empty pattern), unless the whole filter is a "/regex/" whose own '$' is
 * an anchor. */
static const gchar *
find_options(const gchar *filter)
{
	const gchar *dollar = strrchr(filter, '$');
	gsize len = strlen(filter);

	if (dollar == NULL || dollar[1] == '\0')
		return NULL;
	if (len > 1 && filter[0] == '/' && filter[len - 1] == '/')
		return NULL;
	return dollar;
}

static void
parse_network(GsurfAdblockCompiler *self, const gchar *line)
{
	g_autofree gchar *pattern = NULL;
	g_autoptr(GPtrArray) options = NULL;
	g_autoptr(GString) key = g_string_new(NULL);
	NetRule *rule;
	const gchar *opts;
	gboolean exception = FALSE, badfilter = FALSE, unsupported = FALSE;
	gboolean any_type_option = FALSE, negated_types = FALSE;
	g_autofree gchar *to_list = NULL;
	guint types = 0, not_types = 0;
	guint i;

	if (g_str_has_prefix(line, "@@")) {
		exception = TRUE;
		line += 2;
	}
	opts = find_options(line);
	pattern = opts != NULL ? g_strndup(line, opts - line) : g_strdup(line);
	options = opts != NULL ? split_options(opts + 1) : g_ptr_array_new_with_free_func(g_free);

	rule = g_new0(NetRule, 1);
	domain_set_init(&rule->domains);
	rule->exception = exception;

	/* --- options --- */
	g_string_append(key, exception ? "@@" : "");
	g_string_append(key, pattern);
	for (i = 0; i < options->len; i++) {
		gchar *opt = g_ptr_array_index(options, i);
		gboolean negated = (*opt == '~');
		const gchar *name = negated ? opt + 1 : opt;
		const gchar *value = strchr(name, '=');
		g_autofree gchar *bare = value != NULL ? g_strndup(name, value - name) : g_strdup(name);
		guint type;

		if (value != NULL)
			value++;
		if (*opt == '\0')
			continue;
		if (g_str_equal(bare, "badfilter")) {
			badfilter = TRUE;
			continue;
		}
		g_string_append_c(key, i == 0 ? '$' : ',');
		g_string_append(key, opt);

		if ((type = type_from_option(bare)) != 0 && value == NULL) {
			any_type_option = TRUE;
			if (negated) {
				negated_types = TRUE;
				not_types |= type;
			} else {
				types |= type;
			}
		} else if (g_str_equal(bare, "third-party") || g_str_equal(bare, "3p") ||
		           g_str_equal(bare, "strict3p") || g_str_equal(bare, "strict-third-party")) {
			rule->party = negated ? GSURF_ADBLOCK_PARTY_FIRST : GSURF_ADBLOCK_PARTY_THIRD;
		} else if (g_str_equal(bare, "first-party") || g_str_equal(bare, "1p") ||
		           g_str_equal(bare, "strict1p") || g_str_equal(bare, "strict-first-party")) {
			rule->party = negated ? GSURF_ADBLOCK_PARTY_THIRD : GSURF_ADBLOCK_PARTY_FIRST;
		} else if ((g_str_equal(bare, "domain") || g_str_equal(bare, "from")) && value != NULL) {
			if (!parse_domain_list(value, '|', &rule->domains))
				unsupported = TRUE;
		} else if (g_str_equal(bare, "to") && value != NULL) {
			/* Request-domain restriction; expanded below. */
			g_free(to_list);
			to_list = g_strdup(value);
		} else if (g_str_equal(bare, "match-case")) {
			rule->match_case = TRUE;
		} else if (g_str_equal(bare, "important")) {
			rule->important = TRUE;
		} else if ((g_str_equal(bare, "redirect") || g_str_equal(bare, "redirect-rule") ||
		            g_str_equal(bare, "rewrite")) && (value != NULL || exception)) {
			g_autofree gchar *res = g_strdup(value != NULL ? value : "");
			gchar *colon = strrchr(res, ':');

			/* "name:priority"; the colon in "abp-resource:name" is part of
			 * the name, so only an all-digit suffix is a priority. */
			if (colon != NULL && colon[1] != '\0' &&
			    strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
				rule->priority = (gint)g_ascii_strtoll(colon + 1, NULL, 10);
				*colon = '\0';
			}
			rule->runtime = RUNTIME_REDIRECT;
			g_free(rule->runtime_arg);
			rule->runtime_arg = g_steal_pointer(&res);
		} else if (g_str_equal(bare, "empty") || g_str_equal(bare, "mp4")) {
			rule->runtime = RUNTIME_REDIRECT;
			g_free(rule->runtime_arg);
			rule->runtime_arg = g_strdup(g_str_equal(bare, "mp4") ? "noopmp4-1s" : "empty");
		} else if (g_str_equal(bare, "removeparam") || g_str_equal(bare, "queryprune")) {
			rule->runtime = RUNTIME_REMOVEPARAM;
			g_free(rule->runtime_arg);
			rule->runtime_arg = g_strdup(value != NULL ? value : "");
		} else if (g_str_equal(bare, "generichide") || g_str_equal(bare, "ghide")) {
			rule->cosmetic_off |= COSMETIC_OFF_GENERIC;
		} else if (g_str_equal(bare, "specifichide") || g_str_equal(bare, "shide")) {
			rule->cosmetic_off |= COSMETIC_OFF_SPECIFIC;
		} else if (g_str_equal(bare, "elemhide") || g_str_equal(bare, "ehide")) {
			rule->cosmetic_off |= COSMETIC_OFF_GENERIC | COSMETIC_OFF_SPECIFIC;
		} else if (g_str_equal(bare, "genericblock") || g_str_equal(bare, "_") ||
		           g_str_has_prefix(bare, "__") || g_str_equal(bare, "reason") ||
		           g_str_equal(bare, "webrtc")) {
			/* informational / no-op here */
		} else {
			/* csp, header, permissions, replace, urltransform, urlskip,
			 * method, denyallow, ipaddress, cname, inline-script, ... */
			unsupported = TRUE;
		}
	}
	if (badfilter) {
		g_hash_table_add(self->badfilters, g_string_free(g_steal_pointer(&key), FALSE));
		net_rule_free(rule);
		return;
	}
	if (unsupported) {
		self->stats.unsupported++;
		net_rule_free(rule);
		return;
	}
	if (any_type_option)
		rule->types = negated_types ? ((types != 0 ? types : DEFAULT_TYPES | GSURF_ADBLOCK_TYPE_DOCUMENT) & ~not_types)
		                            : types;
	if (rule->types & GSURF_ADBLOCK_TYPE_DOCUMENT && exception) {
		/* "@@...$document": nothing at all is filtered on matching pages. */
		rule->doc_exception = TRUE;
	}

	/* "$to=a.com|b.com" on an empty pattern becomes one "||a.com^" rule
	 * per positive domain; anything else is not expressible. */
	if (to_list != NULL) {
		DomainSet to;

		domain_set_init(&to);
		if ((*pattern != '\0' && !g_str_equal(pattern, "*")) ||
		    !parse_domain_list(to_list, '|', &to) || to.neg->len > 0 ||
		    to.pos->len == 0 || has_entity(to.pos)) {
			domain_set_clear(&to);
			self->stats.unsupported++;
			net_rule_free(rule);
			return;
		}
		g_free(pattern);
		pattern = g_strdup_printf("||%s^", (const gchar *)g_ptr_array_index(to.pos, 0));
		for (i = 1; i < to.pos->len; i++) {
			g_autofree gchar *line_copy = NULL;
			g_autoptr(GString) rebuilt = g_string_new(exception ? "@@" : "");

			g_string_append_printf(rebuilt, "||%s^", (const gchar *)g_ptr_array_index(to.pos, i));
			if (opts != NULL) {
				/* re-parse the sibling with the same options minus to= */
				guint j;
				gboolean first = TRUE;

				for (j = 0; j < options->len; j++) {
					const gchar *o = g_ptr_array_index(options, j);

					if (g_str_has_prefix(o, "to=") || *o == '\0')
						continue;
					g_string_append_c(rebuilt, first ? '$' : ',');
					g_string_append(rebuilt, o);
					first = FALSE;
				}
			}
			parse_network(self, rebuilt->str);
		}
		domain_set_clear(&to);
	}

	/* --- pattern --- */
	{
		gsize plen = strlen(pattern);

		if (plen >= 2 && pattern[0] == '/' && pattern[plen - 1] == '/') {
			g_autofree gchar *raw = g_strndup(pattern + 1, plen - 2);

			rule->pcre = g_strdup(raw);
			rule->regex = regex_to_webkit(raw);
			rule->token = g_strdup("");
			if (rule->regex == NULL && rule->runtime == RUNTIME_NONE) {
				self->stats.unsupported++;
				net_rule_free(rule);
				return;
			}
		} else {
			rule->regex = gsurf_adblock_pattern_to_regex(pattern, &rule->token);
			if (rule->regex == NULL) {
				self->stats.invalid++;
				net_rule_free(rule);
				return;
			}
			rule->pcre = g_strdup(rule->regex);
			/* Remember the host of a pure "||host^" filter. */
			if (g_str_has_prefix(pattern, "||") && g_str_has_suffix(pattern, "^")) {
				g_autofree gchar *host = g_strndup(pattern + 2, plen - 3);

				if (is_plain_hostname(host))
					rule->host = g_ascii_strdown(host, -1);
			}
		}
	}

	rule->key = g_string_free(g_steal_pointer(&key), FALSE);
	g_ptr_array_add(self->net, rule);
}

/* --- cosmetic filters --- */

/* Procedural operators that need the JS engine. */
static gboolean
needs_engine(const gchar *selector)
{
	static const gchar *const ops[] = {
		":has-text(", ":-abp-contains(", ":contains(", ":upward(", ":xpath(",
		":matches-css(", ":matches-css-before(", ":matches-css-after(",
		":min-text-length(", ":matches-attr(", ":matches-path(", ":matches-prop(",
		":matches-media(", ":watch-attr(", ":others(", ":remove()", ":remove-attr(",
		":remove-class(", ":style(", ":nth-ancestor(", ":if(", ":if-not(", ":shadow(",
		NULL
	};
	guint i;

	for (i = 0; ops[i] != NULL; i++)
		if (strstr(selector, ops[i]) != NULL)
			return TRUE;
	return FALSE;
}

/* ":-abp-has(" is ABP's spelling of the native ":has(". */
static gchar *
normalize_selector(const gchar *selector)
{
	g_autoptr(GString) s = g_string_new(selector);

	g_string_replace(s, ":-abp-has(", ":has(", 0);
	g_strstrip(s->str);
	g_string_set_size(s, strlen(s->str));
	return g_string_free(g_steal_pointer(&s), FALSE);
}

static void
add_exception(GHashTable *table, const gchar *key, DomainSet *domains)
{
	GPtrArray *list = g_hash_table_lookup(table, key);
	guint i;

	if (list == NULL) {
		list = g_ptr_array_new_with_free_func(g_free);
		g_hash_table_insert(table, g_strdup(key), list);
	}
	if (domains->pos->len == 0) {
		g_ptr_array_add(list, g_strdup(""));
		return;
	}
	for (i = 0; i < domains->pos->len; i++)
		g_ptr_array_add(list, g_strdup(g_ptr_array_index(domains->pos, i)));
}

static void
parse_cosmetic(GsurfAdblockCompiler *self, const gchar *domains_text, const gchar *selector_text,
               gboolean exception)
{
	DomainSet domains;
	g_autofree gchar *selector = NULL;
	CosmeticRule *rule;

	domain_set_init(&domains);
	if (*domains_text != '\0' && !parse_domain_list(domains_text, ',', &domains)) {
		self->stats.unsupported++;
		domain_set_clear(&domains);
		return;
	}
	selector = normalize_selector(selector_text);
	if (*selector == '\0') {
		self->stats.invalid++;
		domain_set_clear(&domains);
		return;
	}
	if (exception) {
		add_exception(self->cosmetic_exceptions, selector, &domains);
		domain_set_clear(&domains);
		return;
	}

	/* Operators the engine does not implement. */
	if (strstr(selector, ":others(") != NULL || strstr(selector, ":shadow(") != NULL ||
	    strstr(selector, ":matches-prop(") != NULL) {
		self->stats.unsupported++;
		domain_set_clear(&domains);
		return;
	}

	rule = g_new0(CosmeticRule, 1);
	rule->selector = g_steal_pointer(&selector);
	rule->kind = needs_engine(rule->selector) ? COSMETIC_PROCEDURAL : COSMETIC_HIDE;
	rule->domains = domains;
	/* uBO refuses generic procedural filters: running them on every page
	 * would cost every page. */
	if (rule->kind == COSMETIC_PROCEDURAL && rule->domains.pos->len == 0) {
		self->stats.unsupported++;
		cosmetic_rule_free(rule);
		return;
	}
	g_ptr_array_add(self->cosmetic, rule);
}

/* --- scriptlets --- */

/* Split "+js(name, arg, 'quoted, arg', a\, b)" arguments. */
static GPtrArray *
split_scriptlet_args(const gchar *inner)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	const gchar *p = inner;

	while (TRUE) {
		GString *arg = g_string_new(NULL);

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\'' || *p == '"' || *p == '`') {
			gchar quote = *p++;

			while (*p != '\0' && *p != quote) {
				if (*p == '\\' && p[1] == quote)
					p++;
				g_string_append_c(arg, *p++);
			}
			if (*p == quote)
				p++;
			while (*p != '\0' && *p != ',')
				p++;
		} else {
			while (*p != '\0' && *p != ',') {
				if (*p == '\\' && p[1] == ',')
					p++;
				g_string_append_c(arg, *p++);
			}
			while (arg->len > 0 && (arg->str[arg->len - 1] == ' ' || arg->str[arg->len - 1] == '\t'))
				g_string_truncate(arg, arg->len - 1);
		}
		g_ptr_array_add(out, g_string_free(arg, FALSE));
		if (*p != ',')
			break;
		p++;
	}
	return out;
}

/* Canonical scriptlet name: no "ubo-" prefix, no ".js" suffix. */
static gchar *
scriptlet_name(const gchar *raw)
{
	const gchar *name = raw;
	gsize len;

	if (g_str_has_prefix(name, "ubo-"))
		name += 4;
	len = strlen(name);
	if (len > 3 && g_str_has_suffix(name, ".js"))
		len -= 3;
	return g_strndup(name, len);
}

static void
add_scriptlet(GsurfAdblockCompiler *self, DomainSet *domains, GPtrArray *args,
              gboolean exception, gboolean trusted)
{
	g_autofree gchar *name = NULL;
	g_autoptr(GString) key = NULL;
	g_autoptr(GString) call = NULL;
	ScriptletRule *rule;
	const gchar *fn;
	guint i;

	/* "#@#+js()" disables every scriptlet on the listed sites. */
	if (exception && (args->len == 0 || (args->len == 1 && *(gchar *)args->pdata[0] == '\0'))) {
		add_exception(self->scriptlet_exceptions, "", domains);
		return;
	}
	name = scriptlet_name(g_ptr_array_index(args, 0));
	fn = self->scriptlet_aliases != NULL ? g_hash_table_lookup(self->scriptlet_aliases, name) : NULL;
	if (fn == NULL) {
		if (!exception)
			self->stats.unsupported++;
		return;
	}
	if (g_hash_table_contains(self->scriptlet_trusted, fn) && !trusted && !exception) {
		self->stats.unsupported++;
		return;
	}

	key = g_string_new(fn);
	call = g_string_new(fn);
	g_string_append_c(call, '(');
	for (i = 1; i < args->len; i++) {
		g_string_append_c(key, '\x1f');
		g_string_append(key, g_ptr_array_index(args, i));
		if (i > 1)
			g_string_append_c(call, ',');
		append_json_string(call, g_ptr_array_index(args, i));
	}
	g_string_append_c(call, ')');

	if (exception) {
		add_exception(self->scriptlet_exceptions, key->str, domains);
		return;
	}
	rule = g_new0(ScriptletRule, 1);
	rule->fn = g_strdup(fn);
	rule->call = g_string_free(g_steal_pointer(&call), FALSE);
	rule->key = g_string_free(g_steal_pointer(&key), FALSE);
	domain_set_init(&rule->domains);
	for (i = 0; i < domains->pos->len; i++)
		g_ptr_array_add(rule->domains.pos, g_strdup(g_ptr_array_index(domains->pos, i)));
	for (i = 0; i < domains->neg->len; i++)
		g_ptr_array_add(rule->domains.neg, g_strdup(g_ptr_array_index(domains->neg, i)));
	g_ptr_array_add(self->scriptlets, rule);
}

static void
parse_scriptlet(GsurfAdblockCompiler *self, const gchar *domains_text, const gchar *body,
                gboolean exception, gboolean trusted)
{
	DomainSet domains;
	g_autofree gchar *inner = NULL;
	g_autoptr(GPtrArray) args = NULL;
	const gchar *close = strrchr(body, ')');

	if (!g_str_has_prefix(body, "+js(") || close == NULL) {
		self->stats.invalid++;
		return;
	}
	domain_set_init(&domains);
	if (*domains_text != '\0' && !parse_domain_list(domains_text, ',', &domains)) {
		self->stats.unsupported++;
		domain_set_clear(&domains);
		return;
	}
	inner = g_strndup(body + 4, close - body - 4);
	args = *g_strstrip(inner) == '\0' ? g_ptr_array_new_with_free_func(g_free)
	                                  : split_scriptlet_args(inner);
	add_scriptlet(self, &domains, args, exception, trusted);
	domain_set_clear(&domains);
}

/* ABP snippets ("#$#abort-on-property-read foo; json-prune bar") map onto
 * the scriptlets of the same behaviour; the library lists ABP names as
 * aliases. */
static void
parse_abp_snippets(GsurfAdblockCompiler *self, const gchar *domains_text, const gchar *body,
                   gboolean exception)
{
	g_auto(GStrv) snippets = g_strsplit(body, ";", -1);
	DomainSet domains;
	guint i;

	domain_set_init(&domains);
	if (*domains_text == '\0' || !parse_domain_list(domains_text, ',', &domains)) {
		self->stats.unsupported++;
		domain_set_clear(&domains);
		return;
	}
	for (i = 0; snippets[i] != NULL; i++) {
		g_auto(GStrv) argv = NULL;
		g_autoptr(GPtrArray) args = NULL;
		guint j;

		if (*g_strstrip(snippets[i]) == '\0')
			continue;
		if (!g_shell_parse_argv(snippets[i], NULL, &argv, NULL)) {
			self->stats.invalid++;
			continue;
		}
		args = g_ptr_array_new_with_free_func(g_free);
		for (j = 0; argv[j] != NULL; j++)
			g_ptr_array_add(args, g_strdup(argv[j]));
		add_scriptlet(self, &domains, args, exception, FALSE);
	}
	domain_set_clear(&domains);
}

/* --- line dispatch --- */

/* Find a cosmetic separator ("##", "#@#", "#?#", "#$#", ...). Returns its
 * position and length, or NULL. The text before it must look like a domain
 * list, otherwise the '#' belongs to a network pattern. */
static const gchar *
find_cosmetic_separator(const gchar *line, gsize *sep_len)
{
	static const gchar *const seps[] = {
		"#@$?#", "#@?#", "#@$#", "#@%#", "#$?#", "#@#", "#?#", "#$#", "#%#", "##", NULL
	};
	const gchar *p;
	guint i;

	for (p = strchr(line, '#'); p != NULL; p = strchr(p + 1, '#')) {
		for (i = 0; seps[i] != NULL; i++) {
			gsize len = strlen(seps[i]);

			if (strncmp(p, seps[i], len) == 0) {
				const gchar *d;

				for (d = line; d < p; d++)
					if (strchr("/|^$=&?:", *d) != NULL)
						return NULL;
				*sep_len = len;
				return p;
			}
		}
	}
	return NULL;
}

static void
parse_cosmetic_line(GsurfAdblockCompiler *self, const gchar *line, const gchar *sep,
                    gsize sep_len, gboolean trusted)
{
	g_autofree gchar *domains = g_strndup(line, sep - line);
	g_autofree gchar *kind = g_strndup(sep, sep_len);
	const gchar *body = sep + sep_len;
	gboolean exception = strchr(kind, '@') != NULL;

	/* AdGuard JS (#%#) and HTML filtering (##^...) are out of reach. */
	if (strchr(kind, '%') != NULL || *body == '^' || *body == '\0') {
		self->stats.unsupported++;
		return;
	}
	if (strchr(kind, '$') != NULL) {
		const gchar *brace = strchr(body, '{');

		/* AdGuard CSS injection "selector { declarations }". */
		if (brace != NULL && g_str_has_suffix(body, "}")) {
			g_autofree gchar *sel = g_strndup(body, brace - body);
			g_autofree gchar *decl = g_strndup(brace + 1, strlen(brace) - 2);
			g_autofree gchar *styled = g_strdup_printf("%s:style(%s)",
				g_strstrip(sel), g_strstrip(decl));

			parse_cosmetic(self, domains, styled, exception);
			return;
		}
		parse_abp_snippets(self, domains, body, exception);
		return;
	}
	if (g_str_has_prefix(body, "+js(")) {
		parse_scriptlet(self, domains, body, exception, trusted);
		return;
	}
	parse_cosmetic(self, domains, body, exception);
}

/* --- preprocessor (!#if / !#else / !#endif / !#include) --- */

typedef struct {
	const gchar *p;
} CondParser;

static gboolean cond_expr(CondParser *cp);

static void
cond_skip_space(CondParser *cp)
{
	while (*cp->p == ' ' || *cp->p == '\t')
		cp->p++;
}

/* Which uBO environment tokens hold for gsurf: uBO syntax is understood,
 * user stylesheets exist, and no browser-specific branch applies. */
static gboolean
cond_token_value(const gchar *token)
{
	static const gchar *const truthy[] = {
		"true", "ext_ublock", "cap_user_stylesheet", NULL
	};

	return g_strv_contains(truthy, token);
}

static gboolean
cond_factor(CondParser *cp)
{
	gboolean value;

	cond_skip_space(cp);
	if (*cp->p == '!') {
		cp->p++;
		return !cond_factor(cp);
	}
	if (*cp->p == '(') {
		cp->p++;
		value = cond_expr(cp);
		cond_skip_space(cp);
		if (*cp->p == ')')
			cp->p++;
		return value;
	}
	{
		const gchar *start = cp->p;
		g_autofree gchar *token = NULL;

		while (g_ascii_isalnum(*cp->p) || *cp->p == '_')
			cp->p++;
		if (cp->p == start) {
			if (*cp->p != '\0')
				cp->p++;
			return FALSE;
		}
		token = g_strndup(start, cp->p - start);
		return cond_token_value(token);
	}
}

static gboolean
cond_term(CondParser *cp)
{
	gboolean value = cond_factor(cp);

	cond_skip_space(cp);
	while (g_str_has_prefix(cp->p, "&&")) {
		gboolean rhs;

		cp->p += 2;
		rhs = cond_factor(cp);
		value = value && rhs;
		cond_skip_space(cp);
	}
	return value;
}

static gboolean
cond_expr(CondParser *cp)
{
	gboolean value = cond_term(cp);

	cond_skip_space(cp);
	while (g_str_has_prefix(cp->p, "||")) {
		gboolean rhs;

		cp->p += 2;
		rhs = cond_term(cp);
		value = value || rhs;
		cond_skip_space(cp);
	}
	return value;
}

typedef struct {
	gboolean parent_active;
	gboolean condition;
} CondFrame;

static gboolean
cond_active(GArray *stack)
{
	CondFrame *top;

	if (stack->len == 0)
		return TRUE;
	top = &g_array_index(stack, CondFrame, stack->len - 1);
	return top->parent_active && top->condition;
}

/* Resolve "!#include name" next to the including list. Only plain names
 * in the same directory are followed. */
static void
include_list(GsurfAdblockCompiler *self, const gchar *base_dir, const gchar *name, gboolean trusted)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;

	if (base_dir == NULL || *name == '\0' || strchr(name, '/') != NULL ||
	    g_str_has_prefix(name, ".."))
		return;
	if (self->include_depth >= MAX_INCLUDE_DEPTH) {
		g_warning("gsurf adblock: !#include %s nested too deeply", name);
		return;
	}
	path = g_build_filename(base_dir, name, NULL);
	if (!g_file_get_contents(path, &text, NULL, NULL)) {
		g_message("gsurf adblock: !#include %s: not found next to its list", name);
		return;
	}
	self->include_depth++;
	gsurf_adblock_compiler_add_text(self, text, base_dir, trusted);
	self->include_depth--;
}

void
gsurf_adblock_compiler_add_text(GsurfAdblockCompiler *self, const gchar *text,
                                const gchar *base_dir, gboolean trusted)
{
	g_auto(GStrv) lines = NULL;
	g_autoptr(GArray) stack = g_array_new(FALSE, FALSE, sizeof(CondFrame));
	guint i;

	g_return_if_fail(self != NULL);
	if (text == NULL)
		return;

	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		gchar *line = g_strstrip(lines[i]);
		const gchar *sep;
		gsize sep_len = 0;

		if (*line == '\0')
			continue;

		/* Preprocessor directives are evaluated even inside inactive
		 * blocks so nesting stays balanced. */
		if (g_str_has_prefix(line, "!#if ")) {
			CondFrame frame = { cond_active(stack), FALSE };
			CondParser cp = { line + 5 };

			frame.condition = cond_expr(&cp);
			g_array_append_val(stack, frame);
			continue;
		}
		if (g_str_has_prefix(line, "!#else")) {
			if (stack->len > 0) {
				CondFrame *top = &g_array_index(stack, CondFrame, stack->len - 1);
				top->condition = !top->condition;
			}
			continue;
		}
		if (g_str_has_prefix(line, "!#endif")) {
			if (stack->len > 0)
				g_array_set_size(stack, stack->len - 1);
			continue;
		}
		if (!cond_active(stack))
			continue;
		if (g_str_has_prefix(line, "!#include ")) {
			include_list(self, base_dir, g_strstrip(line + 10), trusted);
			continue;
		}

		/* Comments and list headers. A leading '#' is a comment unless it
		 * starts a generic cosmetic filter ("##.ad"). */
		if (*line == '!' || *line == '[')
			continue;
		sep = find_cosmetic_separator(line, &sep_len);
		if (*line == '#' && sep != line)
			continue;

		/* Cosmetic, scriptlet and snippet filters. */
		if (sep != NULL) {
			parse_cosmetic_line(self, line, sep, sep_len, trusted);
			continue;
		}
		/* AdGuard HTML filtering ("example.com$$script[...]"). */
		if (strstr(line, "$$") != NULL || strstr(line, "$@$") != NULL) {
			self->stats.unsupported++;
			continue;
		}

		/* Hosts-file lines ("0.0.0.0 ads.example.com") and bare hostnames
		 * are "||host^" filters. */
		{
			g_auto(GStrv) toks = g_strsplit_set(line, " \t", -1);

			if (toks[0] != NULL && toks[1] != NULL && g_hostname_is_ip_address(toks[0])) {
				guint j;

				for (j = 1; toks[j] != NULL; j++) {
					g_autofree gchar *filter = NULL;

					if (*toks[j] == '#')
						break;
					if (!is_plain_hostname(toks[j]) || g_str_equal(toks[j], "localhost"))
						continue;
					filter = g_strdup_printf("||%s^", toks[j]);
					parse_network(self, filter);
				}
				continue;
			}
		}
		if (is_plain_hostname(line)) {
			g_autofree gchar *filter = g_strdup_printf("||%s^", line);

			parse_network(self, filter);
			continue;
		}
		parse_network(self, line);
	}
}

gboolean
gsurf_adblock_compiler_add_file(GsurfAdblockCompiler *self, const gchar *path,
                                gboolean trusted, GError **error)
{
	g_autofree gchar *text = NULL;
	g_autofree gchar *dir = NULL;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	if (!g_file_get_contents(path, &text, NULL, error))
		return FALSE;
	dir = g_path_get_dirname(path);
	gsurf_adblock_compiler_add_text(self, text, dir, trusted);
	return TRUE;
}

void
gsurf_adblock_compiler_add_allowed_site(GsurfAdblockCompiler *self, const gchar *host)
{
	g_return_if_fail(self != NULL);
	if (host != NULL && *host != '\0')
		g_hash_table_add(self->allowed_sites, g_ascii_strdown(host, -1));
}

/* --- scriptlet library --- */

/* js/scriptlets.js is split by marker comments:
 *   /// helpers                     shared code included in every injection
 *   /// scriptlet <name> <alias>... one scriptlet; its JS function follows
 * The function name is read from the section's first "function NAME(". */
static void
load_scriptlet_library(GsurfAdblockCompiler *self, const gchar *library)
{
	g_auto(GStrv) lines = g_strsplit(library, "\n", -1);
	g_autoptr(GString) section = g_string_new(NULL);
	g_auto(GStrv) names = NULL;
	gboolean in_helpers = FALSE;
	guint i;

	self->scriptlet_aliases = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->scriptlet_sources = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->scriptlet_trusted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; ; i++) {
		const gchar *line = lines[i];
		gboolean marker = line == NULL || g_str_has_prefix(line, "/// ");

		if (marker) {
			/* Close the previous section. */
			if (in_helpers) {
				g_free(self->scriptlet_helpers);
				self->scriptlet_helpers = g_strdup(section->str);
			} else if (names != NULL) {
				const gchar *fn = strstr(section->str, "function ");
				g_autofree gchar *fn_name = NULL;
				guint j;

				if (fn != NULL) {
					const gchar *open = strchr(fn, '(');

					fn_name = g_strstrip(g_strndup(fn + 9, open != NULL ? (gsize)(open - fn - 9) : 0));
				}
				if (fn_name != NULL && *fn_name != '\0') {
					for (j = 0; names[j] != NULL; j++) {
						if (g_str_equal(names[j], "!trusted"))
							g_hash_table_add(self->scriptlet_trusted, g_strdup(fn_name));
						else if (*names[j] != '\0')
							g_hash_table_replace(self->scriptlet_aliases,
								g_strdup(names[j]), g_strdup(fn_name));
					}
					g_hash_table_replace(self->scriptlet_sources,
						g_strdup(fn_name), g_strdup(section->str));
				}
			}
			g_string_truncate(section, 0);
			g_clear_pointer(&names, g_strfreev);
			in_helpers = FALSE;
			if (line == NULL)
				break;
			if (g_str_equal(g_strchomp((gchar *)line), "/// helpers"))
				in_helpers = TRUE;
			else if (g_str_has_prefix(line, "/// scriptlet "))
				names = g_strsplit_set(line + 14, " \t\r", -1);
			continue;
		}
		g_string_append(section, line);
		g_string_append_c(section, '\n');
	}
}

GsurfAdblockCompiler *
gsurf_adblock_compiler_new(const gchar *scriptlet_library, const gchar *cosmetic_engine)
{
	GsurfAdblockCompiler *self = g_new0(GsurfAdblockCompiler, 1);

	self->net = g_ptr_array_new_with_free_func(net_rule_free);
	self->badfilters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->cosmetic = g_ptr_array_new_with_free_func(cosmetic_rule_free);
	self->cosmetic_exceptions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	self->scriptlets = g_ptr_array_new_with_free_func(scriptlet_rule_free);
	self->scriptlet_exceptions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	self->allowed_sites = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (scriptlet_library != NULL)
		load_scriptlet_library(self, scriptlet_library);
	self->cosmetic_engine = g_strdup(cosmetic_engine);
	return self;
}

void
gsurf_adblock_compiler_free(GsurfAdblockCompiler *self)
{
	if (self == NULL)
		return;
	g_ptr_array_unref(self->net);
	g_hash_table_unref(self->badfilters);
	g_ptr_array_unref(self->cosmetic);
	g_hash_table_unref(self->cosmetic_exceptions);
	g_ptr_array_unref(self->scriptlets);
	g_hash_table_unref(self->scriptlet_exceptions);
	g_hash_table_unref(self->allowed_sites);
	g_free(self->scriptlet_helpers);
	g_clear_pointer(&self->scriptlet_aliases, g_hash_table_unref);
	g_clear_pointer(&self->scriptlet_sources, g_hash_table_unref);
	g_clear_pointer(&self->scriptlet_trusted, g_hash_table_unref);
	g_free(self->cosmetic_engine);
	g_free(self);
}

/* --- compilation: exceptions --- */

/* Is @host equal to, or a subdomain of, some entry of @domains? */
static gboolean
covered_by(const gchar *host, GPtrArray *domains)
{
	guint i;

	for (i = 0; i < domains->len; i++)
		if (gsurf_adblock_host_matches_domain(host, g_ptr_array_index(domains, i)))
			return TRUE;
	return FALSE;
}

/* Apply the exception domains recorded for one filter to its domain set:
 *   - a generic exception ("") disables the filter (returns FALSE);
 *   - an exception naming one of the filter's own domains removes it;
 *   - an exception for a subdomain (or any site, for a generic filter)
 *     becomes a negated domain.
 * Returns FALSE when nothing is left of the filter. */
static gboolean
apply_exceptions(DomainSet *domains, GPtrArray *exceptions)
{
	guint i, j;
	gboolean had_pos = domains->pos->len > 0;

	if (exceptions == NULL)
		return TRUE;
	for (i = 0; i < exceptions->len; i++) {
		const gchar *ex = g_ptr_array_index(exceptions, i);
		gboolean removed = FALSE;

		if (*ex == '\0')
			return FALSE;
		for (j = 0; j < domains->pos->len; j++) {
			if (g_str_equal(g_ptr_array_index(domains->pos, j), ex)) {
				g_ptr_array_remove_index(domains->pos, j);
				removed = TRUE;
				break;
			}
		}
		if (!removed && (!had_pos || covered_by(ex, domains->pos)) &&
		    !g_ptr_array_find_with_equal_func(domains->neg, ex, g_str_equal, NULL))
			g_ptr_array_add(domains->neg, g_strdup(ex));
	}
	return !had_pos || domains->pos->len > 0;
}

/* A negated domain only matters inside the filter's positive domains. */
static void
prune_negations(DomainSet *domains)
{
	guint i;

	if (domains->pos->len == 0)
		return;
	for (i = 0; i < domains->neg->len; ) {
		const gchar *neg = g_ptr_array_index(domains->neg, i);

		if (!is_entity(neg) && !covered_by(neg, domains->pos))
			g_ptr_array_remove_index(domains->neg, i);
		else
			i++;
	}
}

/* --- compilation: WebKit JSON --- */

/* A WebKit regex matching a top-level URL on @domain (or an entity). */
static gchar *
top_url_regex(const gchar *domain)
{
	GString *out = g_string_new("^[^:]+://([^/:]*\\.)?");

	if (is_entity(domain)) {
		append_regex_literal(out, domain, strlen(domain) - 2);
		g_string_append(out, "\\.[^/:]+");
	} else {
		append_regex_literal(out, domain, strlen(domain));
	}
	g_string_append(out, "[:/]");
	return g_string_free(out, FALSE);
}

/* Render a domain list as one WebKit trigger condition: if-domain /
 * unless-domain for plain domains, if-top-url / unless-top-url regexes as
 * soon as an entity is involved. */
static void
append_domain_condition(GString *out, GPtrArray *domains, gboolean positive)
{
	gboolean entity = has_entity(domains);
	guint i;

	if (domains->len == 0)
		return;
	if (entity)
		g_string_append(out, positive ? ",\"if-top-url\":[" : ",\"unless-top-url\":[");
	else
		g_string_append(out, positive ? ",\"if-domain\":[" : ",\"unless-domain\":[");
	for (i = 0; i < domains->len; i++) {
		const gchar *d = g_ptr_array_index(domains, i);

		if (i > 0)
			g_string_append_c(out, ',');
		if (entity) {
			g_autofree gchar *re = top_url_regex(d);
			append_json_string(out, re);
		} else {
			g_autofree gchar *star = g_strconcat("*", d, NULL);
			append_json_string(out, star);
		}
	}
	g_string_append_c(out, ']');
}

typedef struct {
	GString *json;
	guint    count;
} RulesetBuilder;

static void
builder_begin_rule(RulesetBuilder *b)
{
	g_string_append(b->json, b->count == 0 ? "[" : ",\n");
	b->count++;
}

static gchar *
builder_finish(RulesetBuilder *b)
{
	if (b->count == 0) {
		g_string_free(b->json, TRUE);
		return NULL;
	}
	g_string_append(b->json, "]\n");
	return g_string_free(b->json, FALSE);
}

/* WebKit resource-type names for a type mask (frames and documents are
 * handled by the caller because they need a load-context). */
static void
append_resource_types(GString *out, guint types)
{
	static const struct { guint type; const gchar *names; } table[] = {
		{ GSURF_ADBLOCK_TYPE_SCRIPT, "\"script\"" },
		{ GSURF_ADBLOCK_TYPE_IMAGE, "\"image\"" },
		{ GSURF_ADBLOCK_TYPE_STYLESHEET, "\"style-sheet\"" },
		{ GSURF_ADBLOCK_TYPE_FONT, "\"font\"" },
		{ GSURF_ADBLOCK_TYPE_MEDIA, "\"media\"" },
		{ GSURF_ADBLOCK_TYPE_XHR, "\"fetch\"" },
		{ GSURF_ADBLOCK_TYPE_WEBSOCKET, "\"websocket\"" },
		{ GSURF_ADBLOCK_TYPE_PING, "\"ping\"" },
		{ GSURF_ADBLOCK_TYPE_OTHER | GSURF_ADBLOCK_TYPE_OBJECT, "\"other\"" },
		{ GSURF_ADBLOCK_TYPE_POPUP, "\"popup\"" },
	};
	gboolean first = TRUE;
	guint i;

	g_string_append(out, ",\"resource-type\":[");
	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		if ((types & table[i].type) == 0)
			continue;
		if (!first)
			g_string_append_c(out, ',');
		g_string_append(out, table[i].names);
		first = FALSE;
	}
	/* SVG documents load like images. */
	if (types & GSURF_ADBLOCK_TYPE_IMAGE)
		g_string_append(out, ",\"svg-document\"");
	g_string_append_c(out, ']');
}

/* One network rule may expand to several WebKit rules: one per
 * load-context it needs, plus a trailing "unless" rule when it has both
 * included and excluded page domains. */
typedef struct {
	guint        types;     /* type mask for "resource-type" */
	const gchar *context;   /* "load-context", or NULL */
	gboolean     all;       /* no "resource-type" at all */
} TypeVariant;

static void
emit_net_rule(RulesetBuilder *b, NetRule *r, const gchar *action)
{
	TypeVariant variants[3];
	guint n = 0, i, other;
	gboolean block = g_str_equal(action, "block");
	GPtrArray *pos = r->domains.pos, *neg = r->domains.neg;

	if (r->types == 0) {
		if (r->host != NULL || !block) {
			variants[n++] = (TypeVariant){ 0, NULL, TRUE };
		} else {
			variants[n++] = (TypeVariant){ DEFAULT_TYPES & ~GSURF_ADBLOCK_TYPE_SUBDOCUMENT, NULL, FALSE };
			variants[n++] = (TypeVariant){ GSURF_ADBLOCK_TYPE_SUBDOCUMENT, "child-frame", FALSE };
		}
	} else {
		guint docs = r->types & (GSURF_ADBLOCK_TYPE_SUBDOCUMENT | GSURF_ADBLOCK_TYPE_DOCUMENT);

		other = r->types & ~(GSURF_ADBLOCK_TYPE_SUBDOCUMENT | GSURF_ADBLOCK_TYPE_DOCUMENT);
		if (other != 0)
			variants[n++] = (TypeVariant){ other, NULL, FALSE };
		if (docs == (GSURF_ADBLOCK_TYPE_SUBDOCUMENT | GSURF_ADBLOCK_TYPE_DOCUMENT))
			variants[n++] = (TypeVariant){ docs, NULL, FALSE };
		else if (docs == GSURF_ADBLOCK_TYPE_SUBDOCUMENT)
			variants[n++] = (TypeVariant){ docs, "child-frame", FALSE };
		else if (docs == GSURF_ADBLOCK_TYPE_DOCUMENT)
			variants[n++] = (TypeVariant){ docs, "top-frame", FALSE };
	}

	for (i = 0; i < n; i++) {
		guint pass;

		/* pass 0: the rule itself; pass 1: excluded domains (block only). */
		for (pass = 0; pass < 2; pass++) {
			GString *j = b->json;

			if (pass == 1 && (!block || pos->len == 0 || neg->len == 0))
				break;
			builder_begin_rule(b);
			g_string_append(j, "{\"trigger\":{\"url-filter\":");
			append_json_string(j, r->regex);
			if (r->match_case)
				g_string_append(j, ",\"url-filter-is-case-sensitive\":true");
			if (!variants[i].all) {
				guint t = variants[i].types;

				if (t & (GSURF_ADBLOCK_TYPE_SUBDOCUMENT | GSURF_ADBLOCK_TYPE_DOCUMENT))
					g_string_append(j, ",\"resource-type\":[\"document\"]");
				else
					append_resource_types(j, t);
			}
			if (variants[i].context != NULL)
				g_string_append_printf(j, ",\"load-context\":[\"%s\"]", variants[i].context);
			if (r->party == GSURF_ADBLOCK_PARTY_THIRD)
				g_string_append(j, ",\"load-type\":[\"third-party\"]");
			else if (r->party == GSURF_ADBLOCK_PARTY_FIRST)
				g_string_append(j, ",\"load-type\":[\"first-party\"]");
			if (pass == 0) {
				if (pos->len > 0)
					append_domain_condition(j, pos, TRUE);
				else
					append_domain_condition(j, neg, FALSE);
				g_string_append_printf(j, "},\"action\":{\"type\":\"%s\"}}", action);
			} else {
				append_domain_condition(j, neg, TRUE);
				g_string_append(j, "},\"action\":{\"type\":\"ignore-previous-rules\"}}");
			}
		}
	}
}

/* "Nothing below applies on these sites": an ignore-previous-rules rule
 * keyed on the top-level page (by domain, or by URL regex). */
static void
emit_site_ignore(RulesetBuilder *b, GPtrArray *hosts, GPtrArray *top_url_regexes)
{
	guint i;

	if (hosts != NULL && hosts->len > 0) {
		builder_begin_rule(b);
		g_string_append(b->json, "{\"trigger\":{\"url-filter\":\".*\"");
		append_domain_condition(b->json, hosts, TRUE);
		g_string_append(b->json, "},\"action\":{\"type\":\"ignore-previous-rules\"}}");
	}
	if (top_url_regexes != NULL && top_url_regexes->len > 0) {
		builder_begin_rule(b);
		g_string_append(b->json, "{\"trigger\":{\"url-filter\":\".*\",\"if-top-url\":[");
		for (i = 0; i < top_url_regexes->len; i++) {
			if (i > 0)
				g_string_append_c(b->json, ',');
			append_json_string(b->json, g_ptr_array_index(top_url_regexes, i));
		}
		g_string_append(b->json, "]},\"action\":{\"type\":\"ignore-previous-rules\"}}");
	}
}

static void
emit_css_rule(RulesetBuilder *b, const gchar *selector, DomainSet *domains)
{
	builder_begin_rule(b);
	g_string_append(b->json, "{\"trigger\":{\"url-filter\":\".*\"");
	if (domains->pos->len > 0)
		append_domain_condition(b->json, domains->pos, TRUE);
	else
		append_domain_condition(b->json, domains->neg, FALSE);
	g_string_append(b->json, "},\"action\":{\"type\":\"css-display-none\",\"selector\":");
	append_json_string(b->json, selector);
	g_string_append(b->json, "}}");
}

static void
add_ruleset(GsurfAdblockResult *result, const gchar *id, RulesetBuilder *b)
{
	GsurfAdblockRuleset *rs;
	guint count = b->count;
	gchar *json = builder_finish(b);

	if (json == NULL)
		return;
	rs = g_new0(GsurfAdblockRuleset, 1);
	rs->id = g_strdup(id);
	rs->json = json;
	rs->rules = count;
	g_ptr_array_add(result->rulesets, rs);
}

static void
ruleset_free(gpointer data)
{
	GsurfAdblockRuleset *rs = data;

	g_free(rs->id);
	g_free(rs->json);
	g_free(rs);
}

/* --- compilation: web-process tables --- */

static GVariant *
strv_variant(GPtrArray *items)
{
	GVariantBuilder b;
	guint i;

	g_variant_builder_init(&b, G_VARIANT_TYPE_STRING_ARRAY);
	for (i = 0; items != NULL && i < items->len; i++)
		g_variant_builder_add(&b, "s", (const gchar *)g_ptr_array_index(items, i));
	return g_variant_builder_end(&b);
}

static GPtrArray *
hash_keys_array(GHashTable *table)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	GHashTableIter it;
	gpointer key;

	g_hash_table_iter_init(&it, table);
	while (g_hash_table_iter_next(&it, &key, NULL))
		g_ptr_array_add(out, g_strdup(key));
	return out;
}

/* A NULL-terminated view of @items (the strings stay owned by @items). */
static const gchar **
borrowed_strv(GPtrArray *items)
{
	const gchar **out = g_new0(const gchar *, items->len + 1);
	guint i;

	for (i = 0; i < items->len; i++)
		out[i] = g_ptr_array_index(items, i);
	return out;
}

static void
append_copies(GPtrArray *dest, GPtrArray *src)
{
	guint i;

	for (i = 0; i < src->len; i++)
		g_ptr_array_add(dest, g_strdup(g_ptr_array_index(src, i)));
}

/* --- compile --- */

GsurfAdblockResult *
gsurf_adblock_compiler_compile(GsurfAdblockCompiler *self, GsurfAdblockCompileFlags flags)
{
	GsurfAdblockResult *result;
	RulesetBuilder net = { g_string_new(NULL), 0 };
	RulesetBuilder generic = { g_string_new(NULL), 0 };
	RulesetBuilder specific = { g_string_new(NULL), 0 };
	g_autoptr(GPtrArray) allowed = hash_keys_array(self->allowed_sites);
	g_autoptr(GPtrArray) doc_hosts = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) doc_regexes = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) generic_off_hosts = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) generic_off_regexes = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) specific_off_hosts = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) specific_off_regexes = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GHashTable) seen_generic = g_hash_table_new(g_str_hash, g_str_equal);
	GVariantBuilder scriptlets, cosmetic, library;
	gboolean web_process = (flags & GSURF_ADBLOCK_COMPILE_WEB_PROCESS) != 0;
	gboolean want_cosmetic = (flags & GSURF_ADBLOCK_COMPILE_COSMETIC) != 0;
	gboolean want_redirects = web_process && (flags & GSURF_ADBLOCK_COMPILE_REDIRECTS) != 0;
	gboolean want_scriptlets = web_process && (flags & GSURF_ADBLOCK_COMPILE_SCRIPTLETS) != 0;
	g_autoptr(GHashTable) used_fns = g_hash_table_new(g_str_hash, g_str_equal);
	guint n_scriptlets = 0, n_cosmetic = 0;
	guint pass, i;

	g_return_val_if_fail(self != NULL, NULL);

	result = g_new0(GsurfAdblockResult, 1);
	result->rulesets = g_ptr_array_new_with_free_func(ruleset_free);
	result->matcher = gsurf_adblock_matcher_new();
	result->stats = self->stats;
	for (i = 0; i < allowed->len; i++)
		gsurf_adblock_matcher_add_allowed_site(result->matcher, g_ptr_array_index(allowed, i));

	/* --- network: blocks, then exceptions, then $important blocks, so
	 * WebKit's ignore-previous-rules gives uBO's precedence. --- */
	for (pass = 0; pass < 3; pass++) {
		for (i = 0; i < self->net->len; i++) {
			NetRule *r = g_ptr_array_index(self->net, i);

			if (g_hash_table_contains(self->badfilters, r->key))
				continue;

			/* Site-level exceptions are collected, not emitted inline. */
			if (r->exception && (r->doc_exception || r->cosmetic_off != 0)) {
				if (pass != 0)
					continue;
				if (r->doc_exception) {
					if (r->host != NULL)
						g_ptr_array_add(doc_hosts, g_strdup(r->host));
					else if (r->regex != NULL)
						g_ptr_array_add(doc_regexes, g_strdup(r->regex));
				}
				if (r->cosmetic_off & COSMETIC_OFF_GENERIC) {
					if (r->host != NULL)
						g_ptr_array_add(generic_off_hosts, g_strdup(r->host));
					else if (r->regex != NULL)
						g_ptr_array_add(generic_off_regexes, g_strdup(r->regex));
				}
				if (r->cosmetic_off & COSMETIC_OFF_SPECIFIC) {
					if (r->host != NULL)
						g_ptr_array_add(specific_off_hosts, g_strdup(r->host));
					else if (r->regex != NULL)
						g_ptr_array_add(specific_off_regexes, g_strdup(r->regex));
				}
				continue;
			}

			/* $redirect / $removeparam go to the request matcher when the
			 * web process can apply them. A redirect without that falls
			 * back to blocking; removeparam has no fallback. */
			if (r->runtime != RUNTIME_NONE) {
				if (pass != 0)
					continue;
				if (want_redirects) {
					g_autoptr(GError) error = NULL;
					g_autofree const gchar **pos = borrowed_strv(r->domains.pos);
					g_autofree const gchar **neg = borrowed_strv(r->domains.neg);

					if (gsurf_adblock_matcher_add(result->matcher, r->pcre, r->token,
					        r->match_case, r->exception,
					        r->runtime == RUNTIME_REDIRECT ? GSURF_ADBLOCK_ACTION_REDIRECT
					                                       : GSURF_ADBLOCK_ACTION_REMOVEPARAM,
					        r->runtime_arg, pos, neg, r->party, r->types, r->priority, &error)) {
						if (r->runtime == RUNTIME_REDIRECT)
							result->stats.redirects++;
						else
							result->stats.removeparams++;
					} else {
						result->stats.invalid++;
					}
					continue;
				}
				if (r->runtime == RUNTIME_REMOVEPARAM || r->exception || r->regex == NULL) {
					result->stats.unsupported++;
					continue;
				}
				result->stats.network++;
				emit_net_rule(&net, r, "block");
				continue;
			}
			if (r->regex == NULL)
				continue;
			if (pass == 0 && !r->exception && !r->important) {
				result->stats.network++;
				emit_net_rule(&net, r, "block");
			} else if (pass == 1 && r->exception) {
				result->stats.network_exceptions++;
				emit_net_rule(&net, r, "ignore-previous-rules");
			} else if (pass == 2 && !r->exception && r->important) {
				result->stats.network++;
				emit_net_rule(&net, r, "block");
			}
		}
	}
	/* Whole-site exceptions and the user's allowlist come last. */
	{
		g_autoptr(GPtrArray) sites = g_ptr_array_new_with_free_func(g_free);

		append_copies(sites, allowed);
		append_copies(sites, doc_hosts);
		emit_site_ignore(&net, sites, doc_regexes);
		append_copies(generic_off_hosts, sites);
		append_copies(specific_off_hosts, sites);
		append_copies(generic_off_regexes, doc_regexes);
		append_copies(specific_off_regexes, doc_regexes);
	}

	/* --- cosmetic --- */
	g_variant_builder_init(&cosmetic, G_VARIANT_TYPE("a(asass)"));
	for (i = 0; want_cosmetic && i < self->cosmetic->len; i++) {
		CosmeticRule *r = g_ptr_array_index(self->cosmetic, i);

		if (!apply_exceptions(&r->domains,
		        g_hash_table_lookup(self->cosmetic_exceptions, r->selector)))
			continue;
		prune_negations(&r->domains);

		if (r->domains.pos->len == 0) {
			/* Generic hiding: one native rule per distinct selector. */
			if (g_hash_table_contains(seen_generic, r->selector) && r->domains.neg->len == 0)
				continue;
			g_hash_table_add(seen_generic, r->selector);
			emit_css_rule(&generic, r->selector, &r->domains);
			result->stats.cosmetic_generic++;
			continue;
		}
		/* Specific hiding is native unless it needs the engine or has
		 * negated subdomains WebKit cannot combine with if-domain. An
		 * entity rule keeps its (top-url) native form either way. */
		if (r->kind == COSMETIC_HIDE &&
		    (r->domains.neg->len == 0 || has_entity(r->domains.pos) || !web_process)) {
			emit_css_rule(&specific, r->selector, &r->domains);
			result->stats.cosmetic_specific++;
			continue;
		}
		if (!web_process || self->cosmetic_engine == NULL) {
			result->stats.unsupported++;
			continue;
		}
		g_variant_builder_add(&cosmetic, "(@as@ass)", strv_variant(r->domains.pos),
			strv_variant(r->domains.neg), r->selector);
		n_cosmetic++;
		if (r->kind == COSMETIC_PROCEDURAL)
			result->stats.procedural++;
		else
			result->stats.cosmetic_specific++;
	}
	emit_site_ignore(&generic, generic_off_hosts, generic_off_regexes);
	emit_site_ignore(&specific, specific_off_hosts, specific_off_regexes);

	add_ruleset(result, "net", &net);
	add_ruleset(result, "cosmetic-generic", &generic);
	add_ruleset(result, "cosmetic-specific", &specific);

	/* --- scriptlets --- */
	g_variant_builder_init(&scriptlets, G_VARIANT_TYPE("a(asasss)"));
	for (i = 0; want_scriptlets && self->scriptlet_sources != NULL && i < self->scriptlets->len; i++) {
		ScriptletRule *r = g_ptr_array_index(self->scriptlets, i);

		if (!apply_exceptions(&r->domains, g_hash_table_lookup(self->scriptlet_exceptions, "")) ||
		    !apply_exceptions(&r->domains, g_hash_table_lookup(self->scriptlet_exceptions, r->key)))
			continue;
		prune_negations(&r->domains);
		g_variant_builder_add(&scriptlets, "(@as@asss)", strv_variant(r->domains.pos),
			strv_variant(r->domains.neg), r->fn, r->call);
		g_hash_table_add(used_fns, r->fn);
		n_scriptlets++;
	}
	if (!want_scriptlets)
		result->stats.unsupported += self->scriptlets->len;
	result->stats.scriptlets = n_scriptlets;

	/* --- page data for the web-process extension --- */
	g_variant_builder_init(&library, G_VARIANT_TYPE("a{ss}"));
	if (n_scriptlets > 0) {
		GHashTableIter it;
		gpointer fn;

		g_hash_table_iter_init(&it, used_fns);
		while (g_hash_table_iter_next(&it, &fn, NULL))
			g_variant_builder_add(&library, "{ss}", (const gchar *)fn,
				(const gchar *)g_hash_table_lookup(self->scriptlet_sources, fn));
	}
	if (web_process && (n_scriptlets > 0 || n_cosmetic > 0)) {
		GVariantDict dict;

		g_variant_dict_init(&dict, NULL);
		g_variant_dict_insert_value(&dict, "scriptlets", g_variant_builder_end(&scriptlets));
		g_variant_dict_insert_value(&dict, "scriptlet-library", g_variant_builder_end(&library));
		g_variant_dict_insert(&dict, "scriptlet-helpers", "s",
			self->scriptlet_helpers != NULL ? self->scriptlet_helpers : "");
		g_variant_dict_insert_value(&dict, "cosmetic", g_variant_builder_end(&cosmetic));
		g_variant_dict_insert(&dict, "cosmetic-engine", "s",
			self->cosmetic_engine != NULL ? self->cosmetic_engine : "");
		g_variant_dict_insert_value(&dict, "allowed-sites", strv_variant(allowed));
		g_variant_dict_insert_value(&dict, "cosmetic-off-sites", strv_variant(specific_off_hosts));
		result->page_data = g_variant_ref_sink(g_variant_dict_end(&dict));
	} else {
		g_variant_builder_clear(&scriptlets);
		g_variant_builder_clear(&library);
		g_variant_builder_clear(&cosmetic);
	}
	return result;
}

void
gsurf_adblock_result_free(GsurfAdblockResult *result)
{
	if (result == NULL)
		return;
	g_ptr_array_unref(result->rulesets);
	gsurf_adblock_matcher_free(result->matcher);
	g_clear_pointer(&result->page_data, g_variant_unref);
	g_free(result);
}
