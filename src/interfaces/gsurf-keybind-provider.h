/*
 * gsurf-keybind-provider.h - Module hook: advertise live keybindings
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules that own keybindings implement #GsurfKeybindProvider so the
 * `?` help overlay can list those keys as they are currently configured.
 * The vfunc is queried at display time (not cached), so a reconfigured
 * or disabled module changes the overlay on the next press.
 */

#ifndef GSURF_KEYBIND_PROVIDER_H
#define GSURF_KEYBIND_PROVIDER_H

#include <glib-object.h>

#include "../boxed/gsurf-keybind-help.h"

G_BEGIN_DECLS

#define GSURF_TYPE_KEYBIND_PROVIDER (gsurf_keybind_provider_get_type())

G_DECLARE_INTERFACE(GsurfKeybindProvider, gsurf_keybind_provider, GSURF, KEYBIND_PROVIDER, GObject)

/**
 * GsurfKeybindProviderInterface:
 * @parent_iface: the parent interface
 * @list_keybinds: append this module's current bindings to @entries
 */
struct _GsurfKeybindProviderInterface
{
	GTypeInterface parent_iface;

	void (*list_keybinds)(GsurfKeybindProvider *self, GPtrArray *entries);
};

/**
 * gsurf_keybind_provider_list_keybinds:
 * @self: a #GsurfKeybindProvider
 * @entries: (element-type GsurfKeybindHelp): caller-owned array receiving rows
 *
 * Appends this module's currently configured keybindings. Called only
 * while the module is active.
 */
void gsurf_keybind_provider_list_keybinds(GsurfKeybindProvider *self, GPtrArray *entries);

G_END_DECLS

#endif /* GSURF_KEYBIND_PROVIDER_H */
