/*
 * gsurf-config.h - YAML-backed configuration
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GsurfConfig loads ~/.config/gsurf/config.yaml (via yaml-glib) into a
 * public struct of core browser settings plus a #GsurfSettings of web
 * engine defaults. The raw `modules:` mapping is retained so module
 * code can read its own options. Mirrors gst's GstConfig (public
 * struct, singleton accessor, YAML loader).
 */

#ifndef GSURF_CONFIG_H
#define GSURF_CONFIG_H

#include <glib-object.h>

#include "gsurf-types.h"
#include "gsurf-enums.h"
#include "config/gsurf-settings.h"

G_BEGIN_DECLS

#define GSURF_TYPE_CONFIG (gsurf_config_get_type())

G_DECLARE_FINAL_TYPE(GsurfConfig, gsurf_config, GSURF, CONFIG, GObject)

/**
 * GsurfConfig:
 * @homepage: startup URI when none is given on the CLI
 * @new_view_uri: URI for newly opened views/tabs
 * @user_agent: user agent override ("" = engine default)
 * @window_title: default window title
 * @window_width: default window width
 * @window_height: default window height
 * @default_zoom: default zoom level (1.0 = 100%)
 * @smooth_scroll: enable smooth scrolling
 * @fullscreen_on_start: start fullscreen
 * @settings: web engine default settings (#GsurfSettings)
 * @keybinds: (element-type utf8 gint): normalized "Mods+Key" -> #GsurfAction
 *
 * Public configuration struct. Fields are read directly by the rest of
 * the library and by C configs (gsurf_config_init()).
 */
struct _GsurfConfig
{
	GObject parent_instance;

	gchar    *homepage;
	gchar    *new_view_uri;
	gchar    *user_agent;
	gchar    *window_title;
	gint      window_width;
	gint      window_height;
	gdouble   default_zoom;
	gboolean  smooth_scroll;
	gboolean  fullscreen_on_start;

	GsurfSettings *settings;

	GHashTable *keybinds;   /* gchar* -> GUINT_TO_POINTER(GsurfAction) */
	GHashTable *mousebinds; /* gchar* ("Mods+ButtonN") -> GsurfAction */

	/* Storage / network / TLS. */
	gchar    *cookie_jar;       /* persistent cookie store path, or NULL */
	gchar    *data_dir;
	gchar    *cache_dir;
	gboolean  enable_disk_cache;
	gchar    *proxy_mode;       /* "default" | "none" | "custom" */
	gchar    *proxy_uri;
	gboolean  do_not_track;
	gboolean  tls_strict;
	gboolean  ephemeral;        /* private browsing (no persistent storage) */

	/*< private >*/
	gchar      *config_path;     /* path the config was loaded from */
	gpointer    modules_mapping; /* YamlMapping* (ref) or NULL */
};

/**
 * gsurf_config_new:
 *
 * Creates a configuration object populated with built-in defaults.
 *
 * Returns: (transfer full): a new #GsurfConfig
 */
GsurfConfig *gsurf_config_new(void);

/**
 * gsurf_config_load_from_file:
 * @self: a #GsurfConfig
 * @path: path to a YAML config file
 * @error: (out) (optional): location for an error
 *
 * Loads and merges a YAML config file over the current values.
 *
 * Returns: %TRUE on success
 */
gboolean gsurf_config_load_from_file(GsurfConfig *self, const gchar *path, GError **error);

/**
 * gsurf_config_load_from_data:
 * @self: a #GsurfConfig
 * @data: YAML text
 * @length: length of @data, or -1 if NUL-terminated
 * @error: (out) (optional): location for an error
 *
 * Loads and merges YAML text over the current values.
 *
 * Returns: %TRUE on success
 */
gboolean gsurf_config_load_from_data(GsurfConfig *self, const gchar *data, gssize length, GError **error);

/**
 * gsurf_config_find_default_path:
 *
 * Searches the standard locations for a config file (XDG config, then
 * the system config dir).
 *
 * Returns: (transfer full) (nullable): the path, or %NULL if none found
 */
gchar *gsurf_config_find_default_path(void);

/**
 * gsurf_config_get_keybind_action:
 * @self: a #GsurfConfig
 * @keystring: a normalized "Mods+Key" string
 *
 * Returns: the bound #GsurfAction, or %GSURF_ACTION_NONE
 */
GsurfAction gsurf_config_get_keybind_action(GsurfConfig *self, const gchar *keystring);

/**
 * gsurf_config_get_mousebind_action:
 * @self: a #GsurfConfig
 * @binding: a normalized "Mods+ButtonN" string
 *
 * Returns: the bound #GsurfAction, or %GSURF_ACTION_NONE
 */
GsurfAction gsurf_config_get_mousebind_action(GsurfConfig *self, const gchar *binding);

/**
 * gsurf_config_get_module_node:
 * @self: a #GsurfConfig
 * @module_name: the module name
 *
 * Returns the raw YAML mapping node for a module's config section under
 * `modules:`, for the module to parse its own options.
 *
 * Returns: (transfer none) (nullable): a YamlNode*, or %NULL
 */
gpointer gsurf_config_get_module_node(GsurfConfig *self, const gchar *module_name);

/**
 * gsurf_config_get_default:
 *
 * Returns: (transfer none): the process-wide default config, or %NULL if
 *   none has been set with gsurf_config_set_default().
 */
GsurfConfig *gsurf_config_get_default(void);

/**
 * gsurf_config_set_default:
 * @config: (nullable) (transfer none): the config to make default
 *
 * Sets the process-wide default config (used by C configs and modules).
 */
void gsurf_config_set_default(GsurfConfig *config);

G_END_DECLS

#endif /* GSURF_CONFIG_H */
