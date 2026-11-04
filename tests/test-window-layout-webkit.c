/*
 * test-window-layout-webkit.c - Real backend windows: view routing, layouts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Runs on a real GsurfGtk3Window / GsurfGtk4Window with real WebKit
 * views and checks what the headless suites (test-window-layout,
 * test-window-lifecycle) cannot:
 *
 *   - mouse buttons pressed on a view widget reach
 *     GsurfWindow::button-press on both toolkits, once per press;
 *   - a GsurfWindowLayout (a tiny one here: two views side by side in a
 *     GtkBox) really replaces the view stack: both views allocated and
 *     drawn, focus into the second makes it active, a view added while
 *     the layout is set is still wired, and removing the layout puts
 *     every view back into the stack with the active one shown;
 *   - the key grab sends a bare key to GsurfWindow::key-press while a
 *     chrome entry has focus (instead of typing into it), and while a
 *     page field has focus.
 *
 * Runs only with GSURF_TEST_GUI=1 (make test-gui).
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <string.h>

#include "test-util.h"
#include "webkit-test-util.h"

/* --- helpers --- */

typedef struct {
	guint count;
	guint button;
	gboolean consume;
} ButtonLog;

static gboolean
on_button_press(GsurfWindow *window, guint button, guint state, gpointer user_data)
{
	ButtonLog *log = user_data;

	(void)window;
	(void)state;
	log->count++;
	log->button = button;
	return log->consume;
}

#ifdef GSURF_BACKEND_GTK3
static gboolean
stop_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	(void)widget;
	(void)event;
	(void)user_data;
	return TRUE;
}
#endif

/* Press mouse @button on @widget the way the backend sees it: GTK3 runs
 * the widget's button-press-event, GTK4 the window's click gesture on
 * the widget (GTK4 cannot synthesize a pointer event, so the gesture's
 * "pressed" is emitted directly and reports button 0). */
static void
press_button(GtkWidget *widget, guint button)
{
#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(widget);
	GtkEventController *found = NULL;
	guint i;

	(void)button;
	for (i = 0; i < g_list_model_get_n_items(controllers); i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (g_strcmp0(gtk_event_controller_get_name(c), "gsurf-view-click") != 0)
			continue;
		/* One gesture per window: a second would emit every press twice. */
		g_assert_null(found);
		found = c; /* still owned by the widget */
	}
	g_assert_nonnull(found);
	g_assert_true(GTK_IS_GESTURE_CLICK(found));
	g_assert_cmpint(gtk_event_controller_get_propagation_phase(found), ==,
		GTK_PHASE_CAPTURE);
	g_assert_cmpuint(gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(found)), ==, 0);
	g_signal_emit_by_name(found, "pressed", 1, 10.0, 10.0);
#else
	GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS);
	gboolean handled = FALSE;
	gulong id;

	event->button.button = button;
	/* The window's handler runs first (connected earlier); this one then
	 * keeps a device-less synthetic event away from WebKit's own. */
	id = g_signal_connect(widget, "button-press-event", G_CALLBACK(stop_event), NULL);
	g_signal_emit_by_name(widget, "button-press-event", event, &handled);
	g_signal_handler_disconnect(widget, id);
	gdk_event_free(event);
#endif
}

/* --- a two-pane layout --- */

/* Shows up to two views side by side in a homogeneous GtkBox; any more
 * stay unparented background views. Activating a background view swaps
 * it in for the second pane. */
typedef struct {
	GObject    parent;
	GtkWidget *box;      /* owned (ref_sink) */
	guint      attached; /* attach - detach count */
} PairLayout;
typedef struct { GObjectClass parent; } PairLayoutClass;

static void pair_layout_iface_init(GsurfWindowLayoutInterface *iface);
GType pair_layout_get_type(void);
G_DEFINE_TYPE_WITH_CODE(PairLayout, pair_layout, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_WINDOW_LAYOUT, pair_layout_iface_init))
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PairLayout, g_object_unref)

static guint
box_n_children(GtkWidget *box)
{
	guint n = 0;
#ifdef GSURF_BACKEND_GTK4
	GtkWidget *child;

	for (child = gtk_widget_get_first_child(box); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		n++;
#else
	g_autoptr(GList) children = gtk_container_get_children(GTK_CONTAINER(box));

	n = g_list_length(children);
#endif
	return n;
}

static void
box_add(GtkWidget *box, GtkWidget *widget)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_widget_set_hexpand(widget, TRUE);
	gtk_widget_set_vexpand(widget, TRUE);
	gtk_box_append(GTK_BOX(box), widget);
#else
	gtk_box_pack_start(GTK_BOX(box), widget, TRUE, TRUE, 0);
	gtk_widget_show(widget);
#endif
}

static void
box_drop(GtkWidget *box, GtkWidget *widget)
{
	if (widget == NULL || gtk_widget_get_parent(widget) != box)
		return;
#ifdef GSURF_BACKEND_GTK4
	gtk_box_remove(GTK_BOX(box), widget);
#else
	gtk_container_remove(GTK_CONTAINER(box), widget);
#endif
}

static gpointer
pair_get_root(GsurfWindowLayout *layout)
{
	return ((PairLayout *)layout)->box;
}

static void
pair_attach(GsurfWindowLayout *layout, GsurfWindow *window)
{
	(void)window;
	((PairLayout *)layout)->attached++;
}

static void
pair_detach(GsurfWindowLayout *layout, GsurfWindow *window)
{
	PairLayout *self = (PairLayout *)layout;
	GPtrArray *views = gsurf_window_get_views(window);
	guint i;

	for (i = 0; i < views->len; i++)
		box_drop(self->box, gsurf_view_get_native_widget(g_ptr_array_index(views, i)));
	self->attached--;
}

static void
pair_add_view(GsurfWindowLayout *layout, GsurfWindow *window, GsurfView *view)
{
	PairLayout *self = (PairLayout *)layout;

	(void)window;
	if (box_n_children(self->box) < 2)
		box_add(self->box, gsurf_view_get_native_widget(view));
}

static void
pair_remove_view(GsurfWindowLayout *layout, GsurfWindow *window, GsurfView *view)
{
	(void)window;
	box_drop(((PairLayout *)layout)->box, gsurf_view_get_native_widget(view));
	gsurf_window_layout_notify_visible(window);
}

static void
pair_activate_view(GsurfWindowLayout *layout, GsurfWindow *window, GsurfView *view)
{
	PairLayout *self = (PairLayout *)layout;
	GtkWidget *widget = gsurf_view_get_native_widget(view);

	if (gtk_widget_get_parent(widget) != self->box) {
#ifdef GSURF_BACKEND_GTK4
		GtkWidget *last = gtk_widget_get_last_child(self->box);
#else
		g_autoptr(GList) children = gtk_container_get_children(GTK_CONTAINER(self->box));
		GtkWidget *last = children != NULL ? g_list_last(children)->data : NULL;
#endif
		if (box_n_children(self->box) >= 2)
			box_drop(self->box, last);
		box_add(self->box, widget);
		gsurf_window_layout_notify_visible(window);
	}
	gsurf_view_focus(view);
}

static gboolean
pair_is_view_visible(GsurfWindowLayout *layout, GsurfWindow *window, GsurfView *view)
{
	(void)window;
	return gtk_widget_get_parent(gsurf_view_get_native_widget(view)) ==
	       ((PairLayout *)layout)->box;
}

static void
pair_layout_iface_init(GsurfWindowLayoutInterface *iface)
{
	iface->get_root = pair_get_root;
	iface->attach = pair_attach;
	iface->detach = pair_detach;
	iface->add_view = pair_add_view;
	iface->remove_view = pair_remove_view;
	iface->activate_view = pair_activate_view;
	iface->is_view_visible = pair_is_view_visible;
}

static void
pair_layout_dispose(GObject *object)
{
	PairLayout *self = (PairLayout *)object;

	g_clear_object(&self->box);
	G_OBJECT_CLASS(pair_layout_parent_class)->dispose(object);
}

static void
pair_layout_class_init(PairLayoutClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = pair_layout_dispose;
}

static void
pair_layout_init(PairLayout *self)
{
	self->box = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
	gtk_box_set_homogeneous(GTK_BOX(self->box), TRUE);
}

/* The allocated width of @widget on either toolkit. */
static gint
width_of(GtkWidget *widget)
{
#ifdef GSURF_BACKEND_GTK4
	return gtk_widget_get_width(widget);
#else
	return gtk_widget_get_allocated_width(widget);
#endif
}

/* Spin (bounded, about 5 s) until @widget is mapped and allocated. */
static void
wait_allocated(GtkWidget *widget)
{
	guint i;

	for (i = 0; i < 200; i++) {
		if (gtk_widget_get_mapped(widget) && width_of(widget) > 1)
			return;
		gsurf_webkit_test_spin(25);
	}
	g_assert_true(gtk_widget_get_mapped(widget));
	g_assert_cmpint(width_of(widget), >, 1);
}

static void
count_signal(gpointer instance, gpointer user_data)
{
	(void)instance;
	(*(guint *)user_data)++;
}

/* --- tests --- */

/* A press on a view widget emits GsurfWindow::button-press exactly once
 * (GTK4 used to route none at all), also after the view widget left the
 * stack and came back (wiring is once per widget and window). */
static void
test_button_routing(void)
{
	g_autoptr(GsurfWindow) window = NULL;
	g_autoptr(GsurfView) view = NULL;
	ButtonLog log = { 0, 0, FALSE };
	GtkWidget *widget;

	if (!gsurf_webkit_test_init())
		return;
	window = gsurf_window_new();
	view = gsurf_view_new();
	gsurf_window_add_view(window, view);
	g_signal_connect(window, "button-press", G_CALLBACK(on_button_press), &log);
	widget = gsurf_view_get_native_widget(view);
	g_assert_nonnull(widget);

	press_button(widget, 8);
	g_assert_cmpuint(log.count, ==, 1);
#ifdef GSURF_BACKEND_GTK3
	g_assert_cmpuint(log.button, ==, 8);
#endif

	/* Out of the window and back: still one emission per press. */
	g_object_ref(view);
	gsurf_window_remove_view(window, view);
	gsurf_window_add_view(window, view);
	g_object_unref(view);
	log.consume = TRUE;
	press_button(widget, 9);
	g_assert_cmpuint(log.count, ==, 2);
#ifdef GSURF_BACKEND_GTK3
	g_assert_cmpuint(log.button, ==, 9);
#endif
}

/* Two real webviews side by side through a layout; focus follows click;
 * removal restores the stack with every view alive. */
static void
test_layout_side_by_side(void)
{
	g_autoptr(GsurfWindow) window = NULL;
	g_autoptr(PairLayout) layout = NULL;
	GsurfView *first;
	GsurfView *second;
	GsurfView *third;
	GtkWidget *w1, *w2, *w3, *stack;
	gpointer weak_first, weak_second, weak_third, weak_layout;
	ButtonLog log = { 0, 0, TRUE };
	guint visible = 0;
	guint i;

	if (!gsurf_webkit_test_init())
		return;

	/* 1. A plain window with two views, the first active. */
	window = gsurf_window_new();
	gsurf_window_set_default_size(window, 1000, 600);
	first = gsurf_view_new();
	gsurf_window_add_view(window, first);
	g_object_unref(first);
	second = gsurf_view_new();
	gsurf_window_add_view(window, second);
	g_object_unref(second);
	weak_first = first;
	weak_second = second;
	g_object_add_weak_pointer(G_OBJECT(first), &weak_first);
	g_object_add_weak_pointer(G_OBJECT(second), &weak_second);
	w1 = gsurf_view_get_native_widget(first);
	w2 = gsurf_view_get_native_widget(second);
	stack = gtk_widget_get_parent(w1);
	g_assert_true(GTK_IS_STACK(stack));
	gsurf_window_present(window);
	wait_allocated(w1);
	g_assert_false(gsurf_window_get_view_visible(window, second));
	g_signal_connect(window, "visible-views-changed", G_CALLBACK(count_signal), &visible);

	/* 2. Install: both views in the box, both allocated side by side, the
	 *    stack hidden. */
	layout = g_object_new(pair_layout_get_type(), NULL);
	weak_layout = layout;
	g_object_add_weak_pointer(G_OBJECT(layout), &weak_layout);
	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	g_assert_cmpuint(visible, ==, 1);
	g_assert_cmpuint(layout->attached, ==, 1);
	g_assert_true(gtk_widget_get_parent(w1) == layout->box);
	g_assert_true(gtk_widget_get_parent(w2) == layout->box);
	g_assert_false(gtk_widget_get_visible(stack));
	wait_allocated(w1);
	wait_allocated(w2);
	g_assert_cmpint(ABS(width_of(w1) - width_of(w2)), <=, 1);
	g_assert_cmpint(width_of(w1) + width_of(w2), >, 900);
	g_assert_true(gsurf_window_get_view_visible(window, first));
	g_assert_true(gsurf_window_get_view_visible(window, second));
	g_assert_true(gsurf_window_get_active_view(window) == first);

	/* 3. Focus into the second view (a click does this) makes it active. */
	gtk_widget_grab_focus(w2);
	for (i = 0; i < 200 && gsurf_window_get_active_view(window) != second; i++)
		gsurf_webkit_test_spin(25);
	g_assert_true(gsurf_window_get_active_view(window) == second);
	/* Back to the first, the same way. */
	gtk_widget_grab_focus(w1);
	for (i = 0; i < 200 && gsurf_window_get_active_view(window) != first; i++)
		gsurf_webkit_test_spin(25);
	g_assert_true(gsurf_window_get_active_view(window) == first);

	/* 4. A view added while the layout is set: a background view (no
	 *    pane left), but wired (buttons) by the backend all the same;
	 *    focus into it does not make it active while it is hidden. */
	third = gsurf_view_new();
	gsurf_window_add_view(window, third);
	g_object_unref(third);
	weak_third = third;
	g_object_add_weak_pointer(G_OBJECT(third), &weak_third);
	w3 = gsurf_view_get_native_widget(third);
	g_assert_null(gtk_widget_get_parent(w3));
	g_assert_false(gsurf_window_get_view_visible(window, third));
	g_signal_connect(window, "button-press", G_CALLBACK(on_button_press), &log);
	press_button(w3, 1);
	g_assert_cmpuint(log.count, ==, 1);
	press_button(w1, 1);
	g_assert_cmpuint(log.count, ==, 2);
	g_signal_handlers_disconnect_by_data(window, &log);

	/* Activating it puts it in the second pane. */
	visible = 0;
	gsurf_window_set_active_view(window, third);
	g_assert_true(gtk_widget_get_parent(w3) == layout->box);
	g_assert_null(gtk_widget_get_parent(w2));
	g_assert_true(gsurf_window_get_view_visible(window, third));
	g_assert_false(gsurf_window_get_view_visible(window, second));
	g_assert_cmpuint(visible, ==, 1);
	wait_allocated(w3);

	/* 5. Remove the layout: every view in the stack again, in order, the
	 *    active one shown; nothing died. */
	visible = 0;
	g_assert_true(gsurf_window_set_layout(window, NULL));
	g_assert_cmpuint(visible, ==, 1);
	g_assert_cmpuint(layout->attached, ==, 0);
	g_assert_cmpuint(box_n_children(layout->box), ==, 0);
	g_assert_true(gtk_widget_get_visible(stack));
	g_assert_true(gtk_widget_get_parent(w1) == stack);
	g_assert_true(gtk_widget_get_parent(w2) == stack);
	g_assert_true(gtk_widget_get_parent(w3) == stack);
	g_assert_true(gtk_stack_get_visible_child(GTK_STACK(stack)) == w3);
	wait_allocated(w3);
	g_assert_true(gsurf_window_get_view_visible(window, third));
	g_assert_false(gsurf_window_get_view_visible(window, first));
	g_assert_nonnull(weak_first);
	g_assert_nonnull(weak_second);
	g_assert_nonnull(weak_third);

	/* Without a layout, focus into a hidden view changes nothing. */
	gtk_widget_grab_focus(w1);
	gsurf_webkit_test_spin(100);
	g_assert_true(gsurf_window_get_active_view(window) == third);

	/* 6. Install again, then dispose the window with it set: the layout
	 *    is detached and released, the views go with the window. */
	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	g_assert_cmpuint(layout->attached, ==, 1);
	g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&window);
	g_clear_object(&layout);
	g_assert_nonnull(weak_layout);
	g_object_unref(window);
	for (i = 0; i < 100 && (weak_first != NULL || weak_layout != NULL); i++)
		gsurf_webkit_test_spin(20);
	g_assert_null(window);
	g_assert_null(weak_layout);
	g_assert_null(weak_first);
	g_assert_null(weak_second);
	g_assert_null(weak_third);
}

typedef struct {
	guint    count;
	guint    keyval;
	gboolean editing; /* the active view's editing state at the time */
} KeyLog;

static gboolean
on_key_press(GsurfWindow *window, guint keyval, guint keycode, guint state,
             gpointer user_data)
{
	KeyLog *log = user_data;
	GsurfView *view = gsurf_window_get_active_view(window);

	(void)keycode;
	(void)state;
	log->count++;
	log->keyval = keyval;
	log->editing = view != NULL && gsurf_view_get_editing(view);
	return TRUE;
}

/* Deliver a bare key press to the toplevel the way the backend sees it:
 * GTK3 through the toplevel's key-press-event (and from there the
 * focused entry), GTK4 through the window's capture-phase key
 * controller. Returns whether the window handled it. */
static gboolean
press_key(GtkWidget *toplevel, guint keyval)
{
	gboolean handled = FALSE;
#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(toplevel);
	GtkEventController *found = NULL;
	guint i;

	for (i = 0; i < g_list_model_get_n_items(controllers) && found == NULL; i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (GTK_IS_EVENT_CONTROLLER_KEY(c) &&
		    gtk_event_controller_get_propagation_phase(c) == GTK_PHASE_CAPTURE)
			found = c; /* still owned by the toplevel */
	}
	g_assert_nonnull(found);
	g_signal_emit_by_name(found, "key-pressed", keyval, 0u, (GdkModifierType)0, &handled);
#else
	GdkEvent *event = gdk_event_new(GDK_KEY_PRESS);
	GdkDisplay *display = gtk_widget_get_display(toplevel);
	GdkSeat *seat = gdk_display_get_default_seat(display);
	GdkKeymapKey *keys = NULL;
	gint n_keys = 0;

	event->key.window = g_object_ref(gtk_widget_get_window(toplevel));
	event->key.send_event = TRUE;
	event->key.time = GDK_CURRENT_TIME;
	event->key.keyval = keyval;
	event->key.string = g_strdup("");
	if (gdk_keymap_get_entries_for_keyval(gdk_keymap_get_for_display(display),
			keyval, &keys, &n_keys) && n_keys > 0)
		event->key.hardware_keycode = (guint16)keys[0].keycode;
	g_free(keys);
	gdk_event_set_device(event, gdk_seat_get_keyboard(seat));
	handled = gtk_widget_event(toplevel, event);
	gdk_event_free(event);
#endif
	return handled;
}

#ifdef GSURF_BACKEND_GTK3
static const gchar *
entry_text(GtkWidget *entry)
{
	return gtk_entry_get_text(GTK_ENTRY(entry));
}
#endif

/* Whether the keyboard focus is in @entry (GTK4 focuses its GtkText). */
static gboolean
entry_has_focus(GtkWidget *toplevel, GtkWidget *entry)
{
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(toplevel));

	return focus != NULL && GTK_IS_EDITABLE(focus) &&
	       (focus == entry || gtk_widget_is_ancestor(focus, entry));
}

/* The key grab: a bare key reaches GsurfWindow::key-press instead of a
 * focused chrome entry, and while a page field is focused. */
static void
test_key_grab(void)
{
	static const gchar *page =
		"<!doctype html><html><body><input id=\"f\" autofocus></body></html>";
	g_autoptr(GsurfWindow) window = NULL;
	g_autoptr(GsurfView) view = NULL;
	KeyLog log = { 0, 0, FALSE };
	GtkWidget *toplevel;
	GtkWidget *entry;
	g_autofree gchar *value = NULL;
	guint i;

	if (!gsurf_webkit_test_init())
		return;
	window = gsurf_window_new();
	view = gsurf_view_new();
	gsurf_window_add_view(window, view);
	entry = gtk_entry_new();
	gsurf_window_add_top_widget(window, entry);
	g_signal_connect(window, "key-press", G_CALLBACK(on_key_press), &log);
	gsurf_window_present(window);
	toplevel = gsurf_window_get_native_widget(window);
	wait_allocated(entry);

	/* 1. Chrome entry focused, no grab: the bare key is the entry's. */
	gtk_widget_grab_focus(entry);
	for (i = 0; i < 100 && !entry_has_focus(toplevel, entry); i++)
		gsurf_webkit_test_spin(25);
	g_assert_true(entry_has_focus(toplevel, entry));
	press_key(toplevel, GDK_KEY_x);
	g_assert_cmpuint(log.count, ==, 0);
#ifdef GSURF_BACKEND_GTK3
	/* GTK3 delivered it to the entry, which typed it. */
	g_assert_cmpstr(entry_text(entry), ==, "x");
#endif

	/* 2. With the grab the window gets it first; consumed, so the entry
	 *    never types it. */
	gsurf_window_set_key_grab(window, TRUE);
	g_assert_true(press_key(toplevel, GDK_KEY_y));
	g_assert_cmpuint(log.count, ==, 1);
	g_assert_cmpuint(log.keyval, ==, GDK_KEY_y);
#ifdef GSURF_BACKEND_GTK3
	g_assert_cmpstr(entry_text(entry), ==, "x");
#endif
	gsurf_window_set_key_grab(window, FALSE);
	press_key(toplevel, GDK_KEY_z);
	g_assert_cmpuint(log.count, ==, 1);

	/* 3. A page field focused: the window hears a bare key with the view
	 *    editing (the gsurf binary's typing guard is what drops it, unless
	 *    the grab is set), and a consumed key never reaches the field. */
	gsurf_view_focus(view);
	gsurf_view_load_uri(view, "about:blank");
	gsurf_webkit_test_wait_finished(gsurf_webkit_test_native(view));
	webkit_web_view_load_html(gsurf_webkit_test_native(view), page, "http://grab.test/");
	gsurf_webkit_test_wait_finished(gsurf_webkit_test_native(view));
	for (i = 0; i < 200 && !gsurf_view_get_editing(view); i++)
		gsurf_webkit_test_spin(25);
	g_assert_true(gsurf_view_get_editing(view));
	gsurf_window_set_key_grab(window, TRUE);
	g_assert_true(press_key(toplevel, GDK_KEY_percent));
	g_assert_cmpuint(log.count, ==, 2);
	g_assert_cmpuint(log.keyval, ==, GDK_KEY_percent);
	g_assert_true(log.editing);
	gsurf_window_set_key_grab(window, FALSE);
	gsurf_webkit_test_spin(100);
	value = gsurf_webkit_test_eval(gsurf_webkit_test_native(view),
		"document.getElementById('f').value");
	g_assert_cmpstr(value, ==, "");
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *tmp = NULL;
	int ret;

	g_test_init(&argc, &argv, NULL);

	/* Private XDG dirs: nothing touches the user's real profile. */
	tmp = g_dir_make_tmp("gsurf-window-layout-XXXXXX", NULL);
	g_assert_nonnull(tmp);
	g_setenv("XDG_DATA_HOME", tmp, TRUE);
	g_setenv("XDG_CACHE_HOME", tmp, TRUE);
	g_setenv("XDG_CONFIG_HOME", tmp, TRUE);

	g_test_add_func("/gsurf/window-webkit/button-routing", test_button_routing);
	g_test_add_func("/gsurf/window-webkit/layout-side-by-side", test_layout_side_by_side);
	g_test_add_func("/gsurf/window-webkit/key-grab", test_key_grab);

	ret = g_test_run();
	gsurf_test_rmtree(tmp);
	return ret;
}
