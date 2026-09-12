/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gsurf.h>

#ifdef GSURF_BACKEND_GTK3
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

/* Native widgets and their shared context can outlive the library wrapper. */
static void
test_retained_native_objects(void)
{
	g_autoptr(GsurfView) view = NULL;
	g_autoptr(WebKitWebView) native = NULL;
	g_autoptr(WebKitUserContentManager) content = NULL;
	WebKitWebContext *context;

	if (g_getenv("GSURF_TEST_GUI") == NULL) {
		g_test_skip("set GSURF_TEST_GUI=1 with a display to test native lifetimes");
		return;
	}
	g_assert_true(gtk_init_check(NULL, NULL));
	view = gsurf_view_new();
	native = g_object_ref(gsurf_view_get_native_widget(view));
	content = g_object_ref(webkit_web_view_get_user_content_manager(native));
	context = webkit_web_view_get_context(native);
	g_assert_cmpuint(g_signal_handler_find(content, G_SIGNAL_MATCH_DATA,
		0, 0, NULL, NULL, view), !=, 0);

	/* Dispose while holding the wrapper to inspect callback data safely. */
	g_object_run_dispose(G_OBJECT(view));
	g_assert_cmpuint(g_signal_handler_find(content, G_SIGNAL_MATCH_DATA,
		0, 0, NULL, NULL, view), ==, 0);
	g_assert_cmpuint(g_signal_handler_find(context, G_SIGNAL_MATCH_DATA,
		0, 0, NULL, NULL, view), ==, 0);
	g_assert_cmpuint(g_signal_handler_find(native, G_SIGNAL_MATCH_DATA,
		0, 0, NULL, NULL, view), ==, 0);
	g_object_run_dispose(G_OBJECT(view));
}
#endif

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#ifdef GSURF_BACKEND_GTK3
	g_test_add_func("/gsurf/backend/retained-native-objects", test_retained_native_objects);
#endif
	return g_test_run();
}
