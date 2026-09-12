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
static guint chrome_calls;
static gboolean last_chrome_top;

static const char *SEARCH_YAML =
	"modules:\n"
	"  search_engines:\n"
	"    enabled: true\n"
	"    default: ddg\n"
	"    default_is_search: true\n"
	"    engines:\n"
	"      ddg: { prefix: \"d \", url: \"https://duckduckgo.com/?q=%s\" }\n"
	"      google: { prefix: \"g \", url: \"https://www.google.com/search?q=%s\" }\n";

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
add_chrome_widget(GsurfWindow *window, gpointer widget, gboolean top)
{
	(void) window;
	(void) widget;
	chrome_calls++;
	last_chrome_top = top;
}

static void
test_kiosk_window_class_init(TestKioskWindowClass *klass)
{
	GSURF_WINDOW_CLASS(klass)->add_chrome_widget = add_chrome_widget;
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
	chrome_calls = 0;
	gsurf_window_add_top_widget(window, GINT_TO_POINTER(1));
	gsurf_window_add_bottom_widget(window, GINT_TO_POINTER(2));
	g_assert_cmpuint(chrome_calls, ==, 0);

	/* Ordinary sessions must retain tabs, popups, inspector and chrome. */
	config->kiosk = FALSE;
	gsurf_window_add_view(window, second);
	g_assert_cmpuint(gsurf_window_get_n_views(window), ==, 2);
	g_assert_null(gsurf_view_emit_create_view(first, "https://example.org"));
	gsurf_view_show_inspector(first);
	g_assert_cmpuint(popup_calls, ==, 1);
	g_assert_cmpuint(inspector_calls, ==, 1);
	gsurf_window_add_top_widget(window, GINT_TO_POINTER(1));
	g_assert_cmpuint(chrome_calls, ==, 1);
	g_assert_true(last_chrome_top);
	gsurf_window_add_bottom_widget(window, GINT_TO_POINTER(2));
	g_assert_cmpuint(chrome_calls, ==, 2);
	g_assert_false(last_chrome_top);
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

static char *
module_so_path(const char *name)
{
	char *exe = g_file_read_link("/proc/self/exe", NULL);
	char *dir;
	char *base;
	char *path;

	g_assert_nonnull(exe);
	dir = g_path_get_dirname(exe);
	base = g_strconcat(name, ".so", NULL);
	path = g_build_filename(dir, "modules", base, NULL);
	g_free(exe);
	g_free(dir);
	g_free(base);
	return path;
}

static void
test_modules_still_dispatch(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autofree char *so = module_so_path("search_engines");
	g_autofree char *out = NULL;
	GsurfModule *mod;

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("search_engines.so not built");
		return;
	}

	/* Kiosk must not disable hook dispatch; chrome hiding is separate. */
	g_assert_true(gsurf_config_load_from_data(config, SEARCH_YAML, -1, &error));
	g_assert_no_error(error);
	config->kiosk = TRUE;
	gsurf_config_set_default(config);
	g_assert_true(gsurf_kiosk_is_enabled());

	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	mod = gsurf_module_manager_load_module(mgr, so, &error);
	g_assert_no_error(error);
	g_assert_nonnull(mod);
	g_assert_cmpstr(gsurf_module_get_name(mod), ==, "search_engines");
	gsurf_module_manager_activate_all(mgr);

	out = gsurf_module_manager_dispatch_rewrite_uri(mgr, "g hello world");
	g_assert_nonnull(out);
	g_assert_true(g_str_has_prefix(out, "https://www.google.com/search?q="));
	g_assert_true(gsurf_kiosk_is_enabled());
	gsurf_config_set_default(NULL);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/kiosk/restrictions", test_restrictions);
	g_test_add_func("/kiosk/actions", test_actions);
	g_test_add_func("/kiosk/modules-still-dispatch", test_modules_still_dispatch);
	return g_test_run();
}
