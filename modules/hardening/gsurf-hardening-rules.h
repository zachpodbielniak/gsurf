/*
 * gsurf-hardening-rules.h - Pure decision logic of the hardening module
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything the hardening module decides about a request, written
 * against GLib/GIO only so the very same code runs in three places:
 *   - the hardening module (UI process), which validates the YAML and
 *     serializes it with gsurf_hardening_policy_to_variant();
 *   - the web-process extension (webext/), which parses that #GVariant
 *     back with gsurf_hardening_policy_new_from_variant() and applies the
 *     decisions in "send-request" and "window-object-cleared";
 *   - the headless unit test (tests/test-hardening.c).
 * Nothing here links libgsurf or WebKit.
 */

#ifndef GSURF_HARDENING_RULES_H
#define GSURF_HARDENING_RULES_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GsurfHardeningFeature:
 * @GSURF_HARDENING_HTTPS_ONLY: upgrade http:// requests to https://
 * @GSURF_HARDENING_STRIP_PARAMS: remove tracking query parameters
 * @GSURF_HARDENING_REFERER: trim or strip cross-origin Referer headers
 * @GSURF_HARDENING_GPC: send `Sec-GPC: 1` and expose
 *   `navigator.globalPrivacyControl`
 * @GSURF_HARDENING_DNT: send `DNT: 1`
 * @GSURF_HARDENING_NO_PREFETCH: cancel speculative requests (link
 *   prefetch, hyperlink-auditing pings) and turn DNS prefetching off
 * @GSURF_HARDENING_WEBRTC: force WebRTC off
 * @GSURF_HARDENING_ALL: every per-site feature
 *
 * The per-site features. An exemption (`exempt:` rule) removes some of
 * them for pages whose top-level host matches. Session-wide protections
 * (third-party cookies, ITP) are not features in this sense: they cannot
 * vary per site.
 */
typedef enum {
	GSURF_HARDENING_HTTPS_ONLY   = 1 << 0,
	GSURF_HARDENING_STRIP_PARAMS = 1 << 1,
	GSURF_HARDENING_REFERER      = 1 << 2,
	GSURF_HARDENING_GPC          = 1 << 3,
	GSURF_HARDENING_DNT          = 1 << 4,
	GSURF_HARDENING_NO_PREFETCH  = 1 << 5,
	GSURF_HARDENING_WEBRTC       = 1 << 6,
	GSURF_HARDENING_ALL          = (1 << 7) - 1
} GsurfHardeningFeature;

/**
 * GsurfHardeningReferer:
 * @GSURF_HARDENING_REFERER_OFF: leave the Referer header alone
 * @GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN: send only the origin
 *   (`scheme://host[:port]/`) to other origins
 * @GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN: send no Referer at all to
 *   other origins
 *
 * The `referer:` policy. Same-origin requests always keep their Referer.
 */
typedef enum {
	GSURF_HARDENING_REFERER_OFF,
	GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN,
	GSURF_HARDENING_REFERER_STRIP_CROSS_ORIGIN
} GsurfHardeningReferer;

/**
 * GsurfHardeningRefererAction:
 * @GSURF_HARDENING_REFERER_KEEP: send the header unchanged
 * @GSURF_HARDENING_REFERER_REPLACE: send the replacement value instead
 * @GSURF_HARDENING_REFERER_REMOVE: drop the header
 *
 * What gsurf_hardening_referer_rewrite() decided.
 */
typedef enum {
	GSURF_HARDENING_REFERER_KEEP,
	GSURF_HARDENING_REFERER_REPLACE,
	GSURF_HARDENING_REFERER_REMOVE
} GsurfHardeningRefererAction;

/* --- names --- */

/**
 * gsurf_hardening_feature_from_name:
 * @name: a feature name as written in `skip:` (e.g. "https_only"), or
 *   "all"
 *
 * Returns: the feature bit(s), or 0 for an unknown name
 */
guint gsurf_hardening_feature_from_name(const gchar *name);

/**
 * gsurf_hardening_feature_to_name:
 * @feature: exactly one #GsurfHardeningFeature bit
 *
 * Returns: (nullable): the YAML name of @feature, or %NULL
 */
const gchar *gsurf_hardening_feature_to_name(guint feature);

/**
 * gsurf_hardening_referer_from_name:
 * @name: "off", "trim_cross_origin" or "strip_cross_origin"
 * @out: (out): the parsed policy
 *
 * Returns: %TRUE when @name is a known policy
 */
gboolean gsurf_hardening_referer_from_name(const gchar *name, GsurfHardeningReferer *out);

/**
 * gsurf_hardening_referer_to_name:
 * @policy: a #GsurfHardeningReferer
 *
 * Returns: the YAML name of @policy
 */
const gchar *gsurf_hardening_referer_to_name(GsurfHardeningReferer policy);

/* --- pure request decisions --- */

/**
 * gsurf_hardening_host_is_local:
 * @host: (nullable): a bare host (no brackets, no port)
 *
 * Whether @host is a local name that never gets an https:// upgrade by
 * default: "localhost" and `*.localhost`, single-label names, the
 * `.local`, `.lan`, `.internal` and `.home.arpa` suffixes, and IP
 * literals that are unspecified, loopback, link-local, private
 * (RFC 1918), shared CGNAT space or IPv6 unique-local.
 *
 * Returns: %TRUE for a local host (and for %NULL / empty)
 */
gboolean gsurf_hardening_host_is_local(const gchar *host);

/**
 * gsurf_hardening_upgrade_uri:
 * @uri: a request URI
 * @upgrade_local: also upgrade local hosts (see
 *   gsurf_hardening_host_is_local())
 *
 * Decides the https-only upgrade: an `http:` URI becomes `https:` with
 * the same host, path, query and fragment; an explicit `:80` is dropped,
 * any other explicit port is kept.
 *
 * Returns: (transfer full) (nullable): the upgraded URI, or %NULL to
 *   leave @uri alone (not http, local host, unparsable)
 */
gchar *gsurf_hardening_upgrade_uri(const gchar *uri, gboolean upgrade_local);

/**
 * gsurf_hardening_is_downgrade:
 * @from: (nullable): the URI of a redirect response
 * @to: the URI the redirect points at
 *
 * Whether a redirect from an https:// URI to http:// would, once
 * upgraded, lead straight back to @from (`https://h/x` → `http://h/x`).
 * Under https-only that loops until the engine gives up, so the
 * extension cancels it instead (Firefox's HTTPS-Only refuses the same
 * downgrade). Redirects to any other http:// URI are just upgraded.
 *
 * Returns: %TRUE for a downgrade loop
 */
gboolean gsurf_hardening_is_downgrade(const gchar *from, const gchar *to);

/**
 * gsurf_hardening_strip_params:
 * @uri: a request URI
 * @globs: (nullable) (array zero-terminated=1): glob patterns of
 *   parameter names (`*` and `?` wildcards, case-insensitive)
 *
 * Removes every query parameter whose (percent-decoded) name matches one
 * of @globs. Other parameters keep their order and exact encoding, and
 * the fragment is preserved; a query left empty loses its `?`.
 *
 * Returns: (transfer full) (nullable): the rewritten URI, or %NULL when
 *   nothing was removed
 */
gchar *gsurf_hardening_strip_params(const gchar *uri, const gchar * const *globs);

/**
 * gsurf_hardening_same_origin:
 * @a: (nullable): a URI
 * @b: (nullable): a URI
 *
 * Compares scheme, host and effective port (80/443 when omitted).
 *
 * Returns: %TRUE when both parse and share one origin
 */
gboolean gsurf_hardening_same_origin(const gchar *a, const gchar *b);

/**
 * gsurf_hardening_origin_of:
 * @uri: a URI
 *
 * Returns: (transfer full) (nullable): `scheme://host[:port]/` (IPv6
 *   hosts bracketed, default ports omitted), or %NULL when @uri has no
 *   host
 */
gchar *gsurf_hardening_origin_of(const gchar *uri);

/**
 * gsurf_hardening_referer_rewrite:
 * @policy: the `referer:` policy
 * @referer: (nullable): the Referer header the request carries
 * @target: the URI the request goes to
 * @replacement: (out) (transfer full) (nullable): the new header value
 *   for %GSURF_HARDENING_REFERER_REPLACE, else %NULL
 *
 * Applies @policy to one request. Same-origin requests and requests
 * without a Referer are kept; an unparsable Referer on a cross-origin
 * request is removed.
 *
 * Returns: what to do with the header
 */
GsurfHardeningRefererAction gsurf_hardening_referer_rewrite(GsurfHardeningReferer policy,
                                                            const gchar *referer,
                                                            const gchar *target,
                                                            gchar **replacement);

/**
 * gsurf_hardening_is_speculative:
 * @purpose: (nullable): the `Purpose` (or `X-Purpose`) header value
 * @sec_purpose: (nullable): the `Sec-Purpose` header value
 * @ping_to: (nullable): the `Ping-To` header value
 * @content_type: (nullable): the `Content-Type` header value
 *
 * Recognizes requests the user never asked for: link prefetches
 * (`Purpose: prefetch`, `Sec-Purpose: prefetch…`) and hyperlink-auditing
 * pings (`Ping-To`, or a `text/ping` body).
 *
 * Returns: %TRUE when the request is speculative
 */
gboolean gsurf_hardening_is_speculative(const gchar *purpose, const gchar *sec_purpose,
                                        const gchar *ping_to, const gchar *content_type);

/* --- exemptions --- */

/**
 * gsurf_hardening_skip_mask:
 * @skip: (nullable): a rule's `skip` value: `as`, a single `s`, or %NULL
 * @unknown: (nullable) (element-type utf8): collects names that are not
 *   features (owned strings)
 * @session_wide: (nullable) (element-type utf8): collects valid option
 *   names that cannot be exempted per site ("third_party_cookies",
 *   "itp"; owned strings)
 *
 * Turns a `skip:` list into feature bits. A missing `skip` (%NULL) means
 * every feature: a rule without `skip:` is a full site exception.
 *
 * Returns: the #GsurfHardeningFeature bits to skip
 */
guint gsurf_hardening_skip_mask(GVariant *skip, GPtrArray *unknown, GPtrArray *session_wide);

typedef struct _GsurfHardeningExemptions GsurfHardeningExemptions;

/**
 * gsurf_hardening_exemptions_new:
 * @rules: (nullable): `a(sa{sv})` as produced by
 *   gsurf_host_rules_to_variant(): (pattern, rule data)
 *
 * Compiles the exemption rules (case-insensitive, unanchored, like
 * #GsurfHostRules). Rules whose pattern does not compile are dropped (the
 * module has already rejected them with an error).
 *
 * Returns: (transfer full): the compiled rules
 */
GsurfHardeningExemptions *gsurf_hardening_exemptions_new(GVariant *rules);

/**
 * gsurf_hardening_exemptions_free:
 * @self: (nullable): the rules
 */
void gsurf_hardening_exemptions_free(GsurfHardeningExemptions *self);

/**
 * gsurf_hardening_exemptions_get_size:
 * @self: the rules
 *
 * Returns: the number of compiled rules
 */
guint gsurf_hardening_exemptions_get_size(GsurfHardeningExemptions *self);

/**
 * gsurf_hardening_exemptions_lookup:
 * @self: the rules
 * @host: (nullable): the TOP-LEVEL page host
 *
 * Returns: the union of the skip masks of every rule matching @host
 *   (0 when none matches, or for %NULL / empty @host)
 */
guint gsurf_hardening_exemptions_lookup(GsurfHardeningExemptions *self, const gchar *host);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfHardeningExemptions, gsurf_hardening_exemptions_free)

/* --- the policy handed to the web process --- */

/**
 * GsurfHardeningPolicy:
 * @https_only: upgrade http:// requests
 * @https_only_local: also upgrade local hosts
 * @strip_params: (array zero-terminated=1): parameter-name globs
 * @referer: the Referer policy
 * @gpc: Global Privacy Control
 * @dnt: Do Not Track
 * @no_prefetch: cancel speculative requests, DNS prefetching off
 * @webrtc_off: force WebRTC off
 * @exempt_rules: `a(sa{sv})` exemption rules as serialized (kept for
 *   gsurf_hardening_policy_to_variant())
 * @exempt: the compiled @exempt_rules
 *
 * The complete per-site configuration, identical in the UI process and
 * the web process.
 */
typedef struct {
	gboolean                  https_only;
	gboolean                  https_only_local;
	gchar                   **strip_params;
	GsurfHardeningReferer     referer;
	gboolean                  gpc;
	gboolean                  dnt;
	gboolean                  no_prefetch;
	gboolean                  webrtc_off;
	GVariant                 *exempt_rules;
	GsurfHardeningExemptions *exempt;
} GsurfHardeningPolicy;

/**
 * gsurf_hardening_policy_new:
 *
 * Returns: (transfer full): a policy with every feature off and no
 *   exemptions
 */
GsurfHardeningPolicy *gsurf_hardening_policy_new(void);

/**
 * gsurf_hardening_policy_free:
 * @self: (nullable): a policy
 */
void gsurf_hardening_policy_free(GsurfHardeningPolicy *self);

/**
 * gsurf_hardening_policy_set_exemptions:
 * @self: a policy
 * @rules: (nullable): `a(sa{sv})` rules; a floating reference is sunk
 *
 * Replaces the exemption rules and recompiles them.
 */
void gsurf_hardening_policy_set_exemptions(GsurfHardeningPolicy *self, GVariant *rules);

/**
 * gsurf_hardening_policy_to_variant:
 * @self: a policy
 *
 * Serializes @self as the `a{sv}` the web-process extension receives
 * under the "hardening" key: `https-only` b, `https-only-local` b,
 * `strip-params` as, `referer` s, `gpc` b, `dnt` b, `no-prefetch` b,
 * `webrtc-off` b, `exempt` a(sa{sv}).
 *
 * Returns: (transfer floating): the dictionary
 */
GVariant *gsurf_hardening_policy_to_variant(GsurfHardeningPolicy *self);

/**
 * gsurf_hardening_policy_new_from_variant:
 * @dict: (nullable): an `a{sv}` from gsurf_hardening_policy_to_variant()
 *
 * Missing or mistyped keys keep their gsurf_hardening_policy_new()
 * defaults (off).
 *
 * Returns: (transfer full): the policy
 */
GsurfHardeningPolicy *gsurf_hardening_policy_new_from_variant(GVariant *dict);

/**
 * gsurf_hardening_policy_get_features:
 * @self: a policy
 *
 * Returns: the #GsurfHardeningFeature bits that are switched on
 */
guint gsurf_hardening_policy_get_features(GsurfHardeningPolicy *self);

/**
 * gsurf_hardening_policy_features_for_host:
 * @self: a policy
 * @top_host: (nullable): the host of the TOP-LEVEL page
 *
 * Returns: the features in force on a page whose top-level host is
 *   @top_host: the enabled features minus that host's exemptions
 */
guint gsurf_hardening_policy_features_for_host(GsurfHardeningPolicy *self,
                                               const gchar *top_host);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfHardeningPolicy, gsurf_hardening_policy_free)

/**
 * GSURF_HARDENING_GPC_JS:
 *
 * The main-world script that exposes `navigator.globalPrivacyControl`
 * (true, read-only, on `Navigator.prototype` like Firefox).
 */
#define GSURF_HARDENING_GPC_JS \
	"(function () {\n" \
	"  try {\n" \
	"    Object.defineProperty(Navigator.prototype, 'globalPrivacyControl', {\n" \
	"      get: function () { return true; },\n" \
	"      configurable: true, enumerable: true\n" \
	"    });\n" \
	"  } catch (e) { }\n" \
	"})();\n"

G_END_DECLS

#endif /* GSURF_HARDENING_RULES_H */
