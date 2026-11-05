/*
 * gsurf-container-registry.h - Isolated browsing containers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A container is a separate browsing identity: its own cookies, storage
 * and cache, like Firefox's contextual identities. Persistent containers
 * keep their data on disk under $XDG_DATA_HOME/gsurf/containers/<id>;
 * temporary containers keep theirs in this process's runtime directory
 * (`$XDG_RUNTIME_DIR/gsurf/<pid>-XXXXXX/temp/<id>`, see
 * gsurf-runtime-dir.h) and are destroyed, data and all, shortly after
 * their last view closes or when the process exits.
 *
 * The registry is backend-agnostic. It decides which container a new
 * view belongs to (gsurf_container_registry_resolve()) and owns one
 * native session object per container (a WebKitWebContext on WebKitGTK
 * 4.1, a WebKitNetworkSession on 6.0), which the active backend creates
 * through a #GsurfSessionBackend vtable. The default container (id
 * %NULL) is the engine's default session and is never owned here.
 *
 * Policy (which container a tab opens in, host assignment) belongs to the
 * `containers` module, which installs a #GsurfContainerResolveFunc; the
 * tabs module reads names and colours from here without linking it.
 */

#ifndef GSURF_CONTAINER_REGISTRY_H
#define GSURF_CONTAINER_REGISTRY_H

#include <glib-object.h>

#include "core/gsurf-view.h"

G_BEGIN_DECLS

/**
 * GSURF_CONTAINER_TEMPORARY:
 *
 * A requested container id meaning "a new temporary container".
 * gsurf_container_registry_resolve() replaces it with the id of a freshly
 * created temporary container.
 */
#define GSURF_CONTAINER_TEMPORARY "temporary"

/**
 * GSURF_CONTAINER_DEFAULT:
 *
 * A requested container id meaning "the default container", stated
 * explicitly. A %NULL request leaves the choice to the resolver (host
 * rules, the opener, the new-tab policy); this token asks for the
 * engine's default session itself. gsurf_container_registry_resolve()
 * turns it into %NULL. Session restore and "reopen in container" use it.
 */
#define GSURF_CONTAINER_DEFAULT "default"

#define GSURF_CONTAINER_ERROR (gsurf_container_error_quark())

/**
 * GsurfContainerError:
 * @GSURF_CONTAINER_ERROR_INVALID_ID: the id is empty, reserved, or not
 *   made of `[A-Za-z0-9_-]`
 * @GSURF_CONTAINER_ERROR_IN_USE: the container still has views
 * @GSURF_CONTAINER_ERROR_NOT_FOUND: no container has that id
 *
 * Errors reported by the container registry.
 */
typedef enum {
	GSURF_CONTAINER_ERROR_INVALID_ID,
	GSURF_CONTAINER_ERROR_IN_USE,
	GSURF_CONTAINER_ERROR_NOT_FOUND
} GsurfContainerError;

GQuark gsurf_container_error_quark(void);

/* --- GsurfContainer --- */

#define GSURF_TYPE_CONTAINER (gsurf_container_get_type())

G_DECLARE_FINAL_TYPE(GsurfContainer, gsurf_container, GSURF, CONTAINER, GObject)

/**
 * gsurf_container_get_id:
 * @self: a #GsurfContainer
 *
 * Returns: (transfer none): the stable id (a path-safe name)
 */
const gchar *gsurf_container_get_id(GsurfContainer *self);

/**
 * gsurf_container_get_name:
 * @self: a #GsurfContainer
 *
 * Returns: (transfer none): the display name
 */
const gchar *gsurf_container_get_name(GsurfContainer *self);

/**
 * gsurf_container_get_color:
 * @self: a #GsurfContainer
 *
 * Returns: (transfer none) (nullable): the colour as a CSS colour string
 *   (e.g. "#f9e2af"), or %NULL
 */
const gchar *gsurf_container_get_color(GsurfContainer *self);

/**
 * gsurf_container_get_temporary:
 * @self: a #GsurfContainer
 *
 * Returns: whether the container is ephemeral
 */
gboolean gsurf_container_get_temporary(GsurfContainer *self);

/**
 * gsurf_container_get_n_views:
 * @self: a #GsurfContainer
 *
 * Returns: the number of live views using the container
 */
guint gsurf_container_get_n_views(GsurfContainer *self);

/**
 * gsurf_container_get_data_dir:
 * @self: a #GsurfContainer
 *
 * The data directory the container's session stores its profile in:
 * $XDG_DATA_HOME/gsurf/containers/<id> for a persistent container, a
 * private directory under the runtime directory for a temporary one.
 *
 * Returns: (transfer none) (type filename) (nullable): the directory, or
 *   %NULL while the container has no session (or an ephemeral fallback)
 */
const gchar *gsurf_container_get_data_dir(GsurfContainer *self);

/* --- backend vtable --- */

/**
 * GsurfSessionBackend:
 * @create: create the native session for a container in @data_dir and
 *   @cache_dir (a temporary container's are removed with it). Both are
 *   %NULL only when a temporary container could not get private
 *   directories; the backend then makes an ephemeral session. Returns a
 *   new reference to the native object.
 * @destroy: release a native session returned by @create
 * @apply_policy: apply the registry's privacy policy to a native session;
 *   @native is %NULL for the engine's default session. @cookie_policy is
 *   -1 to leave the policy alone, else 0 always, 1 never, 2 no third-party.
 *
 * Supplied by the active backend; see
 * gsurf_container_registry_set_session_backend().
 */
typedef struct {
	gpointer (*create)       (const gchar *id, const gchar *data_dir,
	                          const gchar *cache_dir);
	void     (*destroy)      (gpointer native);
	void     (*apply_policy) (gpointer native, gboolean itp, gint cookie_policy);
} GsurfSessionBackend;

/* --- GsurfContainerRegistry --- */

#define GSURF_TYPE_CONTAINER_REGISTRY (gsurf_container_registry_get_type())

G_DECLARE_FINAL_TYPE(GsurfContainerRegistry, gsurf_container_registry,
                     GSURF, CONTAINER_REGISTRY, GObject)

/**
 * GsurfContainerResolveFunc:
 * @registry: the registry
 * @opener: (nullable): the view that opened the new one
 * @requested: (nullable): the container id the caller asked for
 * @uri: (nullable): the first URI the view will load, when known
 * @user_data: data passed to gsurf_container_registry_set_resolver()
 *
 * Chooses the container of a new, unrelated view (popups always share
 * their opener's container). May return #GSURF_CONTAINER_TEMPORARY.
 *
 * Returns: (transfer full) (nullable): a container id, or %NULL for the
 *   default container
 */
typedef gchar *(*GsurfContainerResolveFunc)(GsurfContainerRegistry *registry,
                                           GsurfView *opener,
                                           const gchar *requested,
                                           const gchar *uri,
                                           gpointer user_data);

/**
 * gsurf_container_registry_get_default:
 *
 * Returns: (transfer none): the process-wide registry
 */
GsurfContainerRegistry *gsurf_container_registry_get_default(void);

/**
 * gsurf_container_validate_id:
 * @id: (nullable): a candidate container id
 * @error: return location for a #GError
 *
 * Ids name directories, so they must be 1-64 characters of
 * `[A-Za-z0-9_-]`, and may not be #GSURF_CONTAINER_TEMPORARY,
 * #GSURF_CONTAINER_DEFAULT or start with the temporary prefix "tmp-".
 *
 * Returns: whether @id is usable for a persistent container
 */
gboolean gsurf_container_validate_id(const gchar *id, GError **error);

/**
 * gsurf_container_registry_define:
 * @self: a #GsurfContainerRegistry
 * @id: a valid id (see gsurf_container_validate_id())
 * @name: (nullable): display name; defaults to @id
 * @color: (nullable): CSS colour
 * @error: return location for a #GError
 *
 * Defines (or updates the name and colour of) a persistent container.
 *
 * Returns: (transfer none) (nullable): the container, or %NULL on error
 */
GsurfContainer *gsurf_container_registry_define(GsurfContainerRegistry *self,
                                                const gchar *id,
                                                const gchar *name,
                                                const gchar *color,
                                                GError **error);

/**
 * gsurf_container_registry_undefine:
 * @self: a #GsurfContainerRegistry
 * @id: a container id
 * @error: return location for a #GError
 *
 * Removes a container that no view uses. Its data on disk is kept.
 *
 * Returns: %TRUE when removed
 */
gboolean gsurf_container_registry_undefine(GsurfContainerRegistry *self,
                                           const gchar *id, GError **error);

/**
 * gsurf_container_registry_new_temporary:
 * @self: a #GsurfContainerRegistry
 * @color: (nullable): CSS colour
 *
 * Creates a temporary container with a fresh "tmp-N" id. It is removed
 * once its last view closes (after the cleanup delay), or immediately if
 * no view ever uses it and the delay elapses.
 *
 * Returns: (transfer none): the container
 */
GsurfContainer *gsurf_container_registry_new_temporary(GsurfContainerRegistry *self,
                                                       const gchar *color);

/**
 * gsurf_container_registry_lookup:
 * @self: a #GsurfContainerRegistry
 * @id: (nullable): a container id
 *
 * Returns: (transfer none) (nullable): the container, or %NULL (always
 *   for the default container, @id %NULL)
 */
GsurfContainer *gsurf_container_registry_lookup(GsurfContainerRegistry *self,
                                                const gchar *id);

/**
 * gsurf_container_registry_list:
 * @self: a #GsurfContainerRegistry
 *
 * Returns: (transfer container) (element-type GsurfContainer): every
 *   container, persistent ones in definition order, then temporary ones
 */
GPtrArray *gsurf_container_registry_list(GsurfContainerRegistry *self);

/**
 * gsurf_container_registry_set_resolver:
 * @self: a #GsurfContainerRegistry
 * @func: (nullable) (scope notified) (closure user_data) (destroy destroy): the
 *   resolver, or %NULL to remove it
 * @user_data: data for @func
 * @destroy: (nullable): frees @user_data
 *
 * Installs the policy that places new views. Without one, a view goes in
 * the container it requested, or the default container.
 */
void gsurf_container_registry_set_resolver(GsurfContainerRegistry *self,
                                           GsurfContainerResolveFunc func,
                                           gpointer user_data,
                                           GDestroyNotify destroy);

/**
 * gsurf_container_registry_has_resolver:
 * @self: a #GsurfContainerRegistry
 *
 * Whether a placement policy is installed, i.e. whether containers are
 * in use (the containers module installs one while it is enabled).
 * Chrome uses it to decide whether to offer container choices.
 *
 * Returns: %TRUE when a resolver is set
 */
gboolean gsurf_container_registry_has_resolver(GsurfContainerRegistry *self);

/**
 * gsurf_container_registry_resolve:
 * @self: a #GsurfContainerRegistry
 * @opener: (nullable): the opening view
 * @requested: (nullable): requested container id
 * @uri: (nullable): first URI, when known
 * @related: whether the view is a popup that must share @opener's session
 *
 * Decides the container of a new view: a related popup always uses its
 * opener's container; otherwise the resolver (if any) decides, else
 * @requested. #GSURF_CONTAINER_TEMPORARY becomes a new temporary
 * container, #GSURF_CONTAINER_DEFAULT the default container (%NULL), and
 * an unknown id falls back to the default container with a warning. Called by #GsurfView during construction.
 *
 * Returns: (transfer full) (nullable): the container id, %NULL = default
 */
gchar *gsurf_container_registry_resolve(GsurfContainerRegistry *self,
                                        GsurfView *opener,
                                        const gchar *requested,
                                        const gchar *uri,
                                        gboolean related);

/**
 * gsurf_container_registry_set_session_backend:
 * @self: a #GsurfContainerRegistry
 * @backend: (nullable): the backend vtable (static storage), or %NULL
 *
 * Called by a backend once, before it creates views in a container.
 * Without a backend every view uses the default session.
 */
void gsurf_container_registry_set_session_backend(GsurfContainerRegistry *self,
                                                  const GsurfSessionBackend *backend);

/**
 * gsurf_container_registry_acquire:
 * @self: a #GsurfContainerRegistry
 * @id: (nullable): container id from gsurf_container_registry_resolve()
 *
 * Counts one more view in container @id and returns its native session,
 * creating it on first use. Pair with gsurf_container_registry_release().
 *
 * Returns: (transfer none) (nullable): the native session, or %NULL for
 *   the default session (also when @id is unknown or no backend is set)
 */
gpointer gsurf_container_registry_acquire(GsurfContainerRegistry *self,
                                          const gchar *id);

/**
 * gsurf_container_registry_release:
 * @self: a #GsurfContainerRegistry
 * @id: (nullable): a container id passed to acquire
 *
 * Counts one view less. A temporary container whose count reaches zero
 * is destroyed after the cleanup delay unless a view acquires it again.
 */
void gsurf_container_registry_release(GsurfContainerRegistry *self,
                                      const gchar *id);

/**
 * gsurf_container_registry_set_temp_cleanup_delay:
 * @self: a #GsurfContainerRegistry
 * @seconds: delay before an unused temporary container is destroyed
 *
 * Zero destroys it from an idle callback right after the last release.
 */
void gsurf_container_registry_set_temp_cleanup_delay(GsurfContainerRegistry *self,
                                                     guint seconds);

/**
 * gsurf_container_registry_set_policy:
 * @self: a #GsurfContainerRegistry
 * @itp: enable intelligent tracking prevention
 * @cookie_policy: -1 to leave alone, else 0 always, 1 never, 2 no third-party
 *
 * Applies a privacy policy to the default session and every container
 * session, now and as they are created (used by the hardening module).
 */
void gsurf_container_registry_set_policy(GsurfContainerRegistry *self,
                                         gboolean itp, gint cookie_policy);

/**
 * gsurf_container_registry_get_data_dir:
 * @id: a persistent container id
 *
 * Returns: (transfer full) (type filename): $XDG_DATA_HOME/gsurf/containers/@id
 */
gchar *gsurf_container_registry_get_data_dir(const gchar *id);

/**
 * gsurf_container_registry_get_cache_dir:
 * @id: a persistent container id
 *
 * Returns: (transfer full) (type filename): $XDG_CACHE_HOME/gsurf/containers/@id
 */
gchar *gsurf_container_registry_get_cache_dir(const gchar *id);

G_END_DECLS

#endif /* GSURF_CONTAINER_REGISTRY_H */
