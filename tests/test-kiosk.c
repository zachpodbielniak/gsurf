/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gsurf.h>

/* Concrete test backends exercise base-class guards without a display. */
typedef struct { GsurfView parent; } TestKioskView;
typedef struct { GsurfViewClass parent; } TestKioskViewClass;
typedef struct { GsurfWindow parent; } TestKioskWindow;
typedef struct { GsurfWindowClass parent; } TestKioskWindowClass;

G_DEFINE_TYPE(TestKioskView, test_kiosk_view, GSURF_TYPE_VIEW)
G_DEFINE_TYPE(TestKioskWindow, test_kiosk_window, GSURF_TYPE_WINDOW)

static guint inspector_calls;
static guint popup_calls;

/* Count real dispatch to prove restricted operations never reach the host. */
static void
show_inspector(GsurfView *view)
{
	(void) view;
	inspector_calls++;
}

static void
test_kiosk_view_class_init(TestKioskViewClass *klass)
{
	GSURF_VIEW_CLASS(klass)->show_inspector = show_inspector;
}

static void
test_kiosk_view_init(TestKioskView *self)
{
	(void) self;
}

static void
test_kiosk_window_class_init(TestKioskWindowClass *klass)
{
	(void) klass;
}

static void
test_kiosk_window_init(TestKioskWindow *self)
{
	(void) self;
}

static GsurfView *
create_view(GsurfView *view, const gchar *uri, gpointer data)
{
	(void) view;
	(void) uri;
	(void) data;
	popup_calls++;
	return NULL;
}

static void
test_restrictions(void)
{
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfWindow) window = g_object_new(test_kiosk_window_get_type(), NULL);
	g_autoptr(GsurfView) first = g_object_new(test_kiosk_view_get_type(), NULL);
	g_autoptr(GsurfView) second = g_object_new(test_kiosk_view_get_type(), NULL);

	gsurf_config_set_default(config);
	g_assert_false(gsurf_kiosk_is_enabled());
	config->kiosk = TRUE;
	g_assert_true(gsurf_kiosk_is_enabled());
	gsurf_window_add_view(window, first);
	gsurf_window_add_view(window, second);
	g_assert_cmpuint(gsurf_window_get_n_views(window), ==, 1);
	g_assert_true(gsurf_window_get_active_view(window) == first);
	g_signal_connect(first, "create-view", G_CALLBACK(create_view), NULL);
	g_assert_null(gsurf_view_emit_create_view(first, "https://example.org"));
	gsurf_view_show_inspector(first);
	g_assert_cmpuint(popup_calls, ==, 0);
	g_assert_cmpuint(inspector_calls, ==, 0);

	/* Ordinary sessions must retain tabs, popups and the inspector. */
	config->kiosk = FALSE;
	gsurf_window_add_view(window, second);
	g_assert_cmpuint(gsurf_window_get_n_views(window), ==, 2);
	g_assert_null(gsurf_view_emit_create_view(first, "https://example.org"));
	gsurf_view_show_inspector(first);
	g_assert_cmpuint(popup_calls, ==, 1);
	g_assert_cmpuint(inspector_calls, ==, 1);
	gsurf_config_set_default(NULL);
}

static void
test_actions(void)
{
	/* Preserve navigation while excluding URL entry regardless of binding. */
	g_assert_false(gsurf_kiosk_allows_action(GSURF_ACTION_OPEN_PROMPT));
	g_assert_false(gsurf_kiosk_allows_action(GSURF_ACTION_PASTE_URL));
	g_assert_false(gsurf_kiosk_allows_action(GSURF_ACTION_HOME));
	g_assert_false(gsurf_kiosk_allows_action(GSURF_ACTION_OPEN_NEW_VIEW));
	g_assert_false(gsurf_kiosk_allows_action(GSURF_ACTION_TAB_NEW));
	g_assert_false(gsurf_kiosk_allows_action(GSURF_ACTION_TAB_REOPEN));
	g_assert_false(gsurf_kiosk_allows_action(GSURF_ACTION_FOLLOW_HINTS_NEW_VIEW));
	g_assert_true(gsurf_kiosk_allows_action(GSURF_ACTION_BACK));
	g_assert_true(gsurf_kiosk_allows_action(GSURF_ACTION_RELOAD));
	g_assert_true(gsurf_kiosk_allows_action(GSURF_ACTION_QUIT));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/kiosk/restrictions", test_restrictions);
	g_test_add_func("/kiosk/actions", test_actions);
	return g_test_run();
}
