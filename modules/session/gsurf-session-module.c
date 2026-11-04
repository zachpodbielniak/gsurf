/*
 * gsurf-session-module.c - Persist and restore open views
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Saves the open tabs of every window to a private (0600) YAML file and
 * restores them at startup:
 *
 *   version: 1
 *   windows:
 *     - active: 1                 # index of the active tab (optional)
 *       tabs:
 *         - uri: https://example.org/
 *         - uri: https://www.youtube.com/
 *           container: media      # absent = default container
 *           parent: 0             # index of the opener tab (optional)
 *         - uri: https://news.example/
 *           container: temporary:0
 *       layout: "<state>"           # window layout (optional, see below)
 *
 * Temporary containers are saved as group tokens ("temporary:<n>"):
 * tabs that shared one temporary container come back in one fresh
 * temporary container. Restore recreates each tab with its opener and
 * container (gsurf_view_new_full()), so the tabs module rebuilds the
 * tree from the openers.
 *
 * Window layouts (#GsurfWindowLayout, e.g. the wmux module): when a
 * window has a layout that can serialize, its state is saved as the
 * window's `layout:` string, with every view named by its saved tab index
 * (the numbering `parent` and `active` use; unsaved tabs map to -1). On
 * restore, once the tabs exist and the active tab is set, the layout of
 * the restored window gets the string back with the restored views
 * indexed the same way (gsurf_window_layout_restore()). The layout
 * module must activate before this one (wmux has priority -10) so the
 * layout is installed by then; a missing layout, or one that refuses the
 * string, is reported once and the default arrangement stays.
 *
 * Implements #GsurfNavigationHook (save after each committed
 * navigation), #GsurfInputHandler + #GsurfKeybindProvider (a manual save
 * key). It also saves when a tab closes or moves, and (coalesced in an
 * idle) when the window layout shows other views. The plain-text format
 * of earlier versions (one URI per line) is migrated once. See
 * docs/configuration.org, "Session".
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <string.h>

#define MODULE_NAME        "session"
#define SESSION_VERSION    (1)
#define DEFAULT_FILE       "session.yaml"
#define DEFAULT_KEY_SAVE   "Ctrl+Shift+s"
#define TEMP_TOKEN_PREFIX  GSURF_CONTAINER_TEMPORARY ":"

#define GSURF_TYPE_SESSION_MODULE (gsurf_session_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfSessionModule, gsurf_session_module,
                     GSURF, SESSION_MODULE, GsurfModule)

struct _GsurfSessionModule
{
	GsurfModule parent_instance;

	gchar    *file;         /* YAML session file */
	gchar    *legacy_file;  /* plain-text file migrated from, if present */
	gchar    *key_save;     /* NULL = unbound */
	gboolean  auto_restore;
	gboolean  restoring;    /* suppress saves while restore builds tabs */
	gboolean  connected;    /* window/application signals connected */
	guint     save_idle;    /* pending coalesced save, 0 = none */
};

/* One saved tab. */
typedef struct {
	gchar *uri;
	gchar *container;   /* NULL = default, "temporary:<n>", or an id */
	gint   parent;      /* index of the opener tab in its window, -1 = none */
	guint  file_index;  /* position in the file's `tabs` list (layout index) */
} SessionTab;

/* One saved window. */
typedef struct {
	GPtrArray *tabs;    /* SessionTab* */
	gint       active;  /* -1 = not recorded */
	gchar     *layout;  /* window layout state, NULL = none */
	guint      n_file_tabs; /* length of the file's `tabs` list */
} SessionWindow;

static void gsurf_session_nav_init(GsurfNavigationHookInterface *iface);
static void gsurf_session_input_init(GsurfInputHandlerInterface *iface);
static void gsurf_session_keybind_init(GsurfKeybindProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfSessionModule, gsurf_session_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_NAVIGATION_HOOK, gsurf_session_nav_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_session_input_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_KEYBIND_PROVIDER, gsurf_session_keybind_init))

/* ===================================================================
 * Session model
 * =================================================================== */

static void
session_tab_free(gpointer data)
{
	SessionTab *tab = data;

	g_free(tab->uri);
	g_free(tab->container);
	g_free(tab);
}

static SessionTab *
session_tab_new(const gchar *uri, const gchar *container, gint parent)
{
	SessionTab *tab = g_new0(SessionTab, 1);

	tab->uri = g_strdup(uri);
	tab->container = g_strdup(container);
	tab->parent = parent;
	return tab;
}

static void
session_window_free(gpointer data)
{
	SessionWindow *window = data;

	g_ptr_array_unref(window->tabs);
	g_free(window->layout);
	g_free(window);
}

static SessionWindow *
session_window_new(void)
{
	SessionWindow *window = g_new0(SessionWindow, 1);

	window->tabs = g_ptr_array_new_with_free_func(session_tab_free);
	window->active = -1;
	return window;
}

static GPtrArray *
session_new(void)
{
	return g_ptr_array_new_with_free_func(session_window_free);
}

/* ===================================================================
 * Paths
 * =================================================================== */

/**
 * set_file:
 * @self: the module
 * @configured: (nullable): the `file:` value (with `~/` expanded), or
 *   %NULL for the default
 *
 * The YAML file and the legacy plain-text file are derived from one
 * setting: a path ending in ".yaml"/".yml" is the YAML file and the
 * same path without the suffix is the legacy file; any other path (the
 * old default "~/.local/share/gsurf/session") names the legacy file and
 * the YAML file is that path plus ".yaml".
 */
static void
set_file(GsurfSessionModule *self, const gchar *configured)
{
	g_autofree gchar *path = NULL;

	g_clear_pointer(&self->file, g_free);
	g_clear_pointer(&self->legacy_file, g_free);
	path = (configured != NULL && *configured != '\0')
		? gsurf_expand_home(configured)
		: g_build_filename(g_get_user_data_dir(), "gsurf", DEFAULT_FILE, NULL);

	if (g_str_has_suffix(path, ".yaml")) {
		self->legacy_file = g_strndup(path, strlen(path) - strlen(".yaml"));
		self->file = g_steal_pointer(&path);
	} else if (g_str_has_suffix(path, ".yml")) {
		self->legacy_file = g_strndup(path, strlen(path) - strlen(".yml"));
		self->file = g_steal_pointer(&path);
	} else {
		self->file = g_strconcat(path, ".yaml", NULL);
		self->legacy_file = g_steal_pointer(&path);
	}
}

/* ===================================================================
 * Capture: open windows -> session model
 * =================================================================== */

/* The URI worth saving for @view, or %NULL to skip it. A tab that has
 * not committed anything yet still reports the URI it is loading. */
static const gchar *
view_save_uri(GsurfView *view)
{
	const gchar *uri = gsurf_view_get_uri(view);

	if (uri == NULL || *uri == '\0')
		uri = gsurf_view_get_initial_uri(view);
	if (uri == NULL || *uri == '\0' || g_strcmp0(uri, "about:blank") == 0)
		return NULL;
	return uri;
}

/* The saved form of @view's container: NULL for the default container,
 * the id of a persistent one, or "temporary:<n>" where <n> numbers the
 * distinct temporary containers of the whole session (@temps maps
 * temporary id -> group number). */
static gchar *
container_token(GsurfView *view, GHashTable *temps)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	const gchar *id = gsurf_view_get_container_id(view);
	GsurfContainer *container;
	gpointer group;

	if (id == NULL)
		return NULL;
	container = gsurf_container_registry_lookup(registry, id);
	if (container == NULL || !gsurf_container_get_temporary(container))
		return g_strdup(id);
	if (!g_hash_table_lookup_extended(temps, id, NULL, &group)) {
		group = GUINT_TO_POINTER(g_hash_table_size(temps));
		g_hash_table_insert(temps, g_strdup(id), group);
	}
	return g_strdup_printf(TEMP_TOKEN_PREFIX "%u", GPOINTER_TO_UINT(group));
}

/* GsurfWindowLayoutIndexFunc for capture_window(): the saved index of
 * @view (@user_data is the view -> index table), or -1 when it is not
 * saved (a blank tab). */
static gint
saved_index(GsurfView *view, gpointer user_data)
{
	gpointer index;

	if (!g_hash_table_lookup_extended(user_data, view, NULL, &index))
		return -1;
	return (gint)GPOINTER_TO_UINT(index);
}

/* Append @window's saveable tabs (and its layout, if any) to @session. */
static void
capture_window(GsurfWindow *window, GHashTable *temps, GPtrArray *session)
{
	g_autoptr(GHashTable) index_of = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_autoptr(GPtrArray) saved = g_ptr_array_new();
	SessionWindow *out = session_window_new();
	GsurfView *active = gsurf_window_get_active_view(window);
	guint i, n = gsurf_window_get_n_views(window);

	/* 1. Number the tabs that will be saved, in window order. */
	for (i = 0; i < n; i++) {
		GsurfView *view = gsurf_window_get_nth_view(window, i);
		const gchar *uri = view_save_uri(view);

		if (uri == NULL)
			continue;
		g_hash_table_insert(index_of, view, GUINT_TO_POINTER(saved->len));
		g_ptr_array_add(saved, view);
		g_ptr_array_add(out->tabs, session_tab_new(uri, NULL, -1));
	}

	/* 2. Containers and parents. A parent is the nearest saved ancestor
	 *    in this window: an opener that was skipped (a blank tab) or
	 *    lives elsewhere is passed over; a closed opener ends the chain. */
	for (i = 0; i < saved->len; i++) {
		GsurfView *view = g_ptr_array_index(saved, i);
		SessionTab *tab = g_ptr_array_index(out->tabs, i);
		GsurfView *opener = gsurf_view_get_opener(view);
		guint hops = 0;

		tab->container = container_token(view, temps);
		while (opener != NULL && opener != view && hops++ < n) {
			gpointer index;

			if (g_hash_table_lookup_extended(index_of, opener, NULL, &index)) {
				tab->parent = (gint)GPOINTER_TO_UINT(index);
				break;
			}
			opener = gsurf_view_get_opener(opener);
		}
		if (view == active)
			out->active = (gint)i;
	}

	/* 3. The window layout, with views named by their saved index. A
	 *    layout that cannot serialize (NULL) or has nothing to say ("")
	 *    leaves the field out. */
	out->n_file_tabs = out->tabs->len;
	for (i = 0; i < out->tabs->len; i++)
		((SessionTab *)g_ptr_array_index(out->tabs, i))->file_index = i;
	if (gsurf_window_get_layout(window) != NULL) {
		out->layout = gsurf_window_layout_serialize(gsurf_window_get_layout(window),
			window, saved_index, index_of);
		if (out->layout != NULL && *out->layout == '\0')
			g_clear_pointer(&out->layout, g_free);
	}
	g_ptr_array_add(session, out);
}

/* The windows to save: every window of the host application, else the
 * module manager's active window. */
static GPtrArray *
host_windows(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfApplication *app = gsurf_module_manager_get_application(mgr);
	GPtrArray *out = g_ptr_array_new();
	GsurfWindow *active;

	if (app != NULL) {
		GPtrArray *windows = gsurf_application_get_windows(app);
		guint i;

		for (i = 0; windows != NULL && i < windows->len; i++)
			g_ptr_array_add(out, g_ptr_array_index(windows, i));
	}
	active = gsurf_module_manager_get_active_window(mgr);
	if (out->len == 0 && active != NULL)
		g_ptr_array_add(out, active);
	return out;
}

/* ===================================================================
 * YAML
 * =================================================================== */

static gchar *
session_to_yaml(GPtrArray *session, GError **error)
{
	g_autoptr(YamlBuilder) builder = yaml_builder_new();
	g_autoptr(YamlGenerator) generator = yaml_generator_new();
	guint w, t;

	yaml_builder_begin_mapping(builder);
	yaml_builder_set_member_name(builder, "version");
	yaml_builder_add_int_value(builder, SESSION_VERSION);
	yaml_builder_set_member_name(builder, "windows");
	yaml_builder_begin_sequence(builder);
	for (w = 0; w < session->len; w++) {
		SessionWindow *window = g_ptr_array_index(session, w);

		yaml_builder_begin_mapping(builder);
		if (window->active >= 0) {
			yaml_builder_set_member_name(builder, "active");
			yaml_builder_add_int_value(builder, window->active);
		}
		yaml_builder_set_member_name(builder, "tabs");
		yaml_builder_begin_sequence(builder);
		for (t = 0; t < window->tabs->len; t++) {
			SessionTab *tab = g_ptr_array_index(window->tabs, t);

			yaml_builder_begin_mapping(builder);
			yaml_builder_set_member_name(builder, "uri");
			yaml_builder_add_string_value(builder, tab->uri);
			if (tab->container != NULL) {
				yaml_builder_set_member_name(builder, "container");
				yaml_builder_add_string_value(builder, tab->container);
			}
			if (tab->parent >= 0) {
				yaml_builder_set_member_name(builder, "parent");
				yaml_builder_add_int_value(builder, tab->parent);
			}
			yaml_builder_end_mapping(builder);
		}
		yaml_builder_end_sequence(builder);
		if (window->layout != NULL) {
			yaml_builder_set_member_name(builder, "layout");
			yaml_builder_add_string_value(builder, window->layout);
		}
		yaml_builder_end_mapping(builder);
	}
	yaml_builder_end_sequence(builder);
	yaml_builder_end_mapping(builder);

	yaml_generator_set_root(generator, yaml_builder_get_root(builder));
	return yaml_generator_to_data(generator, NULL, error);
}

/* A scalar member of @m as a string, or %NULL. */
static const gchar *
member_scalar(YamlMapping *m, const gchar *key)
{
	YamlNode *node;

	if (!yaml_mapping_has_member(m, key))
		return NULL;
	node = yaml_mapping_get_member(m, key);
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

/* A non-negative integer member of @m, or -1 (with a warning when the
 * member is present but not such an integer). */
static gint
member_index(YamlMapping *m, const gchar *key, const gchar *where)
{
	const gchar *text;
	guint64 value = 0;

	if (!yaml_mapping_has_member(m, key))
		return -1;
	text = member_scalar(m, key);
	if (text == NULL || !g_ascii_string_to_unsigned(text, 10, 0, G_MAXINT, &value, NULL)) {
		g_warning("gsurf: session: %s: `%s` is not an index; ignored", where, key);
		return -1;
	}
	return (gint)value;
}

/**
 * session_from_yaml:
 * @data: file contents
 * @error: return location for a #GError
 *
 * Parses a session file. A file that is not YAML, or whose root is not
 * a mapping with a `windows` list, is an error. Inside, every bad entry
 * (a tab without a URI, a parent that does not name another tab of the
 * window, an active index out of range, a `layout` that is not a
 * non-empty string) is reported and dropped, and the rest is restored.
 * Unknown keys are ignored (older gsurf ignores `layout` that way).
 *
 * Returns: (transfer full) (nullable): the session, or %NULL on error
 */
static GPtrArray *
session_from_yaml(const gchar *data, GError **error)
{
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(GPtrArray) session = session_new();
	YamlNode *root;
	YamlNode *windows_node;
	YamlSequence *windows;
	guint w, n_windows;

	if (!yaml_parser_load_from_data(parser, data, -1, error))
		return NULL;
	root = yaml_parser_get_root(parser);
	if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			"the session is not a YAML mapping");
		return NULL;
	}
	windows_node = yaml_mapping_has_member(yaml_node_get_mapping(root), "windows")
		? yaml_mapping_get_member(yaml_node_get_mapping(root), "windows") : NULL;
	if (windows_node == NULL || yaml_node_get_node_type(windows_node) != YAML_NODE_SEQUENCE) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			"the session has no `windows` list");
		return NULL;
	}

	windows = yaml_node_get_sequence(windows_node);
	n_windows = yaml_sequence_get_length(windows);
	for (w = 0; w < n_windows; w++) {
		YamlNode *wnode = yaml_sequence_get_element(windows, w);
		YamlMapping *wm;
		YamlNode *tabs_node;
		YamlSequence *tabs;
		SessionWindow *out;
		g_autoptr(GArray) keep = NULL;   /* file index -> kept index or -1 */
		g_autoptr(GArray) parents = NULL;
		guint t, n_tabs;
		gint active;

		if (wnode == NULL || yaml_node_get_node_type(wnode) != YAML_NODE_MAPPING) {
			g_warning("gsurf: session: window %u is not a mapping; skipped", w);
			continue;
		}
		wm = yaml_node_get_mapping(wnode);
		tabs_node = yaml_mapping_has_member(wm, "tabs")
			? yaml_mapping_get_member(wm, "tabs") : NULL;
		if (tabs_node == NULL || yaml_node_get_node_type(tabs_node) != YAML_NODE_SEQUENCE) {
			g_warning("gsurf: session: window %u has no `tabs` list; skipped", w);
			continue;
		}
		tabs = yaml_node_get_sequence(tabs_node);
		n_tabs = yaml_sequence_get_length(tabs);
		out = session_window_new();
		out->n_file_tabs = n_tabs;
		keep = g_array_sized_new(FALSE, FALSE, sizeof(gint), n_tabs);
		parents = g_array_sized_new(FALSE, FALSE, sizeof(gint), n_tabs);

		/* 1. Tabs with a URI; remember each one's file index. */
		for (t = 0; t < n_tabs; t++) {
			YamlNode *tnode = yaml_sequence_get_element(tabs, t);
			g_autofree gchar *where = g_strdup_printf("window %u tab %u", w, t);
			YamlMapping *tm;
			const gchar *uri;
			gint kept = -1;
			gint parent = -1;

			if (tnode != NULL && yaml_node_get_node_type(tnode) == YAML_NODE_MAPPING) {
				tm = yaml_node_get_mapping(tnode);
				uri = member_scalar(tm, "uri");
				if (uri != NULL && *uri != '\0') {
					const gchar *container = member_scalar(tm, "container");

					SessionTab *tab;

					kept = (gint)out->tabs->len;
					parent = member_index(tm, "parent", where);
					tab = session_tab_new(uri,
						(container != NULL && *container != '\0') ? container : NULL,
						-1);
					tab->file_index = t;
					g_ptr_array_add(out->tabs, tab);
				}
			}
			if (kept < 0)
				g_warning("gsurf: session: %s has no `uri`; skipped", where);
			g_array_append_val(keep, kept);
			g_array_append_val(parents, parent);
		}

		/* 2. Parents refer to file indices; map them to kept tabs. A
		 *    parent that is the tab itself, out of range or dropped is
		 *    ignored (the tab becomes a root). */
		for (t = 0; t < n_tabs; t++) {
			gint kept = g_array_index(keep, gint, t);
			gint parent = g_array_index(parents, gint, t);

			if (kept < 0 || parent < 0)
				continue;
			if ((guint)parent >= n_tabs || (guint)parent == t ||
			    g_array_index(keep, gint, parent) < 0) {
				g_warning("gsurf: session: window %u tab %u: parent %d is not "
					"another tab; ignored", w, t, parent);
				continue;
			}
			((SessionTab *)g_ptr_array_index(out->tabs, kept))->parent =
				g_array_index(keep, gint, parent);
		}

		/* 3. The active tab, by file index too. */
		active = member_index(wm, "active", "window");
		if (active >= 0 && (guint)active < n_tabs && g_array_index(keep, gint, active) >= 0)
			out->active = g_array_index(keep, gint, active);
		else if (active >= 0)
			g_warning("gsurf: session: window %u: active tab %d does not exist; ignored",
				w, active);

		/* 4. The window layout: an opaque string for the layout module,
		 *    whose indices are file indices (like `parent`). */
		if (yaml_mapping_has_member(wm, "layout")) {
			const gchar *layout = member_scalar(wm, "layout");

			if (layout == NULL || *layout == '\0')
				g_warning("gsurf: session: window %u: `layout` is not a non-empty "
					"string; ignored", w);
			else
				out->layout = g_strdup(layout);
		}

		if (out->tabs->len == 0) {
			session_window_free(out);
			continue;
		}
		g_ptr_array_add(session, out);
	}
	return g_steal_pointer(&session);
}

/* The plain-text format of gsurf <= 0.2: one URI per line. */
static GPtrArray *
session_from_legacy(const gchar *data)
{
	GPtrArray *session = session_new();
	SessionWindow *window = session_window_new();
	g_auto(GStrv) lines = g_strsplit(data, "\n", -1);
	guint i;

	for (i = 0; lines[i] != NULL; i++) {
		const gchar *uri = g_strstrip(lines[i]);
		SessionTab *tab;

		if (*uri == '\0')
			continue;
		tab = session_tab_new(uri, NULL, -1);
		tab->file_index = window->tabs->len;
		g_ptr_array_add(window->tabs, tab);
	}
	window->n_file_tabs = window->tabs->len;
	if (window->tabs->len > 0) {
		window->active = 0;
		g_ptr_array_add(session, window);
	} else {
		session_window_free(window);
	}
	return session;
}

static gboolean
session_write(GsurfSessionModule *self, GPtrArray *session, GError **error)
{
	g_autofree gchar *data = session_to_yaml(session, error);

	if (data == NULL)
		return FALSE;
	/* The session lists the URLs of open tabs: private (0600 in 0700). */
	return gsurf_private_write(self->file, data, -1, error);
}

/* ===================================================================
 * Save
 * =================================================================== */

static void
session_save(GsurfSessionModule *self)
{
	g_autoptr(GPtrArray) windows = NULL;
	g_autoptr(GPtrArray) session = NULL;
	g_autoptr(GHashTable) temps = NULL;
	g_autoptr(GError) error = NULL;
	guint i, n_views = 0;

	if (self->file == NULL || self->restoring)
		return;
	windows = host_windows();
	for (i = 0; i < windows->len; i++)
		n_views += gsurf_window_get_n_views(g_ptr_array_index(windows, i));
	/* No tab at all (the last one just closed, or no window yet): keep
	 * the previous session instead of saving an empty one. */
	if (n_views == 0)
		return;

	session = session_new();
	temps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (i = 0; i < windows->len; i++)
		capture_window(g_ptr_array_index(windows, i), temps, session);
	if (!session_write(self, session, &error))
		g_warning("gsurf: session: cannot save '%s': %s", self->file, error->message);
}

/* ===================================================================
 * Restore
 * =================================================================== */

/* Resolve a saved container token to a request for gsurf_view_new_full():
 * "default" for the default container, a persistent id that exists, or
 * the id a temporary group already got (@groups) — else "temporary" for
 * the first tab of a group, whose view then names the group's container. */
static const gchar *
container_request(const gchar *token, GHashTable *groups)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	const gchar *id;

	if (token == NULL || g_strcmp0(token, GSURF_CONTAINER_DEFAULT) == 0)
		return GSURF_CONTAINER_DEFAULT;
	if (g_strcmp0(token, GSURF_CONTAINER_TEMPORARY) == 0 ||
	    g_str_has_prefix(token, TEMP_TOKEN_PREFIX)) {
		id = g_hash_table_lookup(groups, token);
		return (id != NULL) ? id : GSURF_CONTAINER_TEMPORARY;
	}
	if (gsurf_container_registry_lookup(registry, token) == NULL) {
		g_warning("gsurf: session: unknown container '%s'; restoring the tab in "
			"the default container", token);
		return GSURF_CONTAINER_DEFAULT;
	}
	return token;
}

/* Whether @view (the startup view) already is where @request (from
 * container_request()) would put a tab, so the first tab can reuse it.
 * A new temporary container is matched by a temporary container that
 * only this view uses. */
static gboolean
initial_view_matches(GsurfView *view, const gchar *request)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	const gchar *id = gsurf_view_get_container_id(view);
	GsurfContainer *container = gsurf_container_registry_lookup(registry, id);

	if (g_strcmp0(request, GSURF_CONTAINER_DEFAULT) == 0)
		return id == NULL;
	if (g_strcmp0(request, GSURF_CONTAINER_TEMPORARY) == 0)
		return container != NULL && gsurf_container_get_temporary(container) &&
		       gsurf_container_get_n_views(container) == 1;
	return g_strcmp0(id, request) == 0;
}

/* Drop the parent of every tab whose parent chain leads back to itself,
 * so the tabs of a hand-edited file with a cycle come back as roots. */
static void
break_parent_cycles(SessionWindow *window)
{
	guint n = window->tabs->len;
	guint i;

	for (i = 0; i < n; i++) {
		SessionTab *tab = g_ptr_array_index(window->tabs, i);
		gint p = tab->parent;
		guint steps = 0;

		while (p >= 0 && steps++ <= n) {
			if ((guint)p == i) {
				g_warning("gsurf: session: tab %u is its own ancestor; "
					"restoring it without a parent", i);
				tab->parent = -1;
				break;
			}
			p = ((SessionTab *)g_ptr_array_index(window->tabs, p))->parent;
		}
	}
}

/* Depth-first creation order: every parent before its children, tabs in
 * file order otherwise (the parent graph is acyclic by now). */
static void
visit_tab(SessionWindow *window, guint index, gboolean *seen, GArray *order)
{
	SessionTab *tab = g_ptr_array_index(window->tabs, index);

	if (seen[index])
		return;
	seen[index] = TRUE;
	if (tab->parent >= 0)
		visit_tab(window, (guint)tab->parent, seen, order);
	g_array_append_val(order, index);
}

/**
 * restore_layout:
 * @window: the window the tabs were restored into
 * @saved: the saved window whose `layout` to apply
 * @number: @saved's position in the file (for the warnings)
 * @views: (array): the restored view of each of @saved's tabs, by kept
 *   index (%NULL where creating the tab failed)
 *
 * Hands @saved's layout string to the layout installed in @window, with
 * the restored views indexed like the file's `tabs` list: entry i is the
 * view of the tab saved at index i, %NULL for a tab that was dropped
 * (no `uri`) or could not be created. Validating the indices in the
 * string against that array is the layout's business. Without a layout,
 * or when the layout refuses the string, the window keeps its default
 * arrangement and one warning names the window; the string is dropped
 * (the next save writes the arrangement actually on screen).
 */
static void
restore_layout(GsurfWindow *window, SessionWindow *saved, guint number, GsurfView **views)
{
	GsurfWindowLayout *layout = gsurf_window_get_layout(window);
	g_autofree GsurfView **by_file = NULL;
	guint i;

	if (saved->layout == NULL)
		return;
	if (layout == NULL) {
		g_warning("gsurf: session: window %u: no window layout is installed; "
			"its saved layout is dropped", number);
		return;
	}
	by_file = g_new0(GsurfView *, MAX(saved->n_file_tabs, 1));
	for (i = 0; i < saved->tabs->len; i++) {
		SessionTab *tab = g_ptr_array_index(saved->tabs, i);

		if (tab->file_index < saved->n_file_tabs)
			by_file[tab->file_index] = views[i];
	}
	if (!gsurf_window_layout_restore(layout, window, saved->layout, by_file,
	                                 saved->n_file_tabs))
		g_warning("gsurf: session: window %u: the window layout cannot restore its "
			"saved layout; keeping the default arrangement", number);
}

/**
 * session_restore:
 * @self: the module
 * @window: the window to restore into
 * @session: the saved session
 *
 * Recreates the saved tabs in @window. gsurf runs one window per
 * process, so the tabs of further saved windows are appended to the
 * same window. Each tab is created with its restored parent as opener
 * and its container, at its saved position. The startup view takes the
 * first tab when it is in that tab's container; otherwise it is removed
 * once the session is restored (a view cannot change container).
 *
 * Last, with every tab in place and the active tab set, the first saved
 * window's `layout` goes to the window's layout (restore_layout()). The
 * layouts of further saved windows are dropped with a warning: their
 * tabs were merged into this window, which has one arrangement.
 *
 * Returns: the number of tabs restored
 */
static guint
session_restore(GsurfSessionModule *self, GsurfWindow *window, GPtrArray *session)
{
	GsurfConfig *config = gsurf_config_get_default();
	g_autoptr(GHashTable) groups = g_hash_table_new_full(g_str_hash, g_str_equal,
		g_free, g_free);
	g_autoptr(GsurfView) initial = NULL;
	GsurfView *first_active = NULL;
	g_autofree GsurfView **first_views = NULL;  /* window 0's views by kept index */
	gboolean reused = FALSE;
	guint restored = 0;
	guint base = 0;
	guint w;

	if (gsurf_window_get_active_view(window) != NULL)
		initial = g_object_ref(gsurf_window_get_active_view(window));
	self->restoring = TRUE;

	for (w = 0; w < session->len; w++) {
		SessionWindow *saved = g_ptr_array_index(session, w);
		g_autofree GsurfView **views = g_new0(GsurfView *, saved->tabs->len);
		g_autofree gboolean *seen = g_new0(gboolean, saved->tabs->len);
		g_autoptr(GArray) order = g_array_new(FALSE, FALSE, sizeof(guint));
		guint i, k;

		break_parent_cycles(saved);
		for (i = 0; i < saved->tabs->len; i++)
			visit_tab(saved, i, seen, order);

		for (k = 0; k < order->len; k++) {
			guint index = g_array_index(order, guint, k);
			SessionTab *tab = g_ptr_array_index(saved->tabs, index);
			GsurfView *opener = (tab->parent >= 0) ? views[tab->parent] : NULL;
			const gchar *request = container_request(tab->container, groups);
			guint position = base;
			guint j;
			GsurfView *view;

			/* Position: after the tabs of this window already
			 * created that come before it in the file. */
			for (j = 0; j < index; j++) {
				if (views[j] != NULL)
					position++;
			}

			/* The very first root may reuse the startup view. */
			if (!reused && restored == 0 && initial != NULL && opener == NULL &&
			    initial_view_matches(initial, request)) {
				view = initial;
				reused = TRUE;
				gsurf_window_move_view(window, view, position);
			} else {
				g_autoptr(GsurfView) created = gsurf_view_new_full(opener, request,
					tab->uri, FALSE);

				if (created == NULL)
					continue;
				if (config != NULL && config->settings != NULL)
					gsurf_view_apply_settings(created, config->settings);
				/* The startup view sits at the end until it is removed. */
				gsurf_window_insert_view(window, created, position);
				view = created;
			}

			/* A temporary group keeps the container its first tab got. */
			if (tab->container != NULL && !g_hash_table_contains(groups, tab->container) &&
			    g_strcmp0(request, GSURF_CONTAINER_TEMPORARY) == 0 &&
			    gsurf_view_get_container_id(view) != NULL)
				g_hash_table_insert(groups, g_strdup(tab->container),
					g_strdup(gsurf_view_get_container_id(view)));

			views[index] = view;
			gsurf_view_load_uri(view, tab->uri);
			restored++;
			if (w == 0 && (gint)index == saved->active)
				first_active = view;
		}
		for (i = 0; i < saved->tabs->len; i++) {
			if (views[i] != NULL)
				base++;
		}
		if (w == 0)
			first_views = g_steal_pointer(&views);
		else if (saved->layout != NULL)
			g_warning("gsurf: session: window %u: its tabs join the first window; "
				"its saved layout is dropped", w);
	}

	/* The startup view was not reused: it goes, the restored tabs stay. */
	if (restored > 0 && initial != NULL && !reused &&
	    gsurf_window_get_view_index(window, initial) >= 0)
		gsurf_window_remove_view(window, initial);
	if (first_active == NULL && restored > 0)
		first_active = gsurf_window_get_nth_view(window, 0);
	if (first_active != NULL)
		gsurf_window_set_active_view(window, first_active);

	/* The layout last: the views exist and the active one is set. Saves
	 * stay suppressed meanwhile (the layout emits visible-views-changed). */
	if (session->len > 0 && first_views != NULL)
		restore_layout(window, g_ptr_array_index(session, 0), 0, first_views);

	self->restoring = FALSE;
	return restored;
}

/* ===================================================================
 * Load (YAML, migration)
 * =================================================================== */

/**
 * session_load:
 * @self: the module
 *
 * Reads the YAML session. When there is none but a legacy plain-text
 * session exists, it is read instead and written once in the new
 * format (the old file is kept). A missing session is not an error;
 * an unreadable or malformed one is reported.
 *
 * Returns: (transfer full) (nullable): the session, or %NULL
 */
static GPtrArray *
session_load(GsurfSessionModule *self)
{
	g_autofree gchar *data = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *session;

	if (self->file == NULL)
		return NULL;

	if (g_file_test(self->file, G_FILE_TEST_EXISTS)) {
		if (!g_file_get_contents(self->file, &data, NULL, &error)) {
			g_warning("gsurf: session: cannot read '%s': %s", self->file, error->message);
			return NULL;
		}
		session = session_from_yaml(data, &error);
		if (session == NULL)
			g_warning("gsurf: session: '%s' is not a valid session: %s",
				self->file, error->message);
		return session;
	}

	/* Migration from the one-URI-per-line file. */
	if (self->legacy_file == NULL ||
	    !g_file_test(self->legacy_file, G_FILE_TEST_IS_REGULAR))
		return NULL;
	if (!g_file_get_contents(self->legacy_file, &data, NULL, &error)) {
		g_warning("gsurf: session: cannot read '%s': %s", self->legacy_file, error->message);
		return NULL;
	}
	session = session_from_legacy(data);
	if (!session_write(self, session, &error))
		g_warning("gsurf: session: cannot migrate '%s' to '%s': %s",
			self->legacy_file, self->file, error->message);
	else
		g_message("gsurf: session: migrated '%s' to '%s'", self->legacy_file, self->file);
	return session;
}

/* ===================================================================
 * Hooks and keys
 * =================================================================== */

static void
gsurf_session_after_navigate(GsurfNavigationHook *hook, GsurfView *view, const gchar *uri)
{
	session_save(GSURF_SESSION_MODULE(hook));
}

static void
gsurf_session_nav_init(GsurfNavigationHookInterface *iface)
{
	iface->after_navigate = gsurf_session_after_navigate;
}

static gboolean
gsurf_session_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                               guint keyval, guint keycode, guint state, GsurfModePolicy mode)
{
	GsurfSessionModule *self = GSURF_SESSION_MODULE(handler);

	if (self->key_save != NULL && gsurf_keys_match(keyval, state, self->key_save)) {
		session_save(self);
		return TRUE;
	}
	return FALSE;
}

static void
gsurf_session_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_session_handle_key_event;
}

static void
gsurf_session_list_keybinds(GsurfKeybindProvider *provider, GPtrArray *entries)
{
	GsurfSessionModule *self = GSURF_SESSION_MODULE(provider);

	gsurf_keybind_help_append(entries, self->key_save,
		"Save the current session", MODULE_NAME, "session-save");
}

static void
gsurf_session_keybind_init(GsurfKeybindProviderInterface *iface)
{
	iface->list_keybinds = gsurf_session_list_keybinds;
}

/* Tab closed or moved: the tree and the order changed. */
static void
on_view_removed(GsurfWindow *window, GsurfView *view, gpointer data)
{
	GsurfSessionModule *self = data;

	if (gsurf_module_is_active(GSURF_MODULE(self)))
		session_save(self);
}

static void
on_view_moved(GsurfWindow *window, GsurfView *view, guint from, guint to, gpointer data)
{
	GsurfSessionModule *self = data;

	if (gsurf_module_is_active(GSURF_MODULE(self)))
		session_save(self);
}

static gboolean
save_idle_cb(gpointer data)
{
	GsurfSessionModule *self = data;

	self->save_idle = 0;
	if (gsurf_module_is_active(GSURF_MODULE(self)))
		session_save(self);
	return G_SOURCE_REMOVE;
}

/* The window layout changed what is on screen (a split, a closed pane,
 * another tab in a pane): save the new arrangement. A layout operation
 * can emit this several times in a row (and every tab switch emits it),
 * so the saves are coalesced into one idle. */
static void
on_visible_views_changed(GsurfWindow *window, gpointer data)
{
	GsurfSessionModule *self = data;

	(void)window;
	if (self->save_idle != 0 || self->restoring ||
	    !gsurf_module_is_active(GSURF_MODULE(self)))
		return;
	self->save_idle = g_idle_add(save_idle_cb, self);
}

static void
cancel_save_idle(GsurfSessionModule *self)
{
	if (self->save_idle != 0) {
		g_source_remove(self->save_idle);
		self->save_idle = 0;
	}
}

static void
watch_window(GsurfSessionModule *self, GsurfWindow *window)
{
	g_signal_handlers_disconnect_by_data(window, self);
	g_signal_connect_object(window, "view-removed", G_CALLBACK(on_view_removed), self, 0);
	g_signal_connect_object(window, "view-moved", G_CALLBACK(on_view_moved), self, 0);
	g_signal_connect_object(window, "visible-views-changed",
		G_CALLBACK(on_visible_views_changed), self, 0);
}

static void
on_window_added(GsurfApplication *app, GsurfWindow *window, gpointer data)
{
	watch_window(GSURF_SESSION_MODULE(data), window);
}

static void
watch_host(GsurfSessionModule *self)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfApplication *app = gsurf_module_manager_get_application(mgr);
	g_autoptr(GPtrArray) windows = host_windows();
	guint i;

	for (i = 0; i < windows->len; i++)
		watch_window(self, g_ptr_array_index(windows, i));
	if (app != NULL) {
		g_signal_handlers_disconnect_by_data(app, self);
		g_signal_connect_object(app, "window-added", G_CALLBACK(on_window_added), self, 0);
	}
	self->connected = TRUE;
}

static void
unwatch_host(GsurfSessionModule *self)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfApplication *app = gsurf_module_manager_get_application(mgr);
	g_autoptr(GPtrArray) windows = NULL;
	guint i;

	cancel_save_idle(self);
	if (!self->connected)
		return;
	windows = host_windows();
	for (i = 0; i < windows->len; i++)
		g_signal_handlers_disconnect_by_data(g_ptr_array_index(windows, i), self);
	if (app != NULL)
		g_signal_handlers_disconnect_by_data(app, self);
	self->connected = FALSE;
}

/* ===================================================================
 * Module lifecycle
 * =================================================================== */

static const gchar *gsurf_session_get_name(GsurfModule *m) { return MODULE_NAME; }
static const gchar *gsurf_session_get_description(GsurfModule *m)
{
	return "Persist and restore the set of open views";
}

static void
gsurf_session_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfSessionModule *self = GSURF_SESSION_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	/* Every configure() starts from the defaults. */
	set_file(self, NULL);
	self->auto_restore = FALSE;
	g_free(self->key_save);
	self->key_save = g_strdup(DEFAULT_KEY_SAVE);

	node = (config != NULL) ? gsurf_config_get_module_node(config, MODULE_NAME) : NULL;
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "file")) {
		const gchar *f = member_scalar(m, "file");

		if (f == NULL || *f == '\0')
			g_warning("gsurf: session: empty `file`; using the default");
		else
			set_file(self, f);
	}
	if (yaml_mapping_has_member(m, "auto_restore"))
		self->auto_restore = yaml_mapping_get_boolean_member(m, "auto_restore");
	if (yaml_mapping_has_member(m, "key_save")) {
		const gchar *key = member_scalar(m, "key_save");

		g_free(self->key_save);
		self->key_save = (key != NULL && *key != '\0') ? g_strdup(key) : NULL;
	}
}

static gboolean
gsurf_session_activate(GsurfModule *module)
{
	GsurfSessionModule *self = GSURF_SESSION_MODULE(module);
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfApplication *app = gsurf_module_manager_get_application(mgr);
	GsurfWindow *window = gsurf_module_manager_get_active_window(mgr);
	g_autoptr(GPtrArray) session = NULL;

	watch_host(self);

	/* Read (and migrate) the session even without auto_restore, so the
	 * old plain-text file is converted once. */
	session = session_load(self);

	if (!self->auto_restore || window == NULL || app == NULL || session == NULL)
		return TRUE;
	/* A URI on the command line takes precedence over the saved session. */
	if (g_object_get_data(G_OBJECT(app), "gsurf-cli-uri") != NULL)
		return TRUE;

	if (session_restore(self, window, session) > 0)
		g_object_set_data(G_OBJECT(app), "gsurf-startup-handled", GINT_TO_POINTER(1));
	return TRUE;
}

static void
gsurf_session_deactivate(GsurfModule *module)
{
	unwatch_host(GSURF_SESSION_MODULE(module));
}

static void
gsurf_session_module_finalize(GObject *object)
{
	GsurfSessionModule *self = GSURF_SESSION_MODULE(object);

	cancel_save_idle(self);
	g_clear_pointer(&self->file, g_free);
	g_clear_pointer(&self->legacy_file, g_free);
	g_clear_pointer(&self->key_save, g_free);
	G_OBJECT_CLASS(gsurf_session_module_parent_class)->finalize(object);
}

static void
gsurf_session_module_class_init(GsurfSessionModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_session_module_finalize;
	module_class->activate = gsurf_session_activate;
	module_class->deactivate = gsurf_session_deactivate;
	module_class->get_name = gsurf_session_get_name;
	module_class->get_description = gsurf_session_get_description;
	module_class->configure = gsurf_session_configure;
}

static void
gsurf_session_module_init(GsurfSessionModule *self)
{
	set_file(self, NULL);
	self->key_save = g_strdup(DEFAULT_KEY_SAVE);
	self->auto_restore = FALSE;
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_SESSION_MODULE;
}
