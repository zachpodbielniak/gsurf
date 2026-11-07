/*
 * gsurf-module-manager.c - Module registry, loader, and hook dispatch
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "module/gsurf-module-manager.h"

#include "interfaces/gsurf-input-handler.h"
#include "interfaces/gsurf-action-handler.h"
#include "interfaces/gsurf-uri-handler.h"
#include "interfaces/gsurf-navigation-hook.h"
#include "interfaces/gsurf-script-injector.h"
#include "interfaces/gsurf-setting-provider.h"
#include "interfaces/gsurf-permission-handler.h"
#include "interfaces/gsurf-download-handler.h"
#include "interfaces/gsurf-cert-handler.h"
#include "interfaces/gsurf-request-filter.h"
#include "interfaces/gsurf-status-provider.h"
#include "interfaces/gsurf-context-menu-provider.h"
#include "interfaces/gsurf-render-overlay.h"
#include "interfaces/gsurf-keybind-provider.h"
#include "boxed/gsurf-keybind-help.h"
#include "util/gsurf-kiosk.h"

#include <gio/gio.h>
#include <gmodule.h>
#include <yaml-glib.h>

struct _GsurfModuleManager
{
	GObject parent_instance;

	GPtrArray   *modules;       /* owned refs to GsurfModule, sorted by priority */
	GHashTable  *by_name;       /* gchar* -> GsurfModule* (borrowed) */
	GPtrArray   *search_paths;  /* owned gchar* dirs, in search order */
	GsurfApplication *app;      /* weak */
	GsurfConfig *config;        /* ref */
	gboolean     input_passthrough; /* INSERT mode: bare keys go to the page */
	GsurfModePolicy base_mode;      /* the mode owner's mode (set_mode()) */
	GsurfModePolicy mode;           /* the published (effective) input mode */
	GsurfActionExecutor executor;   /* the host's keybind actions (0.4.4) */
	gpointer     executor_data;
	GDestroyNotify executor_destroy;
};

G_DEFINE_FINAL_TYPE(GsurfModuleManager, gsurf_module_manager, G_TYPE_OBJECT)

enum {
	PROP_0,
	PROP_MODE,
	N_PROPS
};

static GParamSpec *props[N_PROPS];

enum {
	SIG_MODE_CHANGED,
	SIG_DOWNLOAD_STARTED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

static GsurfModuleManager *default_manager = NULL;

static gint
compare_priority(gconstpointer a, gconstpointer b)
{
	GsurfModule *ma = *(GsurfModule * const *)a;
	GsurfModule *mb = *(GsurfModule * const *)b;
	gint pa = gsurf_module_get_priority(ma);
	gint pb = gsurf_module_get_priority(mb);

	/* Subtracting priorities can overflow and reverse the hook order. */
	return (pa > pb) - (pa < pb);
}

/* --- The published input mode ---
 *
 * The module owning the keyboard modes (modal) sets the base mode. What
 * is published (get_mode(), ::mode-changed, the indicators) is what a
 * bare key does right now: the base mode, except that NORMAL reads
 * INSERT while the active window is typing (a focused page field in its
 * active view, or a focused chrome entry), because those keys go to the
 * field as text whatever the base mode says. FOLLOW and the other modes
 * own the keys themselves and are published unchanged. */

static GsurfModePolicy
effective_mode(GsurfModePolicy base, gboolean typing)
{
	if (base == GSURF_MODE_NORMAL && typing)
		return GSURF_MODE_INSERT;
	return base;
}

/* Recompute the published mode from the base mode and the active
 * window's typing state; notify and emit only when it changes. */
static void
update_mode(GsurfModuleManager *self)
{
	GsurfWindow *window = gsurf_module_manager_get_active_window(self);
	GsurfModePolicy mode;

	mode = effective_mode(self->base_mode,
		window != NULL && gsurf_window_get_typing(window));
	if (self->mode == mode)
		return;
	self->mode = mode;
	g_object_notify_by_pspec(G_OBJECT(self), props[PROP_MODE]);
	g_signal_emit(self, signals[SIG_MODE_CHANGED], 0, mode);
}

static void
on_window_typing(GsurfWindow *window, GParamSpec *pspec, gpointer user_data)
{
	(void)window;
	(void)pspec;
	update_mode(GSURF_MODULE_MANAGER(user_data));
}

static void
watch_window(GsurfModuleManager *self, GsurfWindow *window)
{
	g_signal_connect_object(window, "notify::typing", G_CALLBACK(on_window_typing),
		self, 0);
}

static void
on_window_added(GsurfApplication *app, GsurfWindow *window, gpointer user_data)
{
	GsurfModuleManager *self = user_data;

	(void)app;
	watch_window(self, window);
	update_mode(self);
}

/* The active window may have changed (it is the newest one). */
static void
on_window_removed(GsurfApplication *app, GsurfWindow *window, gpointer user_data)
{
	GsurfModuleManager *self = user_data;

	(void)app;
	g_signal_handlers_disconnect_by_func(window, on_window_typing, self);
	update_mode(self);
}

/* --- Context --- */

void
gsurf_module_manager_set_application(GsurfModuleManager *self, GsurfApplication *app)
{
	GPtrArray *windows;
	guint i;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	if (self->app == app)
		return;
	/* Stop following the old application's windows. */
	if (self->app != NULL) {
		windows = gsurf_application_get_windows(self->app);
		for (i = 0; windows != NULL && i < windows->len; i++)
			g_signal_handlers_disconnect_by_func(g_ptr_array_index(windows, i),
				on_window_typing, self);
		g_signal_handlers_disconnect_by_func(self->app, on_window_added, self);
		g_signal_handlers_disconnect_by_func(self->app, on_window_removed, self);
		g_object_remove_weak_pointer(G_OBJECT(self->app), (gpointer *)&self->app);
	}
	self->app = app;
	/* Follow the typing state of every window: the active one is the
	 * newest, so adding or removing a window can change which counts. */
	if (self->app != NULL) {
		g_object_add_weak_pointer(G_OBJECT(self->app), (gpointer *)&self->app);
		g_signal_connect_object(self->app, "window-added", G_CALLBACK(on_window_added),
			self, 0);
		g_signal_connect_object(self->app, "window-removed",
			G_CALLBACK(on_window_removed), self, 0);
		windows = gsurf_application_get_windows(self->app);
		for (i = 0; windows != NULL && i < windows->len; i++)
			watch_window(self, g_ptr_array_index(windows, i));
	}
	update_mode(self);
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

void
gsurf_module_manager_set_mode(GsurfModuleManager *self, GsurfModePolicy mode)
{
	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	self->base_mode = mode;
	update_mode(self);
}

GsurfModePolicy
gsurf_module_manager_get_mode(GsurfModuleManager *self)
{
	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), GSURF_MODE_NORMAL);
	return self->mode;
}

gboolean
gsurf_module_manager_is_typing(GsurfModuleManager *self, GsurfView *view)
{
	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), FALSE);
	g_return_val_if_fail(view == NULL || GSURF_IS_VIEW(view), FALSE);

	/* The mode owner passes keys to the page (INSERT). */
	if (self->input_passthrough || self->base_mode == GSURF_MODE_INSERT ||
	    self->base_mode == GSURF_MODE_PASSTHROUGH)
		return TRUE;
	/* A focused page field takes the keys, unless a mode that owns them
	 * (FOLLOW: the hint labels) is in effect. */
	return self->base_mode == GSURF_MODE_NORMAL &&
	       view != NULL && gsurf_view_get_editing(view);
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

/* --- Module search paths --- */

void
gsurf_module_manager_add_search_path(GsurfModuleManager *self, const gchar *dir)
{
	guint i;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));
	if (dir == NULL || *dir == '\0')
		return;

	for (i = 0; i < self->search_paths->len; i++)
		if (g_strcmp0(g_ptr_array_index(self->search_paths, i), dir) == 0)
			return; /* already present */

	g_ptr_array_add(self->search_paths, g_strdup(dir));
}

void
gsurf_module_manager_set_search_paths(GsurfModuleManager *self, const gchar *const *dirs)
{
	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	g_ptr_array_set_size(self->search_paths, 0);
	if (dirs != NULL)
		for (guint i = 0; dirs[i] != NULL; i++)
			gsurf_module_manager_add_search_path(self, dirs[i]);
}

void
gsurf_module_manager_clear_search_paths(GsurfModuleManager *self)
{
	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));
	g_ptr_array_set_size(self->search_paths, 0);
}

GPtrArray *
gsurf_module_manager_get_search_paths(GsurfModuleManager *self)
{
	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);
	return self->search_paths;
}

const gchar *
gsurf_module_manager_get_system_module_dir(void)
{
	return GSURF_MODULEDIR;
}

guint
gsurf_module_manager_load_modules(GsurfModuleManager *self)
{
	g_autoptr(GHashTable) seen = NULL; /* basename -> 1, for first-match wins */
	guint p, count = 0;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), 0);

	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (p = 0; p < self->search_paths->len; p++) {
		const gchar *dir = g_ptr_array_index(self->search_paths, p);
		GDir *d = g_dir_open(dir, 0, NULL);
		const gchar *name;

		if (d == NULL)
			continue;

		while ((name = g_dir_read_name(d)) != NULL) {
			g_autofree gchar *path = NULL;
			g_autoptr(GError) error = NULL;

			if (!g_str_has_suffix(name, ".so"))
				continue;
			/* First directory that provides a given file name wins. */
			if (g_hash_table_contains(seen, name))
				continue;
			g_hash_table_add(seen, g_strdup(name));

			path = g_build_filename(dir, name, NULL);
			if (gsurf_module_manager_load_module(self, path, &error) != NULL)
				count++;
			else
				g_warning("gsurf: %s", error->message);
		}
		g_dir_close(d);
	}

	return count;
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
	/* activate() may set a module's priority (modal claims the highest
	 * so its keymap sees keys first); sort again so dispatch honours it
	 * rather than the directory order the modules loaded in. */
	g_ptr_array_sort(self->modules, compare_priority);
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
gsurf_module_manager_dispatch_action(GsurfModuleManager *self, GsurfView *view,
                                     GsurfAction action)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), FALSE);

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		if (!gsurf_module_is_active(m) || !GSURF_IS_ACTION_HANDLER(m))
			continue;
		if (gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(m), view, action))
			return TRUE;
	}
	return FALSE;
}

/* --- Action execution (since 0.4.4) --- */

void
gsurf_module_manager_set_action_executor(GsurfModuleManager *self, GsurfActionExecutor executor,
                                         gpointer user_data, GDestroyNotify destroy)
{
	GDestroyNotify old_destroy;
	gpointer old_data;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	/* Swap first, then free the old data: its destroy notify may call
	 * back into the manager. */
	old_destroy = self->executor_destroy;
	old_data = self->executor_data;
	self->executor = executor;
	self->executor_data = user_data;
	self->executor_destroy = destroy;
	if (old_destroy != NULL)
		old_destroy(old_data);
}

gboolean
gsurf_module_manager_execute_action(GsurfModuleManager *self, GsurfWindow *window,
                                    GsurfView *view, GsurfAction action, GError **error)
{
	const gchar *nick;
	gboolean done;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), FALSE);
	g_return_val_if_fail(window == NULL || GSURF_IS_WINDOW(window), FALSE);
	g_return_val_if_fail(view == NULL || GSURF_IS_VIEW(view), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* 1. A real action. */
	if (action <= GSURF_ACTION_NONE || action >= GSURF_ACTION_LAST) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"%d is not an action", (gint)action);
		return FALSE;
	}
	nick = gsurf_action_to_string(action);

	/* 2. Kiosk mode refuses what a kiosk keybinding would refuse. */
	if (gsurf_kiosk_is_enabled() && !gsurf_kiosk_allows_action(action)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			"action '%s' is not allowed in kiosk mode", nick);
		return FALSE;
	}

	/* 3. Where: the active window and its active view by default. */
	if (window == NULL)
		window = gsurf_module_manager_get_active_window(self);
	if (view == NULL && window != NULL)
		view = gsurf_window_get_active_view(window);

	/* 4. The host's executor knows the built-in actions; without one
	 *    only the modules can perform it. */
	if (self->executor != NULL && window != NULL)
		done = self->executor(window, view, action, self->executor_data);
	else
		done = gsurf_module_manager_dispatch_action(self, view, action);
	if (!done)
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"nothing performed action '%s' (no built-in implementation or module "
			"handled it here)", nick);
	return done;
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

GsurfPolicyDecision
gsurf_module_manager_dispatch_main_response(GsurfModuleManager *self, GsurfView *view, const gchar *uri)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), GSURF_POLICY_USE);

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		GsurfPolicyDecision d;

		if (!gsurf_module_is_active(m) || !GSURF_IS_NAVIGATION_HOOK(m))
			continue;
		d = gsurf_navigation_hook_decide_main_response(GSURF_NAVIGATION_HOOK(m), view, uri);
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
		if (path != NULL) {
			g_signal_emit(self, signals[SIG_DOWNLOAD_STARTED], 0, uri, suggested, path);
			return path;
		}
	}
	g_signal_emit(self, signals[SIG_DOWNLOAD_STARTED], 0, uri, suggested, NULL);
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

GsurfFilterVerdict
gsurf_module_manager_dispatch_filter_request(GsurfModuleManager *self, GsurfView *view,
                                             const gchar *uri, gchar **redirect_uri)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), GSURF_FILTER_ALLOW);

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		GsurfFilterVerdict v;

		if (!gsurf_module_is_active(m) || !GSURF_IS_REQUEST_FILTER(m))
			continue;
		v = gsurf_request_filter_filter_request(GSURF_REQUEST_FILTER(m), view, uri, redirect_uri);
		if (v != GSURF_FILTER_ALLOW)
			return v;
	}
	return GSURF_FILTER_ALLOW;
}

void
gsurf_module_manager_dispatch_populate_menu(GsurfModuleManager *self, GsurfHitTest *hit,
                                            GPtrArray *items)
{
	guint i;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		if (gsurf_module_is_active(m) && GSURF_IS_CONTEXT_MENU_PROVIDER(m))
			gsurf_context_menu_provider_populate(GSURF_CONTEXT_MENU_PROVIDER(m), hit, items);
	}
}

gchar *
gsurf_module_manager_dispatch_status_text(GsurfModuleManager *self, GsurfView *view)
{
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		gchar *text;

		if (!gsurf_module_is_active(m) || !GSURF_IS_STATUS_PROVIDER(m))
			continue;
		text = gsurf_status_provider_get_status_text(GSURF_STATUS_PROVIDER(m), view);
		if (text != NULL)
			return text;
	}
	return NULL;
}

static gint
keybind_help_cmp(gconstpointer a, gconstpointer b)
{
	const GsurfKeybindHelp *ha = *(const GsurfKeybindHelp * const *)a;
	const GsurfKeybindHelp *hb = *(const GsurfKeybindHelp * const *)b;
	gboolean a_core, b_core;
	gint s;

	a_core = (g_strcmp0(ha->source, "core") == 0);
	b_core = (g_strcmp0(hb->source, "core") == 0);
	if (a_core && !b_core)
		return -1;
	if (b_core && !a_core)
		return 1;
	s = g_strcmp0(ha->source, hb->source);
	if (s != 0)
		return s;
	return g_strcmp0(ha->key, hb->key);
}

GPtrArray *
gsurf_module_manager_collect_keybinds(GsurfModuleManager *self)
{
	GPtrArray *entries;
	guint i;

	g_return_val_if_fail(GSURF_IS_MODULE_MANAGER(self), NULL);

	entries = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_keybind_help_free);

	/* Core table first: whatever is in the live config hash. */
	if (self->config != NULL && self->config->keybinds != NULL) {
		GHashTableIter iter;
		gpointer key, value;

		g_hash_table_iter_init(&iter, self->config->keybinds);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			GsurfAction action = (GsurfAction)GPOINTER_TO_UINT(value);

			gsurf_keybind_help_append(entries, (const gchar *)key,
				gsurf_action_get_description(action),
				"core",
				gsurf_action_to_string(action));
		}
	}

	/* Active modules report their *current* keys (post-configure). */
	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);

		if (gsurf_module_is_active(m) && GSURF_IS_KEYBIND_PROVIDER(m))
			gsurf_keybind_provider_list_keybinds(GSURF_KEYBIND_PROVIDER(m), entries);
	}

	g_ptr_array_sort(entries, keybind_help_cmp);
	return entries;
}

void
gsurf_module_manager_dispatch_render_overlay(GsurfModuleManager *self, GsurfView *view,
                                             gpointer draw_target)
{
	guint i;

	g_return_if_fail(GSURF_IS_MODULE_MANAGER(self));

	for (i = 0; i < self->modules->len; i++) {
		GsurfModule *m = g_ptr_array_index(self->modules, i);
		if (gsurf_module_is_active(m) && GSURF_IS_RENDER_OVERLAY(m))
			gsurf_render_overlay_render(GSURF_RENDER_OVERLAY(m), view, draw_target);
	}
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
	g_clear_pointer(&self->search_paths, g_ptr_array_unref);
	g_clear_object(&self->config);
	gsurf_module_manager_set_action_executor(self, NULL, NULL, NULL);
	gsurf_module_manager_set_application(self, NULL);

	G_OBJECT_CLASS(gsurf_module_manager_parent_class)->dispose(object);
}

static void
gsurf_module_manager_get_property(GObject *object, guint prop_id, GValue *value,
                                  GParamSpec *pspec)
{
	GsurfModuleManager *self = GSURF_MODULE_MANAGER(object);

	switch (prop_id) {
	case PROP_MODE:
		g_value_set_enum(value, self->mode);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
gsurf_module_manager_set_property(GObject *object, guint prop_id, const GValue *value,
                                  GParamSpec *pspec)
{
	GsurfModuleManager *self = GSURF_MODULE_MANAGER(object);

	switch (prop_id) {
	case PROP_MODE:
		gsurf_module_manager_set_mode(self, g_value_get_enum(value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
gsurf_module_manager_class_init(GsurfModuleManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = gsurf_module_manager_dispose;
	object_class->get_property = gsurf_module_manager_get_property;
	object_class->set_property = gsurf_module_manager_set_property;

	/**
	 * GsurfModuleManager:mode:
	 *
	 * The input mode in effect (gsurf_module_manager_get_mode()): the
	 * mode published by the module that owns the keyboard modes
	 * (gsurf_module_manager_set_mode(), which setting the property
	 * calls), except that NORMAL reads INSERT while the active window
	 * is typing into a text field (#GsurfWindow:typing).
	 *
	 * Since: 0.4.2
	 */
	props[PROP_MODE] = g_param_spec_enum("mode", NULL, NULL,
		GSURF_TYPE_MODE_POLICY, GSURF_MODE_NORMAL,
		G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(object_class, N_PROPS, props);

	/**
	 * GsurfModuleManager::mode-changed:
	 * @manager: the module manager
	 * @mode: the new #GsurfModePolicy
	 *
	 * Emitted when the input mode in effect changes: through
	 * gsurf_module_manager_set_mode(), or because the user started or
	 * stopped typing into a text field in the active window (a page
	 * field or a chrome entry), or another view or window became active
	 * (see gsurf_module_manager_get_mode()).
	 *
	 * Since: 0.4.2
	 */
	signals[SIG_MODE_CHANGED] = g_signal_new("mode-changed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_MODE_POLICY);

	/**
	 * GsurfModuleManager::download-started:
	 * @manager: the module manager
	 * @uri: (nullable): the download's URI
	 * @suggested: (nullable): the file name the server suggested
	 * @destination: (nullable): the path a #GsurfDownloadHandler chose, or
	 *   %NULL when the engine picks it
	 *
	 * Emitted by gsurf_module_manager_dispatch_decide_destination(), which
	 * the backends call once for every download that starts. The control
	 * server turns it into a `download` event.
	 *
	 * Since: 0.5
	 */
	signals[SIG_DOWNLOAD_STARTED] = g_signal_new("download-started",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
}

static void
gsurf_module_manager_init(GsurfModuleManager *self)
{
	self->modules = g_ptr_array_new_with_free_func(g_object_unref);
	self->by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->search_paths = g_ptr_array_new_with_free_func(g_free);
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
