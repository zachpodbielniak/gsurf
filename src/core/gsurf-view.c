/*
 * gsurf-view.c - Abstract web view
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "util/gsurf-kiosk.h"
#include "core/gsurf-view.h"

#include <errno.h>
#include <pwd.h>
#include <string.h>

typedef struct {
	gboolean editing;       /* an editable DOM element is focused in the page */
	gchar   *hovered_uri;   /* link URI under the pointer, or NULL */
} GsurfViewPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(GsurfView, gsurf_view, G_TYPE_OBJECT)

enum {
	SIG_LOAD_CHANGED,
	SIG_URI_CHANGED,
	SIG_TITLE_CHANGED,
	SIG_PROGRESS_CHANGED,
	SIG_WEB_PROCESS_TERMINATED,
	SIG_HOVERED_URI_CHANGED,
	SIG_FAVICON_CHANGED,
	SIG_CREATE_VIEW,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gsurf_view_finalize(GObject *object)
{
	GsurfViewPrivate *priv = gsurf_view_get_instance_private(GSURF_VIEW(object));
	g_clear_pointer(&priv->hovered_uri, g_free);
	G_OBJECT_CLASS(gsurf_view_parent_class)->finalize(object);
}

static void
gsurf_view_class_init(GsurfViewClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gsurf_view_finalize;

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

/* Turn an absolute or relative filesystem location into a file URI. The
 * path is resolved against the process working directory. */
static gchar *
gsurf_uri_from_local_path(const gchar *path)
{
	g_autoptr(GFile) file = NULL;

	file = g_file_new_for_commandline_arg(path);
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
	if (g_file_test(s, G_FILE_TEST_EXISTS))
		return TRUE;
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

void
gsurf_view_apply_settings(GsurfView *self, GsurfSettings *settings)
{
	GsurfViewClass *klass;

	g_return_if_fail(GSURF_IS_VIEW(self));
	g_return_if_fail(GSURF_IS_SETTINGS(settings));

	klass = GSURF_VIEW_GET_CLASS(self);
	if (klass->apply_settings != NULL)
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
	priv->editing = editing;
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

/* --- Signal emission helpers --- */

void
gsurf_view_emit_load_changed(GsurfView *self, GsurfLoadEvent event)
{
	g_return_if_fail(GSURF_IS_VIEW(self));
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
	g_signal_emit(self, signals[SIG_WEB_PROCESS_TERMINATED], 0);
}
