/*
 * gsurf-keybind-help.h - One row in the dynamic keybinding help overlay
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Describes a currently-active keybinding for the `?` help menu: the
 * key string, a human-readable description, the source (`core` or a
 * module name), and the action nick. Built at display time so the list
 * tracks loaded modules and live config.
 */

#ifndef GSURF_KEYBIND_HELP_H
#define GSURF_KEYBIND_HELP_H

#include <glib-object.h>
#include "../gsurf-types.h"

G_BEGIN_DECLS

#define GSURF_TYPE_KEYBIND_HELP (gsurf_keybind_help_get_type())

/**
 * GsurfKeybindHelp:
 * @key: Canonical key string (e.g. "Ctrl+r", "question")
 * @description: Human-readable description of what the binding does
 * @source: Origin of the binding ("core" or a module name)
 * @action: Action nick (e.g. "reload") or a module-local label
 *
 * One row in the keybinding help overlay.
 */
struct _GsurfKeybindHelp {
	gchar *key;
	gchar *description;
	gchar *source;
	gchar *action;
};

GType gsurf_keybind_help_get_type(void) G_GNUC_CONST;

/**
 * gsurf_keybind_help_new:
 * @key: (nullable): canonical key string
 * @description: (nullable): human-readable description
 * @source: (nullable): "core" or a module name
 * @action: (nullable): action nick or module-local label
 *
 * Returns: (transfer full): a new #GsurfKeybindHelp
 */
GsurfKeybindHelp *gsurf_keybind_help_new(const gchar *key,
                                         const gchar *description,
                                         const gchar *source,
                                         const gchar *action);

GsurfKeybindHelp *gsurf_keybind_help_copy(const GsurfKeybindHelp *help);
void              gsurf_keybind_help_free(GsurfKeybindHelp *help);

const gchar *gsurf_keybind_help_get_key(const GsurfKeybindHelp *help);
const gchar *gsurf_keybind_help_get_description(const GsurfKeybindHelp *help);
const gchar *gsurf_keybind_help_get_source(const GsurfKeybindHelp *help);
const gchar *gsurf_keybind_help_get_action(const GsurfKeybindHelp *help);

void gsurf_keybind_help_set_key(GsurfKeybindHelp *help, const gchar *key);
void gsurf_keybind_help_set_description(GsurfKeybindHelp *help, const gchar *description);
void gsurf_keybind_help_set_source(GsurfKeybindHelp *help, const gchar *source);
void gsurf_keybind_help_set_action(GsurfKeybindHelp *help, const gchar *action);

/**
 * gsurf_keybind_help_pretty_key:
 * @key: (nullable): a canonical key string
 *
 * Rewrites trailing GDK keyval names that users type as punctuation
 * (`question` → `?`, `slash` → `/`, …). Modifier prefixes are preserved.
 *
 * Returns: (transfer full) (nullable): a display string, or %NULL
 */
gchar *gsurf_keybind_help_pretty_key(const gchar *key);

/**
 * gsurf_keybind_help_append:
 * @entries: (element-type GsurfKeybindHelp): destination array
 * @key: (nullable): canonical key string; skipped when %NULL or empty
 * @description: (nullable): human-readable description
 * @source: (nullable): "core" or a module name
 * @action: (nullable): action nick or module-local label
 *
 * Appends a new row. No-op when @entries or @key is empty.
 */
void gsurf_keybind_help_append(GPtrArray *entries,
                               const gchar *key,
                               const gchar *description,
                               const gchar *source,
                               const gchar *action);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfKeybindHelp, gsurf_keybind_help_free)

G_END_DECLS

#endif /* GSURF_KEYBIND_HELP_H */
