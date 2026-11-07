/*
 * gsurf-view.c - Abstract web view
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "util/gsurf-kiosk.h"
#include "util/gsurf-runtime-dir.h"
#include "core/gsurf-view.h"
#include "core/gsurf-container-registry.h"
#include "config/gsurf-config.h"

#include <errno.h>
#include <pwd.h>
#include <string.h>

typedef struct {
	gboolean   editing;       /* an editable DOM element is focused in the page */
	gchar     *hovered_uri;   /* link URI under the pointer, or NULL */

	guint64    id;            /* process-unique, assigned in constructed() */
	GsurfView *opener;        /* weak pointer (cleared on finalize) */
	guint64    opener_id;     /* survives the opener */
	gchar     *requested;     /* construct: requested container id */
	gchar     *container_id;  /* resolved container, NULL = default */
	gboolean   related;       /* popup sharing the opener's session */
	gchar     *initial_uri;   /* construct: placement hint */
	gpointer   session;       /* native session acquired from the registry */
	gboolean   acquired;      /* whether release() is owed */
} GsurfViewPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(GsurfView, gsurf_view, G_TYPE_OBJECT)

enum {
	PROP_0,
	PROP_ID,
	PROP_OPENER,
	PROP_CONTAINER_ID,
	PROP_RELATED,
	PROP_INITIAL_URI,
	PROP_OPENER_ID,
	PROP_EDITING,
	N_PROPS
};

static GParamSpec *props[N_PROPS];

/* id -> GsurfView* (borrowed). Views are main-thread objects, so a plain
 * table suffices; entries are removed in dispose. */
static GHashTable *views_by_id = NULL;
static guint64 next_view_id = 0;

enum {
	SIG_LOAD_CHANGED,
	SIG_URI_CHANGED,
	SIG_TITLE_CHANGED,
	SIG_PROGRESS_CHANGED,
	SIG_WEB_PROCESS_TERMINATED,
	SIG_HOVERED_URI_CHANGED,
	SIG_FAVICON_CHANGED,
	SIG_CREATE_VIEW,
	SIG_SCRIPT_MESSAGE,
	SIG_NEW_VIEW_REQUESTED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Assign the id, then place the view in a container and acquire that
 * container's session before the backend subclass (which chains up first
 * in its own constructed()) creates the native web view. */
static void
gsurf_view_constructed(GObject *object)
{
	GsurfView *self = GSURF_VIEW(object);
	GsurfViewPrivate *priv = gsurf_view_get_instance_private(self);
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();

	G_OBJECT_CLASS(gsurf_view_parent_class)->constructed(object);

	priv->id = ++next_view_id;
	if (views_by_id == NULL)
		views_by_id = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_hash_table_insert(views_by_id, &priv->id, self);

	/* A related view without an opener has nothing to relate to. */
	if (priv->opener == NULL)
		priv->related = FALSE;

	priv->container_id = gsurf_container_registry_resolve(registry, priv->opener,
		priv->requested, priv->initial_uri, priv->related);
	priv->session = gsurf_container_registry_acquire(registry, priv->container_id);
	priv->acquired = TRUE;
}

static void
gsurf_view_dispose(GObject *object)
{
	GsurfView *self = GSURF_VIEW(object);
	GsurfViewPrivate *priv = gsurf_view_get_instance_private(self);

	/* Dispose may run more than once; the release must not. The backend
	 * subclass has already dropped its native view (it disposes before
	 * chaining up), so the session can go away now. */
	if (priv->acquired) {
		priv->acquired = FALSE;
		gsurf_container_registry_release(gsurf_container_registry_get_default(),
			priv->container_id);
		priv->session = NULL;
	}
	if (views_by_id != NULL && priv->id != 0)
		g_hash_table_remove(views_by_id, &priv->id);
	if (priv->opener != NULL)
		g_object_remove_weak_pointer(G_OBJECT(priv->opener), (gpointer *)&priv->opener);
	priv->opener = NULL;

	G_OBJECT_CLASS(gsurf_view_parent_class)->dispose(object);
}

static void
gsurf_view_finalize(GObject *object)
{
	GsurfViewPrivate *priv = gsurf_view_get_instance_private(GSURF_VIEW(object));
	g_clear_pointer(&priv->hovered_uri, g_free);
	g_clear_pointer(&priv->requested, g_free);
	g_clear_pointer(&priv->container_id, g_free);
	g_clear_pointer(&priv->initial_uri, g_free);
	G_OBJECT_CLASS(gsurf_view_parent_class)->finalize(object);
}

static void
gsurf_view_set_property(GObject *object, guint prop_id,
                        const GValue *value, GParamSpec *pspec)
{
	GsurfViewPrivate *priv = gsurf_view_get_instance_private(GSURF_VIEW(object));

	switch (prop_id) {
	case PROP_OPENER: {
		GsurfView *opener = g_value_get_object(value);

		/* Weak: the opener may close long before its children. */
		priv->opener = opener;
		if (opener != NULL) {
			GsurfViewPrivate *opener_priv = gsurf_view_get_instance_private(opener);

			priv->opener_id = opener_priv->id;
			g_object_add_weak_pointer(G_OBJECT(opener), (gpointer *)&priv->opener);
		}
		break;
	}
	case PROP_CONTAINER_ID:
		g_free(priv->requested);
		priv->requested = g_value_dup_string(value);
		break;
	case PROP_RELATED:
		priv->related = g_value_get_boolean(value);
		break;
	case PROP_INITIAL_URI:
		g_free(priv->initial_uri);
		priv->initial_uri = g_value_dup_string(value);
		break;
	case PROP_OPENER_ID:
		/* Only a fallback: a live #GsurfView:opener names its own id
		 * (whichever of the two construct properties is set first). */
		if (priv->opener == NULL && g_value_get_uint64(value) != 0)
			priv->opener_id = g_value_get_uint64(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
gsurf_view_get_property(GObject *object, guint prop_id,
                        GValue *value, GParamSpec *pspec)
{
	GsurfViewPrivate *priv = gsurf_view_get_instance_private(GSURF_VIEW(object));

	switch (prop_id) {
	case PROP_ID:           g_value_set_uint64(value, priv->id); break;
	case PROP_OPENER:       g_value_set_object(value, priv->opener); break;
	case PROP_CONTAINER_ID: g_value_set_string(value, priv->container_id); break;
	case PROP_RELATED:      g_value_set_boolean(value, priv->related); break;
	case PROP_INITIAL_URI:  g_value_set_string(value, priv->initial_uri); break;
	case PROP_OPENER_ID:    g_value_set_uint64(value, priv->opener_id); break;
	case PROP_EDITING:      g_value_set_boolean(value, priv->editing); break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
gsurf_view_class_init(GsurfViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->constructed = gsurf_view_constructed;
	object_class->dispose = gsurf_view_dispose;
	object_class->finalize = gsurf_view_finalize;
	object_class->set_property = gsurf_view_set_property;
	object_class->get_property = gsurf_view_get_property;

	/**
	 * GsurfView:id:
	 *
	 * Process-unique view id, assigned at construction.
	 */
	props[PROP_ID] = g_param_spec_uint64("id", NULL, NULL,
		0, G_MAXUINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfView:opener:
	 *
	 * The view this one was opened from (weak), or %NULL.
	 */
	props[PROP_OPENER] = g_param_spec_object("opener", NULL, NULL,
		GSURF_TYPE_VIEW,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfView:container-id:
	 *
	 * On construction, the requested container id (or
	 * #GSURF_CONTAINER_TEMPORARY); when read, the container actually
	 * chosen by the registry (%NULL = default).
	 */
	props[PROP_CONTAINER_ID] = g_param_spec_string("container-id", NULL, NULL,
		NULL, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfView:related:
	 *
	 * Whether the view is a popup that shares its opener's web process
	 * and session (WebKit's related view). Requires #GsurfView:opener.
	 */
	props[PROP_RELATED] = g_param_spec_boolean("related", NULL, NULL,
		FALSE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfView:initial-uri:
	 *
	 * The first URI the creator intends to load, used only as a hint for
	 * container placement.
	 */
	props[PROP_INITIAL_URI] = g_param_spec_string("initial-uri", NULL, NULL,
		NULL, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfView:opener-id:
	 *
	 * The id of the view this one was opened from (0 for none). Kept
	 * after the opener closes. On construction it is a fallback for a
	 * view whose opener is already gone (a tab reopened in another
	 * container keeps its place in the tab tree): a live
	 * #GsurfView:opener always wins.
	 *
	 * Since: 0.4.2
	 */
	props[PROP_OPENER_ID] = g_param_spec_uint64("opener-id", NULL, NULL,
		0, G_MAXUINT64, 0,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	/**
	 * GsurfView:editing:
	 *
	 * Whether an editable element (a text field, textarea, select or
	 * contenteditable) has focus in the page, so bare keys type into it
	 * (gsurf_view_get_editing()). Notified only when it changes; the
	 * window's #GsurfWindow:typing and the published input mode
	 * (gsurf_module_manager_get_mode()) follow it.
	 *
	 * Since: 0.5.9
	 */
	props[PROP_EDITING] = g_param_spec_boolean("editing", NULL, NULL,
		FALSE, G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(object_class, N_PROPS, props);

	/**
	 * GsurfView::load-changed:
	 * @view: the view
	 * @event: the #GsurfLoadEvent
	 *
	 * Emitted as the load state of @view transitions.
	 */
	signals[SIG_LOAD_CHANGED] = g_signal_new(
		"load-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, GSURF_TYPE_LOAD_EVENT);

	/**
	 * GsurfView::uri-changed:
	 * @view: the view
	 * @uri: the new URI
	 */
	signals[SIG_URI_CHANGED] = g_signal_new(
		"uri-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_STRING);

	/**
	 * GsurfView::title-changed:
	 * @view: the view
	 * @title: the new title
	 */
	signals[SIG_TITLE_CHANGED] = g_signal_new(
		"title-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_STRING);

	/**
	 * GsurfView::progress-changed:
	 * @view: the view
	 * @progress: estimated load progress (0.0-1.0)
	 */
	signals[SIG_PROGRESS_CHANGED] = g_signal_new(
		"progress-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_DOUBLE);

	/**
	 * GsurfView::web-process-terminated:
	 * @view: the view
	 *
	 * Emitted when the web content process crashes or is killed.
	 */
	signals[SIG_WEB_PROCESS_TERMINATED] = g_signal_new(
		"web-process-terminated", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);

	/**
	 * GsurfView::hovered-uri-changed:
	 * @view: the view
	 * @uri: the link URI under the pointer, or %NULL
	 */
	signals[SIG_HOVERED_URI_CHANGED] = g_signal_new(
		"hovered-uri-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_STRING);

	/**
	 * GsurfView::favicon-changed:
	 * @view: the view
	 */
	signals[SIG_FAVICON_CHANGED] = g_signal_new(
		"favicon-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);

	/**
	 * GsurfView::create-view:
	 * @view: the view
	 * @uri: the requested URI (may be empty)
	 *
	 * Emitted to host a popup / new-window request. A handler returns a
	 * new #GsurfView to use, or %NULL to block it.
	 *
	 * Returns: (transfer none) (nullable): the new view, or %NULL
	 */
	signals[SIG_CREATE_VIEW] = g_signal_new(
		"create-view", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_first_wins, NULL, NULL,
		GSURF_TYPE_VIEW, 1, G_TYPE_STRING);

	/**
	 * GsurfView::script-message:
	 * @view: the view
	 * @name: the message handler name (also the signal detail)
	 * @json: (nullable): the posted value serialized as JSON
	 *
	 * Emitted when a page or isolated-world script posts to a handler
	 * registered with gsurf_view_register_script_message(). Connect to
	 * `script-message::<name>` to receive one handler's messages.
	 */
	signals[SIG_SCRIPT_MESSAGE] = g_signal_new(
		"script-message", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

	/**
	 * GsurfView::new-view-requested:
	 * @view: the view the request comes from
	 * @uri: the address to open
	 * @background: %TRUE to keep @view the active view, %FALSE to
	 *   activate the new one
	 *
	 * Emitted by gsurf_view_request_new_view() (the page context menu's
	 * "Open Link in New Tab"): the host opens @uri in a new, unrelated
	 * view whose opener is @view, so tab chrome shows it as @view's
	 * child.
	 *
	 * Returns: %TRUE when a handler opened the view
	 *
	 * Since: 0.4.2
	 */
	signals[SIG_NEW_VIEW_REQUESTED] = g_signal_new(
		"new-view-requested", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_true_handled, NULL, NULL,
		G_TYPE_BOOLEAN, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
}

static void
gsurf_view_init(GsurfView *self)
{
}

/* --- Navigation --- */

/* TRUE if S begins with a real URI scheme ("http:", "about:", "file:", …).
 * Rejects host:port forms ("duckduckgo.com:8080", "localhost:8080") so they
 * are treated as bare addresses: a scheme has no '.' in it and is not followed
 * by a digit (which would be a port). */
static gboolean
gsurf_uri_has_scheme(const gchar *s)
{
	const gchar *p = s;

	if (!g_ascii_isalpha((guchar)*p))
		return FALSE;
	while (*p != '\0'
	       && (g_ascii_isalnum((guchar)*p) || *p == '+' || *p == '-'
	           || *p == '.'))
		p++;
	if (*p != ':')
		return FALSE;                    /* no ':' -> no scheme */
	if (memchr(s, '.', (gsize)(p - s)) != NULL)
		return FALSE;                    /* "host.tld:port" */
	if (g_ascii_isdigit((guchar)p[1]))
		return FALSE;                    /* "host:port" */
	return TRUE;
}

/* Authority is the text before the first path, query, or fragment marker.
 * A dot there is a domain or IPv4 address. "localhost" is special so it does
 * not need a dot. Dots later in the path ("docs/readme.org") are not a host. */
static gboolean
gsurf_uri_authority_is_host(const gchar *s)
{
	const gchar *end;
	gsize len;

	end = s;
	while (*end != '\0' && *end != '/' && *end != '?' && *end != '#')
		end++;
	len = (gsize)(end - s);
	if (len == 0)
		return FALSE;
	if (memchr(s, '.', len) != NULL)
		return TRUE;
	if (len >= 9 && strncmp(s, "localhost", 9) == 0
	    && (len == 9 || s[9] == ':'))
		return TRUE;
	return FALSE;
}

/* TRUE if S (already known to be scheme-less) looks like a host we should
 * load over https rather than a free-text search term. */
static gboolean
gsurf_uri_looks_like_host(const gchar *s)
{
	if (*s == '\0')
		return FALSE;
	if (strchr(s, ' ') != NULL || strchr(s, '\t') != NULL)
		return FALSE;                    /* a search phrase */
	return gsurf_uri_authority_is_host(s);
}

/* TRUE when a file URI names this machine. A foreign host is left intact so
 * it is not silently remapped onto a local path. */
static gboolean
gsurf_uri_file_host_is_local(const gchar *host)
{
	if (host == NULL || *host == '\0')
		return TRUE;
	return g_ascii_strcasecmp(host, "localhost") == 0;
}

/* Canonical file URI. Local forms gain a decoded, dot-segment-free path and
 * percent-encoding, while query and fragment survive (GFile drops both).
 * Userinfo, a port, or a non-local host is only encoding-normalized. */
static gchar *
gsurf_uri_canonicalize_file(const gchar *input)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GUri) parsed = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GUri) built = NULL;
	g_autofree gchar *local = NULL;
	const gchar *host;
	const gchar *path;
	gint port;

	parsed = g_uri_parse(input, G_URI_FLAGS_NONE, &error);
	if (parsed == NULL || g_uri_get_scheme(parsed) == NULL
	    || g_ascii_strcasecmp(g_uri_get_scheme(parsed), "file") != 0)
		return g_strdup(input);

	host = g_uri_get_host(parsed);
	port = g_uri_get_port(parsed);
	path = g_uri_get_path(parsed);
	if (g_uri_get_userinfo(parsed) != NULL || port >= 0
	    || !gsurf_uri_file_host_is_local(host)
	    || path == NULL || !g_path_is_absolute(path))
		return g_uri_to_string(parsed);

	file = g_file_new_for_path(path);
	local = g_file_get_path(file);
	if (local == NULL)
		return g_uri_to_string(parsed);

	/* An empty host is what produces the three-slash form, file:///path. */
	built = g_uri_build(G_URI_FLAGS_NONE, "file", NULL, "", -1, local,
		g_uri_get_query(parsed), g_uri_get_fragment(parsed));
	return g_uri_to_string(built);
}

/* Expand a leading ~, ~/path, or ~user/path. Unknown users return NULL so
 * the caller can keep the text as a search or address. */
static gchar *
gsurf_uri_expand_tilde(const gchar *path)
{
	const gchar *slash;
	const gchar *rest;
	g_autofree gchar *user = NULL;
	struct passwd pwd;
	struct passwd *result;
	gchar *buf;
	gchar *full;
	gsize bufsize;
	gint rc;

	if (path[0] != '~')
		return NULL;
	if (g_get_home_dir() == NULL)
		return NULL;
	if (path[1] == '\0')
		return g_strdup(g_get_home_dir());
	if (path[1] == '/')
		return g_build_filename(g_get_home_dir(), path + 2, NULL);

	slash = strchr(path + 1, '/');
	if (slash == NULL) {
		user = g_strdup(path + 1);
		rest = NULL;
	} else {
		user = g_strndup(path + 1, (gsize)(slash - (path + 1)));
		rest = slash + 1;
	}
	if (user[0] == '\0')
		return NULL;

	bufsize = 16384;
	buf = g_malloc(bufsize);
	result = NULL;
	rc = getpwnam_r(user, &pwd, buf, bufsize, &result);
	while (rc == ERANGE && bufsize < (1u << 20)) {
		bufsize *= 2;
		buf = g_realloc(buf, bufsize);
		rc = getpwnam_r(user, &pwd, buf, bufsize, &result);
	}
	if (rc != 0 || result == NULL || pwd.pw_dir == NULL) {
		g_free(buf);
		return NULL;
	}
	if (rest == NULL || rest[0] == '\0')
		full = g_strdup(pwd.pw_dir);
	else
		full = g_build_filename(pwd.pw_dir, rest, NULL);
	g_free(buf);
	return full;
}

/* Turn an absolute or relative filesystem location into a file URI. A
 * relative path is resolved against the launch directory (the gsurf
 * binary itself works in its runtime directory). */
static gchar *
gsurf_uri_from_local_path(const gchar *path)
{
	g_autoptr(GFile) file = NULL;
	g_autofree gchar *launch = gsurf_get_launch_dir();

	file = g_file_new_for_commandline_arg_and_cwd(path, launch);
	return g_file_get_uri(file);
}

/* Scheme-less text that should be opened as a file rather than https or a
 * search. An existing relative file wins even when its name contains a dot,
 * so "gsurf index.html" opens that file. A missing relative path needs a
 * slash, or an explicit ./ or ../, and must not look like a host. Whitespace
 * keeps a missing relative path as a search phrase. */
static gboolean
gsurf_uri_is_filesystem_path(const gchar *s)
{
	if (g_path_is_absolute(s))
		return TRUE;
	if (g_strcmp0(s, ".") == 0 || g_strcmp0(s, "..") == 0)
		return TRUE;
	if (g_str_has_prefix(s, "./") || g_str_has_prefix(s, "../"))
		return TRUE;
	{
		g_autofree gchar *resolved = gsurf_resolve_path(s);

		if (g_file_test(resolved, G_FILE_TEST_EXISTS))
			return TRUE;
	}
	if (strchr(s, '/') != NULL && strchr(s, ' ') == NULL && strchr(s, '\t') == NULL
	    && !gsurf_uri_authority_is_host(s))
		return TRUE;
	return FALSE;
}

gchar *
gsurf_view_normalize_uri(const gchar *input)
{
	g_autofree gchar *trimmed = NULL;
	g_autofree gchar *expanded = NULL;

	if (input == NULL)
		return NULL;

	trimmed = g_strstrip(g_strdup(input));
	if (trimmed[0] == '\0')
		return g_steal_pointer(&trimmed);

	/* file: is rewritten into a canonical file URI. Every other scheme
	 * (https, about, data, mailto, gopher, gemini, view-source, …) is
	 * preserved, including its original spelling. */
	if (gsurf_uri_has_scheme(trimmed)) {
		if (g_ascii_strncasecmp(trimmed, "file:", 5) == 0)
			return gsurf_uri_canonicalize_file(trimmed);
		return g_steal_pointer(&trimmed);
	}

	if (trimmed[0] == '~') {
		expanded = gsurf_uri_expand_tilde(trimmed);
		if (expanded != NULL)
			return gsurf_uri_from_local_path(expanded);
	} else if (gsurf_uri_is_filesystem_path(trimmed)) {
		return gsurf_uri_from_local_path(trimmed);
	}

	if (!gsurf_uri_looks_like_host(trimmed))
		return g_steal_pointer(&trimmed);

	return g_strconcat("https://", trimmed, NULL);
}

void
gsurf_view_load_uri(GsurfView *self, const gchar *uri)
{
	GsurfViewClass *klass;
	gchar *normalized;

	g_return_if_fail(GSURF_IS_VIEW(self));
	g_return_if_fail(uri != NULL);

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->load_uri == NULL)
		return;

	/* Supply a missing scheme, and turn filesystem paths into file URIs. */
	normalized = gsurf_view_normalize_uri(uri);
	klass->load_uri(self, normalized);
	g_free(normalized);
}

void
gsurf_view_load_html(GsurfView *self, const gchar *html, const gchar *base_uri)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));
	g_return_if_fail(html != NULL);

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->load_html != NULL)
		klass->load_html(self, html, base_uri);
}

void
gsurf_view_reload(GsurfView *self, gboolean bypass_cache)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->reload != NULL)
		klass->reload(self, bypass_cache);
}

void
gsurf_view_stop_loading(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->stop_loading != NULL)
		klass->stop_loading(self);
}

void
gsurf_view_go_back(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->go_back != NULL)
		klass->go_back(self);
}

void
gsurf_view_go_forward(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->go_forward != NULL)
		klass->go_forward(self);
}

gboolean
gsurf_view_can_go_back(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_val_if_fail(GSURF_IS_VIEW(self), FALSE);

	klass = GSURF_VIEW_GET_CLASS(self);
	return klass->can_go_back != NULL ? klass->can_go_back(self) : FALSE;
}

gboolean
gsurf_view_can_go_forward(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_val_if_fail(GSURF_IS_VIEW(self), FALSE);

	klass = GSURF_VIEW_GET_CLASS(self);
	return klass->can_go_forward != NULL ? klass->can_go_forward(self) : FALSE;
}

/* --- State --- */

const gchar *
gsurf_view_get_uri(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);

	klass = GSURF_VIEW_GET_CLASS(self);
	return klass->get_uri != NULL ? klass->get_uri(self) : NULL;
}

const gchar *
gsurf_view_get_title(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);

	klass = GSURF_VIEW_GET_CLASS(self);
	return klass->get_title != NULL ? klass->get_title(self) : NULL;
}

gdouble
gsurf_view_get_estimated_load_progress(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_val_if_fail(GSURF_IS_VIEW(self), 0.0);

	klass = GSURF_VIEW_GET_CLASS(self);
	return klass->get_estimated_load_progress != NULL
		? klass->get_estimated_load_progress(self) : 0.0;
}

/* --- Zoom --- */

void
gsurf_view_set_zoom_level(GsurfView *self, gdouble level)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->set_zoom_level != NULL)
		klass->set_zoom_level(self, level);
}

gdouble
gsurf_view_get_zoom_level(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_val_if_fail(GSURF_IS_VIEW(self), 1.0);

	klass = GSURF_VIEW_GET_CLASS(self);
	return klass->get_zoom_level != NULL ? klass->get_zoom_level(self) : 1.0;
}

/* --- Settings --- */

const gchar *
gsurf_view_get_container_user_agent(GsurfView *self)
{
	GsurfContainer *container;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);

	container = gsurf_container_registry_lookup(gsurf_container_registry_get_default(),
		gsurf_view_get_container_id(self));
	return container != NULL ? gsurf_container_get_user_agent(container) : NULL;
}

/* NULL and "" both mean "the engine's default user agent". */
static gboolean
same_user_agent(const gchar *a, const gchar *b)
{
	return g_strcmp0((a != NULL && *a != '\0') ? a : NULL,
	                 (b != NULL && *b != '\0') ? b : NULL) == 0;
}

void
gsurf_view_apply_settings(GsurfView *self, GsurfSettings *settings)
{
	GsurfViewClass *klass;
	g_autoptr(GsurfSettings) own = NULL;
	const gchar *container_ua;

	g_return_if_fail(GSURF_IS_VIEW(self));
	g_return_if_fail(GSURF_IS_SETTINGS(settings));

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->apply_settings == NULL)
		return;

	/* A container's user agent replaces the GLOBAL one: when @settings
	 * still carries the configured user agent (every caller that applies
	 * the config as is, and navigations no per-site rule matched), the
	 * view sends its container's instead.  A per-site rule's user agent
	 * (the useragent module) differs from the global one and is kept. */
	container_ua = gsurf_view_get_container_user_agent(self);
	if (container_ua != NULL) {
		GsurfConfig *cfg = gsurf_config_get_default();
		const gchar *global_ua = (cfg != NULL && cfg->settings != NULL)
			? cfg->settings->user_agent : NULL;

		if (same_user_agent(settings->user_agent, global_ua) &&
		    !same_user_agent(settings->user_agent, container_ua)) {
			own = gsurf_settings_copy(settings);
			gsurf_settings_set_user_agent(own, container_ua);
			settings = own;
		}
	}
	klass->apply_settings(self, settings);
}

/* --- Scripting / capture --- */

void
gsurf_view_run_javascript_async(GsurfView *self, const gchar *script,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));
	g_return_if_fail(script != NULL);

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->run_javascript_async != NULL)
		klass->run_javascript_async(self, script, cancellable, callback, user_data);
}

gchar *
gsurf_view_run_javascript_finish(GsurfView *self, GAsyncResult *result, GError **error)
{
	GsurfViewClass *klass;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->run_javascript_finish != NULL)
		return klass->run_javascript_finish(self, result, error);

	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		"JavaScript evaluation not supported by this backend");
	return NULL;
}

void
gsurf_view_get_snapshot_async(GsurfView *self, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->get_snapshot_async != NULL)
		klass->get_snapshot_async(self, cancellable, callback, user_data);
}

GBytes *
gsurf_view_get_snapshot_finish(GsurfView *self, GAsyncResult *result, GError **error)
{
	GsurfViewClass *klass;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->get_snapshot_finish != NULL)
		return klass->get_snapshot_finish(self, result, error);

	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		"Snapshots not supported by this backend");
	return NULL;
}

/* --- Embedding --- */

gpointer
gsurf_view_get_native_widget(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);

	klass = GSURF_VIEW_GET_CLASS(self);
	return klass->get_native_widget != NULL ? klass->get_native_widget(self) : NULL;
}

/* --- Editing state --- */

gboolean
gsurf_view_get_editing(GsurfView *self)
{
	GsurfViewPrivate *priv;

	g_return_val_if_fail(GSURF_IS_VIEW(self), FALSE);

	priv = gsurf_view_get_instance_private(self);
	return priv->editing;
}

void
gsurf_view_set_editing(GsurfView *self, gboolean editing)
{
	GsurfViewPrivate *priv;

	g_return_if_fail(GSURF_IS_VIEW(self));

	priv = gsurf_view_get_instance_private(self);
	editing = editing != FALSE;
	if (priv->editing == editing)
		return;
	priv->editing = editing;
	g_object_notify_by_pspec(G_OBJECT(self), props[PROP_EDITING]);
}

void
gsurf_view_blur_editable(GsurfView *self)
{
	g_return_if_fail(GSURF_IS_VIEW(self));

	/* Blur the deepest active element of the top document: through open
	 * shadow roots (document.activeElement is only the shadow host), and
	 * an <iframe> element, whose blur also drops focus out of the frame. */
	gsurf_view_run_javascript_async(self,
		"(function(){var a=document.activeElement;"
		"while(a&&a.shadowRoot&&a.shadowRoot.activeElement)a=a.shadowRoot.activeElement;"
		"if(a&&a.blur)a.blur();})()",
		NULL, NULL, NULL);
	gsurf_view_set_editing(self, FALSE);
}

/* --- Keyboard focus --- */

void
gsurf_view_focus(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->focus != NULL)
		klass->focus(self);
}

/* --- Extended API --- */

#define GSURF_VIEW_VOID_VFUNC(name) \
	GsurfViewClass *klass; \
	g_return_if_fail(GSURF_IS_VIEW(self)); \
	klass = GSURF_VIEW_GET_CLASS(self); \
	if (klass->name != NULL)

void
gsurf_view_show_inspector(GsurfView *self)
{
	if (gsurf_kiosk_is_enabled())
		return;
	GSURF_VIEW_VOID_VFUNC(show_inspector) klass->show_inspector(self);
}

void
gsurf_view_add_user_script(GsurfView *self, const gchar *source, gboolean at_end)
{
	GSURF_VIEW_VOID_VFUNC(add_user_script_full) klass->add_user_script_full(self, source, at_end, NULL);
}

void
gsurf_view_add_user_script_for(GsurfView *self, const gchar *source, gboolean at_end,
                               const gchar *const *allow)
{
	GSURF_VIEW_VOID_VFUNC(add_user_script_full) klass->add_user_script_full(self, source, at_end, allow);
}

void
gsurf_view_add_user_style(GsurfView *self, const gchar *css)
{
	GSURF_VIEW_VOID_VFUNC(add_user_style_full) klass->add_user_style_full(self, css, NULL);
}

void
gsurf_view_add_user_style_for(GsurfView *self, const gchar *css, const gchar *const *allow)
{
	GSURF_VIEW_VOID_VFUNC(add_user_style_full) klass->add_user_style_full(self, css, allow);
}

void
gsurf_view_add_content_filter(GsurfView *self, const gchar *identifier, const gchar *json_rules)
{
	GSURF_VIEW_VOID_VFUNC(add_content_filter) klass->add_content_filter(self, identifier, json_rules);
}

void
gsurf_view_copy_uri(GsurfView *self)
{
	GSURF_VIEW_VOID_VFUNC(copy_uri) klass->copy_uri(self);
}

gchar *
gsurf_view_read_clipboard_text(GsurfView *self)
{
	GsurfViewClass *klass;
	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);
	klass = GSURF_VIEW_GET_CLASS(self);
	return klass->read_clipboard_text != NULL ? klass->read_clipboard_text(self) : NULL;
}

void
gsurf_view_clear_user_content(GsurfView *self)
{
	GSURF_VIEW_VOID_VFUNC(clear_user_content) klass->clear_user_content(self);
}

void
gsurf_view_find(GsurfView *self, const gchar *text, gboolean case_sensitive, gboolean forward)
{
	GSURF_VIEW_VOID_VFUNC(find) klass->find(self, text, case_sensitive, forward);
}

void
gsurf_view_find_next(GsurfView *self)
{
	GSURF_VIEW_VOID_VFUNC(find_next) klass->find_next(self);
}

void
gsurf_view_find_previous(GsurfView *self)
{
	GSURF_VIEW_VOID_VFUNC(find_previous) klass->find_previous(self);
}

void
gsurf_view_set_cookie_accept_policy(GsurfView *self, gint policy)
{
	GSURF_VIEW_VOID_VFUNC(set_cookie_accept_policy) klass->set_cookie_accept_policy(self, policy);
}

void
gsurf_view_set_proxy(GsurfView *self, const gchar *uri)
{
	GSURF_VIEW_VOID_VFUNC(set_proxy) klass->set_proxy(self, uri);
}

#undef GSURF_VIEW_VOID_VFUNC

const gchar *
gsurf_view_get_hovered_uri(GsurfView *self)
{
	GsurfViewPrivate *priv;
	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);
	priv = gsurf_view_get_instance_private(self);
	return priv->hovered_uri;
}

void
gsurf_view_set_hovered_uri(GsurfView *self, const gchar *uri)
{
	GsurfViewPrivate *priv;
	g_return_if_fail(GSURF_IS_VIEW(self));
	priv = gsurf_view_get_instance_private(self);
	if (g_strcmp0(priv->hovered_uri, uri) == 0)
		return;
	g_free(priv->hovered_uri);
	priv->hovered_uri = g_strdup(uri);
	g_signal_emit(self, signals[SIG_HOVERED_URI_CHANGED], 0, priv->hovered_uri);
}

void
gsurf_view_emit_favicon_changed(GsurfView *self)
{
	g_return_if_fail(GSURF_IS_VIEW(self));
	g_signal_emit(self, signals[SIG_FAVICON_CHANGED], 0);
}

GsurfView *
gsurf_view_emit_create_view(GsurfView *self, const gchar *uri)
{
	GsurfView *result = NULL;
	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);
	if (gsurf_kiosk_is_enabled())
		return NULL;
	g_signal_emit(self, signals[SIG_CREATE_VIEW], 0, uri ? uri : "", &result);
	return result;
}

gboolean
gsurf_view_request_new_view(GsurfView *self, const gchar *uri, gboolean background)
{
	gboolean handled = FALSE;

	g_return_val_if_fail(GSURF_IS_VIEW(self), FALSE);
	if (uri == NULL || *uri == '\0' || gsurf_kiosk_is_enabled())
		return FALSE;
	g_signal_emit(self, signals[SIG_NEW_VIEW_REQUESTED], 0, uri, background != FALSE, &handled);
	return handled;
}

gboolean
gsurf_view_has_new_view_handler(GsurfView *self)
{
	g_return_val_if_fail(GSURF_IS_VIEW(self), FALSE);

	return g_signal_has_handler_pending(self, signals[SIG_NEW_VIEW_REQUESTED], 0, FALSE);
}

/* --- Signal emission helpers --- */

void
gsurf_view_emit_load_changed(GsurfView *self, GsurfLoadEvent event)
{
	g_return_if_fail(GSURF_IS_VIEW(self));

	/* A committed load replaced the document, and with it any focused
	 * field: the old page can no longer report its focusout, and a page
	 * the focus tracker cannot run in (an error page, a plain-text or
	 * image document) would never report at all. The new document's
	 * own reports (an autofocused field) arrive after the commit. */
	if (event == GSURF_LOAD_COMMITTED)
		gsurf_view_set_editing(self, FALSE);
	g_signal_emit(self, signals[SIG_LOAD_CHANGED], 0, event);
}

void
gsurf_view_emit_uri_changed(GsurfView *self, const gchar *uri)
{
	g_return_if_fail(GSURF_IS_VIEW(self));
	g_signal_emit(self, signals[SIG_URI_CHANGED], 0, uri);
}

void
gsurf_view_emit_title_changed(GsurfView *self, const gchar *title)
{
	g_return_if_fail(GSURF_IS_VIEW(self));
	g_signal_emit(self, signals[SIG_TITLE_CHANGED], 0, title);
}

void
gsurf_view_emit_progress_changed(GsurfView *self, gdouble progress)
{
	g_return_if_fail(GSURF_IS_VIEW(self));
	g_signal_emit(self, signals[SIG_PROGRESS_CHANGED], 0, progress);
}

void
gsurf_view_emit_web_process_terminated(GsurfView *self)
{
	g_return_if_fail(GSURF_IS_VIEW(self));

	/* The page is gone, and its focused field with it. */
	gsurf_view_set_editing(self, FALSE);
	g_signal_emit(self, signals[SIG_WEB_PROCESS_TERMINATED], 0);
}

/* --- Identity, opener and container --- */

guint64
gsurf_view_get_id(GsurfView *self)
{
	GsurfViewPrivate *priv;

	g_return_val_if_fail(GSURF_IS_VIEW(self), 0);
	priv = gsurf_view_get_instance_private(self);
	return priv->id;
}

GsurfView *
gsurf_view_lookup(guint64 id)
{
	if (views_by_id == NULL || id == 0)
		return NULL;
	return g_hash_table_lookup(views_by_id, &id);
}

GsurfView *
gsurf_view_get_opener(GsurfView *self)
{
	GsurfViewPrivate *priv;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);
	priv = gsurf_view_get_instance_private(self);
	return priv->opener;
}

guint64
gsurf_view_get_opener_id(GsurfView *self)
{
	GsurfViewPrivate *priv;

	g_return_val_if_fail(GSURF_IS_VIEW(self), 0);
	priv = gsurf_view_get_instance_private(self);
	return priv->opener_id;
}

const gchar *
gsurf_view_get_container_id(GsurfView *self)
{
	GsurfViewPrivate *priv;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);
	priv = gsurf_view_get_instance_private(self);
	return priv->container_id;
}

gboolean
gsurf_view_get_related(GsurfView *self)
{
	GsurfViewPrivate *priv;

	g_return_val_if_fail(GSURF_IS_VIEW(self), FALSE);
	priv = gsurf_view_get_instance_private(self);
	return priv->related;
}

gpointer
gsurf_view_get_session(GsurfView *self)
{
	GsurfViewPrivate *priv;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);
	priv = gsurf_view_get_instance_private(self);
	return priv->session;
}

const gchar *
gsurf_view_get_initial_uri(GsurfView *self)
{
	GsurfViewPrivate *priv;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);
	priv = gsurf_view_get_instance_private(self);
	return priv->initial_uri;
}

gpointer
gsurf_view_get_favicon(GsurfView *self)
{
	GsurfViewClass *klass;

	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);
	klass = GSURF_VIEW_GET_CLASS(self);
	return klass->get_favicon != NULL ? klass->get_favicon(self) : NULL;
}

/* --- Script worlds and messages --- */

void
gsurf_view_register_script_message(GsurfView *self, const gchar *name, const gchar *world)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));
	g_return_if_fail(name != NULL && *name != '\0');

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->register_script_message != NULL)
		klass->register_script_message(self, name, world);
}

void
gsurf_view_add_user_script_in_world(GsurfView *self, const gchar *source,
                                    gboolean at_end, const gchar *const *allow,
                                    const gchar *world)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));
	g_return_if_fail(source != NULL);

	/* The main world is the existing allow-list path. */
	if (world == NULL) {
		gsurf_view_add_user_script_for(self, source, at_end, allow);
		return;
	}
	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->add_user_script_in_world != NULL)
		klass->add_user_script_in_world(self, source, at_end, allow, world);
}

/* The backend's evaluation finished: finish it through the backend and
 * complete the caller's task, whose source object is the GsurfView (the
 * backend's own result has the native web view as its source). */
static void
on_in_world_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GTask *task = user_data;
	GsurfView *self = g_task_get_source_object(task);
	GsurfViewClass *klass = GSURF_VIEW_GET_CLASS(self);
	GError *error = NULL;
	gchar *value;

	(void)source;
	value = (klass->run_javascript_in_world_finish != NULL)
		? klass->run_javascript_in_world_finish(self, result, &error)
		: klass->run_javascript_finish(self, result, &error);
	if (error != NULL)
		g_task_return_error(task, error);
	else
		g_task_return_pointer(task, value, g_free);
	g_object_unref(task);
}

void
gsurf_view_run_javascript_in_world_async(GsurfView *self, const gchar *script,
                                         const gchar *world, GCancellable *cancellable,
                                         GAsyncReadyCallback callback, gpointer user_data)
{
	GsurfViewClass *klass;
	GTask *task;

	g_return_if_fail(GSURF_IS_VIEW(self));
	g_return_if_fail(script != NULL);

	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, gsurf_view_run_javascript_in_world_async);

	klass = GSURF_VIEW_GET_CLASS(self);
	if (world == NULL && klass->run_javascript_async != NULL &&
	    klass->run_javascript_finish != NULL) {
		klass->run_javascript_async(self, script, cancellable, on_in_world_done, task);
		return;
	}
	if (world != NULL && klass->run_javascript_in_world_async != NULL &&
	    (klass->run_javascript_in_world_finish != NULL || klass->run_javascript_finish != NULL)) {
		klass->run_javascript_in_world_async(self, script, world, cancellable,
			on_in_world_done, task);
		return;
	}
	/* No support: report it rather than leaving the caller waiting. */
	g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		world != NULL ? "script worlds are not supported by this backend"
		              : "JavaScript evaluation not supported by this backend");
	g_object_unref(task);
}

gchar *
gsurf_view_run_javascript_in_world_finish(GsurfView *self, GAsyncResult *result,
                                          GError **error)
{
	g_return_val_if_fail(GSURF_IS_VIEW(self), NULL);
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

void
gsurf_view_emit_script_message(GsurfView *self, const gchar *name, const gchar *json)
{
	g_return_if_fail(GSURF_IS_VIEW(self));
	g_return_if_fail(name != NULL);

	g_signal_emit(self, signals[SIG_SCRIPT_MESSAGE], g_quark_from_string(name), name, json);
}
