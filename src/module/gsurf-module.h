/*
 * gsurf-module.h - Abstract base module class
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every loadable gsurf module subclasses #GsurfModule and exports a
 * `G_MODULE_EXPORT GType gsurf_module_register(void)` entry point. The
 * module implements one or more hook-point GInterfaces (see
 * src/interfaces/); the #GsurfModuleManager auto-detects them on load.
 * Mirrors gst's GstModule.
 */

#ifndef GSURF_MODULE_H
#define GSURF_MODULE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GSURF_TYPE_MODULE (gsurf_module_get_type())

G_DECLARE_DERIVABLE_TYPE(GsurfModule, gsurf_module, GSURF, MODULE, GObject)

/**
 * GsurfModuleClass:
 * @parent_class: the parent class
 * @activate: activate the module (wire up hooks/state)
 * @deactivate: deactivate the module
 * @get_name: the module's stable name
 * @get_description: a one-line description
 * @configure: receive the configuration object
 *
 * Virtual table for #GsurfModule.
 */
struct _GsurfModuleClass
{
	GObjectClass parent_class;

	gboolean     (*activate)        (GsurfModule *self);
	void         (*deactivate)      (GsurfModule *self);
	const gchar *(*get_name)        (GsurfModule *self);
	const gchar *(*get_description) (GsurfModule *self);
	void         (*configure)       (GsurfModule *self, gpointer config);

	gpointer padding[7];
};

gboolean     gsurf_module_activate(GsurfModule *self);
void         gsurf_module_deactivate(GsurfModule *self);
const gchar *gsurf_module_get_name(GsurfModule *self);
const gchar *gsurf_module_get_description(GsurfModule *self);
void         gsurf_module_configure(GsurfModule *self, gpointer config);
gint         gsurf_module_get_priority(GsurfModule *self);
void         gsurf_module_set_priority(GsurfModule *self, gint priority);
gboolean     gsurf_module_is_active(GsurfModule *self);

/**
 * gsurf_module_get_enabled:
 * @self: a #GsurfModule
 *
 * Returns: whether the module is enabled in configuration. The module
 *   manager only activates enabled modules.
 */
gboolean     gsurf_module_get_enabled(GsurfModule *self);

/**
 * gsurf_module_set_enabled:
 * @self: a #GsurfModule
 * @enabled: whether the module is enabled
 *
 * Sets the enabled flag (usually from the module's `enabled:` config key).
 */
void         gsurf_module_set_enabled(GsurfModule *self, gboolean enabled);

G_END_DECLS

#endif /* GSURF_MODULE_H */
