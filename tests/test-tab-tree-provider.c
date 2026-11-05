/*
 * test-tab-tree-provider.c - The GsurfTabTreeProvider interface
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Headless: a fake provider records every call and answers from a fixed
 * table, so the wrappers are checked to pass their arguments through
 * unchanged; a provider without vfuncs gets the documented defaults
 * ("root", "expanded", "cannot"); bad arguments are refused before any
 * vfunc runs. The tabs module's implementation is covered by
 * test-mcp-webkit (make test-gui) through the MCP tree tools.
 */

#include <gsurf.h>
#include <string.h>

/* --- headless view and window --- */

typedef struct { GsurfView parent; } TestView;
typedef struct { GsurfViewClass parent; } TestViewClass;

GType test_view_get_type(void);
G_DEFINE_TYPE(TestView, test_view, GSURF_TYPE_VIEW)

static void test_view_class_init(TestViewClass *klass) { (void)klass; }
static void test_view_init(TestView *self) { (void)self; }

typedef struct { GsurfWindow parent; } TestWindow;
typedef struct { GsurfWindowClass parent; } TestWindowClass;

GType test_window_get_type(void);
G_DEFINE_TYPE(TestWindow, test_window, GSURF_TYPE_WINDOW)

static void test_window_class_init(TestWindowClass *klass) { (void)klass; }
static void test_window_init(TestWindow *self) { (void)self; }

/* --- a recording provider --- */

typedef struct {
	GObject  parent;
	GString *log;
	gboolean collapsed;
} TestProvider;
typedef struct { GObjectClass parent; } TestProviderClass;

static void test_provider_iface_init(GsurfTabTreeProviderInterface *iface);
GType test_provider_get_type(void);
G_DEFINE_TYPE_WITH_CODE(TestProvider, test_provider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_TAB_TREE_PROVIDER, test_provider_iface_init))

static guint64
p_get_parent(GsurfTabTreeProvider *self, GsurfWindow *window, GsurfView *view)
{
	(void)window;
	g_string_append_printf(((TestProvider *)self)->log, "parent:%" G_GUINT64_FORMAT ",",
		gsurf_view_get_id(view));
	return 7;
}

static gboolean
p_get_collapsed(GsurfTabTreeProvider *self, GsurfWindow *window, GsurfView *view)
{
	(void)window;
	(void)view;
	return ((TestProvider *)self)->collapsed;
}

static gboolean
p_set_collapsed(GsurfTabTreeProvider *self, GsurfWindow *window, GsurfView *view,
                gboolean collapsed)
{
	(void)window;
	(void)view;
	/* The wrapper normalizes the flag. */
	g_assert_true(collapsed == TRUE || collapsed == FALSE);
	((TestProvider *)self)->collapsed = collapsed;
	return TRUE;
}

static gboolean
p_move_view(GsurfTabTreeProvider *self, GsurfWindow *window, GsurfView *view,
            GsurfView *target, GsurfTabTreePosition position)
{
	GEnumClass *klass = g_type_class_ref(GSURF_TYPE_TAB_TREE_POSITION);

	(void)window;
	g_string_append_printf(((TestProvider *)self)->log, "move:%" G_GUINT64_FORMAT ":%s:%s,",
		gsurf_view_get_id(view), target != NULL ? "target" : "end",
		g_enum_get_value(klass, position)->value_nick);
	g_type_class_unref(klass);
	return target != view;
}

static void
test_provider_iface_init(GsurfTabTreeProviderInterface *iface)
{
	iface->get_parent = p_get_parent;
	iface->get_collapsed = p_get_collapsed;
	iface->set_collapsed = p_set_collapsed;
	iface->move_view = p_move_view;
}

static void
test_provider_finalize(GObject *object)
{
	g_string_free(((TestProvider *)object)->log, TRUE);
	G_OBJECT_CLASS(test_provider_parent_class)->finalize(object);
}

static void
test_provider_class_init(TestProviderClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = test_provider_finalize;
}

static void
test_provider_init(TestProvider *self)
{
	self->log = g_string_new(NULL);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(TestProvider, g_object_unref)

/* --- a provider implementing nothing --- */

typedef struct { GObject parent; } TestMinProvider;
typedef struct { GObjectClass parent; } TestMinProviderClass;

static void test_min_provider_iface_init(GsurfTabTreeProviderInterface *iface) { (void)iface; }
GType test_min_provider_get_type(void);
G_DEFINE_TYPE_WITH_CODE(TestMinProvider, test_min_provider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_TAB_TREE_PROVIDER, test_min_provider_iface_init))

static void test_min_provider_class_init(TestMinProviderClass *klass) { (void)klass; }
static void test_min_provider_init(TestMinProvider *self) { (void)self; }

G_DEFINE_AUTOPTR_CLEANUP_FUNC(TestMinProvider, g_object_unref)

/* --- tests --- */

/* Every wrapper reaches its vfunc with its arguments. */
static void
test_delegation(void)
{
	g_autoptr(TestProvider) provider = g_object_new(test_provider_get_type(), NULL);
	g_autoptr(GsurfWindow) window = g_object_new(test_window_get_type(), NULL);
	g_autoptr(GsurfView) a = g_object_new(test_view_get_type(), NULL);
	g_autoptr(GsurfView) b = g_object_new(test_view_get_type(), NULL);
	GsurfTabTreeProvider *p = GSURF_TAB_TREE_PROVIDER(provider);
	g_autofree gchar *want = NULL;

	g_assert_true(G_TYPE_IS_INTERFACE(GSURF_TYPE_TAB_TREE_PROVIDER));
	g_assert_cmpuint(gsurf_tab_tree_provider_get_parent(p, window, a), ==, 7);
	g_assert_false(gsurf_tab_tree_provider_get_collapsed(p, window, a));
	g_assert_true(gsurf_tab_tree_provider_set_collapsed(p, window, a, 5));
	g_assert_true(gsurf_tab_tree_provider_get_collapsed(p, window, a));
	g_assert_true(gsurf_tab_tree_provider_move_view(p, window, a, b,
		GSURF_TAB_TREE_POSITION_AFTER));
	g_assert_true(gsurf_tab_tree_provider_move_view(p, window, b, NULL,
		GSURF_TAB_TREE_POSITION_CHILD));
	g_assert_false(gsurf_tab_tree_provider_move_view(p, window, a, a,
		GSURF_TAB_TREE_POSITION_BEFORE));

	want = g_strdup_printf("parent:%" G_GUINT64_FORMAT ",move:%" G_GUINT64_FORMAT
		":target:after,move:%" G_GUINT64_FORMAT ":end:child,move:%" G_GUINT64_FORMAT
		":target:before,", gsurf_view_get_id(a), gsurf_view_get_id(a),
		gsurf_view_get_id(b), gsurf_view_get_id(a));
	g_assert_cmpstr(provider->log->str, ==, want);
}

/* Without vfuncs: root, expanded, cannot collapse, cannot move. */
static void
test_defaults(void)
{
	g_autoptr(TestMinProvider) provider = g_object_new(test_min_provider_get_type(), NULL);
	g_autoptr(GsurfWindow) window = g_object_new(test_window_get_type(), NULL);
	g_autoptr(GsurfView) a = g_object_new(test_view_get_type(), NULL);
	GsurfTabTreeProvider *p = GSURF_TAB_TREE_PROVIDER(provider);

	g_assert_cmpuint(gsurf_tab_tree_provider_get_parent(p, window, a), ==, 0);
	g_assert_false(gsurf_tab_tree_provider_get_collapsed(p, window, a));
	g_assert_false(gsurf_tab_tree_provider_set_collapsed(p, window, a, TRUE));
	g_assert_false(gsurf_tab_tree_provider_move_view(p, window, a, NULL,
		GSURF_TAB_TREE_POSITION_CHILD));
}

/* Bad arguments are refused before the vfunc runs. */
static void
test_guards(void)
{
	g_autoptr(TestProvider) provider = g_object_new(test_provider_get_type(), NULL);
	g_autoptr(GsurfWindow) window = g_object_new(test_window_get_type(), NULL);
	g_autoptr(GsurfView) a = g_object_new(test_view_get_type(), NULL);
	GsurfTabTreeProvider *p = GSURF_TAB_TREE_PROVIDER(provider);
	g_autoptr(GObject) plain = g_object_new(G_TYPE_OBJECT, NULL);

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*GSURF_IS_VIEW*");
	g_assert_cmpuint(gsurf_tab_tree_provider_get_parent(p, window, NULL), ==, 0);
	g_test_assert_expected_messages();
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*GSURF_IS_WINDOW*");
	g_assert_false(gsurf_tab_tree_provider_set_collapsed(p, NULL, a, TRUE));
	g_test_assert_expected_messages();
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*GSURF_IS_VIEW(target)*");
	g_assert_false(gsurf_tab_tree_provider_move_view(p, window, a, (GsurfView *)plain,
		GSURF_TAB_TREE_POSITION_CHILD));
	g_test_assert_expected_messages();
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*GSURF_IS_TAB_TREE_PROVIDER*");
	g_assert_false(gsurf_tab_tree_provider_get_collapsed((GsurfTabTreeProvider *)plain,
		window, a));
	g_test_assert_expected_messages();
	/* Nothing reached the recording provider. */
	g_assert_cmpstr(provider->log->str, ==, "");
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/tab-tree-provider/delegation", test_delegation);
	g_test_add_func("/tab-tree-provider/defaults", test_defaults);
	g_test_add_func("/tab-tree-provider/guards", test_guards);

	return g_test_run();
}
