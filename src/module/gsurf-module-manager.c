/*
 * gsurf-module-manager.c - Module registry, loader, and hook dispatch
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "module/gsurf-module-manager.h"

#include "interfaces/gsurf-input-handler.h"
#include "interfaces/gsurf-uri-handler.h"
#include "interfaces/gsurf-navigation-hook.h"
#include "interfaces/gsurf-script-injector.h"
#include "interfaces/gsurf-setting-provider.h"
#include "interfaces/gsurf-permission-handler.h"
#include "interfaces/gsurf-download-handler.h"
#include "interfaces/gsurf-cert-handler.h"

#include <gmodule.h>
#include <yaml-glib.h>

struct _GsurfModuleManager
{
	GObject parent_instance;

	GPtrArray   *modules;   /* owned refs to GsurfModule, sorted by priority */
	GHashTable  *by_name;   /* gchar* -> GsurfModule* (borrowed) */
	GsurfApplication *app;  /* weak */
	GsurfConfig *config;    /* ref */
	gboolean     input_passthrough; /* INSERT mode: bare keys go to the page */
};

G_DEFINE_FINAL_TYPE(GsurfModuleManager, gsurf_module_manager, G_TYPE_OBJECT)

static GsurfModuleManager *default_manager = NULL;

static gint
compare_priority(gconstpointer a, gconstpointer b)
{
	GsurfModule *ma = *(GsurfModule * const *)a;
	GsurfModule *mb = *(GsurfModule * const *)b;
	return gsurf_module_get_priority(ma) - gsurf_module_get_priority(mb);
}

/* --- Context --- */

void
gsurf_module_manager_set_application(GsurfModuleManager *self, GsurfApplication *app)
{
	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	if (self->app != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->app), (gpointer *)&self->app);
	self->app = app;
	if (self->app != NULL)
		g_object_add_weak_pointer(G_OBJECT(self->app), (gpointer *)&self->app);
}

GsurfApplication *
gsurf_module_manager_get_application(GsurfModuleManager *self)
{
	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);
	return self->app;
}

void
gsurf_module_manager_set_config(GsurfModuleManager *self, GsurfConfig *config)
{
	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));
	g_set_object(&self->config, config);
}

GsurfConfig *
gsurf_module_manager_get_config(GsurfModuleManager *self)
{
	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);
	return self->config;
}

GsurfWindow *
gsurf_module_manager_get_active_window(GsurfModuleManager *self)
{
	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);

	if (self->app == NULL)
		return NULL;
	return gsurf_application_get_active_window(self->app);
}

GsurfView *
gsurf_module_manager_get_active_view(GsurfModuleManager *self)
{
	GsurfWindow *window;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);

	window = gsurf_module_manager_get_active_window(self);
	return window != NULL ? gsurf_window_get_active_view(window) : NULL;
}

void
gsurf_module_manager_set_input_passthrough(GsurfModuleManager *self, gboolean passthrough)
{
	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));
	self->input_passthrough = passthrough;
}

gboolean
gsurf_module_manager_get_input_passthrough(GsurfModuleManager *self)
{
	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), FALSE);
	return self->input_passthrough;
}

/* --- Loading / registration --- */

void
gsurf_module_manager_add_module(GsurfModuleManager *self, GsurfModule *module)
{
	const gchar *name;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));
	g_return_if_fail(GSURF_IS_MODULE(module));

	g_ptr_array_add(self->modules, g_object_ref(module));

	name = gsurf_module_get_name(module);
	if (name != NULL)
		g_hash_table_replace(self->by_name, g_strdup(name), module);

	if (self->config != NULL) {
		/* Read the module's `enabled:` flag from its config section. */
		if (name != NULL) {
			YamlNode *node = gsurf_config_get_module_node(self->config, name);
			if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING) {
				YamlMapping *m = yaml_node_get_mapping(node);
				if (yaml_mapping_has_member(m, "enabled"))
					gsurf_module_set_enabled(module,
						yaml_mapping_get_boolean_member(m, "enabled"));
			}
		}
		gsurf_module_configure(module, self->config);
	}

	g_ptr_array_sort(self->modules, compare_priority);
}

GsurfModule *
gsurf_module_manager_load_module(GsurfModuleManager *self, const gchar *path, GError **error)
{
	GModule *gmod;
	gpointer sym = NULL;
	GType (*register_fn)(void);
	GType type;
	GsurfModule *module;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);
	g_return_val_if_fail(path != NULL, NULL);

	gmod = g_module_open(path, G_MODULE_BIND_LOCAL);
	if (gmod == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"failed to open module '%s': %s", path, g_module_error());
		return NULL;
	}

	if (!g_module_symbol(gmod, "gsurf_module_register", &sym) || sym == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"module '%s' has no gsurf_module_register symbol", path);
		g_module_close(gmod);
		return NULL;
	}

	/* Keep the module mapped for the lifetime of the process so the
	 * registered GType stays valid. */
	g_module_make_resident(gmod);

	register_fn = (GType (*)(void))sym;
	type = register_fn();
	if (!g_type_is_a(type, GSURF_TYPE_MODULE)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"module '%s' did not register a GsurfModule type", path);
		return NULL;
	}

	module = g_object_new(type, NULL);
	gsurf_module_manager_add_module(self, module);
	g_object_unref(module); /* manager holds its own ref */

	return module;
}

guint
gsurf_module_manager_load_from_directory(GsurfModuleManager *self, const gchar *dir)
{
	GDir *d;
	const gchar *name;
	guint count = 0;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), 0);
	g_return_val_if_fail(dir != NULL, 0);

	d = g_dir_open(dir, 0, NULL);
	if (d == NULL)
		return 0;

	while ((name = g_dir_read_name(d)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autoptr(GError) error = NULL;

		if (!g_str_has_suffix(name, ".so"))
			continue;

		path = g_build_filename(dir, name, NULL);
		if (gsurf_module_manager_load_module(self, path, &error) != NULL)
			count++;
		else
			g_warning("gsurf: %s", error->message);
	}

	g_dir_close(d);
	return count;
}

GsurfModule *
gsurf_module_manager_get_module(GsurfModuleManager *self, const gchar *name)
{
	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);
	return g_hash_table_lookup(self->by_name, name);
}

GPtrArray *
gsurf_module_manager_get_modules(GsurfModuleManager *self)
{
	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);
	return self->modules;
}

void
gsurf_module_manager_activate_all(GsurfModuleManager *self)
{
	guint i;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	g_ptr_array_sort(self->modules, compare_priority);
	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		if (!gsurf_module_get_enabled(m))
			continue;
		if (!gsurf_module_activate(m))
			g_warning("gsurf: failed to activate module '%s'",
				gsurf_module_get_name(m));
	}
}

void
gsurf_module_manager_deactivate_all(GsurfModuleManager *self)
{
	guint i;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	for (i = 0; i < self->modules->len; i++)
		gsurf_module_deactivate(g_ptr_array_index(self->modules, i));
}

/* --- Hook dispatch --- */

gboolean
gsurf_module_manager_dispatch_key_event(GsurfModuleManager *self, GsurfView *view,
                                        guint keyval, guint keycode, guint state,
                                        GsurfModePolicy mode)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), FALSE);

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		if (!gsurf_module_is_active(m) || !GSURF_IS_INPUT_HANDLER(m))
			continue;
		if (gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(m),
				view, keyval, keycode, state, mode))
			return TRUE;
	}
	return FALSE;
}

gboolean
gsurf_module_manager_dispatch_mouse_event(GsurfModuleManager *self, GsurfView *view,
                                          guint button, guint state)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), FALSE);

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		if (!gsurf_module_is_active(m) || !GSURF_IS_INPUT_HANDLER(m))
			continue;
		if (gsurf_input_handler_handle_mouse_event(GSURF_INPUT_HANDLER(m),
				view, button, state))
			return TRUE;
	}
	return FALSE;
}

gchar *
gsurf_module_manager_dispatch_rewrite_uri(GsurfModuleManager *self, const gchar *input)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		gchar *out;

		if (!gsurf_module_is_active(m) || !GSURF_IS_URI_HANDLER(m))
			continue;
		out = gsurf_uri_handler_rewrite_uri(GSURF_URI_HANDLER(m), input);
		if (out != NULL)
			return out;
	}
	return NULL;
}

GsurfPolicyDecision
gsurf_module_manager_dispatch_before_navigate(GsurfModuleManager *self, GsurfView *view, const gchar *uri)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), GSURF_POLICY_USE);

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		GsurfPolicyDecision d;

		if (!gsurf_module_is_active(m) || !GSURF_IS_NAVIGATION_HOOK(m))
			continue;
		d = gsurf_navigation_hook_before_navigate(GSURF_NAVIGATION_HOOK(m), view, uri);
		if (d != GSURF_POLICY_USE)
			return d;
	}
	return GSURF_POLICY_USE;
}

void
gsurf_module_manager_dispatch_after_navigate(GsurfModuleManager *self, GsurfView *view, const gchar *uri)
{
	guint i;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		if (gsurf_module_is_active(m) && GSURF_IS_NAVIGATION_HOOK(m))
			gsurf_navigation_hook_after_navigate(GSURF_NAVIGATION_HOOK(m), view, uri);
	}
}

void
gsurf_module_manager_dispatch_load_changed(GsurfModuleManager *self, GsurfView *view, GsurfLoadEvent event)
{
	guint i;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		if (gsurf_module_is_active(m) && GSURF_IS_NAVIGATION_HOOK(m))
			gsurf_navigation_hook_load_changed(GSURF_NAVIGATION_HOOK(m), view, event);
	}
}

void
gsurf_module_manager_dispatch_inject_scripts(GsurfModuleManager *self, GsurfView *view, const gchar *uri)
{
	guint i;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		if (gsurf_module_is_active(m) && GSURF_IS_SCRIPT_INJECTOR(m))
			gsurf_script_injector_inject(GSURF_SCRIPT_INJECTOR(m), view, uri);
	}
}

void
gsurf_module_manager_dispatch_apply_settings(GsurfModuleManager *self, GsurfView *view,
                                             gpointer settings, const gchar *uri)
{
	guint i;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		if (gsurf_module_is_active(m) && GSURF_IS_SETTING_PROVIDER(m))
			gsurf_setting_provider_apply_settings(GSURF_SETTING_PROVIDER(m),
				view, settings, uri);
	}
}

GsurfPermissionVerdict
gsurf_module_manager_dispatch_permission(GsurfModuleManager *self, GsurfView *view,
                                         const gchar *type, const gchar *origin)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), GSURF_PERMISSION_PROMPT);

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		GsurfPermissionVerdict v;

		if (!gsurf_module_is_active(m) || !GSURF_IS_PERMISSION_HANDLER(m))
			continue;
		v = gsurf_permission_handler_decide(GSURF_PERMISSION_HANDLER(m), view, type, origin);
		if (v != GSURF_PERMISSION_PROMPT)
			return v;
	}
	return GSURF_PERMISSION_PROMPT;
}

gchar *
gsurf_module_manager_dispatch_decide_destination(GsurfModuleManager *self,
                                                 const gchar *uri, const gchar *suggested)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		gchar *path;

		if (!gsurf_module_is_active(m) || !GSURF_IS_DOWNLOAD_HANDLER(m))
			continue;
		path = gsurf_download_handler_decide_destination(GSURF_DOWNLOAD_HANDLER(m),
			uri, suggested);
		if (path != NULL)
			return path;
	}
	return NULL;
}

GsurfTlsDecision
gsurf_module_manager_dispatch_verify_cert(GsurfModuleManager *self,
                                          const gchar *host, guint tls_errors)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), GSURF_TLS_PROMPT);

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		GsurfTlsDecision d;

		if (!gsurf_module_is_active(m) || !GSURF_IS_CERT_HANDLER(m))
			continue;
		d = gsurf_cert_handler_verify(GSURF_CERT_HANDLER(m), host, tls_errors);
		if (d != GSURF_TLS_PROMPT)
			return d;
	}
	return GSURF_TLS_PROMPT;
}

/* --- Lifecycle --- */

static void
gsurf_module_manager_dispose(GObject *object)
{
	GsurfModuleManager *self = GSURF_MODULE_MANAGER(object);

	if (self->modules != NULL)
		gsurf_module_manager_deactivate_all(self);
	g_clear_pointer(&self->modules, g_ptr_array_unref);
	g_clear_pointer(&self->by_name, g_hash_table_unref);
	g_clear_object(&self->config);
	gsurf_module_manager_set_application(self, NULL);

	G_OBJECT_CLASS(gsurf_module_manager_parent_class)->dispose(object);
}

static void
gsurf_module_manager_class_init(GsurfModuleManagerClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = gsurf_module_manager_dispose;
}

static void
gsurf_module_manager_init(GsurfModuleManager *self)
{
	self->modules = g_ptr_array_new_with_free_func(g_object_unref);
	self->by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->app = NULL;
	self->config = NULL;
}

GsurfModuleManager *
gsurf_module_manager_new(void)
{
	return g_object_new(GSURF_TYPE_MODULE_MANAGER, NULL);
}

GsurfModuleManager *
gsurf_module_manager_get_default(void)
{
	if (default_manager == NULL)
		default_manager = gsurf_module_manager_new();
	return default_manager;
}
