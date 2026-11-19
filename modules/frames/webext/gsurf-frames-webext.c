/*
 * gsurf-frames-webext.c - frames web-process extension
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loaded by WebKit into every web process (the frames module registers it
 * through gsurf_web_process_extension_register()).  WebKit's UI-process
 * API runs scripts in a page's main frame only; this extension is where a
 * page's other frames -- cross-origin ones included -- can be reached:
 *   - "window-object-cleared" notes every frame of every page as its
 *     document starts (WebKit has no call that lists a page's frames);
 *   - "user-message-received" answers gsurf (src/core/gsurf-frames.c)
 *     with the frame list, or with a script's value run in one frame.
 * The messages are defined once, in src/core/gsurf-frames-protocol.h.
 *
 * Where a frame is: each frame's window is handed to every other frame's
 * probe, which finds the <iframe>/<frame> element showing it and measures
 * its content box.  The probes run in a private script world, so a page
 * cannot redefine what they call; a value passes from one frame's context
 * to another's directly (a web process has one JavaScript VM), which is
 * what lets a parent recognise a cross-origin child it may not read.
 */

#ifdef GSURF_BACKEND_GTK4
#include <webkit/webkit-web-process-extension.h>
typedef WebKitWebProcessExtension GsurfWebExtension;
#else
#include <webkit2/webkit-web-extension.h>
typedef WebKitWebExtension GsurfWebExtension;
#endif
#include <string.h>
#include "core/gsurf-frames-protocol.h"

/* The script world the probes run in. */
#define PROBE_WORLD "gsurf-frames"

/* Frames nested deeper than this are not placed: a guard against a
 * cycle, not a limit a page reaches. */
#define MAX_DEPTH (32)

/*
 * The probe: given a frame's window, the element of this document that
 * shows it, as [index among the frame elements, content-box left, top,
 * width, height (CSS pixels of this frame's viewport), shown], or null.
 */
#define PROBE_SCRIPT \
	"(function(w){var l=document.querySelectorAll('iframe,frame');" \
	"for(var i=0;i<l.length;i++){var f=l[i];if(f.contentWindow!==w)continue;" \
	"var r=f.getBoundingClientRect(),s=getComputedStyle(f);" \
	"var pl=parseFloat(s.paddingLeft)||0,pt=parseFloat(s.paddingTop)||0," \
	"pr=parseFloat(s.paddingRight)||0,pb=parseFloat(s.paddingBottom)||0;" \
	"var cw=f.clientWidth-pl-pr,ch=f.clientHeight-pt-pb;" \
	"var v=s.visibility!=='hidden'&&s.display!=='none'&&cw>0&&ch>0;" \
	"return [i,r.left+f.clientLeft+pl,r.top+f.clientTop+pt,cw,ch,v];}" \
	"return null;})"

/* Whether this document has an element matching the selector. */
#define HAS_SCRIPT \
	"(function(s){try{return document.querySelector(s)!==null;}catch(e){return false;}})"

/* The main frame's box: its viewport. */
#define VIEWPORT_SCRIPT "[window.innerWidth, window.innerHeight]"

static WebKitScriptWorld *probe_world;
static GHashTable        *page_frames;   /* page id (guint64*) -> GPtrArray of GWeakRef* */

/* --- the frames of a page --- */

static void
weak_ref_free(gpointer data)
{
	GWeakRef *ref = data;

	g_weak_ref_clear(ref);
	g_free(ref);
}

static void
on_page_gone(gpointer data, GObject *where_the_page_was)
{
	guint64 *id = data;

	g_hash_table_remove(page_frames, id);
	g_free(id);
}

static GPtrArray *
frames_of(WebKitWebPage *page, gboolean create)
{
	guint64 id = webkit_web_page_get_id(page);
	GPtrArray *list = g_hash_table_lookup(page_frames, &id);

	if (list == NULL && create) {
		guint64 *key = g_new(guint64, 1);
		guint64 *gone = g_new(guint64, 1);

		*key = *gone = id;
		list = g_ptr_array_new_with_free_func(weak_ref_free);
		g_hash_table_insert(page_frames, key, list);
		g_object_weak_ref(G_OBJECT(page), on_page_gone, gone);
	}
	return list;
}

/* A frame's window object was (re)created for a new document. */
static void
on_window_object_cleared(WebKitScriptWorld *world, WebKitWebPage *page, WebKitFrame *frame,
                         gpointer user_data)
{
	GPtrArray *list = frames_of(page, TRUE);
	GWeakRef *ref;
	guint i = 0;

	/* drop the frames that are gone; stop at this one if it is known */
	while (i < list->len) {
		GObject *known = g_weak_ref_get(g_ptr_array_index(list, i));

		if (known == NULL) {
			g_ptr_array_remove_index(list, i);
			continue;
		}
		g_object_unref(known);
		if (known == (GObject *)frame)
			return;
		i++;
	}
	ref = g_new0(GWeakRef, 1);
	g_weak_ref_init(ref, frame);
	g_ptr_array_add(list, ref);
}

/* --- where each frame is --- */

typedef struct {
	WebKitFrame *frame;      /* ref */
	JSCValue    *window;     /* its window, in the probe world */
	JSCValue    *probe;      /* its probe function */
	gint         parent;     /* the parent's index while collecting; -1: none */
	guint64      parent_id;  /* the parent frame's id; 0: none */
	gint         index;      /* position among the parent's frame elements */
	gdouble      rx, ry;     /* content-box origin in the parent's viewport */
	gdouble      x, y;       /* ... and in the view */
	gdouble      width, height;
	gboolean     shown;      /* the element itself is rendered */
	gboolean     visible;    /* ... and so is every ancestor */
	gint         placed;     /* 0 not yet, 1 placing, 2 placed */
} Node;

static void
node_free(gpointer data)
{
	Node *node = data;

	g_clear_object(&node->frame);
	g_clear_object(&node->window);
	g_clear_object(&node->probe);
	g_free(node);
}

static Node *
node_new(WebKitFrame *frame)
{
	Node *node = g_new0(Node, 1);
	g_autoptr(JSCContext) context =
		webkit_frame_get_js_context_for_script_world(frame, probe_world);

	node->frame = g_object_ref(frame);
	node->parent = -1;
	node->window = jsc_context_evaluate(context, "window", -1);
	node->probe = jsc_context_evaluate(context, PROBE_SCRIPT, -1);
	if (jsc_context_get_exception(context) != NULL)
		jsc_context_clear_exception(context);
	return node;
}

static gdouble
element_double(JSCValue *array, guint index)
{
	g_autoptr(JSCValue) item = jsc_value_object_get_property_at_index(array, index);

	return jsc_value_is_number(item) ? jsc_value_to_double(item) : 0;
}

/* @child's parent: the frame whose probe recognises its window. */
static void
find_parent(GPtrArray *nodes, guint child)
{
	Node *c = g_ptr_array_index(nodes, child);
	guint i;

	if (c->window == NULL)
		return;
	for (i = 0; i < nodes->len; i++) {
		Node *p = g_ptr_array_index(nodes, i);
		g_autoptr(JSCValue) found = NULL;
		g_autoptr(JSCValue) shown = NULL;

		if (i == child || p->probe == NULL || !jsc_value_is_function(p->probe))
			continue;
		found = jsc_value_function_call(p->probe, JSC_TYPE_VALUE, c->window, G_TYPE_NONE);
		if (found == NULL || !jsc_value_is_array(found))
			continue;
		c->parent = (gint)i;
		c->parent_id = webkit_frame_get_id(p->frame);
		c->index = (gint)element_double(found, 0);
		c->rx = element_double(found, 1);
		c->ry = element_double(found, 2);
		c->width = element_double(found, 3);
		c->height = element_double(found, 4);
		shown = jsc_value_object_get_property_at_index(found, 5);
		c->shown = jsc_value_to_boolean(shown);
		return;
	}
}

/* @node's place in the view, its parents' first; FALSE when it is not in
 * the page's frame tree (detached, or a cycle). */
static gboolean
place(GPtrArray *nodes, Node *node, guint depth)
{
	Node *parent;

	if (node->placed == 2)
		return TRUE;
	if (node->placed == 1 || node->parent < 0 || depth > MAX_DEPTH)
		return FALSE;
	node->placed = 1;
	parent = g_ptr_array_index(nodes, node->parent);
	if (!place(nodes, parent, depth + 1)) {
		node->placed = 0;
		return FALSE;
	}
	node->x = parent->x + node->rx;
	node->y = parent->y + node->ry;
	node->visible = parent->visible && node->shown;
	node->placed = 2;
	return TRUE;
}

static gint
by_element_index(gconstpointer a, gconstpointer b)
{
	const Node *x = *(Node *const *)a;
	const Node *y = *(Node *const *)b;

	return x->index - y->index;
}

/* The placed children of nodes[@parent], depth first, in document order. */
static void
append_children(GPtrArray *nodes, gint parent, GPtrArray *out, guint depth)
{
	g_autoptr(GPtrArray) children = g_ptr_array_new();
	guint i;

	if (depth > MAX_DEPTH)
		return;
	for (i = 0; i < nodes->len; i++) {
		Node *n = g_ptr_array_index(nodes, i);

		if (n->parent == parent && n->placed == 2)
			g_ptr_array_add(children, GUINT_TO_POINTER(i));
	}
	for (i = 0; i < children->len; i++)
		children->pdata[i] = g_ptr_array_index(nodes, GPOINTER_TO_UINT(children->pdata[i]));
	g_ptr_array_sort(children, by_element_index);
	for (i = 0; i < children->len; i++) {
		guint at = 0;

		g_ptr_array_add(out, children->pdata[i]);
		if (g_ptr_array_find(nodes, children->pdata[i], &at))
			append_children(nodes, (gint)at, out, depth + 1);
	}
}

/*
 * Every frame of @page in the frame tree, in document order with the main
 * frame first, placed in the view.  The array owns the nodes.
 */
static GPtrArray *
collect(WebKitWebPage *page)
{
	g_autoptr(GPtrArray) nodes = g_ptr_array_new_with_free_func(node_free);
	g_autoptr(GPtrArray) ordered = g_ptr_array_new();
	GPtrArray *list = frames_of(page, FALSE);
	GPtrArray *out = g_ptr_array_new_with_free_func(node_free);
	Node *top = NULL;
	guint i;

	/* the main frame first (webkit_web_page_get_main_frame() is
	 * deprecated: it is one of the frames noted like the others) */
	for (i = 0; list != NULL && i < list->len; i++) {
		WebKitFrame *frame = g_weak_ref_get(g_ptr_array_index(list, i));

		if (frame != NULL && top == NULL && webkit_frame_is_main_frame(frame)) {
			top = node_new(frame);
			g_ptr_array_insert(nodes, 0, top);
		} else if (frame != NULL) {
			g_ptr_array_add(nodes, node_new(frame));
		}
		g_clear_object(&frame);
	}
	if (top == NULL)
		return out;

	{
		g_autoptr(JSCContext) context =
			webkit_frame_get_js_context_for_script_world(top->frame, probe_world);
		g_autoptr(JSCValue) size = jsc_context_evaluate(context, VIEWPORT_SCRIPT, -1);

		if (size != NULL && jsc_value_is_array(size)) {
			top->width = element_double(size, 0);
			top->height = element_double(size, 1);
		}
		top->shown = TRUE;
		top->visible = TRUE;
		top->placed = 2;
	}
	for (i = 1; i < nodes->len; i++)
		find_parent(nodes, i);
	for (i = 1; i < nodes->len; i++)
		place(nodes, g_ptr_array_index(nodes, i), 0);

	/* hand the placed nodes over in document order; free the rest */
	g_ptr_array_add(ordered, top);
	append_children(nodes, 0, ordered, 0);
	for (i = 0; i < nodes->len; i++) {
		Node *n = g_ptr_array_index(nodes, i);

		if (!g_ptr_array_find(ordered, n, NULL))
			node_free(n);
	}
	g_ptr_array_set_free_func(nodes, NULL);
	for (i = 0; i < ordered->len; i++)
		g_ptr_array_add(out, g_ptr_array_index(ordered, i));
	return out;
}

/* --- the frame list --- */

static void
append_json_string(GString *out, const gchar *s)
{
	const guchar *p;

	g_string_append_c(out, '"');
	for (p = (const guchar *)(s != NULL ? s : ""); *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_printf(out, "\\%c", *p);
		else if (*p < 0x20)
			g_string_append_printf(out, "\\u%04x", *p);
		else
			g_string_append_c(out, (gchar)*p);
	}
	g_string_append_c(out, '"');
}

static void
append_member_number(GString *out, const gchar *name, gdouble value)
{
	gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

	g_string_append_printf(out, ",\"%s\":%s", name,
		g_ascii_dtostr(buf, sizeof(buf), value));
}

static gchar *
list_json(WebKitWebPage *page)
{
	g_autoptr(GPtrArray) nodes = collect(page);
	GString *out = g_string_new("[");
	guint i;

	for (i = 0; i < nodes->len; i++) {
		Node *n = g_ptr_array_index(nodes, i);

		if (i > 0)
			g_string_append_c(out, ',');
		g_string_append_printf(out, "{\"id\":%" G_GUINT64_FORMAT ",\"uri\":",
			webkit_frame_get_id(n->frame));
		append_json_string(out, webkit_frame_get_uri(n->frame));
		g_string_append_printf(out, ",\"main\":%s",
			webkit_frame_is_main_frame(n->frame) ? "true" : "false");
		if (n->parent_id != 0)
			g_string_append_printf(out, ",\"parent\":%" G_GUINT64_FORMAT, n->parent_id);
		else
			g_string_append(out, ",\"parent\":null");
		append_member_number(out, "x", n->x);
		append_member_number(out, "y", n->y);
		append_member_number(out, "width", n->width);
		append_member_number(out, "height", n->height);
		g_string_append_printf(out, ",\"visible\":%s}", n->visible ? "true" : "false");
	}
	g_string_append_c(out, ']');
	return g_string_free(out, FALSE);
}

/* --- running a script in one frame --- */

static gboolean
has_selector(Node *node, const gchar *selector)
{
	g_autoptr(JSCContext) context =
		webkit_frame_get_js_context_for_script_world(node->frame, probe_world);
	g_autoptr(JSCValue) fn = jsc_context_evaluate(context, HAS_SCRIPT, -1);
	g_autoptr(JSCValue) found = NULL;

	if (fn == NULL || !jsc_value_is_function(fn))
		return FALSE;
	found = jsc_value_function_call(fn, G_TYPE_STRING, selector, G_TYPE_NONE);
	return found != NULL && jsc_value_to_boolean(found);
}

static gboolean
is_id(const gchar *spec)
{
	const gchar *p;

	for (p = spec; *p != '\0'; p++)
		if (!g_ascii_isdigit(*p))
			return FALSE;
	return p != spec;
}

/*
 * A frame spec that is not an id names a site: "host", "host:port", or
 * "*.host" for the host and its subdomains (a URL is read for its host
 * and port).  It is matched against the frame URI's parsed host -- never
 * against text in the URI, which the page chooses: a frame on
 * evil.example/js.stripe.com/ or on notjs.stripe.com is not js.stripe.com.
 */
typedef struct {
	gchar   *host;        /* lower case, no trailing dot */
	gint     port;        /* -1: any */
	gboolean subdomains;  /* "*.host": the host or one under it */
} Site;

static void
site_clear(Site *site)
{
	g_clear_pointer(&site->host, g_free);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Site, site_clear)

/* @host lower-cased, a trailing dot dropped; NULL when empty. */
static gchar *
normal_host(const gchar *host)
{
	gchar *out;
	gsize len;

	if (host == NULL || *host == '\0')
		return NULL;
	out = g_ascii_strdown(host, -1);
	len = strlen(out);
	if (len > 0 && out[len - 1] == '.')
		out[len - 1] = '\0';
	if (*out == '\0') {
		g_free(out);
		return NULL;
	}
	return out;
}

static gboolean
site_parse(const gchar *spec, Site *site)
{
	g_autofree gchar *url = NULL;
	g_autoptr(GUri) uri = NULL;
	const gchar *rest = spec;

	site->subdomains = FALSE;
	if (g_str_has_prefix(rest, "*.")) {
		site->subdomains = TRUE;
		rest += 2;
	}
	url = strstr(rest, "://") != NULL ? g_strdup(rest) : g_strconcat("http://", rest, NULL);
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL)
		return FALSE;
	/* a bare spec is a site, nothing more: no path, query or user */
	if (strstr(rest, "://") == NULL &&
	    ((g_uri_get_path(uri)[0] != '\0' && g_strcmp0(g_uri_get_path(uri), "/") != 0) ||
	     g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL ||
	     g_uri_get_userinfo(uri) != NULL))
		return FALSE;
	site->host = normal_host(g_uri_get_host(uri));
	site->port = g_uri_get_port(uri);
	return site->host != NULL;
}

/* The port @uri is reached on, its scheme's default when not written. */
static gint
port_of(GUri *uri)
{
	gint port = g_uri_get_port(uri);

	if (port != -1)
		return port;
	return g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") == 0 ? 443 : 80;
}

static gboolean
site_matches(const Site *site, const gchar *frame_uri)
{
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *host = NULL;
	const gchar *scheme;
	gsize hl;
	gsize sl;

	if (frame_uri == NULL)
		return FALSE;
	uri = g_uri_parse(frame_uri, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL)
		return FALSE;
	/* only a document from a network site has a host to trust:
	 * about:, data:, blob: and the like are the embedding page's */
	scheme = g_uri_get_scheme(uri);
	if (g_ascii_strcasecmp(scheme, "http") != 0 && g_ascii_strcasecmp(scheme, "https") != 0)
		return FALSE;
	host = normal_host(g_uri_get_host(uri));
	if (host == NULL)
		return FALSE;
	if (site->port != -1 && port_of(uri) != site->port)
		return FALSE;
	if (strcmp(host, site->host) == 0)
		return TRUE;
	if (!site->subdomains)
		return FALSE;
	/* under it: ends in "." + the host, on a label boundary */
	hl = strlen(host);
	sl = strlen(site->host);
	return hl > sl + 1 && host[hl - sl - 1] == '.' && strcmp(host + hl - sl, site->host) == 0;
}

/*
 * The frame @spec names: the one with that id, or the one frame on that
 * site (that has @require, when given).  Several such frames: the one
 * visible frame among them, else none -- a typed card number is never
 * sent on a guess.  NULL with @why set when there is no single answer.
 */
static Node *
choose(GPtrArray *nodes, const gchar *spec, const gchar *require, gchar **why)
{
	g_autoptr(GPtrArray) candidates = g_ptr_array_new();
	g_auto(Site) site = { NULL, -1, FALSE };
	Node *visible = NULL;
	guint shown = 0;
	guint i;

	if (is_id(spec)) {
		guint64 id = g_ascii_strtoull(spec, NULL, 10);

		for (i = 0; i < nodes->len; i++)
			if (webkit_frame_get_id(((Node *)g_ptr_array_index(nodes, i))->frame) == id)
				return g_ptr_array_index(nodes, i);
		*why = g_strdup_printf("no frame %s (page.frames gives the ids)", spec);
		return NULL;
	}
	if (!site_parse(spec, &site)) {
		*why = g_strdup_printf("'%s' is not a frame id or a site (host, host:port or "
			"*.host)", spec);
		return NULL;
	}
	for (i = 0; i < nodes->len; i++) {
		Node *n = g_ptr_array_index(nodes, i);

		if (!site_matches(&site, webkit_frame_get_uri(n->frame)))
			continue;
		if (require != NULL && *require != '\0' && !has_selector(n, require))
			continue;
		g_ptr_array_add(candidates, n);
		if (n->visible) {
			visible = n;
			shown++;
		}
	}
	if (candidates->len == 1)
		return g_ptr_array_index(candidates, 0);
	if (candidates->len > 1 && shown == 1)
		return visible;
	if (candidates->len == 0) {
		*why = require != NULL && *require != '\0'
			? g_strdup_printf("no frame on '%s' has '%s' (page.frames lists them)", spec,
				require)
			: g_strdup_printf("no frame is on '%s' (page.frames lists them)", spec);
		return NULL;
	}
	{
		GString *ids = g_string_new(NULL);

		for (i = 0; i < candidates->len; i++)
			g_string_append_printf(ids, "%s%" G_GUINT64_FORMAT, i > 0 ? ", " : "",
				webkit_frame_get_id(((Node *)g_ptr_array_index(candidates, i))->frame));
		*why = g_strdup_printf("%u frames on '%s' match (ids %s); name one by its id",
			candidates->len, spec, ids->str);
		g_string_free(ids, TRUE);
	}
	return NULL;
}

static GVariant *
eval_reply(guint status, guint64 id, const gchar *text, gdouble x, gdouble y)
{
	return g_variant_new(GSURF_FRAMES_EVAL_REPLY, status, id, text != NULL ? text : "", x, y);
}

static GVariant *
eval_in_frame(WebKitWebPage *page, const gchar *spec, const gchar *script,
              const gchar *require)
{
	g_autoptr(GPtrArray) nodes = collect(page);
	g_autoptr(GPtrArray) after = NULL;
	g_autoptr(WebKitFrame) frame = NULL;
	g_autoptr(JSCContext) context = NULL;
	g_autoptr(JSCValue) value = NULL;
	g_autofree gchar *text = NULL;
	JSCException *exception;
	g_autofree gchar *why = NULL;
	Node *chosen = choose(nodes, spec, require, &why);
	guint64 id;
	guint i;

	if (chosen == NULL)
		return eval_reply(GSURF_FRAMES_STATUS_NO_FRAME, 0, why, 0, 0);
	frame = g_object_ref(chosen->frame);
	id = webkit_frame_get_id(frame);
	g_clear_pointer(&nodes, g_ptr_array_unref);

	/* the page's own world, as a main-frame script runs */
	context = webkit_frame_get_js_context(frame);
	value = jsc_context_evaluate(context, script, -1);
	exception = jsc_context_get_exception(context);
	if (exception != NULL) {
		g_autofree gchar *why = g_strdup_printf("%s: %s",
			jsc_exception_get_name(exception), jsc_exception_get_message(exception));

		jsc_context_clear_exception(context);
		return eval_reply(GSURF_FRAMES_STATUS_SCRIPT, id, why, 0, 0);
	}
	if (value == NULL || jsc_value_is_undefined(value))
		text = g_strdup("null");
	else if (jsc_value_is_string(value))
		text = jsc_value_to_string(value);
	else
		text = jsc_value_to_json(value, 0);

	/* where the frame is now: the script may have scrolled it */
	after = collect(page);
	for (i = 0; i < after->len; i++) {
		Node *n = g_ptr_array_index(after, i);

		if (webkit_frame_get_id(n->frame) == id)
			return eval_reply(GSURF_FRAMES_STATUS_OK, id, text != NULL ? text : "null",
				n->x, n->y);
	}
	return eval_reply(GSURF_FRAMES_STATUS_OK, id, text != NULL ? text : "null", 0, 0);
}

/* --- messages from gsurf --- */

static gboolean
on_user_message(WebKitWebPage *page, WebKitUserMessage *message, gpointer user_data)
{
	const gchar *name = webkit_user_message_get_name(message);
	GVariant *params = webkit_user_message_get_parameters(message);
	GVariant *reply;

	if (g_strcmp0(name, GSURF_FRAMES_MSG_LIST) == 0) {
		g_autofree gchar *json = list_json(page);

		reply = g_variant_new("(s)", json);
	} else if (g_strcmp0(name, GSURF_FRAMES_MSG_EVAL) == 0) {
		const gchar *spec;
		const gchar *script;
		const gchar *require;

		if (params == NULL ||
		    !g_variant_is_of_type(params, G_VARIANT_TYPE(GSURF_FRAMES_EVAL_PARAMS)))
			reply = eval_reply(GSURF_FRAMES_STATUS_SCRIPT, 0, "bad request", 0, 0);
		else {
			g_variant_get(params, "(&s&s&s)", &spec, &script, &require);
			reply = eval_in_frame(page, spec, script, require);
		}
	} else {
		return FALSE;   /* someone else's */
	}
	webkit_user_message_send_reply(message, webkit_user_message_new(name, reply));
	return TRUE;
}

static void
on_page_created(GsurfWebExtension *extension, WebKitWebPage *page, gpointer user_data)
{
	g_signal_connect(page, "user-message-received", G_CALLBACK(on_user_message), NULL);
}

static void
initialize(GsurfWebExtension *extension)
{
	page_frames = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	probe_world = webkit_script_world_new_with_name(PROBE_WORLD);
	g_signal_connect(extension, "page-created", G_CALLBACK(on_page_created), NULL);
	g_signal_connect(webkit_script_world_get_default(), "window-object-cleared",
		G_CALLBACK(on_window_object_cleared), NULL);
}

#ifdef GSURF_BACKEND_GTK4
G_MODULE_EXPORT void
webkit_web_process_extension_initialize(WebKitWebProcessExtension *extension)
{
	initialize(extension);
}
#else
G_MODULE_EXPORT void
webkit_web_extension_initialize(WebKitWebExtension *extension)
{
	initialize(extension);
}
#endif
