/*
 * test-window-layout.c - GsurfWindowLayout delegation, visibility, key grab
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Headless: a GsurfWindow subclass records every backend vfunc call
 * (insert/remove/active/set_content/connect_view) and a fake layout
 * records every interface call, both into one log, so the exact order
 * gsurf_window_set_layout() promises is checked without a display.
 * test-window-layout-webkit covers the GTK backends with real views.
 *
 * Log entries, comma terminated:
 *   backend   winsert:<v> wremove:<v> wactive:<v> wconnect:<v>
 *             content:<root|null>
 *   layout    attach detach add:<v> lremove:<v> activate:<v>
 *   signals   visible  (GsurfWindow::visible-views-changed)
 *
 * The optional extras of 0.4.2 (groups, drops, view drags) and the view's
 * opener-id / new-view-requested are checked here too, without the log.
 */

#include <gsurf.h>
#include <string.h>

/* One log for everything, reset per test. */
static GString *call_log;

static void
log_call(const gchar *what, GsurfView *view)
{
	if (view == NULL) {
		g_string_append_printf(call_log, "%s,", what);
		return;
	}
	g_string_append_printf(call_log, "%s:%s,", what,
		(const gchar *)g_object_get_data(G_OBJECT(view), "name"));
}

/* Take the log so far, leaving it empty. */
static gchar *
take_log(void)
{
	gchar *out = g_strdup(call_log->str);

	g_string_truncate(call_log, 0);
	return out;
}

#define ASSERT_LOG(expected) \
	G_STMT_START { \
		g_autofree gchar *got__ = take_log(); \
		g_assert_cmpstr(got__, ==, (expected)); \
	} G_STMT_END

/* --- headless view --- */

typedef struct { GsurfView parent; } TestView;
typedef struct { GsurfViewClass parent; } TestViewClass;

GType test_view_get_type(void);
G_DEFINE_TYPE(TestView, test_view, GSURF_TYPE_VIEW)

static void test_view_class_init(TestViewClass *klass) { (void)klass; }
static void test_view_init(TestView *self) { (void)self; }

static GsurfView *
new_view(const gchar *name)
{
	GsurfView *view = g_object_new(test_view_get_type(), NULL);

	g_object_set_data_full(G_OBJECT(view), "name", g_strdup(name), g_free);
	return view;
}

/* --- recording windows: one with set_content, one without --- */

typedef struct { GsurfWindow parent; } TestWindow;
typedef struct { GsurfWindowClass parent; } TestWindowClass;
typedef struct { GsurfWindow parent; } TestPlainWindow;
typedef struct { GsurfWindowClass parent; } TestPlainWindowClass;

GType test_window_get_type(void);
GType test_plain_window_get_type(void);
G_DEFINE_TYPE(TestWindow, test_window, GSURF_TYPE_WINDOW)
G_DEFINE_TYPE(TestPlainWindow, test_plain_window, GSURF_TYPE_WINDOW)

static void w_insert(GsurfWindow *w, GsurfView *v) { (void)w; log_call("winsert", v); }
static void w_remove(GsurfWindow *w, GsurfView *v) { (void)w; log_call("wremove", v); }
static void w_active(GsurfWindow *w, GsurfView *v) { (void)w; log_call("wactive", v); }
static void w_connect(GsurfWindow *w, GsurfView *v) { (void)w; log_call("wconnect", v); }

static void
w_set_content(GsurfWindow *w, gpointer widget)
{
	(void)w;
	g_string_append_printf(call_log, "content:%s,",
		widget == NULL ? "null"
		               : (const gchar *)g_object_get_data(G_OBJECT(widget), "name"));
}

static void
test_window_class_init(TestWindowClass *klass)
{
	GsurfWindowClass *wc = GSURF_WINDOW_CLASS(klass);

	wc->insert_view = w_insert;
	wc->remove_view = w_remove;
	wc->set_active_view = w_active;
	wc->set_content = w_set_content;
	wc->connect_view = w_connect;
}

static void test_window_init(TestWindow *self) { (void)self; }

/* Like libregnum: views, but no native content to swap. */
static void
test_plain_window_class_init(TestPlainWindowClass *klass)
{
	GsurfWindowClass *wc = GSURF_WINDOW_CLASS(klass);

	wc->insert_view = w_insert;
	wc->remove_view = w_remove;
	wc->set_active_view = w_active;
}

static void test_plain_window_init(TestPlainWindow *self) { (void)self; }

static void
on_visible(GsurfWindow *window, gpointer user_data)
{
	(void)window;
	(void)user_data;
	g_string_append(call_log, "visible,");
}

/* A recording window with views @names in order, the first active, the
 * log cleared and visible-views-changed logged. */
static GsurfWindow *
new_window(const gchar *const *names)
{
	GsurfWindow *window = g_object_new(test_window_get_type(), NULL);
	guint i;

	for (i = 0; names != NULL && names[i] != NULL; i++) {
		g_autoptr(GsurfView) view = new_view(names[i]);

		gsurf_window_add_view(window, view);
	}
	g_signal_connect(window, "visible-views-changed", G_CALLBACK(on_visible), NULL);
	g_string_truncate(call_log, 0);
	return window;
}

static GsurfView *
view_named(GsurfWindow *window, const gchar *name)
{
	guint i;

	for (i = 0; i < gsurf_window_get_n_views(window); i++) {
		GsurfView *view = gsurf_window_get_nth_view(window, i);

		if (g_strcmp0(g_object_get_data(G_OBJECT(view), "name"), name) == 0)
			return view;
	}
	g_assert_not_reached();
	return NULL;
}

/* --- fake layouts --- */

/* A full layout: records every call, shows the views in @visible (plus
 * the active one), serializes as "<index list>". */
typedef struct {
	GObject     parent;
	GObject    *root;     /* stands in for the native root widget */
	GHashTable *visible;  /* GsurfView* set, borrowed */
	gchar      *name;
	/* The 0.4.2 extras: what was asked last. */
	gint        selected;     /* select_group index, -1 */
	GsurfView  *drop_view;    /* show_view_at, borrowed */
	GsurfView  *drop_target;
	gint        drop_zone;
	gint        drags;        /* begin_view_drag - end_view_drag */
	gchar      *renamed;      /* "<index>=<name>" of the last rename_group */
} TestLayout;
typedef struct { GObjectClass parent; } TestLayoutClass;

static void test_layout_iface_init(GsurfWindowLayoutInterface *iface);
GType test_layout_get_type(void);
G_DEFINE_TYPE_WITH_CODE(TestLayout, test_layout, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_WINDOW_LAYOUT, test_layout_iface_init))

static gpointer
l_get_root(GsurfWindowLayout *self)
{
	return ((TestLayout *)self)->root;
}

static void
l_attach(GsurfWindowLayout *self, GsurfWindow *window)
{
	(void)window;
	g_string_append_printf(call_log, "%s.attach,", ((TestLayout *)self)->name);
}

static void
l_detach(GsurfWindowLayout *self, GsurfWindow *window)
{
	(void)window;
	g_hash_table_remove_all(((TestLayout *)self)->visible);
	g_string_append_printf(call_log, "%s.detach,", ((TestLayout *)self)->name);
}

static void
l_add(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view)
{
	(void)self;
	(void)window;
	log_call("add", view);
}

static void
l_remove(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view)
{
	(void)window;
	g_hash_table_remove(((TestLayout *)self)->visible, view);
	log_call("lremove", view);
}

static void
l_activate(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view)
{
	(void)self;
	(void)window;
	log_call("activate", view);
}

static gboolean
l_visible(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view)
{
	return gsurf_window_get_active_view(window) == view ||
	       g_hash_table_contains(((TestLayout *)self)->visible, view);
}

/* "i,j,..." of every view @map knows, in window order. */
static gchar *
l_serialize(GsurfWindowLayout *self, GsurfWindow *window,
            GsurfWindowLayoutIndexFunc map, gpointer user_data)
{
	GString *out = g_string_new(NULL);
	guint i;

	(void)self;
	for (i = 0; i < gsurf_window_get_n_views(window); i++) {
		gint index = map(gsurf_window_get_nth_view(window, i), user_data);

		if (index >= 0)
			g_string_append_printf(out, "%s%d", out->len > 0 ? "," : "", index);
	}
	return g_string_free(out, FALSE);
}

/* Accepts "n=<count>" when @n_views matches; makes every view visible. */
static gboolean
l_restore(GsurfWindowLayout *self, GsurfWindow *window, const gchar *state,
          GsurfView **views, guint n_views)
{
	g_autofree gchar *want = g_strdup_printf("n=%u", n_views);
	guint i;

	if (g_strcmp0(state, want) != 0)
		return FALSE;
	for (i = 0; i < n_views; i++)
		g_hash_table_add(((TestLayout *)self)->visible, views[i]);
	gsurf_window_layout_notify_visible(window);
	return TRUE;
}

/* Two groups: 0 "web" (current) showing every view, 1 "" empty. */
static GPtrArray *
l_list_groups(GsurfWindowLayout *self, GsurfWindow *window)
{
	GPtrArray *groups = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_layout_group_unref);
	GsurfLayoutGroup *web = gsurf_layout_group_new(0, "web", TRUE);
	guint i;

	(void)self;
	for (i = 0; i < gsurf_window_get_n_views(window); i++)
		gsurf_layout_group_add_view_id(web, gsurf_view_get_id(gsurf_window_get_nth_view(window, i)));
	g_ptr_array_add(groups, web);
	g_ptr_array_add(groups, gsurf_layout_group_new(1, NULL, FALSE));
	return groups;
}

static gboolean
l_select_group(GsurfWindowLayout *self, GsurfWindow *window, guint index)
{
	(void)window;
	if (index > 1)
		return FALSE;
	((TestLayout *)self)->selected = (gint)index;
	return TRUE;
}

static gboolean
l_show_view_at(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view,
               GsurfView *target, GsurfLayoutDropZone zone)
{
	TestLayout *layout = (TestLayout *)self;

	(void)window;
	layout->drop_view = view;
	layout->drop_target = target;
	layout->drop_zone = (gint)zone;
	return view != target;
}

static gboolean
l_rename_group(GsurfWindowLayout *self, GsurfWindow *window, guint index, const gchar *name)
{
	TestLayout *layout = (TestLayout *)self;

	(void)window;
	if (index > 1)
		return FALSE;
	g_free(layout->renamed);
	layout->renamed = g_strdup_printf("%u=%s", index, name != NULL ? name : "(null)");
	return TRUE;
}

static void
l_begin_drag(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view)
{
	(void)window;
	(void)view;
	((TestLayout *)self)->drags++;
}

static void
l_end_drag(GsurfWindowLayout *self, GsurfWindow *window)
{
	(void)window;
	((TestLayout *)self)->drags--;
}

static void
test_layout_iface_init(GsurfWindowLayoutInterface *iface)
{
	iface->get_root = l_get_root;
	iface->attach = l_attach;
	iface->detach = l_detach;
	iface->add_view = l_add;
	iface->remove_view = l_remove;
	iface->activate_view = l_activate;
	iface->is_view_visible = l_visible;
	iface->serialize = l_serialize;
	iface->restore = l_restore;
	iface->list_groups = l_list_groups;
	iface->select_group = l_select_group;
	iface->show_view_at = l_show_view_at;
	iface->begin_view_drag = l_begin_drag;
	iface->end_view_drag = l_end_drag;
	iface->rename_group = l_rename_group;
}

static void
test_layout_finalize(GObject *object)
{
	TestLayout *self = (TestLayout *)object;

	g_clear_object(&self->root);
	g_clear_pointer(&self->visible, g_hash_table_unref);
	g_clear_pointer(&self->name, g_free);
	g_clear_pointer(&self->renamed, g_free);
	G_OBJECT_CLASS(test_layout_parent_class)->finalize(object);
}

static void
test_layout_class_init(TestLayoutClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = test_layout_finalize;
}

static void
test_layout_init(TestLayout *self)
{
	self->visible = g_hash_table_new(g_direct_hash, g_direct_equal);
	self->selected = -1;
	self->drop_zone = -1;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(TestLayout, g_object_unref)

static TestLayout *
new_layout(const gchar *name)
{
	TestLayout *layout = g_object_new(test_layout_get_type(), NULL);

	layout->name = g_strdup(name);
	layout->root = g_object_new(G_TYPE_OBJECT, NULL);
	g_object_set_data_full(layout->root, "name", g_strconcat(name, ".root", NULL), g_free);
	return layout;
}

/* A layout implementing nothing: every wrapper falls back to its default. */
typedef struct { GObject parent; } TestMinLayout;
typedef struct { GObjectClass parent; } TestMinLayoutClass;

static void test_min_layout_iface_init(GsurfWindowLayoutInterface *iface) { (void)iface; }
GType test_min_layout_get_type(void);
G_DEFINE_TYPE_WITH_CODE(TestMinLayout, test_min_layout, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_WINDOW_LAYOUT, test_min_layout_iface_init))

G_DEFINE_AUTOPTR_CLEANUP_FUNC(TestMinLayout, g_object_unref)

static void test_min_layout_class_init(TestMinLayoutClass *klass) { (void)klass; }
static void test_min_layout_init(TestMinLayout *self) { (void)self; }

static void
setup(void)
{
	if (call_log == NULL)
		call_log = g_string_new(NULL);
	g_string_truncate(call_log, 0);
}

/* --- tests --- */

/* A backend without set_content (libregnum) refuses a layout, warns once
 * per window, never touches the layout, and keeps its stack. */
static void
test_refused_without_content(void)
{
	g_autoptr(GsurfWindow) window = NULL;
	g_autoptr(TestLayout) layout = new_layout("L");
	GsurfView *a;

	setup();
	window = g_object_new(test_plain_window_get_type(), NULL);
	{
		g_autoptr(GsurfView) view = new_view("a");

		gsurf_window_add_view(window, view);
	}
	g_string_truncate(call_log, 0);
	a = view_named(window, "a");

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*cannot host a layout*");
	g_assert_false(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	g_test_assert_expected_messages();
	g_assert_null(gsurf_window_get_layout(window));
	ASSERT_LOG("");

	/* Again: refused silently (warned once), still no calls. */
	g_assert_false(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	ASSERT_LOG("");
	g_assert_cmpuint(G_OBJECT(layout)->ref_count, ==, 1);

	/* Removing nothing is fine; the window still runs its own stack. */
	g_assert_true(gsurf_window_set_layout(window, NULL));
	g_assert_true(gsurf_window_get_view_visible(window, a));
	g_object_ref(a);
	gsurf_window_remove_view(window, a);
	ASSERT_LOG("wremove:a,");
	g_object_unref(a);
}

/* Kiosk shows one view by design: refused with a warning. */
static void
test_refused_in_kiosk(void)
{
	static const gchar *const names[] = { "a", NULL };
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfWindow) window = NULL;
	g_autoptr(TestLayout) layout = new_layout("L");

	setup();
	window = new_window(names);
	config->kiosk = TRUE;
	gsurf_config_set_default(config);

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*kiosk*");
	g_assert_false(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	g_test_assert_expected_messages();
	g_assert_null(gsurf_window_get_layout(window));
	ASSERT_LOG("");

	/* Out of kiosk the same window takes it (the warning was per reason
	 * of the first refusal; a later success is unaffected). */
	gsurf_config_set_default(NULL);
	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	g_assert_true(gsurf_window_get_layout(window) == GSURF_WINDOW_LAYOUT(layout));
	g_assert_true(gsurf_window_set_layout(window, NULL));
}

/* Installing over existing views: out of the stack first, then attach,
 * every view in order, the active one, and the root last; one signal. */
static void
test_attach_existing(void)
{
	static const gchar *const names[] = { "a", "b", "c", NULL };
	g_autoptr(GsurfWindow) window = new_window(names);
	g_autoptr(TestLayout) layout = new_layout("L");

	setup();
	gsurf_window_set_active_view(window, view_named(window, "b"));
	ASSERT_LOG("wactive:b,");

	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	ASSERT_LOG("wremove:a,wremove:b,wremove:c,"
	           "L.attach,add:a,add:b,add:c,activate:b,content:L.root,visible,");
	g_assert_true(gsurf_window_get_layout(window) == GSURF_WINDOW_LAYOUT(layout));
	/* The window holds a reference. */
	g_assert_cmpuint(G_OBJECT(layout)->ref_count, ==, 2);

	/* The same layout again: nothing happens. */
	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	ASSERT_LOG("");

	g_assert_true(gsurf_window_set_layout(window, NULL));
	g_assert_cmpuint(G_OBJECT(layout)->ref_count, ==, 1);
}

/* An empty window: attach and the root, nothing to add or activate; the
 * first view added later is added and then activated through it. */
static void
test_attach_empty(void)
{
	g_autoptr(GsurfWindow) window = new_window(NULL);
	g_autoptr(TestLayout) layout = new_layout("L");
	g_autoptr(GsurfView) a = new_view("a");

	setup();
	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	ASSERT_LOG("L.attach,content:L.root,visible,");
	gsurf_window_add_view(window, a);
	ASSERT_LOG("wconnect:a,add:a,activate:a,");
	g_assert_true(gsurf_window_get_active_view(window) == a);
	g_assert_true(gsurf_window_set_layout(window, NULL));
}

/* While a layout is set, insert/remove/activate go to it, never to the
 * stack; the backend still wires each new view. */
static void
test_delegation(void)
{
	static const gchar *const names[] = { "a", "b", "c", NULL };
	g_autoptr(GsurfWindow) window = new_window(names);
	g_autoptr(TestLayout) layout = new_layout("L");
	g_autoptr(GsurfView) d = new_view("d");
	GsurfView *b;

	setup();
	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	g_string_truncate(call_log, 0);

	/* insert (at an index: the list order still follows it) */
	gsurf_window_insert_view(window, d, 1);
	ASSERT_LOG("wconnect:d,add:d,");
	g_assert_cmpint(gsurf_window_get_view_index(window, d), ==, 1);

	/* activate */
	b = view_named(window, "b");
	gsurf_window_set_active_view(window, b);
	ASSERT_LOG("activate:b,");
	gsurf_window_set_active_view(window, b);
	ASSERT_LOG("");

	/* remove the active view: the layout lets go, then the neighbour at
	 * its index ("c") is activated through the layout. */
	gsurf_window_remove_view(window, b);
	ASSERT_LOG("lremove:b,activate:c,");
	g_assert_true(gsurf_window_get_active_view(window) == view_named(window, "c"));

	/* remove a background view: no activation */
	gsurf_window_remove_view(window, d);
	ASSERT_LOG("lremove:d,");

	/* move only reorders the list; layouts are not told */
	gsurf_window_move_view(window, view_named(window, "c"), 0);
	ASSERT_LOG("");

	/* remove the rest: the last removal clears the active view */
	gsurf_window_remove_view(window, view_named(window, "a"));
	ASSERT_LOG("lremove:a,");
	gsurf_window_remove_view(window, view_named(window, "c"));
	ASSERT_LOG("lremove:c,");
	g_assert_null(gsurf_window_get_active_view(window));
	g_assert_true(gsurf_window_set_layout(window, NULL));
	ASSERT_LOG("L.detach,content:null,visible,");
}

/* Removing the layout: detach, the root leaves, every view goes back into
 * the stack in window order and the active one is shown; once. */
static void
test_detach_restores(void)
{
	static const gchar *const names[] = { "a", "b", "c", NULL };
	g_autoptr(GsurfWindow) window = new_window(names);
	g_autoptr(TestLayout) layout = new_layout("L");
	g_autoptr(GsurfView) d = new_view("d");

	setup();
	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	gsurf_window_add_view(window, d);
	gsurf_window_move_view(window, d, 0);
	gsurf_window_set_active_view(window, view_named(window, "c"));
	g_string_truncate(call_log, 0);

	g_assert_true(gsurf_window_set_layout(window, NULL));
	ASSERT_LOG("L.detach,content:null,winsert:d,winsert:a,winsert:b,winsert:c,"
	           "wactive:c,visible,");
	g_assert_null(gsurf_window_get_layout(window));

	/* Idempotent. */
	g_assert_true(gsurf_window_set_layout(window, NULL));
	ASSERT_LOG("");

	/* Back to the stack for everything. */
	gsurf_window_set_active_view(window, view_named(window, "a"));
	ASSERT_LOG("wactive:a,");
	{
		g_autoptr(GsurfView) e = new_view("e");

		gsurf_window_add_view(window, e);
		ASSERT_LOG("winsert:e,");
	}
}

/* A second layout replaces the first: the first is fully detached (views
 * back in the stack), then the second attached; a single signal. */
static void
test_replace(void)
{
	static const gchar *const names[] = { "a", "b", NULL };
	g_autoptr(GsurfWindow) window = new_window(names);
	g_autoptr(TestLayout) one = new_layout("L1");
	g_autoptr(TestLayout) two = new_layout("L2");

	setup();
	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(one)));
	g_string_truncate(call_log, 0);

	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(two)));
	ASSERT_LOG("L1.detach,content:null,winsert:a,winsert:b,wactive:a,"
	           "wremove:a,wremove:b,L2.attach,add:a,add:b,activate:a,content:L2.root,"
	           "visible,");
	g_assert_cmpuint(G_OBJECT(one)->ref_count, ==, 1);
	g_assert_cmpuint(G_OBJECT(two)->ref_count, ==, 2);
	g_assert_true(gsurf_window_set_layout(window, NULL));
}

/* Visibility: only the active view without a layout; the layout decides
 * with one; its notify emits the signal; views of other windows never. */
static void
test_visibility(void)
{
	static const gchar *const names[] = { "a", "b", "c", NULL };
	g_autoptr(GsurfWindow) window = new_window(names);
	g_autoptr(TestLayout) layout = new_layout("L");
	g_autoptr(TestMinLayout) min = g_object_new(test_min_layout_get_type(), NULL);
	g_autoptr(GsurfView) stranger = new_view("x");
	GsurfView *a = view_named(window, "a");
	GsurfView *b = view_named(window, "b");
	GsurfView *c = view_named(window, "c");

	setup();
	g_assert_true(gsurf_window_get_view_visible(window, a));
	g_assert_false(gsurf_window_get_view_visible(window, b));
	g_assert_false(gsurf_window_get_view_visible(window, stranger));

	/* Without a layout an active change is announced by
	 * active-view-changed, not by visible-views-changed. */
	gsurf_window_set_active_view(window, b);
	ASSERT_LOG("wactive:b,");
	g_assert_true(gsurf_window_get_view_visible(window, b));
	g_assert_false(gsurf_window_get_view_visible(window, a));

	/* A layout showing b (active) and c. */
	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	g_string_truncate(call_log, 0);
	g_hash_table_add(layout->visible, c);
	gsurf_window_layout_notify_visible(window);
	ASSERT_LOG("visible,");
	g_assert_false(gsurf_window_get_view_visible(window, a));
	g_assert_true(gsurf_window_get_view_visible(window, b));
	g_assert_true(gsurf_window_get_view_visible(window, c));
	g_assert_false(gsurf_window_get_view_visible(window, stranger));
	g_assert_true(gsurf_window_layout_is_view_visible(GSURF_WINDOW_LAYOUT(layout), window, c));

	/* A layout without is_view_visible: only the active view. */
	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(min)));
	g_assert_true(gsurf_window_get_view_visible(window, b));
	g_assert_false(gsurf_window_get_view_visible(window, c));
	g_assert_true(gsurf_window_set_layout(window, NULL));
	g_assert_true(gsurf_window_get_view_visible(window, b));
	g_assert_false(gsurf_window_get_view_visible(window, c));
}

/* Saved-index map for serialize: the window position, except "b" which
 * is not saved. */
static gint
map_index(GsurfView *view, gpointer user_data)
{
	if (g_strcmp0(g_object_get_data(G_OBJECT(view), "name"), "b") == 0)
		return -1;
	return gsurf_window_get_view_index(GSURF_WINDOW(user_data), view);
}

/* A layout implementing nothing: attach still works (no root packed is
 * the backend's business), the window keeps its bookkeeping. */
static void
test_minimal_layout(void)
{
	static const gchar *const names[] = { "a", "b", NULL };
	g_autoptr(GsurfWindow) window = new_window(names);
	g_autoptr(TestMinLayout) min = g_object_new(test_min_layout_get_type(), NULL);
	GsurfWindowLayout *l = GSURF_WINDOW_LAYOUT(min);
	GsurfView *views[1];

	setup();
	g_assert_null(gsurf_window_layout_get_root(l));
	g_assert_true(gsurf_window_set_layout(window, l));
	ASSERT_LOG("wremove:a,wremove:b,content:null,visible,");
	gsurf_window_set_active_view(window, view_named(window, "b"));
	ASSERT_LOG("");
	g_assert_true(gsurf_window_get_active_view(window) == view_named(window, "b"));

	/* serialize/restore unsupported */
	views[0] = view_named(window, "a");
	g_assert_null(gsurf_window_layout_serialize(l, window, map_index, window));
	g_assert_false(gsurf_window_layout_restore(l, window, "anything", views, 1));
	g_assert_true(gsurf_window_set_layout(window, NULL));
	ASSERT_LOG("content:null,winsert:a,winsert:b,wactive:b,visible,");
}

/* serialize/restore reach the layout through the public wrappers. */
static void
test_serialize_restore(void)
{
	static const gchar *const names[] = { "a", "b", "c", NULL };
	g_autoptr(GsurfWindow) window = new_window(names);
	g_autoptr(TestLayout) layout = new_layout("L");
	GsurfWindowLayout *l = GSURF_WINDOW_LAYOUT(layout);
	g_autofree gchar *state = NULL;
	GsurfView *views[2];

	setup();
	g_assert_true(gsurf_window_set_layout(window, l));
	state = gsurf_window_layout_serialize(l, window, map_index, window);
	g_assert_cmpstr(state, ==, "0,2");

	views[0] = view_named(window, "a");
	views[1] = view_named(window, "c");
	g_string_truncate(call_log, 0);
	g_assert_false(gsurf_window_layout_restore(l, window, "n=3", views, 2));
	ASSERT_LOG("");
	g_assert_true(gsurf_window_layout_restore(l, window, "n=2", views, 2));
	ASSERT_LOG("visible,");
	g_assert_true(gsurf_window_get_view_visible(window, views[1]));
	g_assert_true(gsurf_window_set_layout(window, NULL));
}

/* Key grab: off by default, a plain boolean, per window. */
static void
test_key_grab(void)
{
	g_autoptr(GsurfWindow) one = new_window(NULL);
	g_autoptr(GsurfWindow) two = new_window(NULL);

	setup();
	g_assert_false(gsurf_window_get_key_grab(one));
	gsurf_window_set_key_grab(one, TRUE);
	g_assert_true(gsurf_window_get_key_grab(one));
	g_assert_false(gsurf_window_get_key_grab(two));
	gsurf_window_set_key_grab(one, 5); /* any non-zero value is TRUE */
	g_assert_cmpint(gsurf_window_get_key_grab(one), ==, TRUE);
	gsurf_window_set_key_grab(one, FALSE);
	g_assert_false(gsurf_window_get_key_grab(one));
	/* Unrelated to layouts and views. */
	ASSERT_LOG("");
}

/* Disposing a window with a layout set detaches it (views back in the
 * stack) and drops its reference; window, layout and views all go. */
static void
test_dispose_with_layout(void)
{
	static const gchar *const names[] = { "a", "b", NULL };
	GsurfWindow *window = new_window(names);
	TestLayout *layout = new_layout("L");
	gpointer weak_window = window;
	gpointer weak_layout = layout;
	gpointer weak_a = view_named(window, "a");
	gpointer weak_b = view_named(window, "b");

	setup();
	g_object_add_weak_pointer(G_OBJECT(window), &weak_window);
	g_object_add_weak_pointer(G_OBJECT(layout), &weak_layout);
	g_object_add_weak_pointer(G_OBJECT(weak_a), &weak_a);
	g_object_add_weak_pointer(G_OBJECT(weak_b), &weak_b);

	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	/* The window keeps the layout alive on its own. */
	g_object_unref(layout);
	g_assert_nonnull(weak_layout);
	g_string_truncate(call_log, 0);

	g_object_unref(window);
	ASSERT_LOG("L.detach,content:null,winsert:a,winsert:b,wactive:a,visible,");
	g_assert_null(weak_window);
	g_assert_null(weak_layout);
	g_assert_null(weak_a);
	g_assert_null(weak_b);
}

/* The layout outlives a window only through its own references; removing
 * it from a window releases the window's reference at once. */
static void
test_layout_lifetime(void)
{
	static const gchar *const names[] = { "a", NULL };
	g_autoptr(GsurfWindow) one = new_window(names);
	g_autoptr(GsurfWindow) two = new_window(names);
	TestLayout *layout = new_layout("L");
	gpointer weak_layout = layout;

	setup();
	g_object_add_weak_pointer(G_OBJECT(layout), &weak_layout);
	/* One layout object may serve two windows (attach is per window). */
	g_assert_true(gsurf_window_set_layout(one, GSURF_WINDOW_LAYOUT(layout)));
	g_assert_true(gsurf_window_set_layout(two, GSURF_WINDOW_LAYOUT(layout)));
	g_object_unref(layout);
	g_assert_true(gsurf_window_set_layout(one, NULL));
	g_assert_nonnull(weak_layout);
	g_assert_true(gsurf_window_set_layout(two, NULL));
	g_assert_null(weak_layout);
}

/* Bad arguments are refused by the wrappers' guards. */
static void
test_bad_arguments(void)
{
	g_autoptr(GsurfWindow) window = new_window(NULL);
	g_autoptr(GObject) not_a_layout = g_object_new(G_TYPE_OBJECT, NULL);

	setup();
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*GSURF_IS_WINDOW_LAYOUT*");
	g_assert_false(gsurf_window_set_layout(window, (GsurfWindowLayout *)not_a_layout));
	g_test_assert_expected_messages();
	g_assert_null(gsurf_window_get_layout(window));

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*GSURF_IS_WINDOW*");
	g_assert_false(gsurf_window_get_view_visible(NULL, NULL));
	g_test_assert_expected_messages();
}

/* --- 0.4.2: groups, drops, drags --- */

static void
count_signal(GsurfWindow *window, gpointer user_data)
{
	(void)window;
	(*(guint *)user_data)++;
}

/* The boxed group: values, ids (no zero, no duplicates), references. */
static void
test_group_boxed(void)
{
	g_autoptr(GsurfLayoutGroup) group = gsurf_layout_group_new(3, "mail", TRUE);
	g_autoptr(GsurfLayoutGroup) unnamed = gsurf_layout_group_new(0, NULL, FALSE);
	GsurfLayoutGroup *copy;
	const guint64 *ids;
	guint n = 42;

	g_assert_true(G_TYPE_IS_BOXED(GSURF_TYPE_LAYOUT_GROUP));
	g_assert_cmpuint(gsurf_layout_group_get_index(group), ==, 3);
	g_assert_cmpstr(gsurf_layout_group_get_name(group), ==, "mail");
	g_assert_true(gsurf_layout_group_get_current(group));
	g_assert_cmpstr(gsurf_layout_group_get_name(unnamed), ==, "");
	g_assert_false(gsurf_layout_group_get_current(unnamed));
	ids = gsurf_layout_group_get_view_ids(group, &n);
	g_assert_cmpuint(n, ==, 0);

	gsurf_layout_group_add_view_id(group, 7);
	gsurf_layout_group_add_view_id(group, 0);
	gsurf_layout_group_add_view_id(group, 5);
	gsurf_layout_group_add_view_id(group, 7);
	ids = gsurf_layout_group_get_view_ids(group, &n);
	g_assert_cmpuint(n, ==, 2);
	g_assert_cmpuint(ids[0], ==, 7);
	g_assert_cmpuint(ids[1], ==, 5);

	/* g_boxed_copy() is a reference. */
	copy = g_boxed_copy(GSURF_TYPE_LAYOUT_GROUP, group);
	g_assert_true(copy == group);
	g_boxed_free(GSURF_TYPE_LAYOUT_GROUP, copy);
	g_assert_cmpstr(gsurf_layout_group_get_name(group), ==, "mail");
}

/* The wrappers reach the vfuncs; a layout without them answers "none";
 * installing or removing a layout, and notify_groups(), emit
 * layout-groups-changed. */
static void
test_groups_and_drops(void)
{
	static const gchar *const names[] = { "a", "b", NULL };
	g_autoptr(GsurfWindow) window = new_window(names);
	g_autoptr(TestLayout) layout = new_layout("L");
	g_autoptr(TestMinLayout) min = g_object_new(test_min_layout_get_type(), NULL);
	g_autoptr(GPtrArray) groups = NULL;
	GsurfWindowLayout *l = GSURF_WINDOW_LAYOUT(layout);
	GsurfWindowLayout *m = GSURF_WINDOW_LAYOUT(min);
	GsurfView *a = view_named(window, "a");
	GsurfView *b = view_named(window, "b");
	const guint64 *ids;
	guint changed = 0;
	guint n;

	setup();
	g_signal_connect(window, "layout-groups-changed", G_CALLBACK(count_signal), &changed);

	/* 1. Installing and removing a layout announce the groups. */
	g_assert_true(gsurf_window_set_layout(window, l));
	g_assert_cmpuint(changed, ==, 1);
	gsurf_window_layout_notify_groups(window);
	g_assert_cmpuint(changed, ==, 2);

	/* 2. Groups, in order, with the views their panes show. */
	groups = gsurf_window_layout_list_groups(l, window);
	g_assert_nonnull(groups);
	g_assert_cmpuint(groups->len, ==, 2);
	g_assert_cmpstr(gsurf_layout_group_get_name(g_ptr_array_index(groups, 0)), ==, "web");
	ids = gsurf_layout_group_get_view_ids(g_ptr_array_index(groups, 0), &n);
	g_assert_cmpuint(n, ==, 2);
	g_assert_cmpuint(ids[0], ==, gsurf_view_get_id(a));
	g_assert_cmpuint(ids[1], ==, gsurf_view_get_id(b));
	g_assert_true(gsurf_window_layout_select_group(l, window, 1));
	g_assert_cmpint(layout->selected, ==, 1);
	g_assert_false(gsurf_window_layout_select_group(l, window, 9));
	/* 0.4.3: rename_group, NULL meaning automatic naming. */
	g_assert_true(gsurf_window_layout_rename_group(l, window, 1, "docs"));
	g_assert_cmpstr(layout->renamed, ==, "1=docs");
	g_assert_true(gsurf_window_layout_rename_group(l, window, 0, NULL));
	g_assert_cmpstr(layout->renamed, ==, "0=(null)");
	g_assert_false(gsurf_window_layout_rename_group(l, window, 7, "x"));

	/* 3. Drops and drags. */
	g_assert_true(gsurf_window_layout_show_view_at(l, window, b, a, GSURF_LAYOUT_DROP_LEFT));
	g_assert_true(layout->drop_view == b && layout->drop_target == a);
	g_assert_cmpint(layout->drop_zone, ==, GSURF_LAYOUT_DROP_LEFT);
	g_assert_false(gsurf_window_layout_show_view_at(l, window, a, a, GSURF_LAYOUT_DROP_CENTER));
	gsurf_window_layout_begin_view_drag(l, window, a);
	g_assert_cmpint(layout->drags, ==, 1);
	gsurf_window_layout_end_view_drag(l, window);
	g_assert_cmpint(layout->drags, ==, 0);

	g_assert_true(gsurf_window_set_layout(window, NULL));
	g_assert_cmpuint(changed, ==, 3);

	/* 4. A layout without the extras: no groups, no drops; the drag
	 *    calls are harmless. */
	g_assert_null(gsurf_window_layout_list_groups(m, window));
	g_assert_false(gsurf_window_layout_select_group(m, window, 0));
	g_assert_false(gsurf_window_layout_rename_group(m, window, 0, "x"));
	g_assert_false(gsurf_window_layout_show_view_at(m, window, a, b, GSURF_LAYOUT_DROP_CENTER));
	gsurf_window_layout_begin_view_drag(m, window, a);
	gsurf_window_layout_end_view_drag(m, window);

	/* 5. Guards. */
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*GSURF_IS_VIEW*");
	g_assert_false(gsurf_window_layout_show_view_at(l, window, a, NULL, GSURF_LAYOUT_DROP_TOP));
	g_test_assert_expected_messages();
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*GSURF_IS_WINDOW*");
	gsurf_window_layout_notify_groups(NULL);
	g_test_assert_expected_messages();
}

/* --- views: opener id, new-view requests --- */

static gboolean
on_new_view_requested(GsurfView *view, const gchar *uri, gboolean background, gpointer data)
{
	GString *log = data;

	(void)view;
	g_string_append_printf(log, "%s:%s,", uri, background ? "bg" : "fg");
	return TRUE;
}

/* GsurfView:opener-id is a construct fallback: kept when no opener
 * object is given, overridden by a live opener; readable. */
static void
test_view_opener_id(void)
{
	g_autoptr(GsurfView) opener = new_view("o");
	g_autoptr(GsurfView) orphan = g_object_new(test_view_get_type(), "opener-id",
		G_GUINT64_CONSTANT(4242), NULL);
	g_autoptr(GsurfView) child = g_object_new(test_view_get_type(), "opener", opener,
		"opener-id", G_GUINT64_CONSTANT(4242), NULL);
	g_autoptr(GsurfView) plain = new_view("p");
	guint64 read = 0;

	g_assert_cmpuint(gsurf_view_get_opener_id(orphan), ==, 4242);
	g_assert_null(gsurf_view_get_opener(orphan));
	g_assert_cmpuint(gsurf_view_get_opener_id(child), ==, gsurf_view_get_id(opener));
	g_assert_true(gsurf_view_get_opener(child) == opener);
	g_assert_cmpuint(gsurf_view_get_opener_id(plain), ==, 0);
	g_object_get(orphan, "opener-id", &read, NULL);
	g_assert_cmpuint(read, ==, 4242);
}

/* gsurf_view_request_new_view(): the host's handler opens it; without a
 * handler (or an empty address) nothing happens. */
static void
test_view_new_view_requested(void)
{
	g_autoptr(GsurfView) view = new_view("v");
	g_autoptr(GString) log = g_string_new(NULL);
	gulong id;

	g_assert_false(gsurf_view_has_new_view_handler(view));
	g_assert_false(gsurf_view_request_new_view(view, "https://example.org/", TRUE));

	id = g_signal_connect(view, "new-view-requested", G_CALLBACK(on_new_view_requested), log);
	g_assert_true(gsurf_view_has_new_view_handler(view));
	g_assert_true(gsurf_view_request_new_view(view, "https://example.org/", TRUE));
	g_assert_true(gsurf_view_request_new_view(view, "https://example.net/", FALSE));
	g_assert_false(gsurf_view_request_new_view(view, "", TRUE));
	g_assert_false(gsurf_view_request_new_view(view, NULL, TRUE));
	g_assert_cmpstr(log->str, ==, "https://example.org/:bg,https://example.net/:fg,");
	g_signal_handler_disconnect(view, id);
	g_assert_false(gsurf_view_has_new_view_handler(view));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/window-layout/refused-without-set-content", test_refused_without_content);
	g_test_add_func("/window-layout/refused-in-kiosk", test_refused_in_kiosk);
	g_test_add_func("/window-layout/attach-existing", test_attach_existing);
	g_test_add_func("/window-layout/attach-empty", test_attach_empty);
	g_test_add_func("/window-layout/delegation", test_delegation);
	g_test_add_func("/window-layout/detach-restores", test_detach_restores);
	g_test_add_func("/window-layout/replace", test_replace);
	g_test_add_func("/window-layout/visibility", test_visibility);
	g_test_add_func("/window-layout/minimal-layout", test_minimal_layout);
	g_test_add_func("/window-layout/serialize-restore", test_serialize_restore);
	g_test_add_func("/window-layout/key-grab", test_key_grab);
	g_test_add_func("/window-layout/dispose-with-layout", test_dispose_with_layout);
	g_test_add_func("/window-layout/layout-lifetime", test_layout_lifetime);
	g_test_add_func("/window-layout/bad-arguments", test_bad_arguments);
	g_test_add_func("/window-layout/group-boxed", test_group_boxed);
	g_test_add_func("/window-layout/groups-and-drops", test_groups_and_drops);
	g_test_add_func("/view/opener-id", test_view_opener_id);
	g_test_add_func("/view/new-view-requested", test_view_new_view_requested);

	return g_test_run();
}
