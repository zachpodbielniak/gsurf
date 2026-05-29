/*
 * gsurf-module.c - Abstract base module class
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "module/gsurf-module.h"

typedef struct {
	gint     priority;
	gboolean active;
	gboolean enabled;
} GsurfModulePrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(GsurfModule, gsurf_module, G_TYPE_OBJECT)

static void
gsurf_module_class_init(GsurfModuleClass *klass)
{
}

static void
gsurf_module_init(GsurfModule *self)
{
	GsurfModulePrivate *priv = gsurf_module_get_instance_private(self);

	priv->priority = 50; /* GSURF_PRIORITY_DEFAULT */
	priv->active = FALSE;
	priv->enabled = FALSE; /* must be explicitly enabled in config */
}

gboolean
gsurf_module_get_enabled(GsurfModule *self)
{
	GsurfModulePrivate *priv;

	g_return_val_if_fail(GSURF_IS_MODULE(self), FALSE);

	priv = gsurf_module_get_instance_private(self);
	return priv->enabled;
}

void
gsurf_module_set_enabled(GsurfModule *self, gboolean enabled)
{
	GsurfModulePrivate *priv;

	g_return_if_fail(GSURF_IS_MODULE(self));

	priv = gsurf_module_get_instance_private(self);
	priv->enabled = enabled;
}

gboolean
gsurf_module_activate(GsurfModule *self)
{
	GsurfModulePrivate *priv;
	GsurfModuleClass *klass;
	gboolean ok = TRUE;

	g_return_val_if_fail(GSURF_IS_MODULE(self), FALSE);

	priv = gsurf_module_get_instance_private(self);
	if (priv->active)
		return TRUE;

	klass = GSURF_MODULE_GET_CLASS(self);
	if (klass->activate != NULL)
		ok = klass->activate(self);

	if (ok)
		priv->active = TRUE;
	return ok;
}

void
gsurf_module_deactivate(GsurfModule *self)
{
	GsurfModulePrivate *priv;
	GsurfModuleClass *klass;

	g_return_if_fail(GSURF_IS_MODULE(self));

	priv = gsurf_module_get_instance_private(self);
	if (!priv->active)
		return;

	klass = GSURF_MODULE_GET_CLASS(self);
	if (klass->deactivate != NULL)
		klass->deactivate(self);

	priv->active = FALSE;
}

const gchar *
gsurf_module_get_name(GsurfModule *self)
{
	GsurfModuleClass *klass;

	g_return_val_if_fail(GSURF_IS_MODULE(self), NULL);

	klass = GSURF_MODULE_GET_CLASS(self);
	return klass->get_name != NULL ? klass->get_name(self) : NULL;
}

const gchar *
gsurf_module_get_description(GsurfModule *self)
{
	GsurfModuleClass *klass;

	g_return_val_if_fail(GSURF_IS_MODULE(self), NULL);

	klass = GSURF_MODULE_GET_CLASS(self);
	return klass->get_description != NULL ? klass->get_description(self) : NULL;
}

void
gsurf_module_configure(GsurfModule *self, gpointer config)
{
	GsurfModuleClass *klass;

	g_return_if_fail(GSURF_IS_MODULE(self));

	klass = GSURF_MODULE_GET_CLASS(self);
	if (klass->configure != NULL)
		klass->configure(self, config);
}

gint
gsurf_module_get_priority(GsurfModule *self)
{
	GsurfModulePrivate *priv;

	g_return_val_if_fail(GSURF_IS_MODULE(self), 50);

	priv = gsurf_module_get_instance_private(self);
	return priv->priority;
}

void
gsurf_module_set_priority(GsurfModule *self, gint priority)
{
	GsurfModulePrivate *priv;

	g_return_if_fail(GSURF_IS_MODULE(self));

	priv = gsurf_module_get_instance_private(self);
	priv->priority = priority;
}

gboolean
gsurf_module_is_active(GsurfModule *self)
{
	GsurfModulePrivate *priv;

	g_return_val_if_fail(GSURF_IS_MODULE(self), FALSE);

	priv = gsurf_module_get_instance_private(self);
	return priv->active;
}
