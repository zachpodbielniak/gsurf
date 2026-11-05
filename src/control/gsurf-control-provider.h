/*
 * gsurf-control-provider.h - Module hook: methods on the control socket
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A module that wants its own control methods (`mymodule.status`, ...)
 * implements #GsurfControlProvider. When the control server starts, and
 * whenever it refreshes its providers (after `modules.toggle`, or
 * gsurf_control_server_refresh_providers()), it asks every active module
 * implementing the interface to register its methods with
 * gsurf_control_server_add_method(); those methods belong to the module
 * and are removed again at the next refresh, so a deactivated module's
 * methods disappear. Name methods `<module>.<verb>` so they cannot clash
 * with the core ones.
 *
 * Since: 0.5
 */

#ifndef GSURF_CONTROL_PROVIDER_H
#define GSURF_CONTROL_PROVIDER_H

#include <glib-object.h>

#include "control/gsurf-control-server.h"

G_BEGIN_DECLS

#define GSURF_TYPE_CONTROL_PROVIDER (gsurf_control_provider_get_type())

G_DECLARE_INTERFACE(GsurfControlProvider, gsurf_control_provider, GSURF, CONTROL_PROVIDER,
                    GObject)

/**
 * GsurfControlProviderInterface:
 * @parent_iface: the parent interface
 * @register_methods: add the provider's methods to @server
 *
 * Interface of a module offering control methods.
 *
 * Since: 0.5
 */
struct _GsurfControlProviderInterface
{
	GTypeInterface parent_iface;

	void (*register_methods) (GsurfControlProvider *self, GsurfControlServer *server);

	/*< private >*/
	gpointer padding[4];
};

/**
 * gsurf_control_provider_register_methods:
 * @self: a #GsurfControlProvider
 * @server: the server to add the methods to
 *
 * Asks @self to register its methods with
 * gsurf_control_server_add_method(). The server calls this; a provider
 * without the vfunc registers nothing.
 *
 * Since: 0.5
 */
void gsurf_control_provider_register_methods(GsurfControlProvider *self,
                                             GsurfControlServer *server);

G_END_DECLS

#endif /* GSURF_CONTROL_PROVIDER_H */
