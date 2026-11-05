/*
 * gsurf-container-registry.c - Isolated browsing containers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/gsurf-container-registry.h"
#include "util/gsurf-private-file.h"
#include "util/gsurf-runtime-dir.h"

#include <string.h>

#define TEMP_PREFIX "tmp-"
#define DEFAULT_TEMP_CLEANUP_DELAY (30)
#define MAX_ID_LENGTH (64)

G_DEFINE_QUARK(gsurf-container-error-quark, gsurf_container_error)

/* ===================================================================
 * GsurfContainer
 * =================================================================== */

struct _GsurfContainer
{
	GObject   parent_instance;

	gchar    *id;
	gchar    *name;
	gchar    *color;
	gboolean  temporary;
	guint     n_views;      /* live views using the container */
	gpointer  native;       /* backend session, created on first acquire */
	gchar    *data_dir;     /* data directory of the session, if any */
	gchar    *temp_root;    /* temporary container: its private runtime
	                         * directory, removed with the session */
	guint     cleanup_id;   /* pending temporary-container destruction */
	gpointer  registry;     /* owning GsurfContainerRegistry (borrowed: it
	                         * outlives every container it holds) */
};

enum {
	CONTAINER_PROP_0,
	CONTAINER_PROP_ID,
	CONTAINER_PROP_NAME,
	CONTAINER_PROP_COLOR,
	CONTAINER_PROP_TEMPORARY,
	CONTAINER_PROP_N_VIEWS,
	CONTAINER_N_PROPS
};

static GParamSpec *container_props[CONTAINER_N_PROPS];

G_DEFINE_FINAL_TYPE(GsurfContainer, gsurf_container, G_TYPE_OBJECT)

static void
gsurf_container_finalize(GObject *object)
{
	GsurfContainer *self = GSURF_CONTAINER(object);

	/* The registry destroys the native session and cancels the cleanup
	 * timer before it drops its reference; nothing native is left here. */
	g_clear_handle_id(&self->cleanup_id, g_source_remove);
	g_free(self->data_dir);
	g_free(self->temp_root);
	g_free(self->id);
	g_free(self->name);
	g_free(self->color);
	G_OBJECT_CLASS(gsurf_container_parent_class)->finalize(object);
}

static void
gsurf_container_get_property(GObject *object, guint prop_id,
                             GValue *value, GParamSpec *pspec)
{
	GsurfContainer *self = GSURF_CONTAINER(object);

	switch (prop_id) {
	case CONTAINER_PROP_ID:        g_value_set_string(value, self->id); break;
	case CONTAINER_PROP_NAME:      g_value_set_string(value, self->name); break;
	case CONTAINER_PROP_COLOR:     g_value_set_string(value, self->color); break;
	case CONTAINER_PROP_TEMPORARY: g_value_set_boolean(value, self->temporary); break;
	case CONTAINER_PROP_N_VIEWS:   g_value_set_uint(value, self->n_views); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
gsurf_container_class_init(GsurfContainerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gsurf_container_finalize;
	object_class->get_property = gsurf_container_get_property;

	/**
	 * GsurfContainer:id:
	 *
	 * The stable, path-safe container id.
	 */
	container_props[CONTAINER_PROP_ID] = g_param_spec_string("id", NULL, NULL,
		NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfContainer:name:
	 *
	 * The display name.
	 */
	container_props[CONTAINER_PROP_NAME] = g_param_spec_string("name", NULL, NULL,
		NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
	/**
	 * GsurfContainer:color:
	 *
	 * The colour (CSS colour string), or %NULL.
	 */
	container_props[CONTAINER_PROP_COLOR] = g_param_spec_string("color", NULL, NULL,
		NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
	/**
	 * GsurfContainer:temporary:
	 *
	 * Whether the container is ephemeral.
	 */
	container_props[CONTAINER_PROP_TEMPORARY] = g_param_spec_boolean("temporary", NULL, NULL,
		FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfContainer:n-views:
	 *
	 * Number of live views using the container.
	 */
	container_props[CONTAINER_PROP_N_VIEWS] = g_param_spec_uint("n-views", NULL, NULL,
		0, G_MAXUINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

	g_object_class_install_properties(object_class, CONTAINER_N_PROPS, container_props);
}

static void
gsurf_container_init(GsurfContainer *self)
{
}

static GsurfContainer *
container_new(const gchar *id, const gchar *name, const gchar *color, gboolean temporary)
{
	GsurfContainer *self = g_object_new(GSURF_TYPE_CONTAINER, NULL);

	self->id = g_strdup(id);
	self->name = g_strdup((name != NULL && *name != '\0') ? name : id);
	self->color = (color != NULL && *color != '\0') ? g_strdup(color) : NULL;
	self->temporary = temporary;
	return self;
}

const gchar *
gsurf_container_get_id(GsurfContainer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTAINER(self), NULL);
	return self->id;
}

const gchar *
gsurf_container_get_name(GsurfContainer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTAINER(self), NULL);
	return self->name;
}

const gchar *
gsurf_container_get_color(GsurfContainer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTAINER(self), NULL);
	return self->color;
}

gboolean
gsurf_container_get_temporary(GsurfContainer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTAINER(self), FALSE);
	return self->temporary;
}

guint
gsurf_container_get_n_views(GsurfContainer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTAINER(self), 0);
	return self->n_views;
}

const gchar *
gsurf_container_get_data_dir(GsurfContainer *self)
{
	g_return_val_if_fail(GSURF_IS_CONTAINER(self), NULL);
	return self->data_dir;
}

/* ===================================================================
 * GsurfContainerRegistry
 * =================================================================== */

struct _GsurfContainerRegistry
{
	GObject parent_instance;

	GPtrArray                 *containers;   /* GsurfContainer*, owned; definition order */
	GHashTable                *by_id;        /* id -> GsurfContainer* (borrowed) */
	guint                      next_temp;    /* counter for "tmp-N" ids */
	guint                      temp_delay;   /* seconds */

	GsurfContainerResolveFunc  resolver;
	gpointer                   resolver_data;
	GDestroyNotify             resolver_destroy;

	const GsurfSessionBackend *backend;

	gboolean                   policy_set;
	gboolean                   itp;
	gint                       cookie_policy;
};

enum {
	SIG_CONTAINER_ADDED,
	SIG_CONTAINER_REMOVED,
	SIG_SESSION_CREATED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GsurfContainerRegistry, gsurf_container_registry, G_TYPE_OBJECT)

/* Destroy a container's native session (if any) through the backend.
 * A temporary container's data goes with it: its private directory
 * under the runtime dir is removed once the session is released. */
static void
container_drop_native(GsurfContainerRegistry *self, GsurfContainer *container)
{
	if (container->native != NULL) {
		if (self->backend != NULL && self->backend->destroy != NULL)
			self->backend->destroy(container->native);
		container->native = NULL;
	}
	g_clear_pointer(&container->data_dir, g_free);
	if (container->temp_root != NULL) {
		g_autoptr(GError) error = NULL;

		if (!gsurf_runtime_dir_remove_tree(container->temp_root, &error))
			g_warning("gsurf: container '%s': %s", container->id, error->message);
		g_clear_pointer(&container->temp_root, g_free);
	}
}

/* The private data/cache directories of a temporary container, below
 * this process's runtime directory. %FALSE (with a warning) when they
 * cannot be made; the backend then falls back to an ephemeral session. */
static gboolean
container_make_temp_dirs(GsurfContainer *container, gchar **data_dir, gchar **cache_dir)
{
	g_autofree gchar *root = gsurf_runtime_dir_get_temp_container_dir(container->id);
	g_autofree gchar *data = NULL;
	g_autofree gchar *cache = NULL;
	g_autoptr(GError) error = NULL;

	if (root == NULL) {
		g_warning("gsurf: container '%s': no runtime directory; "
			"using an ephemeral session", container->id);
		return FALSE;
	}
	/* A leftover of an earlier container with this id (ids restart in
	 * every process, and the process dir is fresh, so only after a failed
	 * removal) must not leak into this one. */
	gsurf_runtime_dir_remove_tree(root, NULL);
	data = g_build_filename(root, "data", NULL);
	cache = g_build_filename(root, "cache", NULL);
	if (!gsurf_private_mkdir(data, &error) || !gsurf_private_mkdir(cache, &error)) {
		g_warning("gsurf: container '%s': %s; using an ephemeral session",
			container->id, error->message);
		gsurf_runtime_dir_remove_tree(root, NULL);
		return FALSE;
	}
	container->temp_root = g_steal_pointer(&root);
	*data_dir = g_steal_pointer(&data);
	*cache_dir = g_steal_pointer(&cache);
	return TRUE;
}

/* Remove @container from the registry; emits ::container-removed. */
static void
registry_remove(GsurfContainerRegistry *self, GsurfContainer *container)
{
	g_autoptr(GsurfContainer) hold = g_object_ref(container);

	g_clear_handle_id(&container->cleanup_id, g_source_remove);
	container_drop_native(self, container);
	g_hash_table_remove(self->by_id, container->id);
	g_ptr_array_remove(self->containers, container);
	g_signal_emit(self, signals[SIG_CONTAINER_REMOVED], 0, container);
}

static void
gsurf_container_registry_dispose(GObject *object)
{
	GsurfContainerRegistry *self = GSURF_CONTAINER_REGISTRY(object);
	guint i;

	if (self->containers != NULL) {
		for (i = 0; i < self->containers->len; i++) {
			GsurfContainer *c = g_ptr_array_index(self->containers, i);

			g_clear_handle_id(&c->cleanup_id, g_source_remove);
			container_drop_native(self, c);
		}
	}
	if (self->resolver_destroy != NULL && self->resolver_data != NULL)
		self->resolver_destroy(self->resolver_data);
	self->resolver = NULL;
	self->resolver_data = NULL;
	self->resolver_destroy = NULL;
	G_OBJECT_CLASS(gsurf_container_registry_parent_class)->dispose(object);
}

static void
gsurf_container_registry_finalize(GObject *object)
{
	GsurfContainerRegistry *self = GSURF_CONTAINER_REGISTRY(object);

	g_clear_pointer(&self->by_id, g_hash_table_unref);
	g_clear_pointer(&self->containers, g_ptr_array_unref);
	G_OBJECT_CLASS(gsurf_container_registry_parent_class)->finalize(object);
}

static void
gsurf_container_registry_class_init(GsurfContainerRegistryClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = gsurf_container_registry_dispose;
	object_class->finalize = gsurf_container_registry_finalize;

	/**
	 * GsurfContainerRegistry::container-added:
	 * @registry: the registry
	 * @container: the new container
	 */
	signals[SIG_CONTAINER_ADDED] = g_signal_new("container-added",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_CONTAINER);

	/**
	 * GsurfContainerRegistry::container-removed:
	 * @registry: the registry
	 * @container: the removed container
	 */
	signals[SIG_CONTAINER_REMOVED] = g_signal_new("container-removed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_CONTAINER);

	/**
	 * GsurfContainerRegistry::session-created:
	 * @registry: the registry
	 * @container: the container whose session was created
	 * @native: the native session (WebKitWebContext or WebKitNetworkSession)
	 *
	 * Emitted after a container's native session is created and the
	 * registry's policy has been applied to it.
	 */
	signals[SIG_SESSION_CREATED] = g_signal_new("session-created",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, GSURF_TYPE_CONTAINER, G_TYPE_POINTER);
}

static void
gsurf_container_registry_init(GsurfContainerRegistry *self)
{
	self->containers = g_ptr_array_new_with_free_func(g_object_unref);
	self->by_id = g_hash_table_new(g_str_hash, g_str_equal);
	self->temp_delay = DEFAULT_TEMP_CLEANUP_DELAY;
	self->cookie_policy = -1;
}

GsurfContainerRegistry *
gsurf_container_registry_get_default(void)
{
	static GsurfContainerRegistry *registry = NULL;

	if (g_once_init_enter_pointer(&registry)) {
		GsurfContainerRegistry *created = g_object_new(GSURF_TYPE_CONTAINER_REGISTRY, NULL);

		g_once_init_leave_pointer(&registry, created);
	}
	return registry;
}

gboolean
gsurf_container_validate_id(const gchar *id, GError **error)
{
	const gchar *p;
	gsize len;

	if (id == NULL || *id == '\0') {
		g_set_error_literal(error, GSURF_CONTAINER_ERROR,
			GSURF_CONTAINER_ERROR_INVALID_ID, "empty container id");
		return FALSE;
	}
	len = strlen(id);
	if (len > MAX_ID_LENGTH) {
		g_set_error(error, GSURF_CONTAINER_ERROR, GSURF_CONTAINER_ERROR_INVALID_ID,
			"container id '%.16s…' is longer than %d characters", id, MAX_ID_LENGTH);
		return FALSE;
	}
	/* Ids become directory names: allow only a conservative alphabet. */
	for (p = id; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-') {
			g_set_error(error, GSURF_CONTAINER_ERROR,
				GSURF_CONTAINER_ERROR_INVALID_ID,
				"container id '%s' may only use letters, digits, '_' and '-'", id);
			return FALSE;
		}
	}
	if (g_strcmp0(id, GSURF_CONTAINER_TEMPORARY) == 0 ||
	    g_str_has_prefix(id, TEMP_PREFIX)) {
		g_set_error(error, GSURF_CONTAINER_ERROR, GSURF_CONTAINER_ERROR_INVALID_ID,
			"container id '%s' is reserved for temporary containers", id);
		return FALSE;
	}
	/* "default" names the engine's default session in requests. */
	if (g_strcmp0(id, GSURF_CONTAINER_DEFAULT) == 0) {
		g_set_error(error, GSURF_CONTAINER_ERROR, GSURF_CONTAINER_ERROR_INVALID_ID,
			"container id '%s' is reserved for the default container", id);
		return FALSE;
	}
	return TRUE;
}

GsurfContainer *
gsurf_container_registry_define(GsurfContainerRegistry *self, const gchar *id,
                                const gchar *name, const gchar *color,
                                GError **error)
{
	GsurfContainer *container;

	g_return_val_if_fail(GSURF_IS_CONTAINER_REGISTRY(self), NULL);

	if (!gsurf_container_validate_id(id, error))
		return NULL;

	/* Redefinition keeps the session and views; only the look changes. */
	container = g_hash_table_lookup(self->by_id, id);
	if (container != NULL) {
		g_free(container->name);
		container->name = g_strdup((name != NULL && *name != '\0') ? name : id);
		g_free(container->color);
		container->color = (color != NULL && *color != '\0') ? g_strdup(color) : NULL;
		g_object_notify_by_pspec(G_OBJECT(container), container_props[CONTAINER_PROP_NAME]);
		g_object_notify_by_pspec(G_OBJECT(container), container_props[CONTAINER_PROP_COLOR]);
		return container;
	}

	container = container_new(id, name, color, FALSE);
	container->registry = self;
	g_ptr_array_add(self->containers, container);
	g_hash_table_insert(self->by_id, container->id, container);
	g_signal_emit(self, signals[SIG_CONTAINER_ADDED], 0, container);
	return container;
}

gboolean
gsurf_container_registry_undefine(GsurfContainerRegistry *self, const gchar *id,
                                  GError **error)
{
	GsurfContainer *container;

	g_return_val_if_fail(GSURF_IS_CONTAINER_REGISTRY(self), FALSE);

	container = (id != NULL) ? g_hash_table_lookup(self->by_id, id) : NULL;
	if (container == NULL) {
		g_set_error(error, GSURF_CONTAINER_ERROR, GSURF_CONTAINER_ERROR_NOT_FOUND,
			"no container '%s'", id != NULL ? id : "(null)");
		return FALSE;
	}
	if (container->n_views > 0) {
		g_set_error(error, GSURF_CONTAINER_ERROR, GSURF_CONTAINER_ERROR_IN_USE,
			"container '%s' still has %u view(s)", id, container->n_views);
		return FALSE;
	}
	registry_remove(self, container);
	return TRUE;
}

/* Timer/idle: destroy a temporary container that stayed unused. */
static gboolean
on_temp_cleanup(gpointer user_data)
{
	GsurfContainer *container = user_data;
	GsurfContainerRegistry *self = container->registry;

	container->cleanup_id = 0;
	if (container->n_views == 0 && g_hash_table_lookup(self->by_id, container->id) == container)
		registry_remove(self, container);
	return G_SOURCE_REMOVE;
}

static void
schedule_temp_cleanup(GsurfContainerRegistry *self, GsurfContainer *container)
{
	g_clear_handle_id(&container->cleanup_id, g_source_remove);
	container->cleanup_id = (self->temp_delay == 0)
		? g_idle_add(on_temp_cleanup, container)
		: g_timeout_add_seconds(self->temp_delay, on_temp_cleanup, container);
}

GsurfContainer *
gsurf_container_registry_new_temporary(GsurfContainerRegistry *self, const gchar *color)
{
	g_autofree gchar *id = NULL;
	GsurfContainer *container;

	g_return_val_if_fail(GSURF_IS_CONTAINER_REGISTRY(self), NULL);

	/* tmp-1, tmp-2, …: skip any id still in use after a wrap-around. */
	do {
		g_free(id);
		id = g_strdup_printf(TEMP_PREFIX "%u", ++self->next_temp);
	} while (g_hash_table_contains(self->by_id, id));

	container = container_new(id, NULL, color, TRUE);
	container->registry = self;
	g_ptr_array_add(self->containers, container);
	g_hash_table_insert(self->by_id, container->id, container);
	g_signal_emit(self, signals[SIG_CONTAINER_ADDED], 0, container);

	/* A container nobody opens a view in must not live forever. */
	schedule_temp_cleanup(self, container);
	return container;
}

GsurfContainer *
gsurf_container_registry_lookup(GsurfContainerRegistry *self, const gchar *id)
{
	g_return_val_if_fail(GSURF_IS_CONTAINER_REGISTRY(self), NULL);
	return (id != NULL) ? g_hash_table_lookup(self->by_id, id) : NULL;
}

GPtrArray *
gsurf_container_registry_list(GsurfContainerRegistry *self)
{
	GPtrArray *out;
	guint i;
	gint pass;

	g_return_val_if_fail(GSURF_IS_CONTAINER_REGISTRY(self), NULL);

	out = g_ptr_array_new();
	/* Persistent first, then temporary, each in creation order. */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < self->containers->len; i++) {
			GsurfContainer *c = g_ptr_array_index(self->containers, i);

			if (c->temporary == (pass == 1))
				g_ptr_array_add(out, c);
		}
	}
	return out;
}

void
gsurf_container_registry_set_resolver(GsurfContainerRegistry *self,
                                      GsurfContainerResolveFunc func,
                                      gpointer user_data, GDestroyNotify destroy)
{
	g_return_if_fail(GSURF_IS_CONTAINER_REGISTRY(self));

	if (self->resolver_destroy != NULL && self->resolver_data != NULL)
		self->resolver_destroy(self->resolver_data);
	self->resolver = func;
	self->resolver_data = user_data;
	self->resolver_destroy = destroy;
}

gboolean
gsurf_container_registry_has_resolver(GsurfContainerRegistry *self)
{
	g_return_val_if_fail(GSURF_IS_CONTAINER_REGISTRY(self), FALSE);

	return self->resolver != NULL;
}

gchar *
gsurf_container_registry_resolve(GsurfContainerRegistry *self, GsurfView *opener,
                                 const gchar *requested, const gchar *uri,
                                 gboolean related)
{
	g_autofree gchar *chosen = NULL;

	g_return_val_if_fail(GSURF_IS_CONTAINER_REGISTRY(self), NULL);

	/* 1. A popup is created by WebKit as a related view of its opener and
	 *    must share its session, whatever any policy says. */
	if (related && opener != NULL) {
		const gchar *id = gsurf_view_get_container_id(opener);

		return (id != NULL && g_hash_table_contains(self->by_id, id)) ? g_strdup(id) : NULL;
	}

	/* 2. Policy, else the caller's request. */
	if (self->resolver != NULL)
		chosen = self->resolver(self, opener, requested, uri, self->resolver_data);
	else
		chosen = g_strdup(requested);

	if (chosen == NULL || *chosen == '\0' ||
	    g_strcmp0(chosen, GSURF_CONTAINER_DEFAULT) == 0)
		return NULL;

	/* 3. "temporary" means a brand-new temporary container. */
	if (g_strcmp0(chosen, GSURF_CONTAINER_TEMPORARY) == 0)
		return g_strdup(gsurf_container_get_id(
			gsurf_container_registry_new_temporary(self, NULL)));

	/* 4. Never place a view in a container that does not exist. */
	if (!g_hash_table_contains(self->by_id, chosen)) {
		g_warning("gsurf: unknown container '%s'; using the default container", chosen);
		return NULL;
	}
	return g_steal_pointer(&chosen);
}

void
gsurf_container_registry_set_session_backend(GsurfContainerRegistry *self,
                                             const GsurfSessionBackend *backend)
{
	g_return_if_fail(GSURF_IS_CONTAINER_REGISTRY(self));

	if (self->backend == backend)
		return;
	self->backend = backend;
	/* Bring the default session in line with a policy set earlier. */
	if (backend != NULL && self->policy_set && backend->apply_policy != NULL)
		backend->apply_policy(NULL, self->itp, self->cookie_policy);
}

gchar *
gsurf_container_registry_get_data_dir(const gchar *id)
{
	return g_build_filename(g_get_user_data_dir(), "gsurf", "containers", id, NULL);
}

gchar *
gsurf_container_registry_get_cache_dir(const gchar *id)
{
	return g_build_filename(g_get_user_cache_dir(), "gsurf", "containers", id, NULL);
}

gpointer
gsurf_container_registry_acquire(GsurfContainerRegistry *self, const gchar *id)
{
	GsurfContainer *container;

	g_return_val_if_fail(GSURF_IS_CONTAINER_REGISTRY(self), NULL);

	container = (id != NULL) ? g_hash_table_lookup(self->by_id, id) : NULL;
	if (container == NULL)
		return NULL;

	container->n_views++;
	g_clear_handle_id(&container->cleanup_id, g_source_remove);
	g_object_notify_by_pspec(G_OBJECT(container), container_props[CONTAINER_PROP_N_VIEWS]);

	/* Create the session on first use. A persistent container gets
	 * private data/cache directories under the XDG dirs; a temporary one
	 * gets them in this process's runtime directory (a tmpfs, removed
	 * with the container and at exit). Only if those cannot be made does
	 * it get none, which tells the backend to make an ephemeral session:
	 * WebKit's ephemeral sessions have no IndexedDB directory and write
	 * Blob files relative to the working directory. */
	if (container->native == NULL && self->backend != NULL && self->backend->create != NULL) {
		g_autofree gchar *data_dir = NULL;
		g_autofree gchar *cache_dir = NULL;

		if (container->temporary) {
			container_make_temp_dirs(container, &data_dir, &cache_dir);
		} else {
			g_autoptr(GError) error = NULL;

			data_dir = gsurf_container_registry_get_data_dir(container->id);
			cache_dir = gsurf_container_registry_get_cache_dir(container->id);
			if (!gsurf_private_mkdir(data_dir, &error) ||
			    !gsurf_private_mkdir(cache_dir, &error))
				g_warning("gsurf: container '%s': %s", container->id, error->message);
		}
		container->native = self->backend->create(container->id, data_dir, cache_dir);
		if (container->native != NULL)
			container->data_dir = g_strdup(data_dir);
		if (container->native != NULL) {
			if (self->policy_set && self->backend->apply_policy != NULL)
				self->backend->apply_policy(container->native, self->itp,
					self->cookie_policy);
			g_signal_emit(self, signals[SIG_SESSION_CREATED], 0, container,
				container->native);
		}
	}
	return container->native;
}

void
gsurf_container_registry_release(GsurfContainerRegistry *self, const gchar *id)
{
	GsurfContainer *container;

	g_return_if_fail(GSURF_IS_CONTAINER_REGISTRY(self));

	container = (id != NULL) ? g_hash_table_lookup(self->by_id, id) : NULL;
	if (container == NULL || container->n_views == 0)
		return;

	container->n_views--;
	g_object_notify_by_pspec(G_OBJECT(container), container_props[CONTAINER_PROP_N_VIEWS]);
	if (container->n_views == 0 && container->temporary)
		schedule_temp_cleanup(self, container);
}

void
gsurf_container_registry_set_temp_cleanup_delay(GsurfContainerRegistry *self,
                                                guint seconds)
{
	g_return_if_fail(GSURF_IS_CONTAINER_REGISTRY(self));
	self->temp_delay = seconds;
}

void
gsurf_container_registry_set_policy(GsurfContainerRegistry *self, gboolean itp,
                                    gint cookie_policy)
{
	guint i;

	g_return_if_fail(GSURF_IS_CONTAINER_REGISTRY(self));

	self->policy_set = TRUE;
	self->itp = itp;
	self->cookie_policy = CLAMP(cookie_policy, -1, 2);
	if (self->backend == NULL || self->backend->apply_policy == NULL)
		return;

	/* The default session, then every live container session. */
	self->backend->apply_policy(NULL, self->itp, self->cookie_policy);
	for (i = 0; i < self->containers->len; i++) {
		GsurfContainer *c = g_ptr_array_index(self->containers, i);

		if (c->native != NULL)
			self->backend->apply_policy(c->native, self->itp, self->cookie_policy);
	}
}
