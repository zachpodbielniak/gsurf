/*
 * gsurf-adblock-filters.h - uBlock Origin / Adblock Plus filter compiler
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Parses filter lists in uBO/ABP syntax (EasyList, EasyPrivacy, uBO's own
 * lists, hand-written filters) and splits them by what can enforce them:
 *
 *   - WebKit content-blocker rulesets (native, compiled once per process):
 *       "net"               network block rules and their `@@` exceptions
 *       "cosmetic-generic"  `##selector` element hiding
 *       "cosmetic-specific" `example.com##selector` element hiding
 *   - a #GsurfAdblockMatcher for `$redirect` / `$removeparam`, applied to
 *     each request by the adblock web-process extension;
 *   - per-page tables for the web-process extension: scriptlets
 *     (`##+js(...)`) injected into the page before its own scripts, and
 *     procedural / `:style()` / `:remove()` cosmetic filters run by an
 *     isolated-world engine.
 *
 * Filters the engine cannot honour (HTML filtering, `$csp`, `$header`,
 * `$replace`, ...) are skipped and counted rather than approximated.
 */

#ifndef GSURF_ADBLOCK_FILTERS_H
#define GSURF_ADBLOCK_FILTERS_H

#include <glib.h>
#include "gsurf-adblock-match.h"

G_BEGIN_DECLS

/**
 * GsurfAdblockStats:
 * @network: network block rules
 * @network_exceptions: network `@@` exception rules
 * @cosmetic_generic: generic element-hiding selectors
 * @cosmetic_specific: site-specific element-hiding rules
 * @procedural: procedural / style / remove cosmetic rules
 * @scriptlets: scriptlet injections
 * @redirects: `$redirect` / `$redirect-rule` rules
 * @removeparams: `$removeparam` rules
 * @unsupported: well-formed filters this engine cannot honour
 * @invalid: malformed filters
 *
 * Per-category filter counts after compilation.
 */
typedef struct {
	guint network;
	guint network_exceptions;
	guint cosmetic_generic;
	guint cosmetic_specific;
	guint procedural;
	guint scriptlets;
	guint redirects;
	guint removeparams;
	guint unsupported;
	guint invalid;
} GsurfAdblockStats;

/**
 * GsurfAdblockRuleset:
 * @id: identifier suffix ("net", "cosmetic-generic", "cosmetic-specific")
 * @json: WebKit content-blocker JSON
 * @rules: number of WebKit rules in @json
 *
 * One compiled content-blocker ruleset.
 */
typedef struct {
	gchar *id;
	gchar *json;
	guint  rules;
} GsurfAdblockRuleset;

/**
 * GsurfAdblockCompileFlags:
 * @GSURF_ADBLOCK_COMPILE_NONE: content-blocker output only
 * @GSURF_ADBLOCK_COMPILE_COSMETIC: compile element-hiding filters
 * @GSURF_ADBLOCK_COMPILE_SCRIPTLETS: compile `+js()` scriptlets
 * @GSURF_ADBLOCK_COMPILE_REDIRECTS: compile `$redirect` / `$removeparam`
 * @GSURF_ADBLOCK_COMPILE_WEB_PROCESS: the web-process extension is
 *   available. Scriptlets, procedural cosmetics, redirects and removeparam
 *   all need it; without it redirects degrade to plain blocks and the rest
 *   is skipped.
 */
typedef enum {
	GSURF_ADBLOCK_COMPILE_NONE        = 0,
	GSURF_ADBLOCK_COMPILE_COSMETIC    = 1 << 0,
	GSURF_ADBLOCK_COMPILE_SCRIPTLETS  = 1 << 1,
	GSURF_ADBLOCK_COMPILE_REDIRECTS   = 1 << 2,
	GSURF_ADBLOCK_COMPILE_WEB_PROCESS = 1 << 3
} GsurfAdblockCompileFlags;

/**
 * GsurfAdblockResult:
 * @rulesets: (element-type GsurfAdblockRuleset): non-empty rulesets
 * @matcher: redirect/removeparam rules (possibly empty)
 * @page_data: (nullable): scriptlet and cosmetic tables for the web-process
 *   extension, or %NULL when there are none
 * @stats: filter counts
 *
 * Everything one compilation produced.
 */
typedef struct {
	GPtrArray           *rulesets;
	GsurfAdblockMatcher *matcher;
	GVariant            *page_data;
	GsurfAdblockStats    stats;
} GsurfAdblockResult;

typedef struct _GsurfAdblockCompiler GsurfAdblockCompiler;

/**
 * gsurf_adblock_compiler_new:
 * @scriptlet_library: (nullable): the scriptlet library source (see
 *   js/scriptlets.js); %NULL disables scriptlets
 * @cosmetic_engine: (nullable): the procedural cosmetic engine source (see
 *   js/cosmetic.js); %NULL disables procedural cosmetics
 *
 * Returns: (transfer full): a new compiler
 */
GsurfAdblockCompiler *gsurf_adblock_compiler_new(const gchar *scriptlet_library,
                                                 const gchar *cosmetic_engine);

/**
 * gsurf_adblock_compiler_free:
 * @self: (nullable): a compiler
 */
void gsurf_adblock_compiler_free(GsurfAdblockCompiler *self);

/**
 * gsurf_adblock_compiler_add_text:
 * @self: a compiler
 * @text: filter list text
 * @base_dir: (nullable): directory `!#include` names are resolved in, or
 *   %NULL to ignore includes
 * @trusted: whether the list may use `trusted-*` scriptlets
 *
 * Parses a filter list.
 */
void gsurf_adblock_compiler_add_text(GsurfAdblockCompiler *self, const gchar *text,
                                     const gchar *base_dir, gboolean trusted);

/**
 * gsurf_adblock_compiler_add_file:
 * @self: a compiler
 * @path: a filter list file
 * @trusted: whether the list may use `trusted-*` scriptlets
 * @error: return location for a read error
 *
 * Parses a filter list file; `!#include` names resolve next to it.
 *
 * Returns: %TRUE when the file was read
 */
gboolean gsurf_adblock_compiler_add_file(GsurfAdblockCompiler *self, const gchar *path,
                                         gboolean trusted, GError **error);

/**
 * gsurf_adblock_compiler_add_allowed_site:
 * @self: a compiler
 * @host: a normalized host; it and its subdomains are never filtered
 */
void gsurf_adblock_compiler_add_allowed_site(GsurfAdblockCompiler *self, const gchar *host);

/**
 * gsurf_adblock_compiler_compile:
 * @self: a compiler
 * @flags: #GsurfAdblockCompileFlags
 *
 * Compiles everything added so far. The compiler can be freed afterwards.
 *
 * Returns: (transfer full): the result
 */
GsurfAdblockResult *gsurf_adblock_compiler_compile(GsurfAdblockCompiler *self,
                                                   GsurfAdblockCompileFlags flags);

/**
 * gsurf_adblock_result_free:
 * @result: (nullable): a result
 */
void gsurf_adblock_result_free(GsurfAdblockResult *result);

/**
 * gsurf_adblock_pattern_to_regex:
 * @pattern: an ABP network pattern (without options), e.g. "||ads.com^"
 * @token: (out) (optional) (transfer full): the best index token, or ""
 *
 * Converts a pattern into the regex subset WebKit's content blocker
 * accepts (which is also valid PCRE).
 *
 * Returns: (transfer full) (nullable): the regex, or %NULL when the
 *   pattern cannot be expressed
 */
gchar *gsurf_adblock_pattern_to_regex(const gchar *pattern, gchar **token);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfAdblockCompiler, gsurf_adblock_compiler_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfAdblockResult, gsurf_adblock_result_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfAdblockMatcher, gsurf_adblock_matcher_free)

G_END_DECLS

#endif /* GSURF_ADBLOCK_FILTERS_H */
