/*
 * gsurf-dark-mode-module.c - Dark mode with per-site rules
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Dark Reader-like dark mode. Four modes: off, native (the page's own
 * dark theme through color-scheme and prefers-color-scheme), filter (a
 * CSS invert filter with brightness/contrast/sepia/grayscale, media
 * re-inverted) and dynamic (the vendored upstream Dark Reader engine).
 * Config rules choose a mode and adjustments per host; a runtime per-site
 * toggle is persisted to $XDG_DATA_HOME/gsurf/dark_mode/sites.yaml and a
 * global toggle switches everything off and on again.
 *
 * Implements:
 *   - #GsurfScriptInjector: one document-start script per view, in the
 *     isolated world "gsurf-dark-mode", carrying the page engine
 *     (js/dark-mode.js) and the current state as JSON. inject() runs once
 *     per view without a URI, so the page decides per host itself.
 *   - #GsurfActionHandler: dark-mode-toggle and dark-mode-toggle-site.
 *   - #GsurfInputHandler / #GsurfKeybindProvider: key_toggle and
 *     key_toggle_site.
 *
 * Runtime changes reach open views twice: as an appended document-start
 * script (new documents start from the new state with no flash) and by
 * evaluating it once in the current main frame (the open page changes
 * at once). See docs/dark-mode.org.
 */

#define G_LOG_DOMAIN "gsurf-dark-mode"

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <glib/gstdio.h>
#include <yaml-glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "gsurf-dark-mode-resources.h"

/* The isolated script world and the message handler the engine posts to. */
#define GSURF_DARK_MODE_WORLD              "gsurf-dark-mode"
#define GSURF_DARK_MODE_MESSAGE            "gsurf_dark_mode"
/* Keep in step with DARKREADER_VERSION in the module Makefile. */
#define GSURF_DARK_MODE_DARKREADER_VERSION "4.9.133"

#define GSURF_DARK_MODE_RES_ENGINE         "/org/gsurf/dark_mode/dark-mode.js"
#define GSURF_DARK_MODE_RES_DARKREADER     "/org/gsurf/dark_mode/darkreader.js"

/* Ctrl+Shift+d stays with the bookmarks module. */
#define GSURF_DARK_MODE_KEY_TOGGLE         "Ctrl+Alt+d"
#define GSURF_DARK_MODE_KEY_TOGGLE_SITE    "Ctrl+Alt+Shift+d"

/* Mode used when a site is toggled on but neither its rule nor the
 * global mode darkens anything (off, or legacy auto). */
#define GSURF_DARK_MODE_FALLBACK           "filter"

/**
 * DarkMode:
 * @DARK_MODE_UNSET: (rules only) keep the global mode
 * @DARK_MODE_OFF: leave the page alone
 * @DARK_MODE_NATIVE: color-scheme dark + prefers-color-scheme rewriting
 * @DARK_MODE_FILTER: CSS invert filter
 * @DARK_MODE_DYNAMIC: the vendored Dark Reader engine
 */
typedef enum {
	DARK_MODE_UNSET = -1,
	DARK_MODE_OFF = 0,
	DARK_MODE_NATIVE,
	DARK_MODE_FILTER,
	DARK_MODE_DYNAMIC
} DarkMode;

static const gchar *const mode_names[] = { "off", "native", "filter", "dynamic" };

/* The four colour adjustments, in percent, with their valid range and
 * the value that leaves the page unchanged. */
#define N_ADJUST 4
static const struct {
	const gchar *key;
	gint         min;
	gint         max;
	gint         neutral;
} adjust_keys[N_ADJUST] = {
	{ "brightness", 0, 200, 100 },
	{ "contrast",   0, 200, 100 },
	{ "sepia",      0, 100, 0 },
	{ "grayscale",  0, 100, 0 },
};

/*
 * DarkSettings:
 *
 * The global settings or one site rule. In a rule, DARK_MODE_UNSET and
 * an adjustment of -1 mean "inherit the global value".
 * follow_system marks native mode with the scheme "light dark" (legacy
 * mode: auto), which follows the system preference instead of forcing
 * dark.
 */
typedef struct {
	gint     mode;
	gboolean follow_system;
	gint     adjust[N_ADJUST];
} DarkSettings;

#define GSURF_TYPE_DARK_MODE_MODULE (gsurf_dark_mode_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfDarkModeModule, gsurf_dark_mode_module,
                     GSURF, DARK_MODE_MODULE, GsurfModule)

struct _GsurfDarkModeModule
{
	GsurfModule      parent_instance;

	/* Configuration (replaced as a whole by configure()). */
	DarkSettings     global;
	GsurfHostRules  *rules;          /* site rule patterns */
	GArray          *rule_settings;  /* DarkSettings, same index as rules */
	gchar           *key_toggle;     /* normalized, NULL = unbound */
	gchar           *key_toggle_site;

	/* Runtime state. */
	gboolean         enabled;        /* the global switch, not persisted */
	GHashTable      *overrides;      /* host -> GINT_TO_POINTER(dark) */
	gchar           *state_file;
	gboolean         state_file_bad; /* unparsable: kept aside on save */
	guint64          version;        /* bumped on every state change */

	/* Views we injected into (weak; removed on finalization). */
	GHashTable      *views;

	/* Assets, loaded from the GResource on first use. */
	gchar           *engine_js;
	gchar           *darkreader_js;
};

enum {
	PROP_0,
	PROP_ENABLED,
	PROP_STATE_JSON,
	PROP_STATE_FILE,
	PROP_PRELOAD_ENGINE,
	PROP_KEY_TOGGLE,
	PROP_KEY_TOGGLE_SITE,
	N_PROPS
};

static GParamSpec *props[N_PROPS];

static void gsurf_dark_mode_injector_init(GsurfScriptInjectorInterface *iface);
static void gsurf_dark_mode_action_init(GsurfActionHandlerInterface *iface);
static void gsurf_dark_mode_input_init(GsurfInputHandlerInterface *iface);
static void gsurf_dark_mode_keybind_init(GsurfKeybindProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfDarkModeModule, gsurf_dark_mode_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SCRIPT_INJECTOR, gsurf_dark_mode_injector_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_ACTION_HANDLER, gsurf_dark_mode_action_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_dark_mode_input_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_KEYBIND_PROVIDER, gsurf_dark_mode_keybind_init))

/* ------------------------------------------------------------------ */
/* Settings helpers                                                    */
/* ------------------------------------------------------------------ */

/* Global defaults: legacy "mode: auto" (native, light dark), neutral
 * adjustments. A missing mode key keeps the module's historical
 * behaviour. */
static void
settings_init_global(DarkSettings *s)
{
	guint i;

	s->mode = DARK_MODE_NATIVE;
	s->follow_system = TRUE;
	for (i = 0; i < N_ADJUST; i++)
		s->adjust[i] = adjust_keys[i].neutral;
}

/* A rule inherits everything until it sets a key. */
static void
settings_init_rule(DarkSettings *s)
{
	guint i;

	s->mode = DARK_MODE_UNSET;
	s->follow_system = FALSE;
	for (i = 0; i < N_ADJUST; i++)
		s->adjust[i] = -1;
}

/* Whether @mode actually darkens a page (legacy auto only follows the
 * system preference). Mirrors is_dark() in js/dark-mode.js. */
static gboolean
mode_is_dark(gint mode, gboolean follow_system)
{
	return mode > DARK_MODE_OFF && !(mode == DARK_MODE_NATIVE && follow_system);
}

/* Lowercase host without IPv6 brackets or trailing dots, matching
 * normalize_host() in the page engine; NULL when nothing is left. */
static gchar *
normalize_host(const gchar *host)
{
	g_autofree gchar *lower = NULL;
	gsize len;

	if (host == NULL)
		return NULL;
	lower = g_ascii_strdown(host, -1);
	len = strlen(lower);
	if (len >= 2 && lower[0] == '[' && lower[len - 1] == ']') {
		memmove(lower, lower + 1, len - 2);
		lower[len - 2] = '\0';
		len -= 2;
	}
	while (len > 0 && lower[len - 1] == '.')
		lower[--len] = '\0';
	if (len == 0)
		return NULL;
	return g_steal_pointer(&lower);
}

/* Parse a YAML boolean scalar (true/false, yes/no, on/off, 1/0). */
static gboolean
parse_bool(const gchar *text, gboolean *out)
{
	static const gchar *const yes[] = { "true", "yes", "on", "1" };
	static const gchar *const no[] = { "false", "no", "off", "0" };
	guint i;

	if (text == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(yes); i++) {
		if (g_ascii_strcasecmp(text, yes[i]) == 0) {
			*out = TRUE;
			return TRUE;
		}
		if (g_ascii_strcasecmp(text, no[i]) == 0) {
			*out = FALSE;
			return TRUE;
		}
	}
	return FALSE;
}

/* The raw text of a scalar node, or NULL for any other node shape. */
static const gchar *
scalar_text(YamlNode *node)
{
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

/*
 * parse_mode:
 * @where: "dark_mode" or "dark_mode.sites[N]" for messages
 * @node: the mode value
 * @force_css: legacy force_dark_css (maps "dark" to filter)
 * @out: (out): the settings to update (mode and follow_system)
 *
 * Accepts off | native | filter | dynamic and the legacy names
 * dark (native, or filter with force_dark_css), auto (native following
 * the system scheme) and light (off). Warns and leaves @out untouched
 * on anything else.
 */
static void
parse_mode(const gchar *where, YamlNode *node, gboolean force_css, DarkSettings *out)
{
	const gchar *text = scalar_text(node);
	guint i;

	if (text == NULL) {
		g_warning("%s.mode: expected one of off, native, filter, dynamic", where);
		return;
	}
	for (i = 0; i < G_N_ELEMENTS(mode_names); i++) {
		if (g_ascii_strcasecmp(text, mode_names[i]) == 0) {
			out->mode = (gint)i;
			out->follow_system = FALSE;
			return;
		}
	}
	/* Legacy values from before per-site rules. */
	if (g_ascii_strcasecmp(text, "dark") == 0) {
		out->mode = force_css ? DARK_MODE_FILTER : DARK_MODE_NATIVE;
		out->follow_system = FALSE;
	} else if (g_ascii_strcasecmp(text, "auto") == 0) {
		out->mode = DARK_MODE_NATIVE;
		out->follow_system = TRUE;
	} else if (g_ascii_strcasecmp(text, "light") == 0) {
		out->mode = DARK_MODE_OFF;
		out->follow_system = FALSE;
	} else {
		g_warning("%s.mode: unknown mode '%s' (expected off, native, filter or dynamic)",
			where, text);
	}
}

/* Read one adjustment: an integer percentage within its range. */
static void
parse_adjust(const gchar *where, guint index, YamlNode *node, DarkSettings *out)
{
	const gchar *text = scalar_text(node);
	gint64 value = 0;

	if (text == NULL ||
	    !g_ascii_string_to_signed(text, 10, G_MININT, G_MAXINT, &value, NULL)) {
		g_warning("%s.%s: expected an integer percentage", where, adjust_keys[index].key);
		return;
	}
	if (value < adjust_keys[index].min || value > adjust_keys[index].max) {
		g_warning("%s.%s: %" G_GINT64_FORMAT " is outside %d-%d", where,
			adjust_keys[index].key, value, adjust_keys[index].min, adjust_keys[index].max);
		return;
	}
	out->adjust[index] = (gint)value;
}

/* Read force_dark_css (legacy) before mode, which depends on it. */
static gboolean
read_force_css(const gchar *where, YamlMapping *m)
{
	YamlNode *node = yaml_mapping_get_member(m, "force_dark_css");
	gboolean value = FALSE;

	if (node == NULL)
		return FALSE;
	if (!parse_bool(scalar_text(node), &value)) {
		g_warning("%s.force_dark_css: expected a boolean", where);
		return FALSE;
	}
	return value;
}

/*
 * parse_settings:
 * @where: prefix for messages
 * @m: the mapping (dark_mode itself or one site rule)
 * @out: (inout): settings to update
 * @extra: (array zero-terminated=1): further keys allowed in @m
 *
 * Applies mode, force_dark_css, and the four adjustments from @m,
 * warning on bad values (which are skipped) and on unknown keys.
 */
static void
parse_settings(const gchar *where, YamlMapping *m, DarkSettings *out,
               const gchar *const *extra)
{
	gboolean force_css = read_force_css(where, m);
	guint i, j, n = yaml_mapping_get_size(m);

	for (i = 0; i < n; i++) {
		const gchar *key = yaml_mapping_get_key(m, i);
		YamlNode *value = yaml_mapping_get_value(m, i);
		gboolean known = FALSE;

		if (g_strcmp0(key, "mode") == 0) {
			parse_mode(where, value, force_css, out);
			continue;
		}
		if (g_strcmp0(key, "force_dark_css") == 0)
			continue;
		for (j = 0; j < N_ADJUST; j++) {
			if (g_strcmp0(key, adjust_keys[j].key) == 0) {
				parse_adjust(where, j, value, out);
				known = TRUE;
				break;
			}
		}
		if (!known && !g_strv_contains(extra, key))
			g_warning("%s: unknown key '%s'", where, key);
	}
}

/* Read a key binding; "" or null unbinds it. Returns the normalized
 * key, @fallback when the value is not a string. */
static gchar *
parse_key(const gchar *where, YamlNode *node, const gchar *fallback)
{
	const gchar *text;

	if (node == NULL)
		return gsurf_keys_normalize(fallback);
	if (yaml_node_get_node_type(node) == YAML_NODE_NULL)
		return NULL;
	text = scalar_text(node);
	if (text == NULL) {
		g_warning("%s: expected a key string such as \"%s\"", where, fallback);
		return gsurf_keys_normalize(fallback);
	}
	if (*text == '\0')
		return NULL;
	return gsurf_keys_normalize(text);
}

/*
 * parse_sites:
 * @self: the module
 * @node: the dark_mode.sites value
 *
 * Builds the rule list. Each element is a mapping with a "match" host
 * regex plus any of mode / force_dark_css / brightness / contrast /
 * sepia / grayscale, or a bare pattern string (a rule that only stops
 * later rules from matching). A bad element or regex is skipped with a
 * warning; bad values inside a good rule are dropped individually.
 */
static void
parse_sites(GsurfDarkModeModule *self, YamlNode *node)
{
	static const gchar *const rule_extra[] = { "match", NULL };
	YamlSequence *seq;
	guint i, n;

	if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
		return;
	if (yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE) {
		g_warning("dark_mode.sites: expected a list of rules");
		return;
	}
	seq = yaml_node_get_sequence(node);
	n = yaml_sequence_get_length(seq);
	for (i = 0; i < n; i++) {
		YamlNode *element = yaml_sequence_get_element(seq, i);
		g_autofree gchar *where = g_strdup_printf("dark_mode.sites[%u]", i);
		g_autoptr(GError) error = NULL;
		const gchar *pattern = NULL;
		YamlMapping *m = NULL;
		DarkSettings rule;

		settings_init_rule(&rule);

		/* 1. The rule's shape and its pattern. */
		switch (yaml_node_get_node_type(element)) {
		case YAML_NODE_SCALAR:
			pattern = yaml_node_get_scalar(element);
			break;
		case YAML_NODE_MAPPING:
			m = yaml_node_get_mapping(element);
			pattern = scalar_text(yaml_mapping_get_member(m, "match"));
			break;
		default:
			break;
		}
		if (pattern == NULL || *pattern == '\0') {
			g_warning("%s: expected a mapping with a \"match\" host pattern", where);
			continue;
		}

		/* 2. The pattern must compile (GRegex; the page uses RegExp). */
		if (!gsurf_host_rules_add(self->rules, pattern, NULL, &error)) {
			g_warning("%s: %s", where, error->message);
			continue;
		}

		/* 3. Its settings; bad values are dropped, the rule is kept. */
		if (m != NULL)
			parse_settings(where, m, &rule, rule_extra);
		g_array_append_val(self->rule_settings, rule);
	}
}

/* ------------------------------------------------------------------ */
/* State JSON (what the page engine receives)                          */
/* ------------------------------------------------------------------ */

static void
add_mode(JsonBuilder *b, gint mode, gboolean follow_system)
{
	json_builder_set_member_name(b, "mode");
	json_builder_add_string_value(b, mode_names[mode]);
	json_builder_set_member_name(b, "scheme");
	json_builder_add_string_value(b, follow_system ? "light dark" : "dark");
}

/* Build the state object described at the top of js/dark-mode.js. */
static gchar *
build_state_json(GsurfDarkModeModule *self)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GList) hosts = NULL;
	GList *l;
	guint i, j;

	json_builder_begin_object(b);

	json_builder_set_member_name(b, "version");
	json_builder_add_int_value(b, (gint64)self->version);
	json_builder_set_member_name(b, "enabled");
	json_builder_add_boolean_value(b, self->enabled);
	json_builder_set_member_name(b, "fallback");
	json_builder_add_string_value(b, GSURF_DARK_MODE_FALLBACK);

	/* Global settings: every key present. */
	json_builder_set_member_name(b, "global");
	json_builder_begin_object(b);
	add_mode(b, self->global.mode, self->global.follow_system);
	for (j = 0; j < N_ADJUST; j++) {
		json_builder_set_member_name(b, adjust_keys[j].key);
		json_builder_add_int_value(b, self->global.adjust[j]);
	}
	json_builder_end_object(b);

	/* Rules: only the keys each one sets. */
	json_builder_set_member_name(b, "rules");
	json_builder_begin_array(b);
	for (i = 0; i < self->rule_settings->len; i++) {
		DarkSettings *r = &g_array_index(self->rule_settings, DarkSettings, i);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "pattern");
		json_builder_add_string_value(b, gsurf_host_rules_get_pattern(self->rules, i));
		if (r->mode != DARK_MODE_UNSET)
			add_mode(b, r->mode, r->follow_system);
		for (j = 0; j < N_ADJUST; j++) {
			if (r->adjust[j] < 0)
				continue;
			json_builder_set_member_name(b, adjust_keys[j].key);
			json_builder_add_int_value(b, r->adjust[j]);
		}
		json_builder_end_object(b);
	}
	json_builder_end_array(b);

	/* Per-site overrides, sorted for stable output. */
	json_builder_set_member_name(b, "overrides");
	json_builder_begin_object(b);
	hosts = g_list_sort(g_hash_table_get_keys(self->overrides), (GCompareFunc)g_strcmp0);
	for (l = hosts; l != NULL; l = l->next) {
		json_builder_set_member_name(b, l->data);
		json_builder_add_boolean_value(b,
			GPOINTER_TO_INT(g_hash_table_lookup(self->overrides, l->data)));
	}
	json_builder_end_object(b);

	json_builder_end_object(b);

	root = json_builder_get_root(b);
	json_generator_set_root(gen, root);
	return json_generator_to_data(gen, NULL);
}

/* ------------------------------------------------------------------ */
/* State file: $XDG_DATA_HOME/gsurf/dark_mode/sites.yaml               */
/* ------------------------------------------------------------------ */

/*
 * load_state_file:
 *
 * Replaces the overrides with the file's "sites" mapping (host:
 * true|false). A missing file means no overrides. Loose permissions are
 * reported (the next save rewrites it 0600); an unparsable file is
 * ignored and set aside as sites.yaml.bad on the next save. Bad entries
 * are skipped individually.
 */
static void
load_state_file(GsurfDarkModeModule *self)
{
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	YamlDocument *doc;
	YamlNode *root, *sites;
	YamlMapping *map;
	guint i, n;

	g_hash_table_remove_all(self->overrides);
	self->state_file_bad = FALSE;
	if (!g_file_test(self->state_file, G_FILE_TEST_EXISTS))
		return;

	/* 1. Permissions: report, but still use the data. */
	if (!gsurf_private_file_is_private(self->state_file, &error)) {
		g_warning("%s: %s; it is rewritten with mode 0600 on the next save",
			self->state_file, error->message);
		g_clear_error(&error);
	}

	/* 2. Parse. */
	parser = yaml_parser_new();
	if (!yaml_parser_load_from_file(parser, self->state_file, &error)) {
		g_warning("%s: %s; ignoring it", self->state_file, error->message);
		self->state_file_bad = TRUE;
		return;
	}
	doc = yaml_parser_get_document(parser, 0);
	root = doc != NULL ? yaml_document_get_root(doc) : NULL;
	if (root == NULL || yaml_node_get_node_type(root) == YAML_NODE_NULL)
		return;   /* empty file */
	if (yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_warning("%s: expected a mapping with a \"sites\" member; ignoring it",
			self->state_file);
		self->state_file_bad = TRUE;
		return;
	}
	map = yaml_node_get_mapping(root);
	sites = yaml_mapping_get_member(map, "sites");
	if (sites == NULL || yaml_node_get_node_type(sites) == YAML_NODE_NULL)
		return;
	if (yaml_node_get_node_type(sites) != YAML_NODE_MAPPING) {
		g_warning("%s: \"sites\" must map host names to true or false; ignoring it",
			self->state_file);
		self->state_file_bad = TRUE;
		return;
	}

	/* 3. Entries. */
	map = yaml_node_get_mapping(sites);
	n = yaml_mapping_get_size(map);
	for (i = 0; i < n; i++) {
		g_autofree gchar *host = normalize_host(yaml_mapping_get_key(map, i));
		gboolean dark = FALSE;

		if (host == NULL) {
			g_warning("%s: empty host name skipped", self->state_file);
			continue;
		}
		if (!parse_bool(scalar_text(yaml_mapping_get_value(map, i)), &dark)) {
			g_warning("%s: %s: expected true or false", self->state_file, host);
			continue;
		}
		g_hash_table_replace(self->overrides, g_steal_pointer(&host), GINT_TO_POINTER(dark));
	}
}

/* Quote @text as a YAML double-quoted scalar (hosts are plain ASCII;
 * quotes, backslashes and control characters are escaped or dropped). */
static void
append_quoted(GString *out, const gchar *text)
{
	const gchar *p;

	g_string_append_c(out, '"');
	for (p = text; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');
		if ((guchar)*p >= 0x20)
			g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');
}

/* Write the overrides atomically with mode 0600 (directory 0700). */
static gboolean
save_state_file(GsurfDarkModeModule *self)
{
	g_autoptr(GString) out = g_string_new(NULL);
	g_autoptr(GList) hosts = NULL;
	g_autoptr(GError) error = NULL;
	GList *l;

	/* Keep an unparsable file the user may want to recover. */
	if (self->state_file_bad) {
		g_autofree gchar *bad = g_strconcat(self->state_file, ".bad", NULL);

		if (g_rename(self->state_file, bad) == 0)
			g_message("dark_mode: kept the unreadable state file as %s", bad);
		self->state_file_bad = FALSE;
	}

	g_string_append(out,
		"# gsurf dark_mode per-site overrides, written by dark-mode-toggle-site.\n"
		"# true: always dark, false: never dark. Overrides the config's\n"
		"# dark_mode.sites rules; see docs/dark-mode.org.\n"
		"version: 1\n");
	hosts = g_list_sort(g_hash_table_get_keys(self->overrides), (GCompareFunc)g_strcmp0);
	if (hosts == NULL)
		g_string_append(out, "sites: {}\n");
	else
		g_string_append(out, "sites:\n");
	for (l = hosts; l != NULL; l = l->next) {
		g_string_append(out, "  ");
		append_quoted(out, l->data);
		g_string_append_printf(out, ": %s\n",
			GPOINTER_TO_INT(g_hash_table_lookup(self->overrides, l->data)) ? "true" : "false");
	}

	if (!gsurf_private_write(self->state_file, out->str, (gssize)out->len, &error)) {
		g_warning("%s: %s", self->state_file, error->message);
		return FALSE;
	}
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Assets and views                                                    */
/* ------------------------------------------------------------------ */

/* Load a compiled-in asset as a NUL-terminated string. */
static gchar *
load_resource(const gchar *path)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = g_resource_lookup_data(gsurf_dark_mode_res_get_resource(),
		path, G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
	gsize size = 0;
	const gchar *data;

	if (bytes == NULL) {
		g_critical("dark_mode: missing built-in asset %s: %s", path, error->message);
		return g_strdup("");
	}
	data = g_bytes_get_data(bytes, &size);
	return g_strndup(data, size);
}

static const gchar *
engine_source(GsurfDarkModeModule *self)
{
	if (self->engine_js == NULL)
		self->engine_js = load_resource(GSURF_DARK_MODE_RES_ENGINE);
	return self->engine_js;
}

static const gchar *
darkreader_source(GsurfDarkModeModule *self)
{
	if (self->darkreader_js == NULL)
		self->darkreader_js = load_resource(GSURF_DARK_MODE_RES_DARKREADER);
	return self->darkreader_js;
}

/* Dark Reader is preloaded into every frame only when the global mode
 * is dynamic: then every page needs it. Sites that reach dynamic mode
 * through a rule or an override load it on demand (main frame). */
static gboolean
preload_engine(GsurfDarkModeModule *self)
{
	return self->global.mode == DARK_MODE_DYNAMIC;
}

static void
on_view_finalized(gpointer data, GObject *where_the_view_was)
{
	GsurfDarkModeModule *self = data;

	g_hash_table_remove(self->views, where_the_view_was);
}

/* Log (at debug level) a failed live update: the page may be between
 * documents, or have no engine (about:blank before the first load). */
static void
on_eval_done(GObject *source, GAsyncResult *result, gpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *value = gsurf_view_run_javascript_in_world_finish(
		GSURF_VIEW(source), result, &error);

	(void)data;
	if (error != NULL)
		g_debug("dark_mode: live update failed: %s", error->message);
}

/* Deliver the current state to every open view: for documents loaded
 * later (appended document-start script) and for the current one. */
static void
push_state(GsurfDarkModeModule *self)
{
	g_autofree gchar *json = NULL;
	g_autofree gchar *script = NULL;
	GHashTableIter iter;
	gpointer key;

	if (g_hash_table_size(self->views) == 0)
		return;
	json = build_state_json(self);
	script = g_strdup_printf(
		"globalThis.__gsurf_dark !== undefined && __gsurf_dark.set_state(%s), void 0;", json);
	g_hash_table_iter_init(&iter, self->views);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		GsurfView *view = key;

		gsurf_view_add_user_script_in_world(view, script, FALSE, NULL,
			GSURF_DARK_MODE_WORLD);
		gsurf_view_run_javascript_in_world_async(view, script, GSURF_DARK_MODE_WORLD,
			NULL, on_eval_done, NULL);
	}
}

/* The engine asks for Dark Reader when it decides "dynamic" but the
 * bundle was not preloaded: load it into the view's main frame and let
 * the engine re-apply. Only our isolated world can post here. */
static void
on_script_message(GsurfView *view, const gchar *name, const gchar *json, gpointer data)
{
	GsurfDarkModeModule *self = data;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *script = NULL;
	JsonNode *root;
	JsonObject *obj;

	(void)name;
	if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL))
		return;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return;
	obj = json_node_get_object(root);
	if (g_strcmp0(json_object_get_string_member_with_default(obj, "type", ""),
	              "need-engine") != 0)
		return;

	script = g_strconcat(darkreader_source(self),
		"\n;globalThis.__gsurf_dark !== undefined && __gsurf_dark.engine_ready(), void 0;\n",
		NULL);
	gsurf_view_run_javascript_in_world_async(view, script, GSURF_DARK_MODE_WORLD,
		NULL, on_eval_done, NULL);
}

/* ------------------------------------------------------------------ */
/* Toggles                                                             */
/* ------------------------------------------------------------------ */

/* Whether config alone (rules, else global) darkens @host. */
static gboolean
configured_dark(GsurfDarkModeModule *self, const gchar *host)
{
	gint index = gsurf_host_rules_find(self->rules, host);

	if (index >= 0) {
		DarkSettings *r = &g_array_index(self->rule_settings, DarkSettings, index);

		if (r->mode != DARK_MODE_UNSET)
			return mode_is_dark(r->mode, r->follow_system);
	}
	return mode_is_dark(self->global.mode, self->global.follow_system);
}

static void
set_enabled(GsurfDarkModeModule *self, gboolean enabled)
{
	enabled = !!enabled;
	if (self->enabled == enabled)
		return;
	self->enabled = enabled;
	self->version++;
	push_state(self);
	g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ENABLED]);
	g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATE_JSON]);
}

/*
 * toggle_site:
 *
 * Flips @view's top-level host between "never dark" and dark (its rule's
 * mode, else the global mode, else filter). An override equal to what
 * the config already does is dropped instead of stored, so the state
 * file only lists real exceptions.
 */
static void
toggle_site(GsurfDarkModeModule *self, GsurfView *view)
{
	g_autofree gchar *raw = gsurf_host_from_uri(gsurf_view_get_uri(view));
	g_autofree gchar *host = normalize_host(raw);
	gboolean configured, current, want;
	gpointer value = NULL;

	if (host == NULL) {
		g_message("gsurf: dark mode: this page has no host to toggle");
		return;
	}
	configured = configured_dark(self, host);
	current = g_hash_table_lookup_extended(self->overrides, host, NULL, &value)
		? GPOINTER_TO_INT(value) : configured;
	want = !current;
	if (want == configured)
		g_hash_table_remove(self->overrides, host);
	else
		g_hash_table_replace(self->overrides, g_strdup(host), GINT_TO_POINTER(want));

	self->version++;
	save_state_file(self);
	push_state(self);
	g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATE_JSON]);
	g_message("gsurf: dark mode %s for %s%s", want ? "on" : "off", host,
		self->enabled ? "" : " (dark mode is switched off globally)");
}

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

static void
gsurf_dark_mode_inject(GsurfScriptInjector *injector, GsurfView *view, const gchar *uri)
{
	GsurfDarkModeModule *self = GSURF_DARK_MODE_MODULE(injector);
	g_autofree gchar *json = NULL;
	g_autofree gchar *script = NULL;

	(void)uri;
	if (view == NULL || g_hash_table_contains(self->views, view))
		return;

	/* Track the view for live updates until it is finalized. */
	g_hash_table_add(self->views, view);
	g_object_weak_ref(G_OBJECT(view), on_view_finalized, self);

	/* On-demand Dark Reader requests from our world. */
	gsurf_view_register_script_message(view, GSURF_DARK_MODE_MESSAGE, GSURF_DARK_MODE_WORLD);
	g_signal_connect_object(view, "script-message::" GSURF_DARK_MODE_MESSAGE,
		G_CALLBACK(on_script_message), self, 0);

	/* Dark Reader first (when every page needs it), then the engine with
	 * the current state baked in. */
	if (preload_engine(self))
		gsurf_view_add_user_script_in_world(view, darkreader_source(self), FALSE, NULL,
			GSURF_DARK_MODE_WORLD);
	json = build_state_json(self);
	script = g_strdup_printf("%s\n__gsurf_dark.init(%s);\n", engine_source(self), json);
	gsurf_view_add_user_script_in_world(view, script, FALSE, NULL, GSURF_DARK_MODE_WORLD);
}

static void
gsurf_dark_mode_injector_init(GsurfScriptInjectorInterface *iface)
{
	iface->inject = gsurf_dark_mode_inject;
}

static gboolean
gsurf_dark_mode_handle_action(GsurfActionHandler *handler, GsurfView *view,
                              GsurfAction action)
{
	GsurfDarkModeModule *self = GSURF_DARK_MODE_MODULE(handler);

	switch (action) {
	case GSURF_ACTION_DARK_MODE_TOGGLE:
		set_enabled(self, !self->enabled);
		g_message("gsurf: dark mode %s", self->enabled ? "on" : "off");
		return TRUE;
	case GSURF_ACTION_DARK_MODE_TOGGLE_SITE:
		if (view == NULL)
			return FALSE;
		toggle_site(self, view);
		return TRUE;
	default:
		return FALSE;
	}
}

static void
gsurf_dark_mode_action_init(GsurfActionHandlerInterface *iface)
{
	iface->handle_action = gsurf_dark_mode_handle_action;
}

static gboolean
gsurf_dark_mode_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                                 guint keyval, guint keycode, guint state,
                                 GsurfModePolicy mode)
{
	GsurfDarkModeModule *self = GSURF_DARK_MODE_MODULE(handler);

	(void)keycode;
	(void)mode;
	if (self->key_toggle != NULL && gsurf_keys_match(keyval, state, self->key_toggle))
		return gsurf_dark_mode_handle_action(GSURF_ACTION_HANDLER(self), view,
			GSURF_ACTION_DARK_MODE_TOGGLE);
	if (self->key_toggle_site != NULL && gsurf_keys_match(keyval, state, self->key_toggle_site))
		return gsurf_dark_mode_handle_action(GSURF_ACTION_HANDLER(self), view,
			GSURF_ACTION_DARK_MODE_TOGGLE_SITE);
	return FALSE;
}

static void
gsurf_dark_mode_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_dark_mode_handle_key_event;
}

static void
gsurf_dark_mode_list_keybinds(GsurfKeybindProvider *provider, GPtrArray *entries)
{
	GsurfDarkModeModule *self = GSURF_DARK_MODE_MODULE(provider);

	gsurf_keybind_help_append(entries, self->key_toggle,
		"Toggle dark mode everywhere", "dark_mode", "dark-mode-toggle");
	gsurf_keybind_help_append(entries, self->key_toggle_site,
		"Toggle dark mode for this site", "dark_mode", "dark-mode-toggle-site");
}

static void
gsurf_dark_mode_keybind_init(GsurfKeybindProviderInterface *iface)
{
	iface->list_keybinds = gsurf_dark_mode_list_keybinds;
}

/* ------------------------------------------------------------------ */
/* GsurfModule                                                         */
/* ------------------------------------------------------------------ */

static const gchar *gsurf_dark_mode_get_name(GsurfModule *m) { return "dark_mode"; }
static const gchar *gsurf_dark_mode_get_description(GsurfModule *m)
{
	return "Dark mode (native, filter or Dark Reader) with per-site rules";
}

/*
 * configure:
 *
 * Rebuilds the whole configuration from defaults, so a second call
 * (or a config without a dark_mode section) leaves no earlier rule or
 * key behind. The state file is re-read; the global switch keeps its
 * runtime value. Open views receive the new state.
 */
static void
gsurf_dark_mode_configure(GsurfModule *module, gpointer config_ptr)
{
	static const gchar *const top_extra[] = {
		"enabled", "key_toggle", "key_toggle_site", "sites", NULL
	};
	GsurfDarkModeModule *self = GSURF_DARK_MODE_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node = NULL;

	/* 1. Back to defaults. */
	settings_init_global(&self->global);
	g_clear_object(&self->rules);
	self->rules = gsurf_host_rules_new();
	g_array_set_size(self->rule_settings, 0);
	g_free(self->key_toggle);
	self->key_toggle = gsurf_keys_normalize(GSURF_DARK_MODE_KEY_TOGGLE);
	g_free(self->key_toggle_site);
	self->key_toggle_site = gsurf_keys_normalize(GSURF_DARK_MODE_KEY_TOGGLE_SITE);

	/* 2. The dark_mode section. */
	if (config != NULL)
		node = gsurf_config_get_module_node(config, "dark_mode");
	if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING) {
		YamlMapping *m = yaml_node_get_mapping(node);

		parse_settings("dark_mode", m, &self->global, top_extra);
		g_free(self->key_toggle);
		self->key_toggle = parse_key("dark_mode.key_toggle",
			yaml_mapping_get_member(m, "key_toggle"), GSURF_DARK_MODE_KEY_TOGGLE);
		g_free(self->key_toggle_site);
		self->key_toggle_site = parse_key("dark_mode.key_toggle_site",
			yaml_mapping_get_member(m, "key_toggle_site"), GSURF_DARK_MODE_KEY_TOGGLE_SITE);
		parse_sites(self, yaml_mapping_get_member(m, "sites"));
	} else if (node != NULL && yaml_node_get_node_type(node) != YAML_NODE_NULL) {
		g_warning("dark_mode: expected a mapping; using the defaults");
	}

	/* 3. Persisted per-site overrides win over the rules above. The
	 * path is resolved here so a changed XDG_DATA_HOME is honoured. */
	g_free(self->state_file);
	self->state_file = g_build_filename(g_get_user_data_dir(), "gsurf", "dark_mode",
		"sites.yaml", NULL);
	load_state_file(self);

	/* 4. Hand the new state to open views. */
	self->version++;
	push_state(self);
	g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATE_JSON]);
}

static gboolean gsurf_dark_mode_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_dark_mode_module_get_property(GObject *object, guint prop_id, GValue *value,
                                    GParamSpec *pspec)
{
	GsurfDarkModeModule *self = GSURF_DARK_MODE_MODULE(object);

	switch (prop_id) {
	case PROP_ENABLED:
		g_value_set_boolean(value, self->enabled);
		break;
	case PROP_STATE_JSON:
		g_value_take_string(value, build_state_json(self));
		break;
	case PROP_STATE_FILE:
		g_value_set_string(value, self->state_file);
		break;
	case PROP_PRELOAD_ENGINE:
		g_value_set_boolean(value, preload_engine(self));
		break;
	case PROP_KEY_TOGGLE:
		g_value_set_string(value, self->key_toggle);
		break;
	case PROP_KEY_TOGGLE_SITE:
		g_value_set_string(value, self->key_toggle_site);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gsurf_dark_mode_module_set_property(GObject *object, guint prop_id, const GValue *value,
                                    GParamSpec *pspec)
{
	GsurfDarkModeModule *self = GSURF_DARK_MODE_MODULE(object);

	switch (prop_id) {
	case PROP_ENABLED:
		set_enabled(self, g_value_get_boolean(value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gsurf_dark_mode_module_finalize(GObject *object)
{
	GsurfDarkModeModule *self = GSURF_DARK_MODE_MODULE(object);
	GHashTableIter iter;
	gpointer key;

	/* Stop watching views that outlive the module. */
	g_hash_table_iter_init(&iter, self->views);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_object_weak_unref(G_OBJECT(key), on_view_finalized, self);
	g_clear_pointer(&self->views, g_hash_table_unref);

	g_clear_object(&self->rules);
	g_clear_pointer(&self->rule_settings, g_array_unref);
	g_clear_pointer(&self->overrides, g_hash_table_unref);
	g_clear_pointer(&self->key_toggle, g_free);
	g_clear_pointer(&self->key_toggle_site, g_free);
	g_clear_pointer(&self->state_file, g_free);
	g_clear_pointer(&self->engine_js, g_free);
	g_clear_pointer(&self->darkreader_js, g_free);
	G_OBJECT_CLASS(gsurf_dark_mode_module_parent_class)->finalize(object);
}

static void
gsurf_dark_mode_module_class_init(GsurfDarkModeModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->get_property = gsurf_dark_mode_module_get_property;
	object_class->set_property = gsurf_dark_mode_module_set_property;
	object_class->finalize = gsurf_dark_mode_module_finalize;
	module_class->activate = gsurf_dark_mode_activate;
	module_class->get_name = gsurf_dark_mode_get_name;
	module_class->get_description = gsurf_dark_mode_get_description;
	module_class->configure = gsurf_dark_mode_configure;

	/* The compiled-in assets (once per process; the module stays loaded
	 * for as long as its type is registered). */
	gsurf_dark_mode_res_register_resource();

	/**
	 * GsurfDarkModeModule:global-on:
	 *
	 * The global switch (dark-mode-toggle). %FALSE turns every page off
	 * regardless of rules and overrides. Runtime only; starts %TRUE.
	 */
	props[PROP_ENABLED] = g_param_spec_boolean("global-on", NULL, NULL, TRUE,
		G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfDarkModeModule:state-json:
	 *
	 * The state handed to the page engine, as JSON (for tests and
	 * debugging; see docs/dark-mode.org).
	 */
	props[PROP_STATE_JSON] = g_param_spec_string("state-json", NULL, NULL, NULL,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfDarkModeModule:state-file:
	 *
	 * Path of the per-site override file.
	 */
	props[PROP_STATE_FILE] = g_param_spec_string("state-file", NULL, NULL, NULL,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfDarkModeModule:preload-engine:
	 *
	 * Whether new views get Dark Reader in every frame at document start
	 * (global mode dynamic).
	 */
	props[PROP_PRELOAD_ENGINE] = g_param_spec_boolean("preload-engine", NULL, NULL, FALSE,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfDarkModeModule:key-toggle:
	 *
	 * The normalized global toggle key, or %NULL when unbound.
	 */
	props[PROP_KEY_TOGGLE] = g_param_spec_string("key-toggle", NULL, NULL, NULL,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfDarkModeModule:key-toggle-site:
	 *
	 * The normalized per-site toggle key, or %NULL when unbound.
	 */
	props[PROP_KEY_TOGGLE_SITE] = g_param_spec_string("key-toggle-site", NULL, NULL, NULL,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(object_class, N_PROPS, props);
}

static void
gsurf_dark_mode_module_init(GsurfDarkModeModule *self)
{
	settings_init_global(&self->global);
	self->rules = gsurf_host_rules_new();
	self->rule_settings = g_array_new(FALSE, TRUE, sizeof(DarkSettings));
	self->key_toggle = gsurf_keys_normalize(GSURF_DARK_MODE_KEY_TOGGLE);
	self->key_toggle_site = gsurf_keys_normalize(GSURF_DARK_MODE_KEY_TOGGLE_SITE);
	self->enabled = TRUE;
	self->overrides = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->state_file = g_build_filename(g_get_user_data_dir(), "gsurf", "dark_mode",
		"sites.yaml", NULL);
	self->views = g_hash_table_new(g_direct_hash, g_direct_equal);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_DARK_MODE_MODULE;
}
