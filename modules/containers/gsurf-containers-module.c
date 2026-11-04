/*
 * gsurf-containers-module.c - Container policy: placement and host assignment
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The core GsurfContainerRegistry isolates browsing identities (one
 * cookie jar, storage and cache per container); this module decides
 * which container a tab uses. It:
 *
 *   - defines the persistent containers listed in the config, each with
 *     a name, a colour and host patterns;
 *   - installs the registry resolver that places every new unrelated
 *     view (explicit request > host rule > opener > `new_tab` policy);
 *   - reassigns top-level navigations: a tab that navigates to a host
 *     assigned to another container is reopened in that container at
 *     the same position (#GsurfNavigationHook);
 *   - offers two actions, "container-new-temp" and
 *     "container-reopen-next", on configurable keys (#GsurfInputHandler,
 *     #GsurfActionHandler, #GsurfKeybindProvider).
 *
 * WebKit cannot move a view to another session once it exists, so every
 * container change means a new view. See docs/containers.org.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <string.h>

#define MODULE_NAME              "containers"
#define DEFAULT_NEW_TAB          GSURF_CONTAINER_TEMPORARY
#define DEFAULT_TEMP_DELAY       (30)
#define DEFAULT_TEMP_COLOR       "#89b4fa"
#define DEFAULT_KEY_NEW_TEMP     "Ctrl+Alt+t"
#define DEFAULT_KEY_REOPEN_NEXT  "Ctrl+Alt+c"

/* Object-data key of the per-view #ViewState. */
#define VIEW_STATE_KEY           "gsurf-containers-state"

#define GSURF_TYPE_CONTAINERS_MODULE (gsurf_containers_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfContainersModule, gsurf_containers_module,
                     GSURF, CONTAINERS_MODULE, GsurfModule)

struct _GsurfContainersModule
{
	GsurfModule parent_instance;

	/* --- configuration (replaced as a whole by every configure()) --- */
	gchar          *new_tab;            /* "temporary" | "default" | id */
	gboolean        inherit_opener;
	guint           temp_cleanup_delay; /* seconds */
	gchar          *temp_color;         /* NULL = no colour */
	gchar          *key_new_temp;       /* NULL = unbound */
	gchar          *key_reopen_next;    /* NULL = unbound */

	GsurfHostRules *rules;              /* host patterns, config order */
	GPtrArray      *rule_ids;           /* gchar*: container id of rule i */
	GPtrArray      *configured;         /* gchar*: ids of the config's containers */
	GPtrArray      *names;              /* gchar*: display name of configured[i] */
	GPtrArray      *colors;             /* gchar*: colour of configured[i] (or NULL) */

	/* --- registry state owned by this module --- */
	GPtrArray      *defined;            /* gchar*: ids this module defined */
	GPtrArray      *stale;              /* gchar*: dropped from the config but
	                                     * still in use; undefined once empty */
	gboolean        installed;          /* install() ran since the last uninstall() */
	gboolean        resolver_installed;
};

/* What the module remembers about a view it has seen navigate. */
typedef struct {
	gboolean  connected;      /* load-changed handler attached */
	gboolean  committed;      /* at least one load committed */
	gchar    *committed_uri;  /* URI of the last committed load */
	gboolean  exempt;         /* opened by this module: no reassignment
	                           * until its first load commits */
	gboolean  pending;        /* a reopen is already scheduled */
} ViewState;

/* A reassignment waiting for the idle callback (see schedule_reopen()). */
typedef struct {
	GsurfContainersModule *self;    /* strong ref */
	GWeakRef               view;
	gchar                 *uri;
	gchar                 *container;
	gboolean               close_old;
} PendingReopen;

static void gsurf_containers_nav_init(GsurfNavigationHookInterface *iface);
static void gsurf_containers_input_init(GsurfInputHandlerInterface *iface);
static void gsurf_containers_action_init(GsurfActionHandlerInterface *iface);
static void gsurf_containers_keybind_init(GsurfKeybindProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfContainersModule, gsurf_containers_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_NAVIGATION_HOOK, gsurf_containers_nav_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_containers_input_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_ACTION_HANDLER, gsurf_containers_action_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_KEYBIND_PROVIDER, gsurf_containers_keybind_init))

/* ===================================================================
 * Small helpers
 * =================================================================== */

/* Index of @id in a GPtrArray of strings, or -1. */
static gint
str_array_find(GPtrArray *array, const gchar *id)
{
	guint i;

	if (array == NULL || id == NULL)
		return -1;
	for (i = 0; i < array->len; i++) {
		if (g_strcmp0(g_ptr_array_index(array, i), id) == 0)
			return (gint)i;
	}
	return -1;
}

static void
str_array_remove(GPtrArray *array, const gchar *id)
{
	gint index = str_array_find(array, id);

	if (index >= 0)
		g_ptr_array_remove_index(array, (guint)index);
}

/* A string member of @m, or %NULL when absent, null or not a scalar. */
static const gchar *
mapping_string(YamlMapping *m, const gchar *key)
{
	YamlNode *node;

	if (!yaml_mapping_has_member(m, key))
		return NULL;
	node = yaml_mapping_get_member(m, key);
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_string(node);
}

/* A key option: absent keeps the default, "" or null unbinds. */
static gchar *
mapping_key(YamlMapping *m, const gchar *key, const gchar *fallback)
{
	const gchar *value;

	if (!yaml_mapping_has_member(m, key))
		return g_strdup(fallback);
	value = mapping_string(m, key);
	return (value != NULL && *value != '\0') ? g_strdup(value) : NULL;
}

/**
 * derive_id:
 * @name: a container's display name
 *
 * Builds the default id of a container from its name: ASCII letters are
 * lowercased, digits, '_' and '-' are kept, and every other character
 * (spaces, punctuation, each non-ASCII character) becomes '-'. The
 * result still has to pass gsurf_container_validate_id().
 *
 * Returns: (transfer full): the derived id
 */
static gchar *
derive_id(const gchar *name)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = name; p != NULL && *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c < 0x80 && (g_ascii_isalnum((gchar)c) || c == '_' || c == '-'))
			g_string_append_c(out, g_ascii_tolower((gchar)c));
		else
			g_string_append_c(out, '-');
	}
	return g_string_free(out, FALSE);
}

/* Per-view state, created on first use and freed with the view. */
static void
view_state_free(gpointer data)
{
	ViewState *state = data;

	g_free(state->committed_uri);
	g_free(state);
}

static ViewState *
view_state(GsurfView *view)
{
	ViewState *state = g_object_get_data(G_OBJECT(view), VIEW_STATE_KEY);

	if (state == NULL) {
		state = g_new0(ViewState, 1);
		g_object_set_data_full(G_OBJECT(view), VIEW_STATE_KEY, state, view_state_free);
	}
	return state;
}

/* The window that owns @view, searching every window of the host. */
static GsurfWindow *
window_for_view(GsurfView *view)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfApplication *app = gsurf_module_manager_get_application(mgr);
	GPtrArray *windows;
	guint i;

	if (app == NULL || view == NULL)
		return NULL;
	windows = gsurf_application_get_windows(app);
	for (i = 0; windows != NULL && i < windows->len; i++) {
		GsurfWindow *window = g_ptr_array_index(windows, i);

		if (gsurf_window_get_view_index(window, view) >= 0)
			return window;
	}
	return NULL;
}

/* "Nothing to lose" URIs: a tab showing one of these with no history is
 * replaced (closed) rather than kept when it is reassigned. */
static gboolean
is_blank_uri(const gchar *uri)
{
	GsurfConfig *config = gsurf_config_get_default();

	if (uri == NULL || *uri == '\0' || g_strcmp0(uri, "about:blank") == 0)
		return TRUE;
	if (config == NULL)
		return FALSE;
	return g_strcmp0(uri, config->new_view_uri) == 0 ||
	       g_strcmp0(uri, config->homepage) == 0;
}

/* ===================================================================
 * Opening views
 * =================================================================== */

static void on_view_load_changed(GsurfView *view, GsurfLoadEvent event, gpointer data);

/* Follow @view's loads, to learn when it first commits. */
static void
track_view(GsurfContainersModule *self, GsurfView *view)
{
	ViewState *state = view_state(view);

	if (state->connected)
		return;
	state->connected = TRUE;
	g_signal_connect_object(view, "load-changed", G_CALLBACK(on_view_load_changed),
		self, 0);
}

static void
on_view_load_changed(GsurfView *view, GsurfLoadEvent event, gpointer data)
{
	ViewState *state;

	(void)data;
	if (event != GSURF_LOAD_COMMITTED && event != GSURF_LOAD_FINISHED)
		return;
	/* The first load that commits (or ends, even failing) ends the
	 * module's exemption: from now on the tab is reassigned like any
	 * other. */
	state = view_state(view);
	state->exempt = FALSE;
	if (event != GSURF_LOAD_COMMITTED)
		return;
	state->committed = TRUE;
	g_free(state->committed_uri);
	state->committed_uri = g_strdup(gsurf_view_get_uri(view));
}

/**
 * open_view:
 * @self: the module
 * @window: where the view goes
 * @index: its position in @window (values past the end append)
 * @opener: (nullable): its opener (tab-tree parent)
 * @container: (nullable): requested container id or token
 * @uri: (nullable): first URI to load
 *
 * Creates a view the way the host's make_tab() does (config settings,
 * #GsurfWindow::view-added wiring), places it at @index, activates it
 * and starts loading @uri. The view is exempt from host reassignment
 * until its first load commits, so an explicit placement (and the
 * redirects of that first load) is honoured.
 *
 * Returns: (transfer none): the view, owned by @window
 */
static GsurfView *
open_view(GsurfContainersModule *self, GsurfWindow *window, guint index,
          GsurfView *opener, const gchar *container, const gchar *uri)
{
	GsurfConfig *config = gsurf_config_get_default();
	g_autoptr(GsurfView) view = NULL;

	view = gsurf_view_new_full(opener, container, uri, FALSE);
	if (view == NULL)
		return NULL;
	if (config != NULL && config->settings != NULL)
		gsurf_view_apply_settings(view, config->settings);

	view_state(view)->exempt = TRUE;
	track_view(self, view);

	gsurf_window_insert_view(window, view, index);
	gsurf_window_set_active_view(window, view);
	if (uri != NULL && *uri != '\0')
		gsurf_view_load_uri(view, uri);
	return view;
}

/* ===================================================================
 * Configuration and registry state
 * =================================================================== */

/* Undefine container @id if no view uses it; otherwise keep it on the
 * stale list until its last view closes (see on_container_n_views()). */
static void
release_container(GsurfContainersModule *self, const gchar *id)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	g_autoptr(GError) error = NULL;
	GsurfContainer *container = gsurf_container_registry_lookup(registry, id);

	if (container == NULL)
		return;
	if (gsurf_container_registry_undefine(registry, id, &error))
		return;
	if (g_error_matches(error, GSURF_CONTAINER_ERROR, GSURF_CONTAINER_ERROR_IN_USE)) {
		g_message("gsurf: containers: keeping '%s' until its last tab closes (%s)",
			id, error->message);
		if (str_array_find(self->stale, id) < 0)
			g_ptr_array_add(self->stale, g_strdup(id));
		return;
	}
	g_warning("gsurf: containers: cannot remove '%s': %s", id, error->message);
}

/* notify::n-views on a stale container: undefine it once unused. */
static void
on_container_n_views(GsurfContainer *container, GParamSpec *pspec, gpointer data)
{
	GsurfContainersModule *self = data;
	const gchar *id = gsurf_container_get_id(container);
	g_autofree gchar *held = NULL;

	(void)pspec;
	if (gsurf_container_get_n_views(container) > 0 || str_array_find(self->stale, id) < 0)
		return;
	held = g_strdup(id);
	g_signal_handlers_disconnect_by_func(container, on_container_n_views, self);
	str_array_remove(self->stale, held);
	str_array_remove(self->defined, held);
	gsurf_container_registry_undefine(gsurf_container_registry_get_default(), held, NULL);
}

/* Undefine every container this module defined that is not in @keep
 * (NULL = undefine all). In-use ones become stale and go later. */
static void
drop_containers(GsurfContainersModule *self, GPtrArray *keep)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	guint i = 0;

	while (i < self->defined->len) {
		const gchar *id = g_ptr_array_index(self->defined, i);
		GsurfContainer *container;

		if (keep != NULL && str_array_find(keep, id) >= 0) {
			i++;
			continue;
		}
		container = gsurf_container_registry_lookup(registry, id);
		if (container == NULL) {
			g_ptr_array_remove_index(self->defined, i);
			continue;
		}
		release_container(self, id);
		if (gsurf_container_registry_lookup(registry, id) == NULL) {
			g_ptr_array_remove_index(self->defined, i);
			continue;
		}
		/* Still in use: watch it, keep it in `defined` until it goes. */
		g_signal_handlers_disconnect_by_func(container, on_container_n_views, self);
		g_signal_connect_object(container, "notify::n-views",
			G_CALLBACK(on_container_n_views), self, 0);
		i++;
	}
}

static gchar *resolve_container(GsurfContainerRegistry *registry, GsurfView *opener,
                                const gchar *requested, const gchar *uri,
                                gpointer user_data);

/* The registry dropped our resolver (replaced or removed). */
static void
on_resolver_removed(gpointer data)
{
	GSURF_CONTAINERS_MODULE(data)->resolver_installed = FALSE;
}

/* Define the configured containers and install the resolver. */
static void
install(GsurfContainersModule *self)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	guint i;

	for (i = 0; i < self->configured->len; i++) {
		const gchar *id = g_ptr_array_index(self->configured, i);
		g_autoptr(GError) error = NULL;

		if (gsurf_container_registry_define(registry, id,
				g_ptr_array_index(self->names, i), g_ptr_array_index(self->colors, i),
				&error) == NULL) {
			g_warning("gsurf: containers: cannot define '%s': %s", id, error->message);
			continue;
		}
		/* Configured again: no longer waiting to be removed. */
		if (str_array_find(self->stale, id) >= 0) {
			GsurfContainer *container = gsurf_container_registry_lookup(registry, id);

			str_array_remove(self->stale, id);
			if (container != NULL)
				g_signal_handlers_disconnect_by_func(container,
					on_container_n_views, self);
		}
		if (str_array_find(self->defined, id) < 0)
			g_ptr_array_add(self->defined, g_strdup(id));
	}

	gsurf_container_registry_set_temp_cleanup_delay(registry, self->temp_cleanup_delay);
	self->installed = TRUE;
	if (!self->resolver_installed) {
		gsurf_container_registry_set_resolver(registry, resolve_container, self,
			on_resolver_removed);
		self->resolver_installed = TRUE;
	}
}

/* Remove everything install() put in the registry. */
static void
uninstall(GsurfContainersModule *self)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();

	if (self->resolver_installed) {
		/* Clears resolver_installed through on_resolver_removed(). */
		gsurf_container_registry_set_resolver(registry, NULL, NULL, NULL);
		self->resolver_installed = FALSE;
	}
	/* Only undo what install() did: a module that never installed (it
	 * was always disabled) leaves another host's settings alone. */
	if (self->installed)
		gsurf_container_registry_set_temp_cleanup_delay(registry, DEFAULT_TEMP_DELAY);
	self->installed = FALSE;
	drop_containers(self, NULL);
}

/* Reset the configuration to the built-in defaults. */
static void
config_reset(GsurfContainersModule *self)
{
	g_free(self->new_tab);
	self->new_tab = g_strdup(DEFAULT_NEW_TAB);
	self->inherit_opener = TRUE;
	self->temp_cleanup_delay = DEFAULT_TEMP_DELAY;
	g_free(self->temp_color);
	self->temp_color = g_strdup(DEFAULT_TEMP_COLOR);
	g_free(self->key_new_temp);
	self->key_new_temp = g_strdup(DEFAULT_KEY_NEW_TEMP);
	g_free(self->key_reopen_next);
	self->key_reopen_next = g_strdup(DEFAULT_KEY_REOPEN_NEXT);
	gsurf_host_rules_clear(self->rules);
	g_ptr_array_set_size(self->rule_ids, 0);
	g_ptr_array_set_size(self->configured, 0);
	g_ptr_array_set_size(self->names, 0);
	g_ptr_array_set_size(self->colors, 0);
}

/**
 * parse_containers:
 * @self: the module
 * @node: the `containers:` sequence
 *
 * Reads the container list into self->configured (ids), self->names,
 * self->colors and the host rules. Every bad entry is reported and skipped; the rest
 * still apply.
 */
static void
parse_containers(GsurfContainersModule *self, YamlNode *node)
{
	YamlSequence *seq;
	guint i, n;

	if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
		return;
	if (yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE) {
		g_warning("gsurf: containers: `containers` must be a list; ignored");
		return;
	}
	seq = yaml_node_get_sequence(node);
	n = yaml_sequence_get_length(seq);
	for (i = 0; i < n; i++) {
		YamlNode *element = yaml_sequence_get_element(seq, i);
		YamlMapping *m;
		const gchar *name;
		const gchar *explicit_id;
		g_autofree gchar *id = NULL;
		g_autoptr(GError) error = NULL;
		YamlNode *hosts;

		/* 1. Each element is a mapping with at least a name. */
		if (element == NULL || yaml_node_get_node_type(element) != YAML_NODE_MAPPING) {
			g_warning("gsurf: containers: entry %u is not a mapping; skipped", i);
			continue;
		}
		m = yaml_node_get_mapping(element);
		name = mapping_string(m, "name");
		explicit_id = mapping_string(m, "id");
		if ((name == NULL || *name == '\0') && (explicit_id == NULL || *explicit_id == '\0')) {
			g_warning("gsurf: containers: entry %u has no `name`; skipped", i);
			continue;
		}

		/* 2. The id: explicit, else derived from the name; validated. */
		id = (explicit_id != NULL && *explicit_id != '\0')
			? g_strdup(explicit_id) : derive_id(name);
		if (!gsurf_container_validate_id(id, &error)) {
			g_warning("gsurf: containers: entry %u ('%s'): %s; skipped",
				i, name != NULL ? name : id, error->message);
			continue;
		}
		if (str_array_find(self->configured, id) >= 0) {
			g_warning("gsurf: containers: duplicate container id '%s' (entry %u); skipped",
				id, i);
			continue;
		}
		g_ptr_array_add(self->configured, g_strdup(id));
		g_ptr_array_add(self->names, g_strdup((name != NULL && *name != '\0') ? name : id));
		g_ptr_array_add(self->colors, g_strdup(mapping_string(m, "color")));

		/* 3. Host patterns: a bad regex loses only itself. */
		hosts = yaml_mapping_has_member(m, "hosts")
			? yaml_mapping_get_member(m, "hosts") : NULL;
		if (hosts == NULL || yaml_node_get_node_type(hosts) == YAML_NODE_NULL)
			continue;
		if (yaml_node_get_node_type(hosts) != YAML_NODE_SEQUENCE) {
			g_warning("gsurf: containers: '%s': `hosts` must be a list; ignored", id);
			continue;
		}
		{
			YamlSequence *patterns = yaml_node_get_sequence(hosts);
			guint j, np = yaml_sequence_get_length(patterns);

			for (j = 0; j < np; j++) {
				YamlNode *p = yaml_sequence_get_element(patterns, j);
				const gchar *pattern = (p != NULL &&
					yaml_node_get_node_type(p) == YAML_NODE_SCALAR)
					? yaml_node_get_string(p) : NULL;
				g_autoptr(GError) rerr = NULL;

				if (pattern == NULL || *pattern == '\0') {
					g_warning("gsurf: containers: '%s': host pattern %u is empty; skipped",
						id, j);
					continue;
				}
				if (!gsurf_host_rules_add(self->rules, pattern, NULL, &rerr)) {
					g_warning("gsurf: containers: '%s': bad host pattern '%s': %s",
						id, pattern, rerr->message);
					continue;
				}
				g_ptr_array_add(self->rule_ids, g_strdup(id));
			}
		}
	}
}

static void
gsurf_containers_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfContainersModule *self = GSURF_CONTAINERS_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m = NULL;
	gboolean enabled;

	/* 1. Start from the defaults: a second configure() replaces the
	 *    first completely, including going back to defaults. */
	config_reset(self);

	node = (config != NULL) ? gsurf_config_get_module_node(config, MODULE_NAME) : NULL;
	if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING)
		m = yaml_node_get_mapping(node);

	/* The manager sets `enabled` before configure(); a host calling
	 * configure() again directly gets the section's flag applied too. */
	if (m != NULL && yaml_mapping_has_member(m, "enabled"))
		gsurf_module_set_enabled(module, yaml_mapping_get_boolean_member(m, "enabled"));
	enabled = gsurf_module_get_enabled(module);

	/* 2. Scalar options. */
	if (m != NULL) {
		if (yaml_mapping_has_member(m, "inherit_opener"))
			self->inherit_opener = yaml_mapping_get_boolean_member(m, "inherit_opener");
		if (yaml_mapping_has_member(m, "temp_cleanup_delay")) {
			gint64 delay = yaml_mapping_get_int_member(m, "temp_cleanup_delay");

			if (delay < 0 || delay > G_MAXUINT32 / 2) {
				g_warning("gsurf: containers: temp_cleanup_delay %" G_GINT64_FORMAT
					" is out of range; using %d", delay, DEFAULT_TEMP_DELAY);
			} else {
				self->temp_cleanup_delay = (guint)delay;
			}
		}
		if (yaml_mapping_has_member(m, "temp_color")) {
			const gchar *color = mapping_string(m, "temp_color");

			g_free(self->temp_color);
			self->temp_color = (color != NULL && *color != '\0') ? g_strdup(color) : NULL;
		}
		g_free(self->key_new_temp);
		self->key_new_temp = mapping_key(m, "key_new_temp", DEFAULT_KEY_NEW_TEMP);
		g_free(self->key_reopen_next);
		self->key_reopen_next = mapping_key(m, "key_reopen_next", DEFAULT_KEY_REOPEN_NEXT);

		/* 3. Containers and their host rules. */
		parse_containers(self, yaml_mapping_has_member(m, "containers")
			? yaml_mapping_get_member(m, "containers") : NULL);

		/* 4. The new-tab policy may name a container defined above. */
		if (yaml_mapping_has_member(m, "new_tab")) {
			const gchar *policy = mapping_string(m, "new_tab");

			if (policy == NULL || *policy == '\0') {
				g_warning("gsurf: containers: empty `new_tab`; using '%s'", DEFAULT_NEW_TAB);
			} else if (g_strcmp0(policy, GSURF_CONTAINER_TEMPORARY) == 0 ||
			           g_strcmp0(policy, GSURF_CONTAINER_DEFAULT) == 0 ||
			           str_array_find(self->configured, policy) >= 0) {
				g_free(self->new_tab);
				self->new_tab = g_strdup(policy);
			} else {
				g_warning("gsurf: containers: `new_tab` names unknown container '%s'; "
					"using '%s'", policy, DEFAULT_NEW_TAB);
			}
		}
	}

	/* 5. Registry: drop what the old config defined and this one does
	 *    not, then define this config's containers and (if enabled) the
	 *    resolver. A disabled module leaves the registry alone. */
	if (enabled) {
		drop_containers(self, self->configured);
		install(self);
	} else {
		uninstall(self);
	}
}

/* ===================================================================
 * Placement policy (the registry resolver)
 * =================================================================== */

/* The container a host rule assigns @uri's host to, or %NULL. */
static const gchar *
assigned_container(GsurfContainersModule *self, const gchar *uri)
{
	g_autofree gchar *host = gsurf_host_from_uri(uri);
	gint index;

	if (host == NULL)
		return NULL;
	index = gsurf_host_rules_find(self->rules, host);
	if (index < 0 || (guint)index >= self->rule_ids->len)
		return NULL;
	return g_ptr_array_index(self->rule_ids, index);
}

/* A fresh temporary container in the configured colour. */
static gchar *
new_temporary(GsurfContainerRegistry *registry, GsurfContainersModule *self)
{
	return g_strdup(gsurf_container_get_id(
		gsurf_container_registry_new_temporary(registry, self->temp_color)));
}

/**
 * resolve_container:
 *
 * The #GsurfContainerResolveFunc installed while the module is enabled.
 * In order:
 *   1. an explicit request wins ("temporary" makes a coloured temporary
 *      container, "default" the default container, an id that id);
 *   2. a host rule matching the host of the initial-URI hint;
 *   3. with `inherit_opener`, the opener's container;
 *   4. the `new_tab` policy.
 * Related popups never get here: the registry keeps them in the
 * opener's container.
 */
static gchar *
resolve_container(GsurfContainerRegistry *registry, GsurfView *opener,
                  const gchar *requested, const gchar *uri, gpointer user_data)
{
	GsurfContainersModule *self = user_data;
	const gchar *assigned;

	/* 1. Explicit request. */
	if (requested != NULL && *requested != '\0') {
		if (g_strcmp0(requested, GSURF_CONTAINER_TEMPORARY) == 0)
			return new_temporary(registry, self);
		return g_strdup(requested);
	}

	/* 2. Host rule for the first URI. */
	assigned = assigned_container(self, uri);
	if (assigned != NULL && gsurf_container_registry_lookup(registry, assigned) != NULL)
		return g_strdup(assigned);

	/* 3. The opener's container (NULL = default). */
	if (self->inherit_opener && opener != NULL)
		return g_strdup(gsurf_view_get_container_id(opener));

	/* 4. New-tab policy. */
	if (g_strcmp0(self->new_tab, GSURF_CONTAINER_TEMPORARY) == 0)
		return new_temporary(registry, self);
	if (g_strcmp0(self->new_tab, GSURF_CONTAINER_DEFAULT) == 0)
		return NULL;
	return g_strdup(self->new_tab);
}

/* ===================================================================
 * Host assignment on navigation
 * =================================================================== */

static gboolean
on_reopen_idle(gpointer data)
{
	PendingReopen *pending = data;
	g_autoptr(GsurfView) view = g_weak_ref_get(&pending->view);
	GsurfWindow *window;
	GsurfView *opener;
	gint index;

	if (view == NULL)
		return G_SOURCE_REMOVE;
	view_state(view)->pending = FALSE;
	window = window_for_view(view);
	if (window == NULL || !gsurf_module_is_active(GSURF_MODULE(pending->self)))
		return G_SOURCE_REMOVE;
	index = gsurf_window_get_view_index(window, view);

	/* A replaced tab hands its place and its opener to the new one; a
	 * tab that stays becomes the new one's opener, just right of it. */
	if (pending->close_old) {
		opener = gsurf_view_get_opener(view);
		open_view(pending->self, window, (guint)index, opener, pending->container,
			pending->uri);
		gsurf_window_remove_view(window, view);
	} else {
		open_view(pending->self, window, (guint)index + 1, view, pending->container,
			pending->uri);
	}
	return G_SOURCE_REMOVE;
}

static void
pending_reopen_free(gpointer data)
{
	PendingReopen *pending = data;

	g_object_unref(pending->self);
	g_weak_ref_clear(&pending->view);
	g_free(pending->uri);
	g_free(pending->container);
	g_free(pending);
}

/**
 * maybe_reassign:
 * @self: the module
 * @view: the navigating view
 * @uri: where its main frame is going
 *
 * Decides whether a top-level navigation must move to another
 * container and, if so, schedules the reopen. The engine is in the
 * middle of a policy decision for @view, so the new view is created
 * (and the old one closed) from an idle callback, never re-entrantly.
 *
 * Returns: %GSURF_POLICY_IGNORE when the navigation moves, else USE
 */
static GsurfPolicyDecision
maybe_reassign(GsurfContainersModule *self, GsurfView *view, const gchar *uri)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	const gchar *target;
	ViewState *state;
	PendingReopen *pending;

	if (view == NULL || uri == NULL || gsurf_kiosk_is_enabled())
		return GSURF_POLICY_USE;
	track_view(self, view);
	state = view_state(view);

	/* 1. Popups stay with their opener: WebKit requires a related view
	 *    to share its opener's session (window.opener, OAuth flows). */
	if (gsurf_view_get_related(view))
		return GSURF_POLICY_USE;

	/* 2. Only hosts assigned to an existing, different container. */
	target = assigned_container(self, uri);
	if (target == NULL || gsurf_container_registry_lookup(registry, target) == NULL ||
	    g_strcmp0(gsurf_view_get_container_id(view), target) == 0)
		return GSURF_POLICY_USE;

	/* 3. Loop guard: a tab this module opened finishes its first load
	 *    where it was put, even if redirects end on a host assigned
	 *    elsewhere; otherwise X -> Y -> X redirects would spawn tabs
	 *    forever. Likewise a view created by anyone for this very host
	 *    (an explicit container request) keeps its first load. */
	if (state->exempt)
		return GSURF_POLICY_USE;
	if (!state->committed) {
		g_autofree gchar *initial = gsurf_host_from_uri(gsurf_view_get_initial_uri(view));
		g_autofree gchar *host = gsurf_host_from_uri(uri);

		if (initial != NULL && g_strcmp0(initial, host) == 0)
			return GSURF_POLICY_USE;
	}

	/* 4. One reopen per view at a time. */
	if (state->pending)
		return GSURF_POLICY_IGNORE;
	state->pending = TRUE;

	pending = g_new0(PendingReopen, 1);
	pending->self = g_object_ref(self);
	g_weak_ref_init(&pending->view, view);
	pending->uri = g_strdup(uri);
	pending->container = g_strdup(target);
	/* A tab with nothing to lose (never left a blank/new-tab page, no
	 * history) is replaced; anything else stays open. */
	pending->close_old = !gsurf_view_can_go_back(view) &&
		(!state->committed || is_blank_uri(state->committed_uri));
	g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, on_reopen_idle, pending, pending_reopen_free);
	return GSURF_POLICY_IGNORE;
}

/* before_navigate also sees iframe navigations and popup requests, and
 * cannot tell them from top-level ones. Only a navigation the view itself
 * reports as its own URI is certainly top-level: a load requested through
 * gsurf_view_load_uri() (typed address, bookmark, session restore), whose
 * URI the engine already shows. Those move before any request is sent;
 * everything else (link clicks, scripts, redirects) is caught at the
 * main-frame response below. */
static GsurfPolicyDecision
gsurf_containers_before_navigate(GsurfNavigationHook *hook, GsurfView *view, const gchar *uri)
{
	GsurfContainersModule *self = GSURF_CONTAINERS_MODULE(hook);

	if (view == NULL || uri == NULL || g_strcmp0(gsurf_view_get_uri(view), uri) != 0)
		return GSURF_POLICY_USE;
	return maybe_reassign(self, view, uri);
}

/* The main frame's response: known to be top-level, so every remaining
 * case (link clicks, script navigations, the end of a redirect chain)
 * is reassigned here. The request already went out with the old
 * container's cookies; see docs/containers.org, "Limits". */
static GsurfPolicyDecision
gsurf_containers_decide_main_response(GsurfNavigationHook *hook, GsurfView *view,
                                      const gchar *uri)
{
	return maybe_reassign(GSURF_CONTAINERS_MODULE(hook), view, uri);
}

static void
gsurf_containers_nav_init(GsurfNavigationHookInterface *iface)
{
	iface->before_navigate = gsurf_containers_before_navigate;
	iface->decide_main_response = gsurf_containers_decide_main_response;
}

/* ===================================================================
 * Actions
 * =================================================================== */

/* container-new-temp: a new active tab in a fresh temporary container. */
static gboolean
action_new_temp(GsurfContainersModule *self, GsurfView *view)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfConfig *config = gsurf_config_get_default();
	GsurfWindow *window = window_for_view(view);

	if (window == NULL)
		window = gsurf_module_manager_get_active_window(mgr);
	if (window == NULL)
		return FALSE;
	open_view(self, window, G_MAXUINT, NULL, GSURF_CONTAINER_TEMPORARY,
		(config != NULL && config->new_view_uri != NULL) ? config->new_view_uri
		                                                 : "about:blank");
	return TRUE;
}

/**
 * next_container:
 * @current: (nullable): the current container id (%NULL = default)
 *
 * The cycle is: default -> each persistent container in registry order
 * -> a new temporary container -> default.
 *
 * Returns: (transfer full): the request for the next container
 */
static gchar *
next_container(const gchar *current)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	g_autoptr(GPtrArray) all = gsurf_container_registry_list(registry);
	g_autoptr(GPtrArray) persistent = g_ptr_array_new();
	GsurfContainer *now = gsurf_container_registry_lookup(registry, current);
	guint i;

	for (i = 0; i < all->len; i++) {
		GsurfContainer *c = g_ptr_array_index(all, i);

		if (!gsurf_container_get_temporary(c))
			g_ptr_array_add(persistent, c);
	}

	/* Temporary -> default. */
	if (now != NULL && gsurf_container_get_temporary(now))
		return g_strdup(GSURF_CONTAINER_DEFAULT);

	/* Default -> first persistent; persistent i -> i + 1; the last one
	 * (or a default with no persistent containers) -> temporary. */
	if (now == NULL)
		return persistent->len > 0
			? g_strdup(gsurf_container_get_id(g_ptr_array_index(persistent, 0)))
			: g_strdup(GSURF_CONTAINER_TEMPORARY);
	for (i = 0; i < persistent->len; i++) {
		if (g_ptr_array_index(persistent, i) == now && i + 1 < persistent->len)
			return g_strdup(gsurf_container_get_id(g_ptr_array_index(persistent, i + 1)));
	}
	return g_strdup(GSURF_CONTAINER_TEMPORARY);
}

/* container-reopen-next: the current page, reopened in the next
 * container of the cycle, in the same place with the same opener
 * (gsurf_window_reopen_view(), shared with the tabs context menu). */
static gboolean
action_reopen_next(GsurfContainersModule *self, GsurfView *view)
{
	GsurfWindow *window;
	g_autofree gchar *next = NULL;
	GsurfView *fresh;

	if (view == NULL || (window = window_for_view(view)) == NULL)
		return FALSE;
	next = next_container(gsurf_view_get_container_id(view));
	fresh = gsurf_window_reopen_view(window, view, next);
	if (fresh == NULL)
		return FALSE;
	/* Explicit placement: exempt from host reassignment until its first
	 * load commits, like every view this module opens. */
	view_state(fresh)->exempt = TRUE;
	track_view(self, fresh);
	return TRUE;
}

static gboolean
gsurf_containers_handle_action(GsurfActionHandler *handler, GsurfView *view,
                               GsurfAction action)
{
	GsurfContainersModule *self = GSURF_CONTAINERS_MODULE(handler);

	if (action != GSURF_ACTION_CONTAINER_NEW_TEMP &&
	    action != GSURF_ACTION_CONTAINER_REOPEN_NEXT)
		return FALSE;
	/* Kiosk: never open views; consume the action so nothing else does. */
	if (gsurf_kiosk_is_enabled())
		return TRUE;
	if (action == GSURF_ACTION_CONTAINER_NEW_TEMP)
		return action_new_temp(self, view);
	return action_reopen_next(self, view);
}

static void
gsurf_containers_action_init(GsurfActionHandlerInterface *iface)
{
	iface->handle_action = gsurf_containers_handle_action;
}

static gboolean
gsurf_containers_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                                  guint keyval, guint keycode, guint state,
                                  GsurfModePolicy mode)
{
	GsurfContainersModule *self = GSURF_CONTAINERS_MODULE(handler);
	GsurfAction action = GSURF_ACTION_NONE;

	(void)keycode;
	(void)mode;
	if (self->key_new_temp != NULL && gsurf_keys_match(keyval, state, self->key_new_temp))
		action = GSURF_ACTION_CONTAINER_NEW_TEMP;
	else if (self->key_reopen_next != NULL &&
	         gsurf_keys_match(keyval, state, self->key_reopen_next))
		action = GSURF_ACTION_CONTAINER_REOPEN_NEXT;
	if (action == GSURF_ACTION_NONE || gsurf_kiosk_is_enabled())
		return FALSE;
	return gsurf_containers_handle_action(GSURF_ACTION_HANDLER(self), view, action);
}

static void
gsurf_containers_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_containers_handle_key_event;
}

static void
gsurf_containers_list_keybinds(GsurfKeybindProvider *provider, GPtrArray *entries)
{
	GsurfContainersModule *self = GSURF_CONTAINERS_MODULE(provider);

	gsurf_keybind_help_append(entries, self->key_new_temp,
		"New tab in a temporary container", MODULE_NAME, "container-new-temp");
	gsurf_keybind_help_append(entries, self->key_reopen_next,
		"Reopen the tab in the next container", MODULE_NAME, "container-reopen-next");
}

static void
gsurf_containers_keybind_init(GsurfKeybindProviderInterface *iface)
{
	iface->list_keybinds = gsurf_containers_list_keybinds;
}

/* ===================================================================
 * Module lifecycle
 * =================================================================== */

static const gchar *gsurf_containers_get_name(GsurfModule *m) { (void)m; return MODULE_NAME; }

static const gchar *
gsurf_containers_get_description(GsurfModule *m)
{
	(void)m;
	return "Container policy: per-host containers, temporary tabs, reopen in container";
}

/* configure() already installed the policy (it must exist before the
 * first view); activation after a deactivate() puts it back. */
static gboolean
gsurf_containers_activate(GsurfModule *module)
{
	GsurfContainersModule *self = GSURF_CONTAINERS_MODULE(module);

	if (!self->installed)
		install(self);
	return TRUE;
}

static void
gsurf_containers_deactivate(GsurfModule *module)
{
	uninstall(GSURF_CONTAINERS_MODULE(module));
}

static void
gsurf_containers_module_dispose(GObject *object)
{
	GsurfContainersModule *self = GSURF_CONTAINERS_MODULE(object);

	if (self->defined != NULL)
		uninstall(self);
	G_OBJECT_CLASS(gsurf_containers_module_parent_class)->dispose(object);
}

static void
gsurf_containers_module_finalize(GObject *object)
{
	GsurfContainersModule *self = GSURF_CONTAINERS_MODULE(object);

	g_free(self->new_tab);
	g_free(self->temp_color);
	g_free(self->key_new_temp);
	g_free(self->key_reopen_next);
	g_clear_object(&self->rules);
	g_clear_pointer(&self->rule_ids, g_ptr_array_unref);
	g_clear_pointer(&self->configured, g_ptr_array_unref);
	g_clear_pointer(&self->names, g_ptr_array_unref);
	g_clear_pointer(&self->colors, g_ptr_array_unref);
	g_clear_pointer(&self->defined, g_ptr_array_unref);
	g_clear_pointer(&self->stale, g_ptr_array_unref);
	G_OBJECT_CLASS(gsurf_containers_module_parent_class)->finalize(object);
}

static void
gsurf_containers_module_class_init(GsurfContainersModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->dispose = gsurf_containers_module_dispose;
	object_class->finalize = gsurf_containers_module_finalize;
	module_class->activate = gsurf_containers_activate;
	module_class->deactivate = gsurf_containers_deactivate;
	module_class->get_name = gsurf_containers_get_name;
	module_class->get_description = gsurf_containers_get_description;
	module_class->configure = gsurf_containers_configure;
}

static void
gsurf_containers_module_init(GsurfContainersModule *self)
{
	self->rules = gsurf_host_rules_new();
	self->rule_ids = g_ptr_array_new_with_free_func(g_free);
	self->configured = g_ptr_array_new_with_free_func(g_free);
	self->names = g_ptr_array_new_with_free_func(g_free);
	self->colors = g_ptr_array_new_with_free_func(g_free);
	self->defined = g_ptr_array_new_with_free_func(g_free);
	self->stale = g_ptr_array_new_with_free_func(g_free);
	config_reset(self);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_CONTAINERS_MODULE;
}
