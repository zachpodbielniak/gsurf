/*
 * gsurf-hardening-module.c - LibreWolf-style privacy hardening
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three enforcement layers, one configuration:
 *   - The hardening web-process extension (webext/) rewrites requests in
 *     WebKit's "send-request": https-only upgrades, tracking-parameter
 *     stripping, cross-origin Referer trimming, `Sec-GPC` / `DNT`
 *     headers, cancelled prefetches and pings, and
 *     `navigator.globalPrivacyControl` in the page. The module hands it
 *     the policy with gsurf_web_process_extension_register().
 *   - #GsurfSettingProvider forces WebRTC (and the DNS-prefetch setting,
 *     a no-op in WebKitGTK >= 2.48) off for every navigation (and has the last word: high priority value, so
 *     it runs after uri_params and friends).
 *   - The container registry's session policy: third-party cookies and
 *     intelligent tracking prevention, on the default session and every
 *     container session (gsurf_container_registry_set_policy()).
 * `exempt:` rules switch named per-site features off for pages whose
 * TOP-LEVEL host matches (LibreWolf's site exceptions). The decision
 * logic is shared with the extension: gsurf-hardening-rules.c.
 */

#define _GNU_SOURCE
#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <dlfcn.h>
#include "gsurf-hardening-rules.h"

/* Where the web-process extension is built/installed, relative to the
 * directory holding hardening.so. */
#define GSURF_HARDENING_WEBEXT_SUBDIR "hardening-webext"
#define GSURF_HARDENING_WEBEXT_NAME   "gsurf-hardening-webext.so"

/* The name the extension's data is registered (and looked up) under. */
#define GSURF_HARDENING_WEBEXT_KEY "hardening"

/* Run the setting provider after every default-priority module. */
#define GSURF_HARDENING_PRIORITY 1000

/* WebKit's own session defaults, restored when hardening lets go. */
#define WEBKIT_DEFAULT_ITP           FALSE
#define WEBKIT_DEFAULT_COOKIE_POLICY 2   /* no third-party */

G_MODULE_EXPORT GType gsurf_module_register(void);

#define GSURF_TYPE_HARDENING_MODULE (gsurf_hardening_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfHardeningModule, gsurf_hardening_module,
                     GSURF, HARDENING_MODULE, GsurfModule)

struct _GsurfHardeningModule
{
	GsurfModule           parent_instance;
	GsurfHardeningPolicy *policy;           /* per-site features + exemptions */
	gboolean              enabled;          /* the last configure() saw enabled: true */
	gboolean              itp;
	gint                  cookie_policy;    /* -1 leave, 0 always, 1 never, 2 no 3rd party */
	gchar                *webext_path;      /* NULL = locate next to hardening.so */
	gboolean              webext_registered;
	gboolean              policy_applied;   /* we changed the registry's policy */
};

static void gsurf_hardening_settings_init(GsurfSettingProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfHardeningModule, gsurf_hardening_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SETTING_PROVIDER, gsurf_hardening_settings_init))

/* The tracking parameters stripped when `strip_params:` is not given. */
static const gchar *const default_strip_params[] = {
	"utm_*", "fbclid", "gclid", "dclid", "mc_eid", "igshid", "_hsenc", "yclid", "msclkid",
	NULL
};

/* --- setting provider --- */

/* Runs at the start of every navigation with the URI about to load,
 * which is the top-level page: exemptions apply by its host. */
static void
gsurf_hardening_apply_settings(GsurfSettingProvider *provider, GsurfView *view,
                               GsurfSettings *settings, const gchar *uri)
{
	GsurfHardeningModule *self = GSURF_HARDENING_MODULE(provider);
	g_autofree gchar *host = NULL;
	guint features;

	if (!self->enabled || settings == NULL)
		return;
	host = gsurf_host_from_uri(uri);
	features = gsurf_hardening_policy_features_for_host(self->policy, host);
	if (features & GSURF_HARDENING_WEBRTC)
		settings->webrtc = FALSE;
	if (features & GSURF_HARDENING_NO_PREFETCH) {
		settings->dns_prefetch = FALSE;
		/* Hyperlink auditing has no working engine switch any more
		 * (see docs/hardening.org); the extension cancels the pings.
		 * Keep the field truthful for backends that do honour it. */
		settings->hyperlink_auditing = FALSE;
	}
}

static void
gsurf_hardening_settings_init(GsurfSettingProviderInterface *iface)
{
	iface->apply_settings = gsurf_hardening_apply_settings;
}

/* --- web-process extension --- */

/* The extension ships next to hardening.so ("hardening-webext/"),
 * wherever the module was loaded from: the build tree, an install, an
 * AppImage. */
static gchar *
find_webext(GsurfHardeningModule *self)
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
	path = g_build_filename(dir, GSURF_HARDENING_WEBEXT_SUBDIR, GSURF_HARDENING_WEBEXT_NAME, NULL);
	if (!g_path_is_absolute(path)) {
		g_autofree gchar *cwd = gsurf_get_launch_dir();
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

/* Undo everything a previous configure() put in force outside the
 * module: the extension registration and the session policy. */
static void
release_effects(GsurfHardeningModule *self)
{
	if (self->webext_registered)
		gsurf_web_process_extension_unregister(GSURF_HARDENING_WEBEXT_KEY);
	self->webext_registered = FALSE;
	if (self->policy_applied)
		gsurf_container_registry_set_policy(gsurf_container_registry_get_default(),
			WEBKIT_DEFAULT_ITP, WEBKIT_DEFAULT_COOKIE_POLICY);
	self->policy_applied = FALSE;
}

/* --- configuration --- */

static gboolean
mapping_bool(YamlMapping *m, const gchar *key, gboolean fallback)
{
	if (m == NULL || !yaml_mapping_has_member(m, key))
		return fallback;
	return yaml_mapping_get_boolean_member(m, key);
}

static gchar *
expand_path(const gchar *path)
{
	if (path == NULL || *path == '\0')
		return NULL;
	/* "~", "~/" and relative paths (against the launch directory). */
	return gsurf_resolve_path(path);
}

/* `strip_params:` — a sequence of glob names; `[]` strips nothing. */
static gchar **
parse_strip_params(YamlMapping *m)
{
	YamlSequence *seq;
	GPtrArray *out;
	guint i, n;

	if (m == NULL || !yaml_mapping_has_member(m, "strip_params"))
		return g_strdupv((gchar **)default_strip_params);
	seq = yaml_mapping_get_sequence_member(m, "strip_params");
	if (seq == NULL) {
		g_warning("gsurf hardening: strip_params must be a list of parameter names; "
			"using the defaults");
		return g_strdupv((gchar **)default_strip_params);
	}
	out = g_ptr_array_new();
	n = yaml_sequence_get_length(seq);
	for (i = 0; i < n; i++) {
		YamlNode *e = yaml_sequence_get_element(seq, i);
		const gchar *name;

		if (e == NULL || yaml_node_get_node_type(e) != YAML_NODE_SCALAR ||
		    (name = yaml_node_get_string(e)) == NULL || *name == '\0') {
			g_warning("gsurf hardening: strip_params[%u] is not a parameter name; ignored", i);
			continue;
		}
		g_ptr_array_add(out, g_strdup(name));
	}
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/* `referer:` — off | trim_cross_origin | strip_cross_origin (or false). */
static GsurfHardeningReferer
parse_referer(YamlMapping *m)
{
	YamlNode *node;
	GsurfHardeningReferer policy = GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN;
	const gchar *name;

	if (m == NULL || (node = yaml_mapping_get_member(m, "referer")) == NULL)
		return policy;
	name = yaml_node_get_node_type(node) == YAML_NODE_SCALAR ? yaml_node_get_string(node) : NULL;
	/* YAML's `false` / `no` read as "off" too. */
	if (name != NULL && (g_ascii_strcasecmp(name, "false") == 0 ||
	                     g_ascii_strcasecmp(name, "no") == 0))
		return GSURF_HARDENING_REFERER_OFF;
	if (!gsurf_hardening_referer_from_name(name, &policy)) {
		g_warning("gsurf hardening: unknown referer policy '%s' (want off, "
			"trim_cross_origin or strip_cross_origin); using trim_cross_origin",
			name != NULL ? name : "(not a string)");
		policy = GSURF_HARDENING_REFERER_TRIM_CROSS_ORIGIN;
	}
	return policy;
}

/* `third_party_cookies:` — allow | block | block_all (or a boolean:
 * true = block). Returns the registry's cookie policy number. */
static gint
parse_cookie_policy(YamlMapping *m)
{
	YamlNode *node;
	const gchar *name;

	if (m == NULL || (node = yaml_mapping_get_member(m, "third_party_cookies")) == NULL)
		return 2;
	name = yaml_node_get_node_type(node) == YAML_NODE_SCALAR ? yaml_node_get_string(node) : NULL;
	if (name != NULL) {
		if (g_ascii_strcasecmp(name, "block") == 0 || g_ascii_strcasecmp(name, "true") == 0)
			return 2;
		if (g_ascii_strcasecmp(name, "allow") == 0 || g_ascii_strcasecmp(name, "false") == 0)
			return 0;
		if (g_ascii_strcasecmp(name, "block_all") == 0)
			return 1;
	}
	g_warning("gsurf hardening: unknown third_party_cookies value '%s' (want allow, "
		"block or block_all); using block", name != NULL ? name : "(not a string)");
	return 2;
}

/* Join an array of names for a message. */
static gchar *
join_names(GPtrArray *names)
{
	g_ptr_array_add(names, NULL);
	return g_strjoinv(", ", (gchar **)names->pdata);
}

/* `exempt:` — host rules whose data carries `skip:`. All or nothing: a
 * bad pattern drops the whole list (with a warning) rather than leaving
 * a half-applied set of exceptions. Unknown `skip:` names are reported
 * and ignored. Returns the a(sa{sv}) rules, or NULL. */
static GVariant *
parse_exemptions(YamlMapping *m)
{
	g_autoptr(GsurfHostRules) rules = NULL;
	g_autoptr(GError) error = NULL;
	YamlNode *node;
	GVariant *variant;
	GVariantIter it;
	const gchar *pattern;
	GVariant *data;
	guint index = 0;

	if (m == NULL || (node = yaml_mapping_get_member(m, "exempt")) == NULL)
		return NULL;
	rules = gsurf_host_rules_new();
	if (!gsurf_host_rules_load(rules, node, "match", &error)) {
		g_warning("gsurf hardening: exempt: %s; no site exceptions are in force",
			error->message);
		return NULL;
	}

	/* Validate every rule's `skip:` names, with the extension's parser. */
	variant = g_variant_ref_sink(gsurf_host_rules_to_variant(rules));
	g_variant_iter_init(&it, variant);
	while (g_variant_iter_next(&it, "(&s@a{sv})", &pattern, &data)) {
		g_autoptr(GVariant) skip = g_variant_lookup_value(data, "skip", NULL);
		g_autoptr(GPtrArray) unknown = g_ptr_array_new_with_free_func(g_free);
		g_autoptr(GPtrArray) session_wide = g_ptr_array_new_with_free_func(g_free);
		guint mask = gsurf_hardening_skip_mask(skip, unknown, session_wide);

		if (unknown->len > 0) {
			g_autofree gchar *names = join_names(unknown);

			g_warning("gsurf hardening: exempt[%u] ('%s'): unknown skip name(s) %s; "
				"known: https_only, strip_params, referer, gpc, dnt, no_prefetch, webrtc, all",
				index, pattern, names);
		}
		if (session_wide->len > 0) {
			g_autofree gchar *names = join_names(session_wide);

			g_warning("gsurf hardening: exempt[%u] ('%s'): %s apply to whole sessions "
				"and cannot be exempted per site; ignored", index, pattern, names);
		}
		if (mask == 0)
			g_warning("gsurf hardening: exempt[%u] ('%s') skips nothing", index, pattern);
		g_variant_unref(data);
		index++;
	}
	return variant;
}

static void
gsurf_hardening_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfHardeningModule *self = GSURF_HARDENING_MODULE(module);
	GsurfConfig *config = config_ptr;
	g_autofree gchar *webext = NULL;
	YamlNode *node;
	YamlMapping *m = NULL;
	GsurfHardeningPolicy *policy;

	/* Start from a clean slate: a reconfigure (including one to a config
	 * without a hardening section) replaces everything. */
	release_effects(self);
	g_clear_pointer(&self->webext_path, g_free);

	node = config != NULL ? gsurf_config_get_module_node(config, "hardening") : NULL;
	if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING)
		m = yaml_node_get_mapping(node);

	/* `enabled:` is normally applied by the manager before configure();
	 * honour it here too so a later configure() can switch it. */
	self->enabled = mapping_bool(m, "enabled", gsurf_module_get_enabled(module));
	if (m != NULL && yaml_mapping_has_member(m, "enabled"))
		gsurf_module_set_enabled(module, self->enabled);

	/* Per-site features (defaults: LibreWolf-like). DNT falls back to the
	 * core `network.do_not_track` key when the module does not set it. */
	policy = gsurf_hardening_policy_new();
	policy->https_only = mapping_bool(m, "https_only", TRUE);
	policy->https_only_local = mapping_bool(m, "https_only_local", FALSE);
	g_strfreev(policy->strip_params);
	policy->strip_params = parse_strip_params(m);
	policy->referer = parse_referer(m);
	policy->gpc = mapping_bool(m, "gpc", TRUE);
	policy->dnt = mapping_bool(m, "dnt", config != NULL && config->do_not_track);
	policy->no_prefetch = mapping_bool(m, "no_prefetch", TRUE);
	policy->webrtc_off = !mapping_bool(m, "webrtc", FALSE);
	gsurf_hardening_policy_set_exemptions(policy, parse_exemptions(m));
	g_clear_pointer(&self->policy, gsurf_hardening_policy_free);
	self->policy = policy;

	/* Session-wide protections. */
	self->itp = mapping_bool(m, "itp", TRUE);
	self->cookie_policy = parse_cookie_policy(m);
	if (m != NULL && yaml_mapping_has_member(m, "web_process_extension"))
		self->webext_path = expand_path(
			yaml_mapping_get_string_member(m, "web_process_extension"));

	if (!self->enabled)
		return;

	/* The session policy reaches existing and future sessions. */
	gsurf_container_registry_set_policy(gsurf_container_registry_get_default(),
		self->itp, self->cookie_policy);
	self->policy_applied = TRUE;

	/* The request-level features need the extension in every web
	 * process started from now on. */
	webext = find_webext(self);
	if (webext == NULL) {
		g_message("gsurf hardening: web-process extension not found next to hardening.so; "
			"https_only, strip_params, referer, gpc, dnt and prefetch blocking are disabled");
		return;
	}
	gsurf_web_process_extension_register(GSURF_HARDENING_WEBEXT_KEY, webext,
		gsurf_hardening_policy_to_variant(self->policy));
	self->webext_registered = TRUE;
}

static gboolean gsurf_hardening_activate(GsurfModule *m) { return TRUE; }

static const gchar *gsurf_hardening_get_name(GsurfModule *m) { return "hardening"; }
static const gchar *gsurf_hardening_get_description(GsurfModule *m)
{
	return "LibreWolf-style privacy hardening with per-site exemptions";
}

static void
gsurf_hardening_module_finalize(GObject *object)
{
	GsurfHardeningModule *self = GSURF_HARDENING_MODULE(object);

	release_effects(self);
	g_clear_pointer(&self->policy, gsurf_hardening_policy_free);
	g_clear_pointer(&self->webext_path, g_free);

	G_OBJECT_CLASS(gsurf_hardening_module_parent_class)->finalize(object);
}

static void
gsurf_hardening_module_class_init(GsurfHardeningModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_hardening_module_finalize;
	module_class->activate = gsurf_hardening_activate;
	module_class->get_name = gsurf_hardening_get_name;
	module_class->get_description = gsurf_hardening_get_description;
	module_class->configure = gsurf_hardening_configure;
}

static void
gsurf_hardening_module_init(GsurfHardeningModule *self)
{
	self->policy = gsurf_hardening_policy_new();
	self->cookie_policy = -1;
	gsurf_module_set_priority(GSURF_MODULE(self), GSURF_HARDENING_PRIORITY);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_HARDENING_MODULE;
}
