/*
 * gsurf-keybind-help.c - Dynamic keybinding help row
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gsurf-keybind-help.h"

#include <string.h>

G_DEFINE_BOXED_TYPE(GsurfKeybindHelp, gsurf_keybind_help,
	gsurf_keybind_help_copy, gsurf_keybind_help_free)

GsurfKeybindHelp *
gsurf_keybind_help_new(const gchar *key,
                       const gchar *description,
                       const gchar *source,
                       const gchar *action)
{
	GsurfKeybindHelp *help;

	help = g_new0(GsurfKeybindHelp, 1);
	help->key = g_strdup(key);
	help->description = g_strdup(description);
	help->source = g_strdup(source);
	help->action = g_strdup(action);
	return help;
}

GsurfKeybindHelp *
gsurf_keybind_help_copy(const GsurfKeybindHelp *help)
{
	g_return_val_if_fail(help != NULL, NULL);

	return gsurf_keybind_help_new(help->key, help->description,
		help->source, help->action);
}

void
gsurf_keybind_help_free(GsurfKeybindHelp *help)
{
	if (help == NULL)
		return;

	g_free(help->key);
	g_free(help->description);
	g_free(help->source);
	g_free(help->action);
	g_free(help);
}

const gchar *
gsurf_keybind_help_get_key(const GsurfKeybindHelp *help)
{
	g_return_val_if_fail(help != NULL, NULL);
	return help->key;
}

const gchar *
gsurf_keybind_help_get_description(const GsurfKeybindHelp *help)
{
	g_return_val_if_fail(help != NULL, NULL);
	return help->description;
}

const gchar *
gsurf_keybind_help_get_source(const GsurfKeybindHelp *help)
{
	g_return_val_if_fail(help != NULL, NULL);
	return help->source;
}

const gchar *
gsurf_keybind_help_get_action(const GsurfKeybindHelp *help)
{
	g_return_val_if_fail(help != NULL, NULL);
	return help->action;
}

void
gsurf_keybind_help_set_key(GsurfKeybindHelp *help, const gchar *key)
{
	g_return_if_fail(help != NULL);
	g_free(help->key);
	help->key = g_strdup(key);
}

void
gsurf_keybind_help_set_description(GsurfKeybindHelp *help, const gchar *description)
{
	g_return_if_fail(help != NULL);
	g_free(help->description);
	help->description = g_strdup(description);
}

void
gsurf_keybind_help_set_source(GsurfKeybindHelp *help, const gchar *source)
{
	g_return_if_fail(help != NULL);
	g_free(help->source);
	help->source = g_strdup(source);
}

void
gsurf_keybind_help_set_action(GsurfKeybindHelp *help, const gchar *action)
{
	g_return_if_fail(help != NULL);
	g_free(help->action);
	help->action = g_strdup(action);
}

gchar *
gsurf_keybind_help_pretty_key(const gchar *key)
{
	const gchar *plus;
	const gchar *token;
	const gchar *pretty;
	gsize prefix_len;

	if (key == NULL || *key == '\0')
		return NULL;

	plus = strrchr(key, '+');
	token = (plus != NULL) ? plus + 1 : key;
	prefix_len = (gsize)(token - key);

	if (g_strcmp0(token, "question") == 0)
		pretty = "?";
	else if (g_strcmp0(token, "slash") == 0)
		pretty = "/";
	else if (g_strcmp0(token, "plus") == 0)
		pretty = "+";
	else if (g_strcmp0(token, "minus") == 0)
		pretty = "-";
	else if (g_strcmp0(token, "equal") == 0)
		pretty = "=";
	else if (g_strcmp0(token, "period") == 0)
		pretty = ".";
	else if (g_strcmp0(token, "comma") == 0)
		pretty = ",";
	else if (g_strcmp0(token, "space") == 0)
		pretty = "Space";
	else
		return g_strdup(key);

	if (prefix_len == 0)
		return g_strdup(pretty);
	return g_strdup_printf("%.*s%s", (int)prefix_len, key, pretty);
}

void
gsurf_keybind_help_append(GPtrArray *entries,
                          const gchar *key,
                          const gchar *description,
                          const gchar *source,
                          const gchar *action)
{
	if (entries == NULL || key == NULL || *key == '\0')
		return;

	g_ptr_array_add(entries,
		gsurf_keybind_help_new(key, description, source, action));
}
