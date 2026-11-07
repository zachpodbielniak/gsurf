/*
 * test-input-mode.c - the input mode in effect, headless
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The mode indicators (chromebar, the wmux status line) and the control
 * protocol show gsurf_module_manager_get_mode(). It must say what a bare
 * key does right now, so it is derived from three pieces of state, each
 * checked here without a toolkit or a web engine:
 *
 *  1. GsurfView:editing, a page field's focus: notified only on change,
 *     cleared when a load commits (a new document) and when the web
 *     process dies (the field is gone either way).
 *  2. GsurfWindow:typing: a chrome entry's focus, or the active view's
 *     editing state, following the active view as it changes (switch,
 *     removal of the active view, the last view leaving).
 *  3. The manager's mode in effect: the mode owner's mode, but NORMAL
 *     reads INSERT while the active window is typing; FOLLOW and INSERT
 *     are unchanged; it follows the active (newest) window and the
 *     application, and emits mode-changed only on a real change. Its key
 *     routing (gsurf_module_manager_is_typing()) lets a focused field
 *     take the keys only in NORMAL.
 */

#include <gsurf.h>

/* --- Headless view and window: bookkeeping only --- */

typedef struct { GsurfView parent; } ModeView;
typedef struct { GsurfViewClass parent; } ModeViewClass;
typedef struct { GsurfWindow parent; } ModeWindow;
typedef struct { GsurfWindowClass parent; } ModeWindowClass;

GType mode_view_get_type(void);
GType mode_window_get_type(void);
G_DEFINE_TYPE(ModeView, mode_view, GSURF_TYPE_VIEW)
G_DEFINE_TYPE(ModeWindow, mode_window, GSURF_TYPE_WINDOW)

static void mode_view_class_init(ModeViewClass *klass) { (void)klass; }
static void mode_view_init(ModeView *self) { (void)self; }
static void mode_window_class_init(ModeWindowClass *klass) { (void)klass; }
static void mode_window_init(ModeWindow *self) { (void)self; }

static GsurfView *
new_view(void)
{
	return g_object_new(mode_view_get_type(), NULL);
}

static GsurfWindow *
new_window(void)
{
	return g_object_new(mode_window_get_type(), NULL);
}

static void
count_notify(GObject *object, GParamSpec *pspec, gpointer user_data)
{
	(void)object;
	(void)pspec;
	(*(guint *)user_data)++;
}

/* Append each emitted mode's label, so the order is checked too. */
static void
log_mode(GsurfModuleManager *mgr, GsurfModePolicy mode, gpointer user_data)
{
	(void)mgr;
	g_string_append_printf((GString *)user_data, "%s,", gsurf_mode_policy_get_label(mode));
}

/* 1. The view's editing state: a property notified only on a change, and
 *    cleared by a committed load and by a web process crash (not by the
 *    other load events: the old document is still there). */
static void
test_view_editing(void)
{
	g_autoptr(GsurfView) view = new_view();
	gboolean editing = TRUE;
	guint notifies = 0;

	g_signal_connect(view, "notify::editing", G_CALLBACK(count_notify), &notifies);
	g_assert_false(gsurf_view_get_editing(view));
	gsurf_view_set_editing(view, FALSE);
	g_assert_cmpuint(notifies, ==, 0);
	gsurf_view_set_editing(view, TRUE);
	gsurf_view_set_editing(view, TRUE);
	g_assert_cmpuint(notifies, ==, 1);
	g_object_get(view, "editing", &editing, NULL);
	g_assert_true(editing);

	gsurf_view_emit_load_changed(view, GSURF_LOAD_STARTED);
	gsurf_view_emit_load_changed(view, GSURF_LOAD_REDIRECTED);
	g_assert_true(gsurf_view_get_editing(view));
	gsurf_view_emit_load_changed(view, GSURF_LOAD_COMMITTED);
	g_assert_false(gsurf_view_get_editing(view));
	g_assert_cmpuint(notifies, ==, 2);
	/* The new document's autofocused field reports after the commit. */
	gsurf_view_set_editing(view, TRUE);
	gsurf_view_emit_load_changed(view, GSURF_LOAD_FINISHED);
	g_assert_true(gsurf_view_get_editing(view));

	gsurf_view_emit_web_process_terminated(view);
	g_assert_false(gsurf_view_get_editing(view));
	g_assert_cmpuint(notifies, ==, 4);
}

/* 2. The window is typing while a chrome entry or the active view's page
 *    field has the keys; a background view's field does not count; the
 *    state follows switches and removals, and notifies only on change. */
static void
test_window_typing(void)
{
	g_autoptr(GsurfWindow) window = new_window();
	g_autoptr(GsurfView) a = new_view();
	g_autoptr(GsurfView) b = new_view();
	gboolean typing = TRUE;
	guint notifies = 0;
	guint entry_notifies = 0;

	g_signal_connect(window, "notify::typing", G_CALLBACK(count_notify), &notifies);
	g_signal_connect(window, "notify::entry-focus", G_CALLBACK(count_notify),
		&entry_notifies);
	g_assert_false(gsurf_window_get_typing(window));

	/* A chrome entry. */
	gsurf_window_set_entry_focus(window, TRUE);
	gsurf_window_set_entry_focus(window, TRUE);
	g_assert_true(gsurf_window_get_entry_focus(window));
	g_assert_true(gsurf_window_get_typing(window));
	g_assert_cmpuint(entry_notifies, ==, 1);
	g_assert_cmpuint(notifies, ==, 1);
	gsurf_window_set_entry_focus(window, FALSE);
	g_assert_false(gsurf_window_get_typing(window));
	g_assert_cmpuint(notifies, ==, 2);

	/* The active view's field; a background view's field is ignored. */
	gsurf_window_add_view(window, a);
	gsurf_window_add_view(window, b);
	g_assert_true(gsurf_window_get_active_view(window) == a);
	gsurf_view_set_editing(b, TRUE);
	g_assert_false(gsurf_window_get_typing(window));
	gsurf_view_set_editing(a, TRUE);
	g_assert_true(gsurf_window_get_typing(window));
	g_object_get(window, "typing", &typing, NULL);
	g_assert_true(typing);
	g_assert_cmpuint(notifies, ==, 3);

	/* Both views editing: a switch keeps it, silently. */
	gsurf_window_set_active_view(window, b);
	g_assert_true(gsurf_window_get_typing(window));
	g_assert_cmpuint(notifies, ==, 3);
	/* Only the old view stops editing: no change. */
	gsurf_view_set_editing(a, FALSE);
	g_assert_true(gsurf_window_get_typing(window));
	gsurf_view_set_editing(b, FALSE);
	g_assert_false(gsurf_window_get_typing(window));
	/* Back to a view that is not editing, then it starts. */
	gsurf_window_set_active_view(window, a);
	gsurf_view_set_editing(b, TRUE);
	g_assert_false(gsurf_window_get_typing(window));
	gsurf_window_set_active_view(window, b);
	g_assert_true(gsurf_window_get_typing(window));

	/* The active view leaves: its neighbour (not editing) is active. */
	gsurf_window_remove_view(window, b);
	g_assert_true(gsurf_window_get_active_view(window) == a);
	g_assert_false(gsurf_window_get_typing(window));
	/* The removed view is no longer followed. */
	gsurf_view_set_editing(b, FALSE);
	gsurf_view_set_editing(b, TRUE);
	g_assert_false(gsurf_window_get_typing(window));
	/* The last view leaves while editing: nothing is active, not typing. */
	gsurf_view_set_editing(a, TRUE);
	g_assert_true(gsurf_window_get_typing(window));
	gsurf_window_remove_view(window, a);
	g_assert_null(gsurf_window_get_active_view(window));
	g_assert_false(gsurf_window_get_typing(window));
	/* An entry still counts with no view at all. */
	gsurf_window_set_entry_focus(window, TRUE);
	g_assert_true(gsurf_window_get_typing(window));
}

/* A view outlives its window: the window stops following it. */
static void
test_window_dispose(void)
{
	g_autoptr(GsurfView) view = new_view();
	GsurfWindow *window = new_window();

	gsurf_window_add_view(window, view);
	g_object_unref(window);
	/* No handler of the dead window may run. */
	gsurf_view_set_editing(view, TRUE);
	gsurf_view_set_editing(view, FALSE);
}

/* 3. The mode in effect: NORMAL reads INSERT while the active window
 *    types; the owner's other modes are unchanged; a change of base
 *    mode, typing, active view or active window updates it; mode-changed
 *    fires once per real change. */
static void
test_effective_mode(void)
{
	g_autoptr(GsurfModuleManager) mgr = gsurf_module_manager_new();
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfApplication) app = gsurf_application_new(config);
	g_autoptr(GsurfView) a = new_view();
	g_autoptr(GsurfView) b = new_view();
	g_autoptr(GString) log = g_string_new(NULL);
	GsurfWindow *first = new_window();
	GsurfWindow *second = new_window();
	GsurfModePolicy read = GSURF_MODE_FIND;
	guint notifies = 0;

	gsurf_module_manager_set_application(mgr, app);
	g_signal_connect(mgr, "mode-changed", G_CALLBACK(log_mode), log);
	g_signal_connect(mgr, "notify::mode", G_CALLBACK(count_notify), &notifies);

	/* A window added later is followed. */
	gsurf_window_add_view(first, a);
	gsurf_window_add_view(first, b);
	gsurf_application_add_window(app, first);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);

	/* A page field in the active view. */
	gsurf_view_set_editing(a, TRUE);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_INSERT);
	g_object_get(mgr, "mode", &read, NULL);
	g_assert_cmpint(read, ==, GSURF_MODE_INSERT);
	/* Another tab without one, and back. */
	gsurf_window_set_active_view(first, b);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
	gsurf_window_set_active_view(first, a);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_INSERT);
	/* The field blurs. */
	gsurf_view_set_editing(a, FALSE);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
	/* A chrome entry (the address bar). */
	gsurf_window_set_entry_focus(first, TRUE);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_INSERT);
	gsurf_window_set_entry_focus(first, FALSE);
	g_assert_cmpstr(log->str, ==,
		"INSERT,NORMAL,INSERT,NORMAL,INSERT,NORMAL,");
	g_assert_cmpuint(notifies, ==, 6);
	g_string_truncate(log, 0);

	/* The owner's modes: INSERT stays INSERT whatever the focus; FOLLOW
	 * (hints up) stays FOLLOW over a field; NORMAL again reads INSERT. */
	gsurf_module_manager_set_mode(mgr, GSURF_MODE_INSERT);
	gsurf_view_set_editing(a, TRUE);
	gsurf_view_set_editing(a, FALSE);
	gsurf_module_manager_set_mode(mgr, GSURF_MODE_FOLLOW);
	gsurf_view_set_editing(a, TRUE);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_FOLLOW);
	gsurf_module_manager_set_mode(mgr, GSURF_MODE_NORMAL);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_INSERT);
	/* NORMAL published while typing: no change, no signal. */
	gsurf_module_manager_set_mode(mgr, GSURF_MODE_NORMAL);
	g_assert_cmpstr(log->str, ==, "INSERT,FOLLOW,INSERT,");
	g_string_truncate(log, 0);

	/* The newest window is the active one: it decides. */
	gsurf_application_add_window(app, second);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
	/* The first window's state no longer counts... */
	gsurf_view_set_editing(a, FALSE);
	gsurf_view_set_editing(a, TRUE);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
	gsurf_window_set_entry_focus(second, TRUE);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_INSERT);
	gsurf_window_set_entry_focus(second, FALSE);
	/* ...until the second one closes. */
	gsurf_application_remove_window(app, second);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_INSERT);
	g_assert_cmpstr(log->str, ==, "NORMAL,INSERT,NORMAL,INSERT,");
	/* A removed window is not followed any more. */
	gsurf_window_set_entry_focus(second, TRUE);
	gsurf_view_set_editing(a, FALSE);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
	gsurf_view_set_editing(a, TRUE);

	/* Without an application, only the owner's mode counts. */
	gsurf_module_manager_set_application(mgr, NULL);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
	gsurf_view_set_editing(a, FALSE);
	gsurf_view_set_editing(a, TRUE);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
	/* Set again: the windows it already has are followed. */
	gsurf_module_manager_set_application(mgr, app);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_INSERT);
	gsurf_view_set_editing(a, FALSE);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);

	gsurf_module_manager_set_application(mgr, NULL);
	g_object_unref(second);
	gsurf_window_remove_view(first, a);
	gsurf_window_remove_view(first, b);
	/* The application still holds the first window; it goes with it. */
	g_object_unref(first);
}

/* The manager may outlive the application (and the reverse): the weak
 * application pointer and the window handlers must not dangle. */
static void
test_application_dies(void)
{
	g_autoptr(GsurfModuleManager) mgr = gsurf_module_manager_new();
	g_autoptr(GsurfConfig) config = gsurf_config_new();
	g_autoptr(GsurfView) view = new_view();
	GsurfApplication *app = gsurf_application_new(config);
	GsurfWindow *window = new_window();

	gsurf_module_manager_set_application(mgr, app);
	gsurf_window_add_view(window, view);
	gsurf_application_add_window(app, window);
	gsurf_view_set_editing(view, TRUE);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_INSERT);
	/* The window survives the application (a ref of ours). */
	g_object_unref(app);
	g_assert_null(gsurf_module_manager_get_application(mgr));
	gsurf_view_set_editing(view, FALSE);
	g_assert_cmpint(gsurf_module_manager_get_mode(mgr), ==, GSURF_MODE_NORMAL);
	gsurf_window_remove_view(window, view);
	g_object_unref(window);
}

/* Key routing: a field takes bare keys only in NORMAL; INSERT (the
 * passthrough flag or the mode) always passes them; FOLLOW keeps them. */
static void
test_is_typing(void)
{
	g_autoptr(GsurfModuleManager) mgr = gsurf_module_manager_new();
	g_autoptr(GsurfView) view = new_view();

	g_assert_false(gsurf_module_manager_is_typing(mgr, view));
	g_assert_false(gsurf_module_manager_is_typing(mgr, NULL));
	gsurf_view_set_editing(view, TRUE);
	g_assert_true(gsurf_module_manager_is_typing(mgr, view));
	g_assert_false(gsurf_module_manager_is_typing(mgr, NULL));

	gsurf_module_manager_set_mode(mgr, GSURF_MODE_FOLLOW);
	g_assert_false(gsurf_module_manager_is_typing(mgr, view));
	gsurf_module_manager_set_mode(mgr, GSURF_MODE_INSERT);
	g_assert_true(gsurf_module_manager_is_typing(mgr, view));
	gsurf_view_set_editing(view, FALSE);
	g_assert_true(gsurf_module_manager_is_typing(mgr, view));
	g_assert_true(gsurf_module_manager_is_typing(mgr, NULL));
	gsurf_module_manager_set_mode(mgr, GSURF_MODE_NORMAL);
	g_assert_false(gsurf_module_manager_is_typing(mgr, view));
	gsurf_module_manager_set_input_passthrough(mgr, TRUE);
	g_assert_true(gsurf_module_manager_is_typing(mgr, view));
	gsurf_module_manager_set_input_passthrough(mgr, FALSE);

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*GSURF_IS_MODULE_MANAGER*");
	g_assert_false(gsurf_module_manager_is_typing(NULL, view));
	g_test_assert_expected_messages();
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/gsurf/input-mode/view-editing", test_view_editing);
	g_test_add_func("/gsurf/input-mode/window-typing", test_window_typing);
	g_test_add_func("/gsurf/input-mode/window-dispose", test_window_dispose);
	g_test_add_func("/gsurf/input-mode/effective-mode", test_effective_mode);
	g_test_add_func("/gsurf/input-mode/application-dies", test_application_dies);
	g_test_add_func("/gsurf/input-mode/is-typing", test_is_typing);

	return g_test_run();
}
