/*
 * gsurf-config.c - YAML-backed configuration
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "config/gsurf-config.h"

#include <yaml-glib.h>
#include <stdio.h>
#include <string.h>

G_DEFINE_TYPE(GsurfConfig, gsurf_config, G_TYPE_OBJECT)

static GsurfConfig *default_config = NULL;

/* Reassemble "ctrl+SHIFT+r" into the canonical "Ctrl+Shift+r" so config
 * and dispatch agree regardless of order/case. The key token keeps its
 * original spelling (a GDK keyval name such as "r", "slash", "Return"). */
static gchar *
normalize_keystring(const gchar *s)
{
	gchar **parts;
	gboolean ctrl = FALSE, alt = FALSE, shift = FALSE, super = FALSE;
	const gchar *key = NULL;
	GString *out;
	guint i;

	if (s == NULL || *s == '\0')
		return NULL;

	parts = g_strsplit(s, "+", -1);
	for (i = 0; parts[i] != NULL; i++) {
		gchar *tok = g_strstrip(parts[i]);

		if (*tok == '\0')
			continue;
		if (g_ascii_strcasecmp(tok, "ctrl") == 0 ||
		    g_ascii_strcasecmp(tok, "control") == 0)
			ctrl = TRUE;
		else if (g_ascii_strcasecmp(tok, "alt") == 0 ||
		         g_ascii_strcasecmp(tok, "mod1") == 0)
			alt = TRUE;
		else if (g_ascii_strcasecmp(tok, "shift") == 0)
			shift = TRUE;
		else if (g_ascii_strcasecmp(tok, "super") == 0 ||
		         g_ascii_strcasecmp(tok, "logo") == 0 ||
		         g_ascii_strcasecmp(tok, "mod4") == 0)
			super = TRUE;
		else
			key = tok;
	}

	out = g_string_new(NULL);
	if (ctrl)  g_string_append(out, "Ctrl+");
	if (alt)   g_string_append(out, "Alt+");
	if (shift) g_string_append(out, "Shift+");
	if (super) g_string_append(out, "Super+");
	if (key != NULL)
		g_string_append(out, key);

	g_strfreev(parts);
	return g_string_free(out, FALSE);
}

static void
parse_webkit_settings(GsurfSettings *s, YamlMapping *w)
{
	if (yaml_mapping_has_member(w, "javascript"))
		s->javascript = yaml_mapping_get_boolean_member(w, "javascript");
	if (yaml_mapping_has_member(w, "images"))
		s->images = yaml_mapping_get_boolean_member(w, "images");
	if (yaml_mapping_has_member(w, "webgl"))
		s->webgl = yaml_mapping_get_boolean_member(w, "webgl");
	if (yaml_mapping_has_member(w, "webaudio"))
		s->webaudio = yaml_mapping_get_boolean_member(w, "webaudio");
	if (yaml_mapping_has_member(w, "media"))
		s->media = yaml_mapping_get_boolean_member(w, "media");
	if (yaml_mapping_has_member(w, "media_stream"))
		s->media_stream = yaml_mapping_get_boolean_member(w, "media_stream");
	if (yaml_mapping_has_member(w, "webrtc"))
		s->webrtc = yaml_mapping_get_boolean_member(w, "webrtc");
	if (yaml_mapping_has_member(w, "plugins"))
		s->plugins = yaml_mapping_get_boolean_member(w, "plugins");
	if (yaml_mapping_has_member(w, "smooth_scrolling"))
		s->smooth_scrolling = yaml_mapping_get_boolean_member(w, "smooth_scrolling");
	if (yaml_mapping_has_member(w, "caret_browsing"))
		s->caret_browsing = yaml_mapping_get_boolean_member(w, "caret_browsing");
	if (yaml_mapping_has_member(w, "dns_prefetch"))
		s->dns_prefetch = yaml_mapping_get_boolean_member(w, "dns_prefetch");
	if (yaml_mapping_has_member(w, "hyperlink_auditing"))
		s->hyperlink_auditing = yaml_mapping_get_boolean_member(w, "hyperlink_auditing");
	if (yaml_mapping_has_member(w, "site_quirks"))
		s->site_quirks = yaml_mapping_get_boolean_member(w, "site_quirks");
	if (yaml_mapping_has_member(w, "developer_extras"))
		s->developer_extras = yaml_mapping_get_boolean_member(w, "developer_extras");
	if (yaml_mapping_has_member(w, "javascript_can_open_windows"))
		s->js_can_open_windows = yaml_mapping_get_boolean_member(w, "javascript_can_open_windows");
	if (yaml_mapping_has_member(w, "javascript_can_access_clipboard"))
		s->js_can_access_clipboard = yaml_mapping_get_boolean_member(w, "javascript_can_access_clipboard");
	if (yaml_mapping_has_member(w, "default_font_size"))
		s->default_font_size = (guint)yaml_mapping_get_int_member(w, "default_font_size");
	if (yaml_mapping_has_member(w, "default_monospace_font_size"))
		s->default_monospace_font_size = (guint)yaml_mapping_get_int_member(w, "default_monospace_font_size");
	if (yaml_mapping_has_member(w, "default_charset"))
		gsurf_settings_set_default_charset(s, yaml_mapping_get_string_member(w, "default_charset"));
}

static void
parse_keybinds(GsurfConfig *self, YamlMapping *kb)
{
	GList *keys, *l;

	keys = yaml_mapping_get_members(kb);
	for (l = keys; l != NULL; l = l->next) {
		const gchar *keystr = l->data;
		const gchar *action_str = yaml_mapping_get_string_member(kb, keystr);
		GsurfAction action;
		gchar *norm;

		if (action_str == NULL)
			continue;

		action = gsurf_action_from_string(action_str);
		if (action == GSURF_ACTION_NONE)
			continue;

		norm = normalize_keystring(keystr);
		if (norm != NULL)
			g_hash_table_replace(self->keybinds, norm,
				GUINT_TO_POINTER(action));
	}
	g_list_free(keys);
}

static void
parse_root(GsurfConfig *self, YamlMapping *root)
{
	YamlMapping *m;

	/* browser: */
	m = yaml_mapping_get_mapping_member(root, "browser");
	if (m != NULL) {
		if (yaml_mapping_has_member(m, "homepage")) {
			g_free(self->homepage);
			self->homepage = g_strdup(yaml_mapping_get_string_member(m, "homepage"));
		}
		if (yaml_mapping_has_member(m, "new_view_uri")) {
			g_free(self->new_view_uri);
			self->new_view_uri = g_strdup(yaml_mapping_get_string_member(m, "new_view_uri"));
		}
		if (yaml_mapping_has_member(m, "default_zoom"))
			self->default_zoom = yaml_mapping_get_double_member(m, "default_zoom");
		if (yaml_mapping_has_member(m, "smooth_scroll"))
			self->smooth_scroll = yaml_mapping_get_boolean_member(m, "smooth_scroll");
		if (yaml_mapping_has_member(m, "fullscreen_on_start"))
			self->fullscreen_on_start = yaml_mapping_get_boolean_member(m, "fullscreen_on_start");
	}

	/* user_agent: (top level) */
	if (yaml_mapping_has_member(root, "user_agent")) {
		g_free(self->user_agent);
		self->user_agent = g_strdup(yaml_mapping_get_string_member(root, "user_agent"));
		gsurf_settings_set_user_agent(self->settings, self->user_agent);
	}

	/* webkit: */
	m = yaml_mapping_get_mapping_member(root, "webkit");
	if (m != NULL)
		parse_webkit_settings(self->settings, m);

	/* window: */
	m = yaml_mapping_get_mapping_member(root, "window");
	if (m != NULL) {
		if (yaml_mapping_has_member(m, "title")) {
			g_free(self->window_title);
			self->window_title = g_strdup(yaml_mapping_get_string_member(m, "title"));
		}
		if (yaml_mapping_has_member(m, "geometry")) {
			const gchar *geo = yaml_mapping_get_string_member(m, "geometry");
			gint w = 0, h = 0;
			if (geo != NULL && sscanf(geo, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
				self->window_width = w;
				self->window_height = h;
			}
		}
	}

	/* keybinds: */
	m = yaml_mapping_get_mapping_member(root, "keybinds");
	if (m != NULL)
		parse_keybinds(self, m);

	/* modules: (retained for module code to parse its own section) */
	m = yaml_mapping_get_mapping_member(root, "modules");
	if (m != NULL) {
		g_clear_pointer(&self->modules_mapping, yaml_mapping_unref);
		self->modules_mapping = yaml_mapping_ref(m);
	}
}

static gboolean
load_document(GsurfConfig *self, YamlDocument *doc, GError **error)
{
	YamlNode *root;
	YamlMapping *map;

	root = yaml_document_get_root(doc);
	if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			"config root is not a mapping");
		return FALSE;
	}

	map = yaml_node_get_mapping(root);
	parse_root(self, map);
	return TRUE;
}

gboolean
gsurf_config_load_from_file(GsurfConfig *self, const gchar *path, GError **error)
{
	g_autoptr(YamlParser) parser = NULL;
	YamlDocument *doc;
	gboolean ok;

	g_return_val_if_fail(GSURF_IS_CONFIG(self), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_file(parser, path, error))
		return FALSE;

	doc = yaml_parser_get_document(parser, 0);
	if (doc == NULL)
		return FALSE;

	ok = load_document(self, doc, error);
	if (ok) {
		g_free(self->config_path);
		self->config_path = g_strdup(path);
	}
	return ok;
}

gboolean
gsurf_config_load_from_data(GsurfConfig *self, const gchar *data, gssize length, GError **error)
{
	g_autoptr(YamlParser) parser = NULL;
	YamlDocument *doc;
	gboolean ok;

	g_return_val_if_fail(GSURF_IS_CONFIG(self), FALSE);
	g_return_val_if_fail(data != NULL, FALSE);

	if (length < 0)
		length = (gssize)strlen(data);

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, data, length, error))
		return FALSE;

	doc = yaml_parser_get_document(parser, 0);
	if (doc == NULL)
		return FALSE;

	ok = load_document(self, doc, error);
	return ok;
}

gchar *
gsurf_config_find_default_path(void)
{
	const gchar *xdg = g_get_user_config_dir();
	gchar *path;

	path = g_build_filename(xdg, "gsurf", "config.yaml", NULL);
	if (g_file_test(path, G_FILE_TEST_EXISTS))
		return path;
	g_free(path);

	path = g_build_filename(GSURF_SYSCONFDIR, "gsurf", "config.yaml", NULL);
	if (g_file_test(path, G_FILE_TEST_EXISTS))
		return path;
	g_free(path);

	return NULL;
}

GsurfAction
gsurf_config_get_keybind_action(GsurfConfig *self, const gchar *keystring)
{
	gpointer val;

	g_return_val_if_fail(GSURF_IS_CONFIG(self), GSURF_ACTION_NONE);

	if (keystring == NULL)
		return GSURF_ACTION_NONE;

	val = g_hash_table_lookup(self->keybinds, keystring);
	return val != NULL ? (GsurfAction)GPOINTER_TO_UINT(val) : GSURF_ACTION_NONE;
}

gpointer
gsurf_config_get_module_node(GsurfConfig *self, const gchar *module_name)
{
	g_return_val_if_fail(GSURF_IS_CONFIG(self), NULL);

	if (self->modules_mapping == NULL || module_name == NULL)
		return NULL;

	return yaml_mapping_get_member((YamlMapping *)self->modules_mapping, module_name);
}

GsurfConfig *
gsurf_config_get_default(void)
{
	return default_config;
}

void
gsurf_config_set_default(GsurfConfig *config)
{
	g_set_object(&default_config, config);
}

static void
gsurf_config_finalize(GObject *object)
{
	GsurfConfig *self = GSURF_CONFIG(object);

	g_clear_pointer(&self->homepage, g_free);
	g_clear_pointer(&self->new_view_uri, g_free);
	g_clear_pointer(&self->user_agent, g_free);
	g_clear_pointer(&self->window_title, g_free);
	g_clear_pointer(&self->config_path, g_free);
	g_clear_pointer(&self->keybinds, g_hash_table_unref);
	g_clear_pointer(&self->modules_mapping, yaml_mapping_unref);
	g_clear_object(&self->settings);

	if (default_config == self)
		default_config = NULL;

	G_OBJECT_CLASS(gsurf_config_parent_class)->finalize(object);
}

static void
gsurf_config_class_init(GsurfConfigClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gsurf_config_finalize;
}

static void
gsurf_config_init(GsurfConfig *self)
{
	self->homepage = g_strdup(GSURF_DEFAULT_HOMEPAGE);
	self->new_view_uri = g_strdup(GSURF_DEFAULT_HOMEPAGE);
	self->user_agent = NULL;
	self->window_title = g_strdup("gsurf");
	self->window_width = GSURF_DEFAULT_WIDTH;
	self->window_height = GSURF_DEFAULT_HEIGHT;
	self->default_zoom = GSURF_DEFAULT_ZOOM;
	self->smooth_scroll = TRUE;
	self->fullscreen_on_start = FALSE;

	self->settings = gsurf_settings_new();
	self->keybinds = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->modules_mapping = NULL;
	self->config_path = NULL;
}

GsurfConfig *
gsurf_config_new(void)
{
	return g_object_new(GSURF_TYPE_CONFIG, NULL);
}
