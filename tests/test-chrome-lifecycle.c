/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-chrome-lifecycle.c - window-chrome module lifetimes and key handling
 *
 * Loads the real chromebar, find_bar and status_bar shared modules against
 * model-only views and a plain GtkBox standing in for the window chrome, on
 * GTK3 and GTK4 alike (the modules share modules/include/gsurf-chrome-compat.h).
 * Covers: tab-switch state, deactivation, reactivation, retained widgets,
 * repeated disposal, the host container dying first, and the address-bar /
 * find-bar key handling (Escape, Enter, Shift+Enter) that each backend
 * implements differently (GTK3 "key-press-event", GTK4 a capture-phase
 * GtkEventControllerKey).
 *
 * The tabs module is not covered: it is still GTK3-only and is being
 * rewritten separately.
 *
 * Needs a display, so every case skips unless GSURF_TEST_GUI=1 (make
 * test-gui runs it under Xvfb). No web process or network is used.
 */
#include <gsurf.h>

#if defined(GSURF_BACKEND_GTK3) || defined(GSURF_BACKEND_GTK4)
#include <gtk/gtk.h>

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
} TestChromeView;
typedef struct { GsurfViewClass parent; } TestChromeViewClass;
typedef struct { GsurfWindow parent; GtkWidget *box; } TestChromeWindow;
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

static void
test_chrome_view_finalize(GObject *object)
{
	TestChromeView *self = (TestChromeView *)object;

	g_free(self->loaded);
	g_free(self->find_text);
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

static void
test_chrome_window_dispose(GObject *object)
{
	TestChromeWindow *self = (TestChromeWindow *)object;

	if (self->box != NULL) {
		/* GTK3 destroys the container (removing its children); a GTK4
		 * GtkBox removes its children when it is disposed. */
#ifdef GSURF_BACKEND_GTK4
		g_object_run_dispose(G_OBJECT(self->box));
#else
		gtk_widget_destroy(self->box);
#endif
		g_clear_object(&self->box);
	}
	G_OBJECT_CLASS(test_chrome_window_parent_class)->dispose(object);
}

static void
test_chrome_window_class_init(TestChromeWindowClass *klass)
{
	GSURF_WINDOW_CLASS(klass)->add_chrome_widget = add_chrome;
	G_OBJECT_CLASS(klass)->dispose = test_chrome_window_dispose;
}

static void
test_chrome_window_init(TestChromeWindow *self)
{
	self->box = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
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
#endif
	return g_test_run();
}
