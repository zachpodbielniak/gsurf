/*
 * gsurf-adblock-match.h - Request-time filter matching (redirect/removeparam)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * WebKit's native content blocker can only block or hide. Filters that
 * rewrite a request — uBO's `$redirect` / `$redirect-rule` (serve a neutered
 * stand-in resource) and `$removeparam` (strip tracking parameters) — are
 * evaluated here instead. The same matcher runs in two places:
 *   - the adblock module (UI process), which builds it from the filter
 *     lists and answers #GsurfRequestFilter queries;
 *   - the adblock web-process extension, which receives it serialized as a
 *     #GVariant and applies it to every request a page sends.
 * Only GLib and libsoup (public-suffix data) are used, so the file links
 * into both without pulling in GTK.
 */

#ifndef GSURF_ADBLOCK_MATCH_H
#define GSURF_ADBLOCK_MATCH_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GsurfAdblockType:
 * @GSURF_ADBLOCK_TYPE_SCRIPT: a script
 * @GSURF_ADBLOCK_TYPE_IMAGE: an image
 * @GSURF_ADBLOCK_TYPE_STYLESHEET: a stylesheet
 * @GSURF_ADBLOCK_TYPE_FONT: a web font
 * @GSURF_ADBLOCK_TYPE_MEDIA: audio or video
 * @GSURF_ADBLOCK_TYPE_XHR: XMLHttpRequest / fetch()
 * @GSURF_ADBLOCK_TYPE_SUBDOCUMENT: a frame's document
 * @GSURF_ADBLOCK_TYPE_DOCUMENT: the top-level document
 * @GSURF_ADBLOCK_TYPE_WEBSOCKET: a WebSocket connection
 * @GSURF_ADBLOCK_TYPE_PING: a ping / beacon
 * @GSURF_ADBLOCK_TYPE_OTHER: anything else
 * @GSURF_ADBLOCK_TYPE_POPUP: a pop-up window
 * @GSURF_ADBLOCK_TYPE_OBJECT: a plugin object
 *
 * Request types, as a bitmask. A filter's type set of 0 matches every type.
 */
typedef enum {
	GSURF_ADBLOCK_TYPE_SCRIPT      = 1 << 0,
	GSURF_ADBLOCK_TYPE_IMAGE       = 1 << 1,
	GSURF_ADBLOCK_TYPE_STYLESHEET  = 1 << 2,
	GSURF_ADBLOCK_TYPE_FONT        = 1 << 3,
	GSURF_ADBLOCK_TYPE_MEDIA       = 1 << 4,
	GSURF_ADBLOCK_TYPE_XHR         = 1 << 5,
	GSURF_ADBLOCK_TYPE_SUBDOCUMENT = 1 << 6,
	GSURF_ADBLOCK_TYPE_DOCUMENT    = 1 << 7,
	GSURF_ADBLOCK_TYPE_WEBSOCKET   = 1 << 8,
	GSURF_ADBLOCK_TYPE_PING        = 1 << 9,
	GSURF_ADBLOCK_TYPE_OTHER       = 1 << 10,
	GSURF_ADBLOCK_TYPE_POPUP       = 1 << 11,
	GSURF_ADBLOCK_TYPE_OBJECT      = 1 << 12
} GsurfAdblockType;

#define GSURF_ADBLOCK_TYPE_ALL ((1u << 13) - 1)

/**
 * GsurfAdblockParty:
 * @GSURF_ADBLOCK_PARTY_ANY: first- and third-party requests
 * @GSURF_ADBLOCK_PARTY_FIRST: only requests to the page's own site
 * @GSURF_ADBLOCK_PARTY_THIRD: only requests to other sites
 */
typedef enum {
	GSURF_ADBLOCK_PARTY_ANY,
	GSURF_ADBLOCK_PARTY_FIRST,
	GSURF_ADBLOCK_PARTY_THIRD
} GsurfAdblockParty;

/**
 * GsurfAdblockAction:
 * @GSURF_ADBLOCK_ACTION_REDIRECT: serve a named stand-in resource
 * @GSURF_ADBLOCK_ACTION_REMOVEPARAM: strip query parameters
 */
typedef enum {
	GSURF_ADBLOCK_ACTION_REDIRECT,
	GSURF_ADBLOCK_ACTION_REMOVEPARAM
} GsurfAdblockAction;

typedef struct _GsurfAdblockMatcher GsurfAdblockMatcher;

/**
 * gsurf_adblock_matcher_new:
 *
 * Returns: (transfer full): an empty matcher
 */
GsurfAdblockMatcher *gsurf_adblock_matcher_new(void);

/**
 * gsurf_adblock_matcher_free:
 * @self: (nullable): a matcher
 */
void gsurf_adblock_matcher_free(GsurfAdblockMatcher *self);

/**
 * gsurf_adblock_matcher_add:
 * @self: a matcher
 * @regex: PCRE pattern matched against the request URL
 * @token: (nullable): a lowercase alphanumeric token every matching URL
 *   contains as a whole token, used to index the rule; %NULL or "" puts the
 *   rule on the always-checked list
 * @match_case: match @regex case-sensitively
 * @exception: this is an `@@` exception rule
 * @action: what the rule does
 * @arg: (nullable): resource name (redirect) or parameter spec
 *   (removeparam); "" / %NULL on an exception means "every"
 * @if_domains: (nullable) (array zero-terminated=1): page domains the rule
 *   is limited to ("example.com" or entity "example.*")
 * @unless_domains: (nullable) (array zero-terminated=1): page domains the
 *   rule never applies on
 * @party: first-/third-party restriction
 * @types: #GsurfAdblockType mask, 0 for every type
 * @priority: redirect priority (higher wins)
 * @error: return location for an invalid @regex
 *
 * Adds one rule.
 *
 * Returns: %TRUE when the rule was added
 */
gboolean gsurf_adblock_matcher_add(GsurfAdblockMatcher *self, const gchar *regex,
                                   const gchar *token, gboolean match_case,
                                   gboolean exception, GsurfAdblockAction action,
                                   const gchar *arg, const gchar *const *if_domains,
                                   const gchar *const *unless_domains,
                                   GsurfAdblockParty party, guint types, gint priority,
                                   GError **error);

/**
 * gsurf_adblock_matcher_add_allowed_site:
 * @self: a matcher
 * @host: a site (and its subdomains) on which nothing is rewritten
 */
void gsurf_adblock_matcher_add_allowed_site(GsurfAdblockMatcher *self, const gchar *host);

/**
 * gsurf_adblock_matcher_get_size:
 * @self: a matcher
 *
 * Returns: the number of rules
 */
guint gsurf_adblock_matcher_get_size(GsurfAdblockMatcher *self);

/**
 * gsurf_adblock_matcher_serialize:
 * @self: a matcher
 *
 * Returns: (transfer full): the rules and allowed sites as a non-floating
 *   #GVariant for gsurf_adblock_matcher_new_from_variant()
 */
GVariant *gsurf_adblock_matcher_serialize(GsurfAdblockMatcher *self);

/**
 * gsurf_adblock_matcher_new_from_variant:
 * @variant: data from gsurf_adblock_matcher_serialize()
 *
 * Returns: (transfer full) (nullable): the matcher, or %NULL when @variant
 *   has the wrong type
 */
GsurfAdblockMatcher *gsurf_adblock_matcher_new_from_variant(GVariant *variant);

/**
 * gsurf_adblock_matcher_process:
 * @self: a matcher
 * @url: the request URL
 * @page_url: (nullable): the URL of the page making the request
 * @type: the request's #GsurfAdblockType, or 0 when unknown
 * @blocked: (out) (optional): set to %TRUE when a redirect matched but its
 *   resource is unknown, so the request should be cancelled instead
 *
 * Applies every matching rule. A redirect wins over parameter removal.
 *
 * Returns: (transfer full) (nullable): the URL to load instead (a `data:`
 *   stand-in or @url without the removed parameters), or %NULL to load
 *   @url unchanged
 */
gchar *gsurf_adblock_matcher_process(GsurfAdblockMatcher *self, const gchar *url,
                                     const gchar *page_url, guint type, gboolean *blocked);

/**
 * gsurf_adblock_guess_type:
 * @url: a request URL
 *
 * Infers a request type from the URL path's extension, for engines that do
 * not report the type of a request.
 *
 * Returns: a single #GsurfAdblockType, or 0 when the extension says nothing
 */
guint gsurf_adblock_guess_type(const gchar *url);

/**
 * gsurf_adblock_resource_get_uri:
 * @name: a uBO redirect resource name or alias (e.g. "noop.js", "1x1.gif")
 *
 * Returns: (transfer full) (nullable): a `data:` URI serving the resource,
 *   or %NULL when the name is unknown
 */
gchar *gsurf_adblock_resource_get_uri(const gchar *name);

/**
 * gsurf_adblock_host_matches_domain:
 * @host: a lowercase hostname
 * @domain: "example.com" (matches it and its subdomains) or an entity
 *   "example.*" (matches example plus any public suffix, and subdomains)
 *
 * Returns: %TRUE when @host is covered by @domain
 */
gboolean gsurf_adblock_host_matches_domain(const gchar *host, const gchar *domain);

G_END_DECLS

#endif /* GSURF_ADBLOCK_MATCH_H */
