/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-chrome-lifecycle.c - window-chrome module lifetimes and key handling
 *
 * Loads the real chromebar, find_bar, status_bar and tabs shared modules
 * against model-only views and plain GtkBoxes standing in for the window
 * chrome (top/bottom and side), on GTK3 and GTK4 alike (the modules share
 * modules/include/gsurf-chrome-compat.h).
 * Covers: tab-switch state, deactivation, reactivation, retained widgets,
 * repeated disposal, the host container dying first, and the address-bar /
 * find-bar key handling (Escape, Enter, Shift+Enter) that each backend
 * implements differently (GTK3 "key-press-event", GTK4 a capture-phase
 * GtkEventControllerKey).
 *
 * The tabs cases cover the strip and the tree sidebar: toggling by key
 * (Ctrl+Shift+.) and by action, rows and indentation following the tree,
 * collapse and expansion on activation, moving subtrees, row clicks,
 * promote and close_children, title/favicon updates, container stripes
 * following the registry, strip hiding, kiosk, the tab context menu
 * (right click on a row or a strip tab) without containers, and the same
 * lifetime rules as the other chrome modules. With a fake window layout
 * installed: the "visible" style of tabs shown in another pane following
 * visible-views-changed, and the Open in Split entries dispatching the
 * pane-split actions with the right view.
 *
 * Needs a display, so every case skips unless GSURF_TEST_GUI=1 (make
 * test-gui runs it under Xvfb). No web process or network is used.
 */
#include <gsurf.h>

#if defined(GSURF_BACKEND_GTK3) || defined(GSURF_BACKEND_GTK4)
#include <gtk/gtk.h>
#include <string.h>

/* Must match GSURF_CHROME_KEY_CONTROLLER_NAME in
 * modules/include/gsurf-chrome-compat.h (the GTK4 key controller name). */
#define CHROME_KEY_CONTROLLER_NAME "gsurf-chrome-key"

/* Model-only views avoid web processes and network access in chrome tests.
 * They record the calls the chrome modules make so key handling can be
 * asserted without a page. */
typedef struct {
	GsurfView    parent;
	const gchar *uri;
	gdouble      progress;
	gchar       *loaded;       /* last gsurf_view_load_uri() argument */
	gchar       *find_text;    /* last gsurf_view_find() text */
	gboolean     find_forward; /* last gsurf_view_find() direction */
	guint        find_calls;
	guint        next_calls;
	guint        prev_calls;
	guint        focus_calls;
	gchar       *title;        /* returned by gsurf_view_get_title() */
	gpointer     favicon;      /* native favicon (cairo surface / texture) */
} TestChromeView;
typedef struct { GsurfViewClass parent; } TestChromeViewClass;
typedef struct {
	GsurfWindow parent;
	GtkWidget  *box;
	GtkWidget  *side;
	gpointer    content;   /* last set_content() argument (a layout root) */
} TestChromeWindow;
typedef struct { GsurfWindowClass parent; } TestChromeWindowClass;

G_DEFINE_TYPE(TestChromeView, test_chrome_view, GSURF_TYPE_VIEW)
G_DEFINE_TYPE(TestChromeWindow, test_chrome_window, GSURF_TYPE_WINDOW)

/* --- Backend helpers (the only #ifdefs in the test) --- */

/* Initialise GTK; the GTK4 entry point takes no arguments. */
static gboolean
test_gtk_init(void)
{
#ifdef GSURF_BACKEND_GTK4
	return gtk_init_check();
#else
	return gtk_init_check(NULL, NULL);
#endif
}

/* Count a container's children and return the first and last of them. */
static guint
box_children(GtkWidget *box, GtkWidget **first, GtkWidget **last)
{
	guint n = 0;

	if (first != NULL)
		*first = NULL;
	if (last != NULL)
		*last = NULL;
#ifdef GSURF_BACKEND_GTK4
	{
		GtkWidget *child;

		for (child = gtk_widget_get_first_child(box); child != NULL;
		     child = gtk_widget_get_next_sibling(child)) {
			if (first != NULL && *first == NULL)
				*first = child;
			if (last != NULL)
				*last = child;
			n++;
		}
	}
#else
	{
		GList *children, *l;

		children = gtk_container_get_children(GTK_CONTAINER(box));
		for (l = children; l != NULL; l = l->next) {
			if (first != NULL && *first == NULL)
				*first = l->data;
			if (last != NULL)
				*last = l->data;
			n++;
		}
		g_list_free(children);
	}
#endif
	return n;
}

static const gchar *
entry_text(GtkWidget *entry)
{
#ifdef GSURF_BACKEND_GTK4
	return gtk_editable_get_text(GTK_EDITABLE(entry));
#else
	return gtk_entry_get_text(GTK_ENTRY(entry));
#endif
}

static void
set_entry_text(GtkWidget *entry, const gchar *text)
{
#ifdef GSURF_BACKEND_GTK4
	gtk_editable_set_text(GTK_EDITABLE(entry), text);
#else
	gtk_entry_set_text(GTK_ENTRY(entry), text);
#endif
}

#ifdef GSURF_BACKEND_GTK4
/* The module's key controller on @widget, or NULL once it was removed. */
static GtkEventController *
find_key_controller(GtkWidget *widget)
{
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(widget);
	GtkEventController *found = NULL;
	guint i, n;

	n = g_list_model_get_n_items(controllers);
	for (i = 0; i < n && found == NULL; i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (g_strcmp0(gtk_event_controller_get_name(c), CHROME_KEY_CONTROLLER_NAME) == 0)
			found = c; /* still owned by the widget once c is released */
	}
	return found;
}
#endif

#ifdef GSURF_BACKEND_GTK3
/* Marks a key press the module's handler let through (see send_key()). */
static gboolean
record_declined(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	*(gboolean *)user_data = TRUE;
	return TRUE;
}
#endif

/* Deliver a key press to a chrome widget the way its backend would, before
 * the widget's own key handling, and return whether the module consumed it.
 * The widget's own handling never runs, so the entry text is unchanged. */
static gboolean
send_key(GtkWidget *widget, guint keyval, GdkModifierType state)
{
	gboolean handled = FALSE;

#ifdef GSURF_BACKEND_GTK4
	GtkEventController *controller = find_key_controller(widget);

	g_assert_nonnull(controller);
	g_assert_cmpint(gtk_event_controller_get_propagation_phase(controller), ==,
		GTK_PHASE_CAPTURE);
	g_signal_emit_by_name(controller, "key-pressed", keyval, 0u, state, &handled);
#else
	GdkEvent *event = gdk_event_new(GDK_KEY_PRESS);

	/* The entry's own handler (reached when the module declines a key)
	 * needs a window to find the display; the root window will do for a
	 * widget that is never realized. The event owns this reference. */
	event->key.window = g_object_ref(gdk_get_default_root_window());
	event->key.keyval = keyval;
	event->key.state = state;
	gboolean reached = FALSE;
	gulong id;

	/* Handlers run in connection order before the entry's class handler.
	 * This one, connected after the module's, only runs when the module
	 * declined the key; it then consumes it so the entry never edits. */
	id = g_signal_connect(widget, "key-press-event", G_CALLBACK(record_declined), &reached);
	g_signal_emit_by_name(widget, "key-press-event", event, &handled);
	g_signal_handler_disconnect(widget, id);
	gdk_event_free(event);
	handled = handled && !reached;
#endif
	return handled;
}

/* --- Model-only view and window --- */

/* Return stable, controllable page state through the public view contract. */
static const gchar *
test_uri(GsurfView *view)
{
	return ((TestChromeView *)view)->uri;
}

static gdouble
test_progress(GsurfView *view)
{
	return ((TestChromeView *)view)->progress;
}

static void
test_load_uri(GsurfView *view, const gchar *uri)
{
	TestChromeView *self = (TestChromeView *)view;

	g_free(self->loaded);
	self->loaded = g_strdup(uri);
}

static void
test_find(GsurfView *view, const gchar *text, gboolean case_sensitive, gboolean forward)
{
	TestChromeView *self = (TestChromeView *)view;

	(void)case_sensitive;
	g_free(self->find_text);
	self->find_text = g_strdup(text);
	self->find_forward = forward;
	self->find_calls++;
}

static void
test_find_next(GsurfView *view)
{
	((TestChromeView *)view)->next_calls++;
}

static void
test_find_previous(GsurfView *view)
{
	((TestChromeView *)view)->prev_calls++;
}

static void
test_focus(GsurfView *view)
{
	((TestChromeView *)view)->focus_calls++;
}

static const gchar *
test_title(GsurfView *view)
{
	return ((TestChromeView *)view)->title;
}

static gpointer
test_favicon(GsurfView *view)
{
	return ((TestChromeView *)view)->favicon;
}

/* Give a view a 32x32 favicon in the backend's native image type. */
static void
set_test_favicon(GsurfView *view)
{
	TestChromeView *self = (TestChromeView *)view;

#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GBytes) bytes = NULL;
	guint8 *pixels = g_malloc0(32 * 32 * 4);

	bytes = g_bytes_new_take(pixels, 32 * 32 * 4);
	g_clear_object(&self->favicon);
	self->favicon = gdk_memory_texture_new(32, 32, GDK_MEMORY_DEFAULT, bytes, 32 * 4);
#else
	g_clear_pointer(&self->favicon, cairo_surface_destroy);
	self->favicon = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 32);
#endif
	gsurf_view_emit_favicon_changed(view);
}

static void
test_chrome_view_finalize(GObject *object)
{
	TestChromeView *self = (TestChromeView *)object;

	g_free(self->loaded);
	g_free(self->find_text);
	g_free(self->title);
#ifdef GSURF_BACKEND_GTK4
	g_clear_object(&self->favicon);
#else
	g_clear_pointer(&self->favicon, cairo_surface_destroy);
#endif
	G_OBJECT_CLASS(test_chrome_view_parent_class)->finalize(object);
}

static void
test_chrome_view_class_init(TestChromeViewClass *klass)
{
	GsurfViewClass *view_class = GSURF_VIEW_CLASS(klass);

	G_OBJECT_CLASS(klass)->finalize = test_chrome_view_finalize;
	view_class->get_uri = test_uri;
	view_class->get_estimated_load_progress = test_progress;
	view_class->load_uri = test_load_uri;
	view_class->find = test_find;
	view_class->find_next = test_find_next;
	view_class->find_previous = test_find_previous;
	view_class->focus = test_focus;
	view_class->get_title = test_title;
	view_class->get_favicon = test_favicon;
}

static void
test_chrome_view_init(TestChromeView *self)
{
	self->uri = "https://example.org/";
	self->progress = 1.0;
}

/* A real GTK container exercises widget ownership without showing a window. */
static void
add_chrome(GsurfWindow *window, gpointer widget, gboolean top)
{
	GtkWidget *box = ((TestChromeWindow *)window)->box;

	(void) top;
#ifdef GSURF_BACKEND_GTK4
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(widget));
#else
	gtk_container_add(GTK_CONTAINER(box), GTK_WIDGET(widget));
#endif
}

/* Side panels go into their own box, so tests can tell them apart. */
static void
add_side(GsurfWindow *window, gpointer widget, GsurfChromeEdge edge)
{
	GtkWidget *side = ((TestChromeWindow *)window)->side;

	(void)edge;
#ifdef GSURF_BACKEND_GTK4
	gtk_box_append(GTK_BOX(side), GTK_WIDGET(widget));
#else
	gtk_container_add(GTK_CONTAINER(side), GTK_WIDGET(widget));
	/* Like the real GTK3 window, which shows a packed side panel. */
	gtk_widget_show_all(GTK_WIDGET(widget));
#endif
}

/* GTK3 destroys a container (removing its children); a GTK4 GtkBox
 * removes its children when it is disposed. */
static void
destroy_box(GtkWidget **box)
{
	if (*box == NULL)
		return;
#ifdef GSURF_BACKEND_GTK4
	g_object_run_dispose(G_OBJECT(*box));
#else
	gtk_widget_destroy(*box);
#endif
	g_clear_object(box);
}

static void
test_chrome_window_dispose(GObject *object)
{
	TestChromeWindow *self = (TestChromeWindow *)object;

	destroy_box(&self->box);
	destroy_box(&self->side);
	G_OBJECT_CLASS(test_chrome_window_parent_class)->dispose(object);
}

/* Lets the window take a #GsurfWindowLayout (gsurf_window_set_layout()
 * refuses a window without set_content); the root is only recorded. */
static void
set_content(GsurfWindow *window, gpointer widget)
{
	((TestChromeWindow *)window)->content = widget;
}

static void
test_chrome_window_class_init(TestChromeWindowClass *klass)
{
	GSURF_WINDOW_CLASS(klass)->add_chrome_widget = add_chrome;
	GSURF_WINDOW_CLASS(klass)->add_side_widget = add_side;
	GSURF_WINDOW_CLASS(klass)->set_content = set_content;
	G_OBJECT_CLASS(klass)->dispose = test_chrome_window_dispose;
}

static void
test_chrome_window_init(TestChromeWindow *self)
{
	self->box = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
	self->side = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
}

/* --- Fixture --- */

typedef struct {
	GsurfApplication   *app;
	GsurfWindow        *window;
	GsurfView          *first;
	GsurfView          *second;
	GsurfModuleManager *manager;
	gchar              *path;
} Chrome;

/* Assert the container holds exactly the activation's single chrome widget. */
static GtkWidget *
only_child(GsurfWindow *window)
{
	GtkWidget *child;

	g_assert_cmpuint(box_children(((TestChromeWindow *)window)->box, &child, NULL), ==, 1);
	return child;
}

/* No signal emitter may retain a disposed or deactivated module as data.
 * On GTK4 the chrome key controller must be gone from the widget too. */
static void
assert_disconnected(gpointer emitter, gpointer module)
{
	g_assert_cmpuint(g_signal_handler_find(emitter, G_SIGNAL_MATCH_DATA,
		0, 0, NULL, NULL, module), ==, 0);
#ifdef GSURF_BACKEND_GTK4
	if (GTK_IS_WIDGET(emitter))
		g_assert_null(find_key_controller(GTK_WIDGET(emitter)));
#endif
}

/* The find bar's search entry is the last child of its bar. */
static GtkWidget *
find_bar_entry(GtkWidget *bar)
{
	GtkWidget *entry;

	g_assert_cmpuint(box_children(bar, NULL, &entry), ==, 2);
	g_assert_true(GTK_IS_SEARCH_ENTRY(entry));
	return entry;
}

/* Set up an application with one model window holding two model views,
 * the first still loading, and load the named real shared module.
 * Returns FALSE (after marking the test skipped) without a display. */
static gboolean
chrome_setup(Chrome *c, const gchar *name, GsurfModule **module)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *exe = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *filename = NULL;

	*c = (Chrome){ 0 };
	if (g_getenv("GSURF_TEST_GUI") == NULL) {
		g_test_skip("set GSURF_TEST_GUI=1 with a display to test chrome lifetimes");
		return FALSE;
	}
	g_assert_true(test_gtk_init());
	c->app = gsurf_application_new(NULL);
	c->window = g_object_new(test_chrome_window_get_type(), NULL);
	c->first = g_object_new(test_chrome_view_get_type(), NULL);
	c->second = g_object_new(test_chrome_view_get_type(), NULL);
	((TestChromeView *)c->first)->progress = 0.25;
	((TestChromeView *)c->second)->uri = "https://example.net/";
	gsurf_window_add_view(c->window, c->first);
	gsurf_window_add_view(c->window, c->second);
	gsurf_application_add_window(c->app, c->window);
	gsurf_module_manager_set_application(gsurf_module_manager_get_default(), c->app);

	/* Load the actual shared module rather than copying its implementation. */
	exe = g_file_read_link("/proc/self/exe", &error);
	g_assert_no_error(error);
	dir = g_path_get_dirname(exe);
	filename = g_strconcat(name, ".so", NULL);
	c->path = g_build_filename(dir, "modules", filename, NULL);
	c->manager = gsurf_module_manager_new();
	*module = gsurf_module_manager_load_module(c->manager, c->path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(*module);
	return TRUE;
}

static void
chrome_teardown(Chrome *c)
{
	gsurf_module_manager_set_application(gsurf_module_manager_get_default(), NULL);
	g_clear_object(&c->manager);
	g_clear_object(&c->app);
	g_clear_object(&c->window);
	g_clear_object(&c->first);
	g_clear_object(&c->second);
	g_clear_pointer(&c->path, g_free);
}

/* --- Lifetime --- */

static void
test_chrome_lifetime(gconstpointer data)
{
	const gchar *name = data;
	gboolean is_status = g_str_equal(name, "status_bar");
	gboolean is_address = g_str_equal(name, "chromebar");
	gboolean is_find = g_str_equal(name, "find_bar");
	g_autoptr(GError) error = NULL;
	Chrome c;
	GsurfModule *module;
	GtkWidget *widget;
	GtkWidget *entry = NULL;

	if (!chrome_setup(&c, name, &module))
		return;
	g_assert_true(gsurf_module_activate(module));
	widget = only_child(c.window);
	if (is_status)
		g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(widget)), ==, "https://example.org/  [25%]");
	if (is_find) {
		/* The find bar is packed but hidden until it is opened. */
		g_assert_false(gtk_widget_get_visible(widget));
		entry = find_bar_entry(widget);
	}

	/* Completed tabs must not inherit the previous tab's loading percentage. */
	gsurf_window_set_active_view(c.window, c.second);
	if (is_status)
		g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(widget)), ==, "https://example.net/");
	else if (is_address)
		g_assert_cmpstr(entry_text(widget), ==, "https://example.net/");
	gsurf_window_remove_view(c.window, c.first);
	gsurf_window_remove_view(c.window, c.second);
	if (is_status)
		g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(widget)), ==, "");
	else if (is_address)
		g_assert_cmpstr(entry_text(widget), ==, "");
	gsurf_window_add_view(c.window, c.first);

	/* Keep native objects alive to catch callbacks surviving deactivation. */
	g_object_ref(widget);
	if (entry != NULL)
		g_object_ref(entry);
	gsurf_module_deactivate(module);
	assert_disconnected(c.window, module);
	assert_disconnected(c.first, module);
	assert_disconnected(widget, module);
	if (entry != NULL) {
		assert_disconnected(entry, module);
		g_clear_object(&entry);
	}
	g_assert_cmpuint(box_children(((TestChromeWindow *)c.window)->box, NULL, NULL), ==, 0);
	/* The detached widget belongs to nobody but the test now. */
	g_assert_null(gtk_widget_get_parent(widget));
	g_object_unref(widget);

	/* Deactivating twice is harmless. */
	gsurf_module_deactivate(module);
	g_assert_true(gsurf_module_activate(module));
	widget = g_object_ref(only_child(c.window));
	if (is_find)
		entry = g_object_ref(find_bar_entry(widget));

	/* Repeated disposal must release all resources even without deactivate. */
	g_object_run_dispose(G_OBJECT(module));
	g_object_run_dispose(G_OBJECT(module));
	assert_disconnected(c.window, module);
	assert_disconnected(c.first, module);
	assert_disconnected(widget, module);
	if (entry != NULL) {
		assert_disconnected(entry, module);
		g_clear_object(&entry);
	}
	g_object_unref(widget);
	g_clear_object(&c.manager);

	/* The reverse lifetime is valid too: the host container dies first. */
	c.manager = gsurf_module_manager_new();
	module = gsurf_module_manager_load_module(c.manager, c.path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(module);
	g_assert_true(gsurf_module_activate(module));
	only_child(c.window);
	gsurf_module_manager_set_application(gsurf_module_manager_get_default(), NULL);
	g_clear_object(&c.app);
	g_clear_object(&c.window);
	gsurf_view_emit_uri_changed(c.first, "https://example.org/");
	gsurf_view_emit_progress_changed(c.first, 0.5);
	gsurf_module_deactivate(module);
	assert_disconnected(c.first, module);
	chrome_teardown(&c);
}

/* --- Key handling --- */

/* Escape abandons an address-bar edit and returns to the page; Enter loads
 * the input in the active view; other keys stay with the entry. */
static void
test_address_bar_keys(void)
{
	Chrome c;
	GsurfModule *module;
	GtkWidget *entry;
	TestChromeView *first;

	if (!chrome_setup(&c, "chromebar", &module))
		return;
	first = (TestChromeView *)c.first;
	g_assert_true(gsurf_module_activate(module));
	entry = only_child(c.window);
	g_assert_cmpstr(entry_text(entry), ==, "https://example.org/");

	/* The open-prompt action is the address bar's. */
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module),
		c.first, GSURF_ACTION_OPEN_PROMPT));
	g_assert_false(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module),
		c.first, GSURF_ACTION_FIND));

	/* Ordinary keys are typing, not commands. */
	set_entry_text(entry, "half-typed");
	g_assert_false(send_key(entry, GDK_KEY_a, 0));
	g_assert_false(send_key(entry, GDK_KEY_Return, 0));
	g_assert_cmpstr(entry_text(entry), ==, "half-typed");

	/* Escape restores the page's URI and hands focus back to the page. */
	first->focus_calls = 0;
	g_assert_true(send_key(entry, GDK_KEY_Escape, 0));
	g_assert_cmpstr(entry_text(entry), ==, "https://example.org/");
	g_assert_cmpuint(first->focus_calls, ==, 1);

	/* Enter ("activate") loads the input and returns focus to the page. */
	set_entry_text(entry, "example.com/path");
	g_signal_emit_by_name(entry, "activate");
	g_assert_nonnull(first->loaded);
	g_assert_true(g_str_has_suffix(first->loaded, "example.com/path"));
	g_assert_cmpuint(first->focus_calls, ==, 2);

	/* An empty bar loads nothing. */
	g_clear_pointer(&first->loaded, g_free);
	set_entry_text(entry, "");
	g_signal_emit_by_name(entry, "activate");
	g_assert_null(first->loaded);

	g_object_ref(entry);
	gsurf_module_deactivate(module);
	assert_disconnected(entry, module);
	g_object_unref(entry);
	chrome_teardown(&c);
}

/* The find action reveals the bar; Enter confirms a search and returns to
 * the page where n/N step; Shift+Enter steps back; Escape closes and clears. */
static void
test_find_bar_keys(void)
{
	Chrome c;
	GsurfModule *module;
	GtkWidget *bar;
	GtkWidget *entry;
	TestChromeView *first;

	if (!chrome_setup(&c, "find_bar", &module))
		return;
	first = (TestChromeView *)c.first;
	g_assert_true(gsurf_module_activate(module));
	bar = only_child(c.window);
	entry = find_bar_entry(bar);
	g_assert_false(gtk_widget_get_visible(bar));

	/* n/N belong to the page until a search is up. */
	g_assert_false(gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(module),
		c.first, GDK_KEY_n, 0, GSURF_MOD_NONE, GSURF_MODE_NORMAL));

	/* The find action (Ctrl+f) and the open key (/) reveal the bar. */
	g_assert_true(gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module),
		c.first, GSURF_ACTION_FIND));
	g_assert_true(gtk_widget_get_visible(bar));
	gtk_widget_set_visible(bar, FALSE);
	g_assert_true(gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(module),
		c.first, GDK_KEY_slash, 0, GSURF_MOD_NONE, GSURF_MODE_NORMAL));
	g_assert_true(gtk_widget_get_visible(bar));

	/* Enter confirms the search (forward) and hands focus to the page. */
	set_entry_text(entry, "needle");
	first->focus_calls = 0;
	g_signal_emit_by_name(entry, "activate");
	g_assert_cmpstr(first->find_text, ==, "needle");
	g_assert_true(first->find_forward);
	g_assert_cmpuint(first->focus_calls, ==, 1);
	g_assert_true(gtk_widget_get_visible(bar));

	/* Now n/N step through the matches from the page. */
	g_assert_true(gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(module),
		c.first, GDK_KEY_n, 0, GSURF_MOD_NONE, GSURF_MODE_NORMAL));
	g_assert_cmpuint(first->next_calls, ==, 1);
	g_assert_true(gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(module),
		c.first, GDK_KEY_N, 0, GSURF_MOD_SHIFT, GSURF_MODE_NORMAL));
	g_assert_cmpuint(first->prev_calls, ==, 1);

	/* In the entry: plain keys and plain Enter are the entry's, Shift+Enter
	 * steps backwards (and is consumed before it can activate). */
	g_assert_false(send_key(entry, GDK_KEY_a, 0));
	g_assert_false(send_key(entry, GDK_KEY_Return, 0));
	g_assert_true(send_key(entry, GDK_KEY_Return, GDK_SHIFT_MASK));
	g_assert_true(send_key(entry, GDK_KEY_KP_Enter, GDK_SHIFT_MASK));
	g_assert_cmpuint(first->prev_calls, ==, 3);

	/* Escape clears the query and highlight, hides the bar, and returns
	 * focus to the page; n is then free again. */
	g_assert_true(send_key(entry, GDK_KEY_Escape, 0));
	g_assert_cmpstr(entry_text(entry), ==, "");
	g_assert_cmpstr(first->find_text, ==, "");
	g_assert_cmpuint(first->focus_calls, ==, 2);
	g_assert_false(gtk_widget_get_visible(bar));
	g_assert_false(gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(module),
		c.first, GDK_KEY_n, 0, GSURF_MOD_NONE, GSURF_MODE_NORMAL));

	/* With no view at all Shift+Enter is still consumed, without a call. */
	gsurf_window_remove_view(c.window, c.first);
	gsurf_window_remove_view(c.window, c.second);
	g_assert_true(send_key(entry, GDK_KEY_Return, GDK_SHIFT_MASK));
	g_assert_cmpuint(first->prev_calls, ==, 3);

	g_object_ref(entry);
	gsurf_module_deactivate(module);
	assert_disconnected(entry, module);
	g_object_unref(entry);
	chrome_teardown(&c);
}

/* --- Tabs: widget lookup and input (backend #ifdefs kept here) --- */

/* Must match modules/tabs/gsurf-tabs-private.h and the compat header. */
#define TABS_ID_DATA        "gsurf-tab-view-id"
#define TABS_CSS_DATA       "gsurf-tabs-css-provider"
#define CHROME_CLICK_PREFIX "gsurf-chrome-click-"
#define TWISTY_EXPANDED     "▾"
#define TWISTY_COLLAPSED    "▸"

/* The children of a container, in order. */
static GPtrArray *
children_of(GtkWidget *widget)
{
	GPtrArray *out = g_ptr_array_new();

#ifdef GSURF_BACKEND_GTK4
	GtkWidget *child;

	for (child = gtk_widget_get_first_child(widget); child != NULL;
	     child = gtk_widget_get_next_sibling(child))
		g_ptr_array_add(out, child);
#else
	if (GTK_IS_CONTAINER(widget)) {
		GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
		GList *l;

		for (l = children; l != NULL; l = l->next)
			g_ptr_array_add(out, l->data);
		g_list_free(children);
	}
#endif
	return out;
}

/* Depth-first search for a widget by name. */
static GtkWidget *
find_named(GtkWidget *root, const gchar *name)
{
	g_autoptr(GPtrArray) children = NULL;
	guint i;

	if (g_strcmp0(gtk_widget_get_name(root), name) == 0)
		return root;
	children = children_of(root);
	for (i = 0; i < children->len; i++) {
		GtkWidget *found = find_named(g_ptr_array_index(children, i), name);

		if (found != NULL)
			return found;
	}
	return NULL;
}

static gboolean
has_class(GtkWidget *widget, const gchar *css_class)
{
#ifdef GSURF_BACKEND_GTK4
	return gtk_widget_has_css_class(widget, css_class);
#else
	return gtk_style_context_has_class(gtk_widget_get_style_context(widget), css_class);
#endif
}

/* Whether a row still has a click gesture (GTK4) or click handler (GTK3)
 * from @module. */
static gboolean
has_click_handler(GtkWidget *widget, gpointer module)
{
#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(widget);
	guint i;

	for (i = 0; i < g_list_model_get_n_items(controllers); i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (g_str_has_prefix(gtk_event_controller_get_name(c) ?: "", CHROME_CLICK_PREFIX))
			return TRUE;
	}
	(void)module;
	return FALSE;
#else
	return g_signal_handler_find(widget, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, module) != 0;
#endif
}

/* Click mouse @button on a sidebar row: the primary button acts on its
 * release (a press may start a drag), the others on their press. */
static void
click_row(GtkWidget *row, guint button)
{
#ifdef GSURF_BACKEND_GTK4
	g_autofree gchar *name = g_strdup_printf("%s%u",
		button == 1 ? "gsurf-chrome-release-" : CHROME_CLICK_PREFIX, button);
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(row);
	GtkEventController *found = NULL;
	guint i;

	for (i = 0; i < g_list_model_get_n_items(controllers) && found == NULL; i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (g_strcmp0(gtk_event_controller_get_name(c), name) == 0)
			found = c; /* still owned by the row */
	}
	g_assert_nonnull(found);
	g_signal_emit_by_name(found, button == 1 ? "released" : "pressed", 1, -1.0, -1.0);
#else
	GdkEvent *event = gdk_event_new(button == 1 ? GDK_BUTTON_RELEASE : GDK_BUTTON_PRESS);
	gboolean handled = FALSE;

	event->button.button = button;
	g_signal_emit_by_name(row, button == 1 ? "button-release-event" : "button-press-event",
		event, &handled);
	gdk_event_free(event);
	g_assert_true(handled);
#endif
}

static gboolean
image_is_set(GtkWidget *image)
{
	return gtk_image_get_storage_type(GTK_IMAGE(image)) != GTK_IMAGE_EMPTY;
}

/* --- Tabs: fixture helpers --- */

static guint64
widget_id(GtkWidget *widget)
{
	guint64 *id = g_object_get_data(G_OBJECT(widget), TABS_ID_DATA);

	return id != NULL ? *id : 0;
}

/* The only side panel (the sidebar). */
static GtkWidget *
side_child(GsurfWindow *window)
{
	GtkWidget *child;

	g_assert_cmpuint(box_children(((TestChromeWindow *)window)->side, &child, NULL), ==, 1);
	return child;
}

/* The sidebar's rows, in order. */
static GPtrArray *
sidebar_rows(GsurfWindow *window)
{
	GtkWidget *list = find_named(side_child(window), "gsurf-tab-sidebar-list");

	g_assert_nonnull(list);
	return children_of(list);
}

/* The row of @view (asserting it exists). */
static GtkWidget *
row_of(GsurfWindow *window, GsurfView *view)
{
	g_autoptr(GPtrArray) rows = sidebar_rows(window);
	guint i;

	for (i = 0; i < rows->len; i++) {
		if (widget_id(g_ptr_array_index(rows, i)) == gsurf_view_get_id(view))
			return g_ptr_array_index(rows, i);
	}
	g_assert_not_reached();
	return NULL;
}

static GtkWidget *
row_part(GsurfWindow *window, GsurfView *view, const gchar *name)
{
	GtkWidget *part = find_named(row_of(window, view), name);

	g_assert_nonnull(part);
	return part;
}

static gint
row_indent(GsurfWindow *window, GsurfView *view)
{
	return gtk_widget_get_margin_start(row_part(window, view, "gsurf-tab-row-box"));
}

static const gchar *
twisty_of(GsurfWindow *window, GsurfView *view)
{
	return gtk_button_get_label(GTK_BUTTON(row_part(window, view, "gsurf-tab-twisty")));
}

/* The sidebar rows' view ids as "a b c" in display order, using the
 * given names for the views. */
static gchar *
rows_string(GsurfWindow *window, GsurfView **views, const gchar **names, guint n)
{
	g_autoptr(GPtrArray) rows = sidebar_rows(window);
	GString *s = g_string_new(NULL);
	guint i;
	guint j;

	for (i = 0; i < rows->len; i++) {
		guint64 id = widget_id(g_ptr_array_index(rows, i));
		const gchar *name = "?";

		for (j = 0; j < n; j++) {
			if (views[j] != NULL && gsurf_view_get_id(views[j]) == id)
				name = names[j];
		}
		g_string_append_printf(s, "%s%s", i > 0 ? " " : "", name);
	}
	return g_string_free(s, FALSE);
}

/* A model view opened from @opener, in @container, titled @title. */
static GsurfView *
tab_view(GsurfView *opener, const gchar *container, const gchar *title)
{
	TestChromeView *view = g_object_new(test_chrome_view_get_type(),
		"opener", opener, "container-id", container, NULL);

	view->title = g_strdup(title);
	return GSURF_VIEW(view);
}

static void
configure_tabs(GsurfModule *module, const gchar *yaml)
{
	g_autoptr(GsurfConfig) config = gsurf_config_new();

	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, NULL));
	gsurf_module_configure(module, config);
}

static gboolean
tabs_action(GsurfModule *module, GsurfView *view, GsurfAction action)
{
	return gsurf_action_handler_handle_action(GSURF_ACTION_HANDLER(module), view, action);
}

static gboolean
tabs_key(GsurfModule *module, GsurfView *view, guint keyval, guint mods)
{
	return gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(module), view,
		keyval, 0, mods, GSURF_MODE_NORMAL);
}

/* The module's stylesheet as text. */
static gchar *
tabs_css(GsurfModule *module)
{
	GtkCssProvider *css = g_object_get_data(G_OBJECT(module), TABS_CSS_DATA);

	g_assert_nonnull(css);
	return gtk_css_provider_to_string(css);
}

/* --- Tabs: lifetime --- */

/* Strip and sidebar follow the chrome-module lifetime rules. */
static void
test_tabs_lifetime(void)
{
	g_autoptr(GError) error = NULL;
	Chrome c;
	GsurfModule *module;
	GtkWidget *strip;
	GtkWidget *sidebar;
	GtkWidget *row;

	if (!chrome_setup(&c, "tabs", &module))
		return;
	configure_tabs(module, "modules: {}\n");
	g_assert_true(gsurf_module_activate(module));
	strip = only_child(c.window);
	sidebar = side_child(c.window);
	g_assert_cmpstr(gtk_widget_get_name(strip), ==, "gsurf-tab-strip");
	g_assert_cmpstr(gtk_widget_get_name(sidebar), ==, "gsurf-tab-sidebar");
	/* Two tabs show the strip; the sidebar starts hidden (sidebar: false). */
	g_assert_true(gtk_widget_get_visible(strip));
	g_assert_false(gtk_widget_get_visible(sidebar));
	g_assert_cmpuint(box_children(strip, NULL, NULL), ==, 2);
	{
		g_autoptr(GPtrArray) rows = sidebar_rows(c.window);

		g_assert_cmpuint(rows->len, ==, 2);
	}
	/* One tab hides the strip (show_single: false). */
	gsurf_window_remove_view(c.window, c.second);
	g_assert_false(gtk_widget_get_visible(strip));
	g_assert_cmpuint(box_children(strip, NULL, NULL), ==, 1);
	gsurf_window_add_view(c.window, c.second);
	g_assert_true(gtk_widget_get_visible(strip));

	/* Keep native objects alive to catch callbacks surviving deactivation. */
	g_object_ref(strip);
	g_object_ref(sidebar);
	row = g_object_ref(row_of(c.window, c.first));
	gsurf_module_deactivate(module);
	assert_disconnected(c.window, module);
	assert_disconnected(c.first, module);
	assert_disconnected(c.second, module);
	assert_disconnected(strip, module);
	assert_disconnected(sidebar, module);
	assert_disconnected(row, module);
	g_assert_false(has_click_handler(row, module));
	g_assert_null(g_object_get_data(G_OBJECT(module), TABS_CSS_DATA));
	g_assert_cmpuint(box_children(((TestChromeWindow *)c.window)->box, NULL, NULL), ==, 0);
	g_assert_cmpuint(box_children(((TestChromeWindow *)c.window)->side, NULL, NULL), ==, 0);
	g_assert_null(gtk_widget_get_parent(strip));
	g_assert_null(gtk_widget_get_parent(sidebar));
	g_object_unref(row);
	g_object_unref(strip);
	g_object_unref(sidebar);

	/* Deactivating twice is harmless; reactivation builds one of each. */
	gsurf_module_deactivate(module);
	g_assert_true(gsurf_module_activate(module));
	strip = g_object_ref(only_child(c.window));
	sidebar = g_object_ref(side_child(c.window));
	row = g_object_ref(row_of(c.window, c.second));

	/* Repeated disposal must release all resources even without deactivate. */
	g_object_run_dispose(G_OBJECT(module));
	g_object_run_dispose(G_OBJECT(module));
	assert_disconnected(c.window, module);
	assert_disconnected(c.first, module);
	assert_disconnected(strip, module);
	assert_disconnected(sidebar, module);
	assert_disconnected(row, module);
	g_assert_false(has_click_handler(row, module));
	g_object_unref(row);
	g_object_unref(strip);
	g_object_unref(sidebar);
	g_clear_object(&c.manager);

	/* The reverse lifetime is valid too: the window dies first. */
	c.manager = gsurf_module_manager_new();
	module = gsurf_module_manager_load_module(c.manager, c.path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(module);
	g_assert_true(gsurf_module_activate(module));
	only_child(c.window);
	gsurf_module_manager_set_application(gsurf_module_manager_get_default(), NULL);
	g_clear_object(&c.app);
	g_clear_object(&c.window);
	assert_disconnected(c.first, module);
	gsurf_view_emit_title_changed(c.first, "after");
	gsurf_view_emit_favicon_changed(c.first);
	gsurf_module_deactivate(module);
	assert_disconnected(c.first, module);
	chrome_teardown(&c);
}

/* --- Tabs: the tree sidebar --- */

static void
test_tabs_sidebar_tree(void)
{
	Chrome c;
	GsurfModule *module;
	GtkWidget *strip;
	GtkWidget *sidebar;
	GsurfView *c1;
	GsurfView *g1;
	GsurfView *gg;
	g_autofree gchar *order = NULL;
	const gchar *names[] = { "first", "second", "c1", "g1", "gg" };
	GsurfView *views[5];
	guint mods = GSURF_MOD_CTRL | GSURF_MOD_SHIFT;

	if (!chrome_setup(&c, "tabs", &module))
		return;
	configure_tabs(module,
		"modules:\n  tabs:\n    enabled: true\n"
		"    indent: 10\n    max_depth: 2\n    show_single: true\n");
	g_assert_true(gsurf_module_activate(module));
	strip = only_child(c.window);
	sidebar = side_child(c.window);
	g_assert_false(gtk_widget_get_visible(sidebar));

	/* 1. Ctrl+Shift+. (keyval "greater") toggles the sidebar and, with
	 *    sidebar_hides_strip (default), hides the strip. */
	g_assert_false(tabs_key(module, c.first, GDK_KEY_period, GSURF_MOD_CTRL));
	g_assert_true(tabs_key(module, c.first, GDK_KEY_greater, mods));
	g_assert_true(gtk_widget_get_visible(sidebar));
	g_assert_false(gtk_widget_get_visible(strip));
	/* The action does the same. */
	g_assert_true(tabs_action(module, c.first, GSURF_ACTION_TAB_SIDEBAR_TOGGLE));
	g_assert_false(gtk_widget_get_visible(sidebar));
	g_assert_true(gtk_widget_get_visible(strip));
	g_assert_true(tabs_action(module, c.first, GSURF_ACTION_TAB_SIDEBAR_TOGGLE));
	g_assert_true(gtk_widget_get_visible(sidebar));

	/* 2. Roots: no indentation, no twisty. */
	g_assert_cmpint(row_indent(c.window, c.first), ==, 0);
	g_assert_cmpstr(twisty_of(c.window, c.first), ==, "");
	g_assert_false(gtk_widget_get_sensitive(row_part(c.window, c.first, "gsurf-tab-twisty")));

	/* 3. Children go after their opener's last descendant and indent;
	 *    deeper than max_depth draws at max_depth. */
	c1 = tab_view(c.first, NULL, "c1");
	g1 = tab_view(c1, NULL, "g1");
	gg = tab_view(g1, NULL, "gg");
	views[0] = c.first; views[1] = c.second; views[2] = c1; views[3] = g1; views[4] = gg;
	gsurf_window_add_view(c.window, c1);
	g_assert_cmpint(gsurf_window_get_view_index(c.window, c1), ==, 1);
	gsurf_window_add_view(c.window, g1);
	gsurf_window_add_view(c.window, gg);
	g_assert_cmpint(gsurf_window_get_view_index(c.window, gg), ==, 3);
	order = rows_string(c.window, views, names, 5);
	g_assert_cmpstr(order, ==, "first c1 g1 gg second");
	g_clear_pointer(&order, g_free);
	g_assert_cmpint(row_indent(c.window, c1), ==, 10);
	g_assert_cmpint(row_indent(c.window, g1), ==, 20);
	g_assert_cmpint(row_indent(c.window, gg), ==, 20);
	g_assert_cmpstr(twisty_of(c.window, c.first), ==, TWISTY_EXPANDED);
	g_assert_true(gtk_widget_get_sensitive(row_part(c.window, c.first, "gsurf-tab-twisty")));
	g_assert_cmpuint(box_children(strip, NULL, NULL), ==, 5);

	/* 4. Collapse hides the descendants (they stay open); activating a
	 *    hidden tab expands its ancestors. */
	gsurf_window_set_active_view(c.window, c.first);
	g_assert_true(gsurf_window_get_active_view(c.window) == c.first);
	g_assert_true(tabs_action(module, c.first, GSURF_ACTION_TAB_TREE_COLLAPSE));
	g_assert_cmpstr(twisty_of(c.window, c.first), ==, TWISTY_COLLAPSED);
	g_assert_false(gtk_widget_get_visible(row_of(c.window, c1)));
	g_assert_false(gtk_widget_get_visible(row_of(c.window, gg)));
	g_assert_true(gtk_widget_get_visible(row_of(c.window, c.second)));
	g_assert_cmpuint(gsurf_window_get_n_views(c.window), ==, 5);
	gsurf_window_set_active_view(c.window, gg);
	g_assert_true(gtk_widget_get_visible(row_of(c.window, gg)));
	g_assert_true(gtk_widget_get_visible(row_of(c.window, c1)));
	g_assert_cmpstr(twisty_of(c.window, c.first), ==, TWISTY_EXPANDED);
	g_assert_true(has_class(row_of(c.window, gg), "gsurf-tab-row-active"));
	g_assert_false(has_class(row_of(c.window, c.first), "gsurf-tab-row-active"));

	/* 5. The twisty collapses too; the active tab inside the collapsed
	 *    subtree hands activation to the collapsed tab. */
	g_signal_emit_by_name(row_part(c.window, c.first, "gsurf-tab-twisty"), "clicked");
	g_assert_true(gsurf_window_get_active_view(c.window) == c.first);
	g_assert_false(gtk_widget_get_visible(row_of(c.window, c1)));
	g_signal_emit_by_name(row_part(c.window, c.first, "gsurf-tab-twisty"), "clicked");
	g_assert_true(gtk_widget_get_visible(row_of(c.window, c1)));

	/* 6. tab-tree-collapse on a leaf collapses its parent and activates
	 *    it; the key does the same. */
	gsurf_window_set_active_view(c.window, gg);
	g_assert_true(tabs_key(module, gg, GDK_KEY_braceleft, mods));
	g_assert_true(gsurf_window_get_active_view(c.window) == g1);
	g_assert_cmpstr(twisty_of(c.window, g1), ==, TWISTY_COLLAPSED);
	g_assert_false(gtk_widget_get_visible(row_of(c.window, gg)));
	g_assert_true(tabs_key(module, g1, GDK_KEY_braceleft, mods));
	g_assert_true(gtk_widget_get_visible(row_of(c.window, gg)));

	/* 7. Moving a tab moves its subtree among its siblings. */
	gsurf_window_set_active_view(c.window, c.second);
	g_assert_true(tabs_action(module, c.second, GSURF_ACTION_TAB_MOVE_UP));
	g_assert_cmpint(gsurf_window_get_view_index(c.window, c.second), ==, 0);
	g_assert_cmpint(gsurf_window_get_view_index(c.window, gg), ==, 4);
	order = rows_string(c.window, views, names, 5);
	g_assert_cmpstr(order, ==, "second first c1 g1 gg");
	g_clear_pointer(&order, g_free);
	/* No previous sibling: nothing moves. */
	g_assert_true(tabs_key(module, c.second, GDK_KEY_Page_Up, mods));
	g_assert_cmpint(gsurf_window_get_view_index(c.window, c.second), ==, 0);
	g_assert_true(tabs_key(module, c.second, GDK_KEY_Page_Down, mods));
	order = rows_string(c.window, views, names, 5);
	g_assert_cmpstr(order, ==, "first c1 g1 gg second");
	g_clear_pointer(&order, g_free);
	gsurf_window_set_active_view(c.window, c.first);
	g_assert_true(tabs_action(module, c.first, GSURF_ACTION_TAB_MOVE_DOWN));
	g_assert_cmpint(gsurf_window_get_view_index(c.window, c.first), ==, 1);
	g_assert_cmpint(gsurf_window_get_view_index(c.window, c1), ==, 2);
	g_assert_true(tabs_action(module, c.first, GSURF_ACTION_TAB_MOVE_UP));

	/* 8. A move by someone else re-syncs the rows from the window. */
	gsurf_window_move_view(c.window, c.second, 0);
	order = rows_string(c.window, views, names, 5);
	g_assert_cmpstr(order, ==, "second first c1 g1 gg");
	g_clear_pointer(&order, g_free);
	gsurf_window_move_view(c.window, c.second, 4);

	/* 9. Clicking a row activates it; a middle click closes it, and its
	 *    child is promoted. */
	click_row(row_of(c.window, c1), 1);
	g_assert_true(gsurf_window_get_active_view(c.window) == c1);
	click_row(row_of(c.window, g1), 2);
	g_assert_cmpint(gsurf_window_get_view_index(c.window, g1), ==, -1);
	views[3] = NULL;
	g_assert_cmpint(row_indent(c.window, gg), ==, 20);
	/* The close button closes; gg moves up to first. */
	g_signal_emit_by_name(row_part(c.window, c1, "gsurf-tab-close"), "clicked");
	g_assert_cmpint(gsurf_window_get_view_index(c.window, c1), ==, -1);
	views[2] = NULL;
	g_assert_cmpint(row_indent(c.window, gg), ==, 10);
	order = rows_string(c.window, views, names, 5);
	g_assert_cmpstr(order, ==, "first gg second");
	g_clear_pointer(&order, g_free);

	/* 10. Title and favicon updates touch the row. */
	g_free(((TestChromeView *)c.first)->title);
	((TestChromeView *)c.first)->title = g_strdup("Hello");
	gsurf_view_emit_title_changed(c.first, "Hello");
	g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(row_part(c.window, c.first,
		"gsurf-tab-title"))), ==, "Hello");
	g_assert_false(image_is_set(row_part(c.window, c.first, "gsurf-tab-icon")));
	set_test_favicon(c.first);
	g_assert_true(image_is_set(row_part(c.window, c.first, "gsurf-tab-icon")));
	g_assert_false(image_is_set(row_part(c.window, gg, "gsurf-tab-icon")));

	/* 11. sidebar_hides_strip: false keeps the strip with the sidebar;
	 *     sidebar: true starts with it shown. */
	configure_tabs(module,
		"modules:\n  tabs:\n    enabled: true\n"
		"    sidebar: true\n    sidebar_hides_strip: false\n"
		"    sidebar_position: right\n    sidebar_width: 300\n");
	gsurf_module_deactivate(module);
	g_assert_true(gsurf_module_activate(module));
	strip = only_child(c.window);
	sidebar = side_child(c.window);
	g_assert_true(gtk_widget_get_visible(sidebar));
	g_assert_true(gtk_widget_get_visible(strip));
	/* The expanding row titles must not make the sidebar expand (GTK4
	 * propagates hexpand up from children). */
	g_assert_false(gtk_widget_compute_expand(sidebar, GTK_ORIENTATION_HORIZONTAL));
	{
		gint width = 0;

		gtk_widget_get_size_request(sidebar, &width, NULL);
		g_assert_cmpint(width, ==, 300);
	}
	/* Reactivation rebuilds the tree from the window order and openers
	 * alone: gg's opener g1 is closed, so gg is a root now (the promotion
	 * to first was runtime state). */
	g_assert_cmpint(row_indent(c.window, gg), ==, 0);
	g_assert_cmpstr(twisty_of(c.window, c.first), ==, "");

	/* 12. strip: false never shows the strip, with the sidebar shown or
	 *     hidden, even with show_single; the sidebar still toggles. */
	configure_tabs(module,
		"modules:\n  tabs:\n    enabled: true\n"
		"    strip: false\n    show_single: true\n");
	gsurf_module_deactivate(module);
	g_assert_true(gsurf_module_activate(module));
	strip = only_child(c.window);
	sidebar = side_child(c.window);
	g_assert_false(gtk_widget_get_visible(strip));
	g_assert_false(gtk_widget_get_visible(sidebar));
	g_assert_true(tabs_action(module, c.first, GSURF_ACTION_TAB_SIDEBAR_TOGGLE));
	g_assert_true(gtk_widget_get_visible(sidebar));
	g_assert_false(gtk_widget_get_visible(strip));
	g_assert_true(tabs_action(module, c.first, GSURF_ACTION_TAB_SIDEBAR_TOGGLE));
	g_assert_false(gtk_widget_get_visible(sidebar));
	g_assert_false(gtk_widget_get_visible(strip));

	/* 13. Reconfiguring back to defaults brings the strip back. */
	configure_tabs(module, "modules:\n  tabs:\n    enabled: true\n");
	gsurf_module_deactivate(module);
	g_assert_true(gsurf_module_activate(module));
	strip = only_child(c.window);
	g_assert_true(gtk_widget_get_visible(strip));

	gsurf_module_deactivate(module);
	g_object_unref(c1);
	g_object_unref(g1);
	g_object_unref(gg);
	chrome_teardown(&c);
}

/* on_close_parent: close_children closes the subtree, but never the
 * window's last view. */
static void
test_tabs_close_children(void)
{
	Chrome c;
	GsurfModule *module;
	GsurfView *k1;
	GsurfView *k2;
	GsurfView *k3;

	if (!chrome_setup(&c, "tabs", &module))
		return;
	configure_tabs(module,
		"modules:\n  tabs:\n    enabled: true\n    on_close_parent: close_children\n");
	g_assert_true(gsurf_module_activate(module));
	k1 = tab_view(c.first, NULL, "k1");
	k2 = tab_view(k1, NULL, "k2");
	gsurf_window_add_view(c.window, k1);
	gsurf_window_add_view(c.window, k2);
	g_assert_cmpuint(gsurf_window_get_n_views(c.window), ==, 4);

	gsurf_window_set_active_view(c.window, c.first);
	g_assert_true(tabs_action(module, c.first, GSURF_ACTION_TAB_CLOSE));
	g_assert_cmpuint(gsurf_window_get_n_views(c.window), ==, 1);
	g_assert_true(gsurf_window_get_nth_view(c.window, 0) == c.second);
	{
		g_autoptr(GPtrArray) rows = sidebar_rows(c.window);

		g_assert_cmpuint(rows->len, ==, 1);
	}

	/* Closing the parent of the only other view keeps that view. */
	k3 = tab_view(c.second, NULL, "k3");
	gsurf_window_add_view(c.window, k3);
	gsurf_window_set_active_view(c.window, c.second);
	g_assert_true(tabs_action(module, c.second, GSURF_ACTION_TAB_CLOSE));
	g_assert_cmpuint(gsurf_window_get_n_views(c.window), ==, 1);
	g_assert_true(gsurf_window_get_nth_view(c.window, 0) == k3);
	g_assert_cmpint(row_indent(c.window, k3), ==, 0);
	/* The last view itself is never closed by the module. */
	g_assert_true(tabs_action(module, k3, GSURF_ACTION_TAB_CLOSE));
	g_assert_cmpuint(gsurf_window_get_n_views(c.window), ==, 1);

	gsurf_module_deactivate(module);
	g_object_unref(k1);
	g_object_unref(k2);
	g_object_unref(k3);
	chrome_teardown(&c);
}

/* Every label of @model in order, "|"-terminated; a submenu's labels
 * are bracketed after its own. */
static void
append_menu_labels(GMenuModel *model, GString *out)
{
	gint i;

	for (i = 0; i < g_menu_model_get_n_items(model); i++) {
		g_autofree gchar *label = NULL;
		g_autoptr(GMenuModel) section = NULL;
		g_autoptr(GMenuModel) submenu = NULL;

		if (g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label))
			g_string_append_printf(out, "%s|", label);
		section = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION);
		if (section != NULL)
			append_menu_labels(section, out);
		submenu = g_menu_model_get_item_link(model, i, G_MENU_LINK_SUBMENU);
		if (submenu != NULL) {
			g_string_append_c(out, '[');
			append_menu_labels(submenu, out);
			g_string_append_c(out, ']');
		}
	}
}

/* The labels of the tab menu @module opened last. */
static gchar *
tab_menu_labels(GsurfModule *module)
{
	GMenuModel *model = g_object_get_data(G_OBJECT(module), "gsurf-tabs-menu-model");
	GString *out = g_string_new(NULL);

	g_assert_nonnull(model);
	append_menu_labels(model, out);
	return g_string_free(out, FALSE);
}

/* The tab context menu without containers (no placement policy): no
 * container entries; tree entries only on a tab with children; its
 * actions act on the tab that was right-clicked; a strip tab opens the
 * same menu; off screen no menu widget is made; deactivation drops the
 * actions; reactivation brings them back. */
static void
test_tabs_menu(void)
{
	Chrome c;
	GsurfModule *module;
	GActionGroup *actions;
	GsurfView *k1;
	GsurfView *k2;
	GtkWidget *strip;
	GtkWidget *tab = NULL;
	GtkWidget *button = NULL;
	gchar *labels;

	if (!chrome_setup(&c, "tabs", &module))
		return;
	configure_tabs(module, "modules:\n  tabs:\n    enabled: true\n");
	g_assert_true(gsurf_module_activate(module));
	actions = g_object_get_data(G_OBJECT(module), "gsurf-tabs-menu-actions");
	g_assert_nonnull(actions);
	k1 = tab_view(c.first, NULL, "k1");
	k2 = tab_view(c.first, NULL, "k2");
	gsurf_window_add_view(c.window, k1);
	gsurf_window_add_view(c.window, k2);

	/* 1. A parent: tree entries, no containers, no on-screen menu. */
	click_row(row_of(c.window, c.first), 3);
	labels = tab_menu_labels(module);
	g_assert_cmpstr(labels, ==,
		"Reload Tab|Duplicate Tab|Collapse Tree|Close Children|Close Tab|");
	g_free(labels);
	g_assert_null(g_object_get_data(G_OBJECT(module), "gsurf-tabs-menu-widget"));
	g_assert_true(g_action_group_get_action_enabled(actions, "close"));

	/* 2. Its tree actions act on it: collapse (the menu then offers
	 *    Expand), expand. */
	g_action_group_activate_action(actions, "toggle-collapse", NULL);
	g_assert_cmpstr(twisty_of(c.window, c.first), ==, "▸");
	click_row(row_of(c.window, c.first), 3);
	labels = tab_menu_labels(module);
	g_assert_cmpstr(labels, ==,
		"Reload Tab|Duplicate Tab|Expand Tree|Close Children|Close Tab|");
	g_free(labels);
	g_action_group_activate_action(actions, "toggle-collapse", NULL);
	g_assert_cmpstr(twisty_of(c.window, c.first), ==, "▾");

	/* 3. A leaf: no tree entries. Close closes that tab, not the
	 *    active one. */
	click_row(row_of(c.window, k2), 3);
	labels = tab_menu_labels(module);
	g_assert_cmpstr(labels, ==, "Reload Tab|Duplicate Tab|Close Tab|");
	g_free(labels);
	gsurf_window_set_active_view(c.window, c.second);
	g_action_group_activate_action(actions, "close", NULL);
	g_assert_cmpint(gsurf_window_get_view_index(c.window, k2), <, 0);
	g_assert_true(gsurf_window_get_active_view(c.window) == c.second);

	/* 4. Close Children on the parent keeps the parent. */
	click_row(row_of(c.window, c.first), 3);
	g_action_group_activate_action(actions, "close-children", NULL);
	g_assert_cmpint(gsurf_window_get_view_index(c.window, k1), <, 0);
	g_assert_cmpint(gsurf_window_get_view_index(c.window, c.first), >=, 0);
	g_assert_cmpuint(gsurf_window_get_n_views(c.window), ==, 2);

	/* 5. A strip tab opens the same menu for its tab. */
	strip = only_child(c.window);
	g_assert_cmpuint(box_children(strip, &tab, NULL), ==, 2);
	box_children(tab, &button, NULL);
	g_assert_true(GTK_IS_BUTTON(button));
	click_row(button, 3);
	labels = tab_menu_labels(module);
	g_assert_cmpstr(labels, ==, "Reload Tab|Duplicate Tab|Close Tab|");
	g_free(labels);

	/* 6. Deactivation drops the actions and the model; reactivation
	 *    makes new ones. */
	gsurf_module_deactivate(module);
	g_assert_null(g_object_get_data(G_OBJECT(module), "gsurf-tabs-menu-actions"));
	g_assert_null(g_object_get_data(G_OBJECT(module), "gsurf-tabs-menu-model"));
	g_assert_true(gsurf_module_activate(module));
	g_assert_nonnull(g_object_get_data(G_OBJECT(module), "gsurf-tabs-menu-actions"));

	gsurf_module_deactivate(module);
	g_object_unref(k1);
	g_object_unref(k2);
	chrome_teardown(&c);
}

/* --- Tabs: a window layout (panes) --- */

/* A fake #GsurfWindowLayout: no widgets; the active view plus the views
 * in @shown are "on screen". The tests change @shown and call
 * gsurf_window_layout_notify_visible() as a real layout does. */
typedef struct {
	GObject     parent;
	GObject    *root;   /* stands in for the native root widget */
	GHashTable *shown;  /* GsurfView* set, borrowed */
} PaneLayout;
typedef struct { GObjectClass parent; } PaneLayoutClass;

static void pane_layout_iface_init(GsurfWindowLayoutInterface *iface);
GType pane_layout_get_type(void);
G_DEFINE_TYPE_WITH_CODE(PaneLayout, pane_layout, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_WINDOW_LAYOUT, pane_layout_iface_init))
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PaneLayout, g_object_unref)

static gpointer pane_get_root(GsurfWindowLayout *l) { return ((PaneLayout *)l)->root; }
static void pane_attach(GsurfWindowLayout *l, GsurfWindow *w) { (void)l; (void)w; }
static void pane_add(GsurfWindowLayout *l, GsurfWindow *w, GsurfView *v) { (void)l; (void)w; (void)v; }
static void pane_activate(GsurfWindowLayout *l, GsurfWindow *w, GsurfView *v) { (void)l; (void)w; (void)v; }

static void
pane_detach(GsurfWindowLayout *l, GsurfWindow *w)
{
	(void)w;
	g_hash_table_remove_all(((PaneLayout *)l)->shown);
}

static void
pane_remove(GsurfWindowLayout *l, GsurfWindow *w, GsurfView *v)
{
	(void)w;
	g_hash_table_remove(((PaneLayout *)l)->shown, v);
}

static gboolean
pane_visible(GsurfWindowLayout *l, GsurfWindow *w, GsurfView *v)
{
	return gsurf_window_get_active_view(w) == v ||
	       g_hash_table_contains(((PaneLayout *)l)->shown, v);
}

static void
pane_layout_iface_init(GsurfWindowLayoutInterface *iface)
{
	iface->get_root = pane_get_root;
	iface->attach = pane_attach;
	iface->detach = pane_detach;
	iface->add_view = pane_add;
	iface->remove_view = pane_remove;
	iface->activate_view = pane_activate;
	iface->is_view_visible = pane_visible;
}

static void
pane_layout_finalize(GObject *object)
{
	PaneLayout *self = (PaneLayout *)object;

	g_clear_object(&self->root);
	g_hash_table_unref(self->shown);
	G_OBJECT_CLASS(pane_layout_parent_class)->finalize(object);
}

static void
pane_layout_class_init(PaneLayoutClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pane_layout_finalize;
}

static void
pane_layout_init(PaneLayout *self)
{
	self->root = g_object_new(G_TYPE_OBJECT, NULL);
	self->shown = g_hash_table_new(g_direct_hash, g_direct_equal);
}

/* Show (or hide) @view in a pane of @layout and tell the window. */
static void
pane_show(PaneLayout *layout, GsurfWindow *window, GsurfView *view, gboolean shown)
{
	if (shown)
		g_hash_table_add(layout->shown, view);
	else
		g_hash_table_remove(layout->shown, view);
	gsurf_window_layout_notify_visible(window);
}

/* A stand-in for the module owning the panes (wmux): records the
 * pane-split actions it is offered and the view they came with. */
typedef struct {
	GsurfModule  parent;
	GsurfAction  last;
	GsurfView   *view;   /* borrowed, compared only */
	guint        calls;
} SplitModule;
typedef struct { GsurfModuleClass parent; } SplitModuleClass;

static void split_module_iface_init(GsurfActionHandlerInterface *iface);
GType split_module_get_type(void);
G_DEFINE_TYPE_WITH_CODE(SplitModule, split_module, GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_ACTION_HANDLER, split_module_iface_init))
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SplitModule, g_object_unref)

static gboolean
split_module_handle(GsurfActionHandler *handler, GsurfView *view, GsurfAction action)
{
	SplitModule *self = (SplitModule *)handler;

	if (action != GSURF_ACTION_PANE_SPLIT_RIGHT && action != GSURF_ACTION_PANE_SPLIT_DOWN)
		return FALSE;
	self->last = action;
	self->view = view;
	self->calls++;
	return TRUE;
}

static void
split_module_iface_init(GsurfActionHandlerInterface *iface)
{
	iface->handle_action = split_module_handle;
}

static const gchar *split_module_name(GsurfModule *m) { (void)m; return "test-split"; }

static void
split_module_class_init(SplitModuleClass *klass)
{
	GSURF_MODULE_CLASS(klass)->get_name = split_module_name;
}

static void split_module_init(SplitModule *self) { self->last = GSURF_ACTION_NONE; }

/* The strip tab (its outer box) of @view. */
static GtkWidget *
strip_tab_of(GsurfWindow *window, GsurfView *view)
{
	g_autoptr(GPtrArray) tabs = children_of(only_child(window));
	guint i;

	for (i = 0; i < tabs->len; i++) {
		GtkWidget *button = NULL;

		box_children(g_ptr_array_index(tabs, i), &button, NULL);
		if (widget_id(button) == gsurf_view_get_id(view))
			return g_ptr_array_index(tabs, i);
	}
	g_assert_not_reached();
	return NULL;
}

/* Tabs shown in another pane get the "visible" style in the sidebar and
 * the strip, following visible-views-changed; the active tab keeps the
 * active style only; without a layout nothing is marked. A row is only
 * touched when its state changes. The stylesheet carries visible_color,
 * which is validated and reset by a second configure(). Deactivation
 * disconnects visible-views-changed. */
static void
test_tabs_visible(void)
{
	g_autoptr(PaneLayout) layout = NULL;
	g_autofree gchar *css = NULL;
	Chrome c;
	GsurfModule *module;
	GsurfView *third;

	if (!chrome_setup(&c, "tabs", &module))
		return;
	configure_tabs(module, "modules:\n  tabs:\n    enabled: true\n"
		"    visible_color: '#123456'\n");
	g_assert_true(gsurf_module_activate(module));
	third = tab_view(NULL, NULL, "third");
	gsurf_window_add_view(c.window, third);
	gsurf_window_set_active_view(c.window, c.first);
	css = tabs_css(module);
	g_assert_nonnull(strstr(css, "gsurf-tab-row-visible"));
	g_assert_nonnull(strstr(css, "gsurf-tab-visible"));
	g_assert_nonnull(strstr(css, "rgb(18,52,86)"));
	g_clear_pointer(&css, g_free);

	/* 1. No layout: only the active view is visible, nothing is marked. */
	g_assert_false(has_class(row_of(c.window, c.second), "gsurf-tab-row-visible"));
	g_assert_false(has_class(strip_tab_of(c.window, c.second), "gsurf-tab-visible"));

	/* 2. A layout showing the second tab in another pane marks it, not
	 *    the active tab and not the background tab. */
	layout = g_object_new(pane_layout_get_type(), NULL);
	g_hash_table_add(layout->shown, c.second);
	g_assert_true(gsurf_window_set_layout(c.window, GSURF_WINDOW_LAYOUT(layout)));
	g_assert_true(((TestChromeWindow *)c.window)->content == layout->root);
	g_assert_true(has_class(row_of(c.window, c.second), "gsurf-tab-row-visible"));
	g_assert_true(has_class(strip_tab_of(c.window, c.second), "gsurf-tab-visible"));
	g_assert_false(has_class(row_of(c.window, c.first), "gsurf-tab-row-visible"));
	g_assert_true(has_class(row_of(c.window, c.first), "gsurf-tab-row-active"));
	g_assert_false(has_class(strip_tab_of(c.window, c.first), "gsurf-tab-visible"));
	g_assert_false(has_class(row_of(c.window, third), "gsurf-tab-row-visible"));

	/* 3. The layout swaps panes: the marks follow. */
	g_hash_table_remove(layout->shown, c.second);
	pane_show(layout, c.window, third, TRUE);
	g_assert_false(has_class(row_of(c.window, c.second), "gsurf-tab-row-visible"));
	g_assert_true(has_class(row_of(c.window, third), "gsurf-tab-row-visible"));
	g_assert_true(has_class(strip_tab_of(c.window, third), "gsurf-tab-visible"));

	/* 4. Activating the tab in the other pane: active style, not both. */
	gsurf_window_set_active_view(c.window, third);
	g_assert_true(has_class(row_of(c.window, third), "gsurf-tab-row-active"));
	g_assert_false(has_class(row_of(c.window, third), "gsurf-tab-row-visible"));
	g_assert_false(has_class(strip_tab_of(c.window, third), "gsurf-tab-visible"));
	gsurf_window_set_active_view(c.window, c.first);
	g_assert_true(has_class(row_of(c.window, third), "gsurf-tab-row-visible"));

	/* 5. Removing the layout: nothing is marked any more. */
	g_assert_true(gsurf_window_set_layout(c.window, NULL));
	g_assert_false(has_class(row_of(c.window, third), "gsurf-tab-row-visible"));
	g_assert_false(has_class(strip_tab_of(c.window, third), "gsurf-tab-visible"));

	/* 6. A bad visible_color warns and keeps the default; a second
	 *    configure() without the key goes back to the default too. */
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*tabs.visible_color*not a CSS colour*");
	configure_tabs(module, "modules:\n  tabs:\n    visible_color: 'nope; }'\n");
	g_test_assert_expected_messages();
	gsurf_module_deactivate(module);
	g_assert_true(gsurf_module_activate(module));
	css = tabs_css(module);
	g_assert_null(strstr(css, "rgb(18,52,86)"));
	g_assert_nonnull(strstr(css, "rgb(30,30,46)"));
	g_clear_pointer(&css, g_free);

	/* 7. Deactivation disconnects visible-views-changed with the rest:
	 *    a later change reaches nothing of the module. */
	g_assert_true(gsurf_window_set_layout(c.window, GSURF_WINDOW_LAYOUT(layout)));
	gsurf_module_deactivate(module);
	assert_disconnected(c.window, module);
	pane_show(layout, c.window, c.second, TRUE);
	g_assert_true(gsurf_window_set_layout(c.window, NULL));

	gsurf_window_remove_view(c.window, third);
	g_object_unref(third);
	chrome_teardown(&c);
}

/* With a layout installed the tab menu offers Open in Split Right and
 * Below; they dispatch pane-split-right / pane-split-down with the view
 * of the tab the menu was opened on (not the active one) to the module
 * owning the panes. Without a layout the entries are gone. */
static void
test_tabs_menu_split(void)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	g_autoptr(PaneLayout) layout = NULL;
	g_autoptr(SplitModule) split = NULL;
	GActionGroup *actions;
	GtkWidget *button = NULL;
	Chrome c;
	GsurfModule *module;
	gchar *labels;

	if (!chrome_setup(&c, "tabs", &module))
		return;
	split = g_object_new(split_module_get_type(), NULL);
	gsurf_module_manager_add_module(mgr, GSURF_MODULE(split));
	g_assert_true(gsurf_module_activate(GSURF_MODULE(split)));
	configure_tabs(module, "modules:\n  tabs:\n    enabled: true\n");
	g_assert_true(gsurf_module_activate(module));
	actions = g_object_get_data(G_OBJECT(module), "gsurf-tabs-menu-actions");
	g_assert_nonnull(actions);

	/* 1. No layout: no split entries. */
	click_row(row_of(c.window, c.second), 3);
	labels = tab_menu_labels(module);
	g_assert_cmpstr(labels, ==, "Reload Tab|Duplicate Tab|Close Tab|");
	g_free(labels);

	/* 2. With a layout: the entries, after Reload/Duplicate. */
	layout = g_object_new(pane_layout_get_type(), NULL);
	g_assert_true(gsurf_window_set_layout(c.window, GSURF_WINDOW_LAYOUT(layout)));
	gsurf_window_set_active_view(c.window, c.first);
	click_row(row_of(c.window, c.second), 3);
	labels = tab_menu_labels(module);
	g_assert_cmpstr(labels, ==, "Reload Tab|Duplicate Tab|Open in Split Right|"
		"Open in Split Below|Close Tab|");
	g_free(labels);

	/* 3. They dispatch with the menu's tab, not the active one. */
	g_action_group_activate_action(actions, "split-right", NULL);
	g_assert_cmpuint(split->calls, ==, 1);
	g_assert_cmpint(split->last, ==, GSURF_ACTION_PANE_SPLIT_RIGHT);
	g_assert_true(split->view == c.second);
	click_row(row_of(c.window, c.first), 3);
	g_action_group_activate_action(actions, "split-down", NULL);
	g_assert_cmpuint(split->calls, ==, 2);
	g_assert_cmpint(split->last, ==, GSURF_ACTION_PANE_SPLIT_DOWN);
	g_assert_true(split->view == c.first);

	/* 4. A strip tab (its label button) offers them too. */
	box_children(strip_tab_of(c.window, c.second), &button, NULL);
	click_row(button, 3);
	labels = tab_menu_labels(module);
	g_assert_nonnull(strstr(labels, "Open in Split Right|Open in Split Below|"));
	g_free(labels);

	/* 5. The layout goes while the menu's actions live: they do nothing
	 *    (nothing owns panes), and the next menu has no entries. */
	g_assert_true(gsurf_window_set_layout(c.window, NULL));
	g_action_group_activate_action(actions, "split-right", NULL);
	g_assert_cmpuint(split->calls, ==, 2);
	click_row(row_of(c.window, c.second), 3);
	labels = tab_menu_labels(module);
	g_assert_null(strstr(labels, "Split"));
	g_free(labels);

	/* 6. A closed tab: the actions are no-ops. */
	g_assert_true(gsurf_window_set_layout(c.window, GSURF_WINDOW_LAYOUT(layout)));
	click_row(row_of(c.window, c.second), 3);
	gsurf_window_remove_view(c.window, c.second);
	g_action_group_activate_action(actions, "split-down", NULL);
	g_assert_cmpuint(split->calls, ==, 2);
	g_assert_true(gsurf_window_set_layout(c.window, NULL));

	gsurf_module_deactivate(module);
	gsurf_module_deactivate(GSURF_MODULE(split));
	chrome_teardown(&c);
}

/* Container stripes follow the registry: colour, name, added, removed. */
static void
test_tabs_containers(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *css = NULL;
	g_autofree gchar *css2 = NULL;
	Chrome c;
	GsurfModule *module;
	GsurfView *work;
	GtkWidget *stripe;

	if (!chrome_setup(&c, "tabs", &module))
		return;
	g_assert_nonnull(gsurf_container_registry_define(registry, "work", "Work", "#ff0000", &error));
	g_assert_no_error(error);
	configure_tabs(module, "modules:\n  tabs:\n    enabled: true\n    sidebar: true\n");
	g_assert_true(gsurf_module_activate(module));

	/* A view in a container gets a coloured stripe and names it. */
	work = tab_view(NULL, "work", "w");
	g_assert_cmpstr(gsurf_view_get_container_id(work), ==, "work");
	gsurf_window_add_view(c.window, work);
	stripe = row_part(c.window, work, "gsurf-tab-stripe");
	g_assert_true(has_class(stripe, "gsurf-ctr-work"));
	g_assert_true(has_class(stripe, "gsurf-tab-stripe-on"));
	g_assert_cmpstr(gtk_widget_get_tooltip_text(stripe), ==, "Work");
	g_assert_nonnull(strstr(gtk_widget_get_tooltip_text(row_of(c.window, work)), "Container: Work"));
	css = tabs_css(module);
	g_assert_nonnull(strstr(css, "gsurf-ctr-work"));
	/* Every module rule parsed (GTK drops what it cannot parse). */
	g_assert_nonnull(strstr(css, "gsurf-tab-row-active"));
	g_assert_nonnull(strstr(css, "min-height"));
	g_assert_nonnull(strstr(css, "min-width"));
	g_assert_nonnull(strstr(css, "font-weight"));
	g_assert_nonnull(strstr(css, "gsurf-tab-stripe-on"));
	/* The default container shows no stripe. */
	stripe = row_part(c.window, c.first, "gsurf-tab-stripe");
	g_assert_false(has_class(stripe, "gsurf-tab-stripe-on"));
	g_assert_null(gtk_widget_get_tooltip_text(stripe));

	/* Renaming and recolouring update the row and the stylesheet. */
	g_assert_nonnull(gsurf_container_registry_define(registry, "work", "Job", "#00ff00", &error));
	stripe = row_part(c.window, work, "gsurf-tab-stripe");
	g_assert_cmpstr(gtk_widget_get_tooltip_text(stripe), ==, "Job");
	css2 = tabs_css(module);
	g_assert_cmpstr(css, !=, css2);
	g_clear_pointer(&css, g_free);

	/* New and removed containers appear in and leave the stylesheet; a
	 * colour that is not CSS never reaches it. */
	g_assert_nonnull(gsurf_container_registry_define(registry, "play", "Play", "#0000ff", &error));
	g_assert_nonnull(gsurf_container_registry_define(registry, "bad", "Bad", "red; } * {", &error));
	css = tabs_css(module);
	g_assert_nonnull(strstr(css, "gsurf-ctr-play"));
	g_assert_null(strstr(css, "gsurf-ctr-bad"));
	g_clear_pointer(&css, g_free);
	g_assert_true(gsurf_container_registry_undefine(registry, "play", &error));
	g_assert_true(gsurf_container_registry_undefine(registry, "bad", &error));
	css = tabs_css(module);
	g_assert_null(strstr(css, "gsurf-ctr-play"));

	/* Deactivation drops the container handlers. */
	gsurf_window_remove_view(c.window, work);
	gsurf_module_deactivate(module);
	assert_disconnected(registry, module);
	assert_disconnected(gsurf_container_registry_lookup(registry, "work"), module);
	g_object_unref(work);
	chrome_teardown(&c);
	g_assert_true(gsurf_container_registry_undefine(registry, "work", &error));
	g_assert_no_error(error);
}

/* Kiosk: no strip, no sidebar; the tree keys are harmless. */
static void
test_tabs_kiosk(void)
{
	g_autoptr(GsurfConfig) kiosk = gsurf_config_new();
	Chrome c;
	GsurfModule *module;

	if (!chrome_setup(&c, "tabs", &module))
		return;
	kiosk->kiosk = TRUE;
	gsurf_config_set_default(kiosk);
	g_assert_true(gsurf_module_activate(module));
	g_assert_cmpuint(box_children(((TestChromeWindow *)c.window)->box, NULL, NULL), ==, 0);
	g_assert_cmpuint(box_children(((TestChromeWindow *)c.window)->side, NULL, NULL), ==, 0);
	/* No menu at all, so no Open in Split either (kiosk also refuses a
	 * window layout). */
	g_assert_null(g_object_get_data(G_OBJECT(module), "gsurf-tabs-menu-actions"));
	g_assert_true(tabs_action(module, c.first, GSURF_ACTION_TAB_SIDEBAR_TOGGLE));
	g_assert_true(tabs_action(module, c.first, GSURF_ACTION_TAB_TREE_COLLAPSE));
	g_assert_true(tabs_action(module, c.first, GSURF_ACTION_TAB_MOVE_DOWN));
	g_assert_cmpuint(box_children(((TestChromeWindow *)c.window)->side, NULL, NULL), ==, 0);
	gsurf_module_deactivate(module);
	gsurf_config_set_default(NULL);
	chrome_teardown(&c);
}
#endif

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#if defined(GSURF_BACKEND_GTK3) || defined(GSURF_BACKEND_GTK4)
	g_test_add_data_func("/gsurf/chrome/status-bar", "status_bar", test_chrome_lifetime);
	g_test_add_data_func("/gsurf/chrome/address-bar", "chromebar", test_chrome_lifetime);
	g_test_add_data_func("/gsurf/chrome/find-bar", "find_bar", test_chrome_lifetime);
	g_test_add_func("/gsurf/chrome/address-bar-keys", test_address_bar_keys);
	g_test_add_func("/gsurf/chrome/find-bar-keys", test_find_bar_keys);
	g_test_add_func("/gsurf/chrome/tabs", test_tabs_lifetime);
	g_test_add_func("/gsurf/chrome/tabs-sidebar", test_tabs_sidebar_tree);
	g_test_add_func("/gsurf/chrome/tabs-close-children", test_tabs_close_children);
	g_test_add_func("/gsurf/chrome/tabs-menu", test_tabs_menu);
	g_test_add_func("/gsurf/chrome/tabs-visible", test_tabs_visible);
	g_test_add_func("/gsurf/chrome/tabs-menu-split", test_tabs_menu_split);
	g_test_add_func("/gsurf/chrome/tabs-containers", test_tabs_containers);
	g_test_add_func("/gsurf/chrome/tabs-kiosk", test_tabs_kiosk);
#endif
	return g_test_run();
}
