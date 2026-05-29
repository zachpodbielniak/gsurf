/*
 * gsurf - Default C Configuration
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Install: cp default-config.c ~/.config/gsurf/config.c
 * Auto-compile: gsurf (compiles with content-hash caching via crispy)
 *
 * The CRISPY_PARAMS define below is optional. If present, the config
 * compiler extracts it and passes the value as extra flags to gcc.
 * Remove or modify it if you need custom include paths or libraries.
 */

/* #define CRISPY_PARAMS "-I/custom/path -lmylib" */

#include <gsurf/gsurf.h>

/*
 * gsurf_config_init:
 *
 * Entry point called after the YAML config is loaded but before modules
 * are activated. Any values set here override the YAML.
 *
 * The GsurfConfig singleton is available via gsurf_config_get_default().
 * You have full access to all GObject APIs and all gsurf types.
 *
 * Returns: TRUE on success, FALSE to fall back to YAML-only config.
 */
G_MODULE_EXPORT gboolean
gsurf_config_init(void)
{
	GsurfConfig *config;

	config = gsurf_config_get_default();
	if (config == NULL)
		return FALSE;

	/* --- Browser --- */
	/* g_free(config->homepage); */
	/* config->homepage = g_strdup("https://duckduckgo.com"); */
	/* config->default_zoom = 1.25; */

	/* --- Window --- */
	/* config->window_width = 1280; */
	/* config->window_height = 800; */
	/* g_free(config->window_title); config->window_title = g_strdup("gsurf"); */

	/* --- Web engine defaults (GsurfSettings) --- */
	/* config->settings->javascript = TRUE; */
	/* config->settings->webgl = FALSE; */
	/* gsurf_settings_set_user_agent(config->settings, "MyAgent/1.0"); */

	/* --- Keybinds (normalized "Mods+Key" -> action nick) --- */
	/* g_hash_table_replace(config->keybinds, g_strdup("Ctrl+m"),
	 *     GUINT_TO_POINTER(GSURF_ACTION_RELOAD)); */

	/* --- Module options are configured via YAML; see
	 *     --generate-yaml-config and gsurf_config_get_module_node(). --- */

	(void)config;
	return TRUE;
}
