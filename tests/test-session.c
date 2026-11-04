/*
 * test-session.c - The session module: YAML format, migration, restore
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the real session.so through a module manager. Saving, the file
 * format, migration, error paths and reconfiguring run headless with
 * minimal GsurfView/GsurfWindow subclasses. Restoring creates real
 * views (gsurf_view_new_full() uses the compiled backend), so those
 * tests need WebKit and run only with GSURF_TEST_GUI=1 (make test-gui).
 *
 * Window layouts: a fake #GsurfWindowLayout (FakeLayout) serializes as
 * "g<generation>:<index>,<index>,..." (the index the module maps each
 * window view to, -1 included) and records what restore() is given.
 */

#include <gsurf.h>
#include <glib/gstdio.h>
#include <yaml-glib.h>
#include <string.h>
#include <sys/stat.h>

#include "test-util.h"
#include "webkit-test-util.h"

/* --- headless view / window --- */

typedef struct {
	GsurfView  parent;
	gchar     *uri;
} TestView;
typedef struct { GsurfViewClass parent; } TestViewClass;
typedef struct { GsurfWindow parent; } TestWindow;
typedef struct { GsurfWindowClass parent; } TestWindowClass;

GType test_view_get_type(void);
GType test_window_get_type(void);
G_DEFINE_TYPE(TestView, test_view, GSURF_TYPE_VIEW)
G_DEFINE_TYPE(TestWindow, test_window, GSURF_TYPE_WINDOW)

static void
test_view_load_uri(GsurfView *view, const gchar *uri)
{
	g_free(((TestView *)view)->uri);
	((TestView *)view)->uri = g_strdup(uri);
}

static const gchar *
test_view_get_uri(GsurfView *view)
{
	return ((TestView *)view)->uri;
}

static void
test_view_finalize(GObject *object)
{
	g_free(((TestView *)object)->uri);
	G_OBJECT_CLASS(test_view_parent_class)->finalize(object);
}

static void
test_view_class_init(TestViewClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = test_view_finalize;
	GSURF_VIEW_CLASS(klass)->load_uri = test_view_load_uri;
	GSURF_VIEW_CLASS(klass)->get_uri = test_view_get_uri;
}

static void test_view_init(TestView *self) { (void)self; }

/* A window layout needs somewhere to put its root (set_content); the
 * headless window just ignores it. */
static void
test_window_set_content(GsurfWindow *window, gpointer widget)
{
	(void)window;
	(void)widget;
}

static void
test_window_class_init(TestWindowClass *klass)
{
	GSURF_WINDOW_CLASS(klass)->set_content = test_window_set_content;
}

static void test_window_init(TestWindow *self) { (void)self; }

/* --- a fake window layout --- */

typedef enum {
	FAKE_SERIALIZE_INDICES,   /* "g<generation>:<i>,<j>,..." */
	FAKE_SERIALIZE_NULL,      /* cannot serialize */
	FAKE_SERIALIZE_EMPTY      /* "" */
} FakeSerialize;

typedef struct {
	GObject       parent;
	GObject      *root;            /* GtkWidget in GUI tests, else a GObject */
	FakeSerialize serialize;
	guint         generation;      /* part of the serialized state */
	guint         serialize_calls;
	gboolean      restore_result;  /* what restore() answers */
	guint         restore_calls;
	gchar        *restored_state;  /* last restore() state */
	GPtrArray    *restored_views;  /* last restore() views (borrowed, NULLs kept) */
	GsurfView    *active_at_restore;
} FakeLayout;
typedef struct { GObjectClass parent; } FakeLayoutClass;

static void fake_layout_iface_init(GsurfWindowLayoutInterface *iface);
GType fake_layout_get_type(void);
G_DEFINE_TYPE_WITH_CODE(FakeLayout, fake_layout, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_WINDOW_LAYOUT, fake_layout_iface_init))

static gpointer fake_get_root(GsurfWindowLayout *l) { return ((FakeLayout *)l)->root; }
static void fake_attach(GsurfWindowLayout *l, GsurfWindow *w) { (void)l; (void)w; }
static void fake_detach(GsurfWindowLayout *l, GsurfWindow *w) { (void)l; (void)w; }
static void fake_add(GsurfWindowLayout *l, GsurfWindow *w, GsurfView *v) { (void)l; (void)w; (void)v; }
static void fake_remove(GsurfWindowLayout *l, GsurfWindow *w, GsurfView *v) { (void)l; (void)w; (void)v; }
static void fake_activate(GsurfWindowLayout *l, GsurfWindow *w, GsurfView *v) { (void)l; (void)w; (void)v; }

static gchar *
fake_serialize(GsurfWindowLayout *layout, GsurfWindow *window,
               GsurfWindowLayoutIndexFunc map, gpointer user_data)
{
	FakeLayout *self = (FakeLayout *)layout;
	GString *out;
	guint i;

	self->serialize_calls++;
	if (self->serialize == FAKE_SERIALIZE_NULL)
		return NULL;
	if (self->serialize == FAKE_SERIALIZE_EMPTY)
		return g_strdup("");
	out = g_string_new(NULL);
	g_string_append_printf(out, "g%u:", self->generation);
	for (i = 0; i < gsurf_window_get_n_views(window); i++)
		g_string_append_printf(out, "%s%d", i > 0 ? "," : "",
			map(gsurf_window_get_nth_view(window, i), user_data));
	return g_string_free(out, FALSE);
}

static gboolean
fake_restore(GsurfWindowLayout *layout, GsurfWindow *window, const gchar *state,
             GsurfView **views, guint n_views)
{
	FakeLayout *self = (FakeLayout *)layout;
	guint i;

	self->restore_calls++;
	g_free(self->restored_state);
	self->restored_state = g_strdup(state);
	g_ptr_array_set_size(self->restored_views, 0);
	for (i = 0; i < n_views; i++)
		g_ptr_array_add(self->restored_views, views[i]);
	self->active_at_restore = gsurf_window_get_active_view(window);
	/* Like a real layout, rearranging changes what is on screen. */
	gsurf_window_layout_notify_visible(window);
	return self->restore_result;
}

static void
fake_layout_iface_init(GsurfWindowLayoutInterface *iface)
{
	iface->get_root = fake_get_root;
	iface->attach = fake_attach;
	iface->detach = fake_detach;
	iface->add_view = fake_add;
	iface->remove_view = fake_remove;
	iface->activate_view = fake_activate;
	iface->serialize = fake_serialize;
	iface->restore = fake_restore;
}

static void
fake_layout_finalize(GObject *object)
{
	FakeLayout *self = (FakeLayout *)object;

	g_clear_object(&self->root);
	g_free(self->restored_state);
	g_ptr_array_unref(self->restored_views);
	G_OBJECT_CLASS(fake_layout_parent_class)->finalize(object);
}

static void
fake_layout_class_init(FakeLayoutClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = fake_layout_finalize;
}

static void
fake_layout_init(FakeLayout *self)
{
	self->restore_result = TRUE;
	self->restored_views = g_ptr_array_new();
}

/* A fake layout whose root is @root (owned; a floating GtkWidget is
 * sunk), installed in @window. Returns a borrowed pointer: the window
 * keeps the layout alive until it is disposed. */
static FakeLayout *
install_fake_layout(GsurfWindow *window, GObject *root)
{
	FakeLayout *layout = g_object_new(fake_layout_get_type(), NULL);

	layout->root = g_object_ref_sink(root);
	g_assert_true(gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(layout)));
	g_object_unref(layout);
	return layout;
}

/* Run pending idles (the coalesced layout save), bounded. */
static void
drain_idles(void)
{
	guint i;

	for (i = 0; i < 100 && g_main_context_iteration(NULL, FALSE); i++)
		;
}

/* A headless tab in @window showing @uri. */
static GsurfView *
add_view(GsurfWindow *window, GsurfView *opener, const gchar *container, const gchar *uri)
{
	GsurfView *view = g_object_new(test_view_get_type(),
		"opener", opener, "container-id", container, NULL);

	if (uri != NULL)
		gsurf_view_load_uri(view, uri);
	gsurf_window_add_view(window, view);
	g_object_unref(view);
	return view;
}

/* --- host and module --- */

typedef struct {
	gchar              *dir;      /* private temp dir for session files */
	gchar              *file;     /* <dir>/session.yaml */
	GsurfConfig        *config;
	GsurfApplication   *app;
	GsurfWindow        *window;
	GsurfModuleManager *mgr;
	GsurfModule        *module;
} Host;

static gchar *
module_so_path(const gchar *name)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);
	g_autofree gchar *base = g_strconcat(name, ".so", NULL);

	return g_build_filename(dir, "modules", base, NULL);
}

static GsurfConfig *
config_from(const gchar *yaml)
{
	g_autoptr(GError) error = NULL;
	GsurfConfig *config = gsurf_config_new();

	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	return config;
}

/* The standard module section for @host, plus @extra lines. */
static gchar *
section(Host *host, gboolean auto_restore, const gchar *extra)
{
	return g_strdup_printf("modules:\n  session:\n    enabled: true\n"
		"    auto_restore: %s\n    file: '%s'\n%s",
		auto_restore ? "true" : "false", host->file, extra != NULL ? extra : "");
}

/* Prepare the host (application + window on the default manager)
 * without loading the module; @window may be a real or headless one. */
static void
host_init(Host *host, GsurfWindow *window)
{
	memset(host, 0, sizeof(*host));
	host->dir = g_dir_make_tmp("gsurf-session-XXXXXX", NULL);
	g_assert_nonnull(host->dir);
	host->file = g_build_filename(host->dir, "session.yaml", NULL);
	host->config = gsurf_config_new();
	gsurf_config_set_default(host->config);
	host->app = gsurf_application_new(host->config);
	host->window = window;
	gsurf_application_add_window(host->app, window);
	gsurf_module_manager_set_application(gsurf_module_manager_get_default(), host->app);
}

/* Load and activate session.so with @yaml. FALSE (skipped) without it. */
static gboolean
host_load(Host *host, const gchar *yaml)
{
	g_autofree gchar *so = module_so_path("session");
	g_autoptr(GsurfConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("session.so not built");
		return FALSE;
	}
	config = config_from(yaml);
	host->mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(host->mgr, config);
	host->module = gsurf_module_manager_load_module(host->mgr, so, &error);
	g_assert_no_error(error);
	g_assert_nonnull(host->module);
	gsurf_module_manager_activate_all(host->mgr);
	return TRUE;
}

static void
host_free(Host *host)
{
	if (host->mgr != NULL) {
		gpointer weak = host->module;

		g_object_add_weak_pointer(G_OBJECT(host->module), &weak);
		g_clear_object(&host->mgr);
		g_assert_null(weak);
	}
	gsurf_module_manager_set_application(gsurf_module_manager_get_default(), NULL);
	gsurf_config_set_default(NULL);
	g_clear_object(&host->window);
	g_clear_object(&host->app);
	g_clear_object(&host->config);
	gsurf_test_rmtree(host->dir);
	g_free(host->dir);
	g_free(host->file);
}

/* Press the save key (Ctrl+Shift+s by default). */
static gboolean
press_save(Host *host)
{
	return gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(host->module), NULL,
		'S', 0, GSURF_MOD_CTRL | GSURF_MOD_SHIFT, GSURF_MODE_NORMAL);
}

/* --- reading a saved session back --- */

typedef struct {
	YamlParser  *parser;
	YamlMapping *root;
} Saved;

static void
saved_load(Saved *saved, const gchar *path)
{
	g_autoptr(GError) error = NULL;

	saved->parser = yaml_parser_new();
	g_assert_true(yaml_parser_load_from_file(saved->parser, path, &error));
	g_assert_no_error(error);
	saved->root = yaml_node_get_mapping(yaml_parser_get_root(saved->parser));
	g_assert_nonnull(saved->root);
}

static void
saved_clear(Saved *saved)
{
	g_clear_object(&saved->parser);
}

static YamlMapping *
saved_window(Saved *saved, guint index)
{
	YamlSequence *windows = yaml_mapping_get_sequence_member(saved->root, "windows");

	g_assert_nonnull(windows);
	g_assert_cmpuint(index, <, yaml_sequence_get_length(windows));
	return yaml_sequence_get_mapping_element(windows, index);
}

static guint
saved_n_tabs(Saved *saved, guint window)
{
	return yaml_sequence_get_length(
		yaml_mapping_get_sequence_member(saved_window(saved, window), "tabs"));
}

static YamlMapping *
saved_tab(Saved *saved, guint window, guint index)
{
	return yaml_sequence_get_mapping_element(
		yaml_mapping_get_sequence_member(saved_window(saved, window), "tabs"), index);
}

static const gchar *
tab_string(YamlMapping *tab, const gchar *key)
{
	return yaml_mapping_has_member(tab, key) ? yaml_mapping_get_string_member(tab, key)
	                                         : NULL;
}

static gint
tab_parent(YamlMapping *tab)
{
	return yaml_mapping_has_member(tab, "parent")
		? (gint)yaml_mapping_get_int_member(tab, "parent") : -1;
}

static guint
file_mode(const gchar *path)
{
	GStatBuf st;

	g_assert_cmpint(g_stat(path, &st), ==, 0);
	return st.st_mode & 0777;
}

/* ===== headless: save format ===== */

#define TRICKY_URI "https://ex.example/a?b=c&d=e#frag: x 'q' \"dq\" ü"

static void
test_save_format(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	g_autofree gchar *yaml = NULL;
	Host host;
	Saved saved = { 0 };
	GsurfView *v0, *v1, *v2, *v4, *blank;
	const gchar *t2;

	host_init(&host, g_object_new(test_window_get_type(), NULL));
	g_assert_nonnull(gsurf_container_registry_define(registry, "media", "Media", NULL, NULL));
	yaml = section(&host, FALSE, NULL);
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}

	/* default root, a Media child, two tabs sharing one temporary
	 * container (parent/child), a second temporary container, a blank
	 * tab (not saved) and a child of the blank tab (re-parented to the
	 * blank tab's own opener). */
	v0 = add_view(host.window, NULL, NULL, "https://one.example/");
	v1 = add_view(host.window, v0, "media", TRICKY_URI);
	v2 = add_view(host.window, NULL, GSURF_CONTAINER_TEMPORARY, "https://t1.example/");
	t2 = gsurf_view_get_container_id(v2);
	g_assert_nonnull(t2);
	add_view(host.window, v2, t2, "https://t1-child.example/");
	v4 = add_view(host.window, NULL, GSURF_CONTAINER_TEMPORARY, "https://t2.example/");
	blank = add_view(host.window, v0, NULL, "about:blank");
	add_view(host.window, blank, NULL, "https://grandchild.example/");
	gsurf_window_set_active_view(host.window, v1);
	(void)v4;

	g_assert_true(press_save(&host));
	g_assert_true(g_file_test(host.file, G_FILE_TEST_IS_REGULAR));
	/* URLs are private: 0600 in a 0700 directory. */
	g_assert_cmpuint(file_mode(host.file), ==, 0600);

	saved_load(&saved, host.file);
	g_assert_cmpint(yaml_mapping_get_int_member(saved.root, "version"), ==, 1);
	g_assert_cmpint(yaml_mapping_get_int_member(saved_window(&saved, 0), "active"), ==, 1);
	g_assert_cmpuint(saved_n_tabs(&saved, 0), ==, 6);

	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 0), "uri"), ==, "https://one.example/");
	g_assert_null(tab_string(saved_tab(&saved, 0, 0), "container"));
	g_assert_cmpint(tab_parent(saved_tab(&saved, 0, 0)), ==, -1);

	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 1), "uri"), ==, TRICKY_URI);
	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 1), "container"), ==, "media");
	g_assert_cmpint(tab_parent(saved_tab(&saved, 0, 1)), ==, 0);

	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 2), "container"), ==, "temporary:0");
	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 3), "container"), ==, "temporary:0");
	g_assert_cmpint(tab_parent(saved_tab(&saved, 0, 3)), ==, 2);
	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 4), "container"), ==, "temporary:1");

	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 5), "uri"), ==,
		"https://grandchild.example/");
	g_assert_cmpint(tab_parent(saved_tab(&saved, 0, 5)), ==, 0);
	saved_clear(&saved);

	/* An existing file with a looser mode is replaced at 0600. */
	g_assert_cmpint(g_chmod(host.file, 0644), ==, 0);
	g_assert_true(press_save(&host));
	g_assert_cmpuint(file_mode(host.file), ==, 0600);

	host_free(&host);
	g_assert_true(gsurf_container_registry_undefine(registry, "media", NULL));
}

/* ===== headless: save triggers ===== */

static void
test_save_triggers(void)
{
	g_autofree gchar *yaml = NULL;
	Host host;
	Saved saved = { 0 };
	GsurfView *a, *b, *c;

	host_init(&host, g_object_new(test_window_get_type(), NULL));
	yaml = section(&host, FALSE, NULL);
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	a = add_view(host.window, NULL, NULL, "https://a.example/");
	b = add_view(host.window, NULL, NULL, "https://b.example/");
	c = add_view(host.window, NULL, NULL, "https://c.example/");

	/* A committed navigation saves. */
	gsurf_navigation_hook_after_navigate(GSURF_NAVIGATION_HOOK(host.module), a,
		"https://a.example/");
	saved_load(&saved, host.file);
	g_assert_cmpuint(saved_n_tabs(&saved, 0), ==, 3);
	saved_clear(&saved);

	/* Moving a tab saves the new order. */
	gsurf_window_move_view(host.window, c, 0);
	saved_load(&saved, host.file);
	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 0), "uri"), ==, "https://c.example/");
	saved_clear(&saved);

	/* Closing a tab saves the rest. */
	gsurf_window_remove_view(host.window, b);
	saved_load(&saved, host.file);
	g_assert_cmpuint(saved_n_tabs(&saved, 0), ==, 2);
	saved_clear(&saved);

	/* Closing the last tabs keeps the last non-empty session. */
	gsurf_window_remove_view(host.window, a);
	gsurf_window_remove_view(host.window, c);
	saved_load(&saved, host.file);
	g_assert_cmpuint(saved_n_tabs(&saved, 0), ==, 1);
	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 0), "uri"), ==, "https://c.example/");
	saved_clear(&saved);

	/* Unrelated keys are not consumed. */
	g_assert_false(gsurf_input_handler_handle_key_event(GSURF_INPUT_HANDLER(host.module),
		NULL, 'x', 0, 0, GSURF_MODE_NORMAL));
	host_free(&host);
}

/* ===== headless: window layouts (save side) ===== */

/* A window with a layout saves `layout:`: the layout's state with every
 * view named by its saved tab index (blank tabs -1). A layout that cannot
 * serialize, or says nothing, writes no field. visible-views-changed
 * saves once per burst (an idle), not while inactive, and deactivation
 * drops a pending save. */
static void
test_save_layout(void)
{
	g_autofree gchar *yaml = NULL;
	Host host;
	Saved saved = { 0 };
	FakeLayout *layout;
	GsurfView *a;

	host_init(&host, g_object_new(test_window_get_type(), NULL));
	yaml = section(&host, FALSE, NULL);
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	layout = install_fake_layout(host.window, g_object_new(G_TYPE_OBJECT, NULL));
	a = add_view(host.window, NULL, NULL, "https://a.example/");
	add_view(host.window, a, NULL, "about:blank");
	add_view(host.window, NULL, NULL, "https://b.example/");
	add_view(host.window, a, NULL, "https://c.example/");
	drain_idles();

	/* 1. Indices are the saved ones: the blank tab is -1 and the tabs
	 *    after it close up, as `parent` numbers them. */
	layout->generation = 1;
	g_assert_true(press_save(&host));
	saved_load(&saved, host.file);
	g_assert_cmpuint(saved_n_tabs(&saved, 0), ==, 3);
	g_assert_cmpstr(tab_string(saved_window(&saved, 0), "layout"), ==, "g1:0,-1,1,2");
	g_assert_cmpint(tab_parent(saved_tab(&saved, 0, 2)), ==, 0);
	g_assert_cmpint(yaml_mapping_get_int_member(saved.root, "version"), ==, 1);
	saved_clear(&saved);

	/* 2. visible-views-changed saves, coalesced: two emissions, one
	 *    serialize, once the main loop runs. */
	layout->generation = 2;
	layout->serialize_calls = 0;
	gsurf_window_layout_notify_visible(host.window);
	gsurf_window_layout_notify_visible(host.window);
	g_assert_cmpuint(layout->serialize_calls, ==, 0);
	drain_idles();
	g_assert_cmpuint(layout->serialize_calls, ==, 1);
	saved_load(&saved, host.file);
	g_assert_cmpstr(tab_string(saved_window(&saved, 0), "layout"), ==, "g2:0,-1,1,2");
	saved_clear(&saved);

	/* 3. Deactivation drops a pending save and the handler. */
	gsurf_window_layout_notify_visible(host.window);
	gsurf_module_deactivate(host.module);
	drain_idles();
	g_assert_cmpuint(layout->serialize_calls, ==, 1);
	gsurf_window_layout_notify_visible(host.window);
	drain_idles();
	g_assert_cmpuint(layout->serialize_calls, ==, 1);
	g_assert_true(gsurf_module_activate(host.module));

	/* 4. No state (NULL or ""): no field. */
	layout->serialize = FAKE_SERIALIZE_NULL;
	g_assert_true(press_save(&host));
	saved_load(&saved, host.file);
	g_assert_false(yaml_mapping_has_member(saved_window(&saved, 0), "layout"));
	saved_clear(&saved);
	layout->serialize = FAKE_SERIALIZE_EMPTY;
	g_assert_true(press_save(&host));
	saved_load(&saved, host.file);
	g_assert_false(yaml_mapping_has_member(saved_window(&saved, 0), "layout"));
	saved_clear(&saved);

	/* 5. No layout: no field, as before. */
	layout->serialize = FAKE_SERIALIZE_INDICES;
	g_assert_true(gsurf_window_set_layout(host.window, NULL));
	drain_idles();
	g_assert_true(press_save(&host));
	saved_load(&saved, host.file);
	g_assert_false(yaml_mapping_has_member(saved_window(&saved, 0), "layout"));
	saved_clear(&saved);

	host_free(&host);
}

/* A `layout` that is not a non-empty string is reported when the file is
 * read (it is read at activation even without auto_restore) and the rest
 * of the window stays; a string layout with an unknown key next to it is
 * accepted silently (unknown keys are ignored, which is also how older
 * gsurf reads `layout`). */
static void
test_layout_field_errors(void)
{
	g_autofree gchar *yaml = NULL;
	Host host;

	host_init(&host, g_object_new(test_window_get_type(), NULL));
	yaml = section(&host, FALSE, NULL);
	g_assert_true(g_file_set_contents(host.file,
		"version: 1\n"
		"windows:\n"
		"  - tabs:\n      - uri: https://a.example/\n"
		"    layout: { panes: 2 }\n"
		"  - tabs:\n      - uri: https://b.example/\n"
		"    layout: [ 1, 2 ]\n"
		"  - tabs:\n      - uri: https://c.example/\n"
		"    layout: ''\n"
		"  - tabs:\n      - uri: https://d.example/\n"
		"    layout: ~\n"
		"  - tabs:\n      - uri: https://e.example/\n"
		"    layout: 'h(0,1)'\n"
		"    future_key: 7\n", -1, NULL));
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*window 0: `layout` is not*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*window 1: `layout` is not*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*window 2: `layout` is not*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*window 3: `layout` is not*");
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_test_assert_expected_messages();
	host_free(&host);
}

/* ===== headless: migration ===== */

static void
test_migration(void)
{
	g_autofree gchar *legacy = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *legacy_after = NULL;
	Host host;
	Saved saved = { 0 };

	host_init(&host, g_object_new(test_window_get_type(), NULL));
	legacy = g_build_filename(host.dir, "session", NULL);
	g_assert_true(g_file_set_contents(legacy,
		"https://old-one.example/\n\n  https://old-two.example/  \n", -1, NULL));

	/* The old default `file:` (no .yaml suffix) names the legacy file;
	 * the YAML session goes next to it. Migration happens at activation
	 * even without auto_restore. */
	yaml = g_strdup_printf("modules:\n  session:\n    enabled: true\n    file: '%s'\n", legacy);
	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*migrated*");
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_test_assert_expected_messages();
	g_assert_true(g_file_test(host.file, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(file_mode(host.file), ==, 0600);
	saved_load(&saved, host.file);
	g_assert_cmpuint(saved_n_tabs(&saved, 0), ==, 2);
	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 0), "uri"), ==, "https://old-one.example/");
	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 1), "uri"), ==, "https://old-two.example/");
	g_assert_null(tab_string(saved_tab(&saved, 0, 1), "container"));
	g_assert_cmpint(yaml_mapping_get_int_member(saved_window(&saved, 0), "active"), ==, 0);
	saved_clear(&saved);
	/* The old file is kept. */
	g_assert_true(g_file_get_contents(legacy, &legacy_after, NULL, NULL));
	g_assert_true(g_str_has_prefix(legacy_after, "https://old-one.example/"));

	/* Once the YAML exists the legacy file is never read again. */
	g_assert_true(g_file_set_contents(legacy, "https://ignored.example/\n", -1, NULL));
	gsurf_module_deactivate(host.module);
	g_assert_true(gsurf_module_activate(host.module));
	saved_load(&saved, host.file);
	g_assert_cmpuint(saved_n_tabs(&saved, 0), ==, 2);
	saved_clear(&saved);

	host_free(&host);
}

/* ===== headless: bad and missing files ===== */

static void
test_bad_files(void)
{
	g_autofree gchar *yaml = NULL;
	Host host;

	host_init(&host, g_object_new(test_window_get_type(), NULL));
	yaml = section(&host, TRUE, NULL);

	/* Missing: nothing to restore, no complaint, startup not handled. */
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_assert_null(g_object_get_data(G_OBJECT(host.app), "gsurf-startup-handled"));
	gsurf_module_deactivate(host.module);

	/* Not YAML, not a mapping, no windows: reported, nothing restored. */
	g_assert_true(g_file_set_contents(host.file, "{ [ unbalanced", -1, NULL));
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*is not a valid session*");
	gsurf_module_activate(host.module);
	g_test_assert_expected_messages();
	gsurf_module_deactivate(host.module);

	g_assert_true(g_file_set_contents(host.file, "- a\n- b\n", -1, NULL));
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*not a YAML mapping*");
	gsurf_module_activate(host.module);
	g_test_assert_expected_messages();
	gsurf_module_deactivate(host.module);

	g_assert_true(g_file_set_contents(host.file, "version: 1\n", -1, NULL));
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*no `windows` list*");
	gsurf_module_activate(host.module);
	g_test_assert_expected_messages();
	gsurf_module_deactivate(host.module);

	/* Entries without anything restorable are reported one by one. */
	g_assert_true(g_file_set_contents(host.file,
		"windows:\n"
		"  - just-a-string\n"
		"  - active: 0\n"
		"  - tabs:\n"
		"      - container: media\n"
		"      - 42\n"
		"      - uri: ''\n", -1, NULL));
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*window 0 is not a mapping*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*window 1 has no `tabs` list*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*window 2 tab 0 has no `uri`*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*window 2 tab 1 has no `uri`*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*window 2 tab 2 has no `uri`*");
	gsurf_module_activate(host.module);
	g_test_assert_expected_messages();
	g_assert_null(g_object_get_data(G_OBJECT(host.app), "gsurf-startup-handled"));
	gsurf_module_deactivate(host.module);

	/* Unreadable (a directory in its place). */
	g_assert_cmpint(g_unlink(host.file), ==, 0);
	g_assert_cmpint(g_mkdir(host.file, 0700), ==, 0);
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*cannot read*");
	gsurf_module_activate(host.module);
	g_test_assert_expected_messages();
	/* ... and saving there fails with a report, not a crash. */
	add_view(host.window, NULL, NULL, "https://x.example/");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*cannot save*");
	g_assert_true(press_save(&host));
	g_test_assert_expected_messages();

	host_free(&host);
}

/* ===== headless: a URI on the command line wins ===== */

static void
test_cli_precedence(void)
{
	g_autofree gchar *yaml = NULL;
	Host host;

	host_init(&host, g_object_new(test_window_get_type(), NULL));
	g_assert_true(g_file_set_contents(host.file,
		"version: 1\nwindows:\n  - tabs:\n      - uri: https://saved.example/\n", -1, NULL));
	g_object_set_data(G_OBJECT(host.app), "gsurf-cli-uri", (gpointer)"https://cli.example/");
	yaml = section(&host, TRUE, NULL);
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	/* Nothing restored (that would have created views), not handled. */
	g_assert_cmpuint(gsurf_window_get_n_views(host.window), ==, 0);
	g_assert_null(g_object_get_data(G_OBJECT(host.app), "gsurf-startup-handled"));
	host_free(&host);
}

/* ===== headless: configuration ===== */

static void
test_reconfigure(void)
{
	g_autofree gchar *yaml = NULL;
	g_autoptr(GPtrArray) keys = NULL;
	g_autoptr(GsurfConfig) config = NULL;
	g_autofree gchar *yml = NULL;
	g_autofree gchar *legacy_yaml = NULL;
	g_autofree gchar *default_file = NULL;
	Host host;

	host_init(&host, g_object_new(test_window_get_type(), NULL));
	yaml = section(&host, FALSE, "    key_save: Ctrl+Alt+s\n");
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	add_view(host.window, NULL, NULL, "https://a.example/");

	keys = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_keybind_help_free);
	gsurf_keybind_provider_list_keybinds(GSURF_KEYBIND_PROVIDER(host.module), keys);
	g_assert_cmpuint(keys->len, ==, 1);
	g_assert_cmpstr(gsurf_keybind_help_get_key(g_ptr_array_index(keys, 0)), ==, "Ctrl+Alt+s");
	g_assert_false(press_save(&host));

	/* ".yml" is used as is. */
	yml = g_build_filename(host.dir, "s.yml", NULL);
	g_clear_pointer(&yaml, g_free);
	yaml = g_strdup_printf("modules:\n  session:\n    enabled: true\n    file: '%s'\n", yml);
	config = config_from(yaml);
	gsurf_module_configure(host.module, config);
	g_clear_object(&config);
	g_assert_true(press_save(&host));          /* back to the default key */
	g_assert_true(g_file_test(yml, G_FILE_TEST_IS_REGULAR));

	/* A path without the suffix is the legacy name: YAML goes to
	 * <path>.yaml. An empty key unbinds; an empty file is reported. */
	legacy_yaml = g_build_filename(host.dir, "old.yaml", NULL);
	g_clear_pointer(&yaml, g_free);
	yaml = g_strdup_printf("modules:\n  session:\n    enabled: true\n    file: '%s/old'\n"
		"    key_save: ''\n", host.dir);
	config = config_from(yaml);
	gsurf_module_configure(host.module, config);
	g_clear_object(&config);
	g_assert_false(press_save(&host));
	g_ptr_array_set_size(keys, 0);
	gsurf_keybind_provider_list_keybinds(GSURF_KEYBIND_PROVIDER(host.module), keys);
	g_assert_cmpuint(keys->len, ==, 0);
	gsurf_navigation_hook_after_navigate(GSURF_NAVIGATION_HOOK(host.module), NULL, NULL);
	g_assert_true(g_file_test(legacy_yaml, G_FILE_TEST_IS_REGULAR));

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*empty `file`*");
	config = config_from("modules:\n  session:\n    enabled: true\n    file: ''\n");
	gsurf_module_configure(host.module, config);
	g_clear_object(&config);
	g_test_assert_expected_messages();

	/* No section: defaults ($XDG_DATA_HOME/gsurf/session.yaml, the
	 * default key). */
	config = config_from("browser:\n  homepage: about:blank\n");
	gsurf_module_configure(host.module, config);
	g_clear_object(&config);
	g_assert_true(press_save(&host));
	default_file = g_build_filename(g_get_user_data_dir(), "gsurf", "session.yaml", NULL);
	g_assert_true(g_file_test(default_file, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(file_mode(default_file), ==, 0600);

	host_free(&host);
}

/* ===== GUI: restore ===== */

/* The real-window host for restore tests: a window with one startup
 * view in @initial_container, like main.c builds before activation. */
static GsurfView *
gui_host_init(Host *host, const gchar *initial_container)
{
	GsurfView *initial;

	host_init(host, gsurf_window_new());
	initial = gsurf_view_new_full(NULL, initial_container, NULL, FALSE);
	gsurf_window_add_view(host->window, initial);
	g_object_unref(initial);
	return initial;
}

static const gchar *
uri_of(GsurfView *view)
{
	const gchar *uri = gsurf_view_get_uri(view);

	return (uri != NULL) ? uri : gsurf_view_get_initial_uri(view);
}

#define SESSION_FIXTURE \
	"version: 1\n" \
	"windows:\n" \
	"  - active: 3\n" \
	"    tabs:\n" \
	"      - uri: http://127.0.0.1:9/root\n" \
	"      - uri: http://127.0.0.1:9/work\n" \
	"        container: work\n" \
	"        parent: 0\n" \
	"      - uri: http://127.0.0.1:9/t0-child\n" \
	"        container: temporary:0\n" \
	"        parent: 3\n" \
	"      - uri: http://127.0.0.1:9/t0\n" \
	"        container: temporary:0\n" \
	"      - uri: http://127.0.0.1:9/t1\n" \
	"        container: temporary:1\n" \
	"      - uri: http://127.0.0.1:9/gone\n" \
	"        container: no-such-container\n" \
	"        parent: 1\n" \
	"  - tabs:\n" \
	"      - uri: http://127.0.0.1:9/second-window\n"

static void
test_restore_round_trip(void)
{
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	g_autofree gchar *yaml = NULL;
	Host host;
	Saved saved = { 0 };
	GsurfView *initial;
	GsurfView *v[7];
	guint i;

	if (!gsurf_webkit_test_init())
		return;
	g_assert_nonnull(gsurf_container_registry_define(registry, "work", "Work", NULL, NULL));
	initial = gui_host_init(&host, NULL);
	g_assert_true(g_file_set_contents(host.file, SESSION_FIXTURE, -1, NULL));
	yaml = section(&host, TRUE, NULL);

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*unknown container 'no-such-container'*");
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_test_assert_expected_messages();
	g_assert_nonnull(g_object_get_data(G_OBJECT(host.app), "gsurf-startup-handled"));

	/* Order: the file's order; the second window's tab is appended. */
	g_assert_cmpuint(gsurf_window_get_n_views(host.window), ==, 7);
	for (i = 0; i < 7; i++)
		v[i] = gsurf_window_get_nth_view(host.window, i);
	g_assert_cmpstr(uri_of(v[0]), ==, "http://127.0.0.1:9/root");
	g_assert_cmpstr(uri_of(v[2]), ==, "http://127.0.0.1:9/t0-child");
	g_assert_cmpstr(uri_of(v[6]), ==, "http://127.0.0.1:9/second-window");

	/* The startup view (default container) took the first tab. */
	g_assert_true(v[0] == initial);

	/* Containers: default, work, one fresh temporary container shared
	 * by group 0, another for group 1, default for the unknown one. */
	g_assert_null(gsurf_view_get_container_id(v[0]));
	g_assert_cmpstr(gsurf_view_get_container_id(v[1]), ==, "work");
	g_assert_nonnull(gsurf_view_get_container_id(v[2]));
	g_assert_true(gsurf_container_get_temporary(gsurf_container_registry_lookup(registry,
		gsurf_view_get_container_id(v[2]))));
	g_assert_cmpstr(gsurf_view_get_container_id(v[2]), ==, gsurf_view_get_container_id(v[3]));
	g_assert_nonnull(gsurf_view_get_container_id(v[4]));
	g_assert_cmpstr(gsurf_view_get_container_id(v[4]), !=, gsurf_view_get_container_id(v[3]));
	g_assert_null(gsurf_view_get_container_id(v[5]));

	/* Openers rebuilt from parents (a child listed before its parent too). */
	g_assert_null(gsurf_view_get_opener(v[0]));
	g_assert_true(gsurf_view_get_opener(v[1]) == v[0]);
	g_assert_true(gsurf_view_get_opener(v[2]) == v[3]);
	g_assert_null(gsurf_view_get_opener(v[3]));
	g_assert_true(gsurf_view_get_opener(v[5]) == v[1]);
	g_assert_true(gsurf_window_get_active_view(host.window) == v[3]);

	/* Saving again gives the same session (one window now). */
	g_assert_true(press_save(&host));
	saved_load(&saved, host.file);
	g_assert_cmpuint(saved_n_tabs(&saved, 0), ==, 7);
	g_assert_cmpint(yaml_mapping_get_int_member(saved_window(&saved, 0), "active"), ==, 3);
	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 1), "container"), ==, "work");
	g_assert_cmpint(tab_parent(saved_tab(&saved, 0, 1)), ==, 0);
	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 2), "container"), ==, "temporary:0");
	g_assert_cmpint(tab_parent(saved_tab(&saved, 0, 2)), ==, 3);
	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 3), "container"), ==, "temporary:0");
	g_assert_cmpstr(tab_string(saved_tab(&saved, 0, 4), "container"), ==, "temporary:1");
	g_assert_null(tab_string(saved_tab(&saved, 0, 5), "container"));
	g_assert_cmpint(tab_parent(saved_tab(&saved, 0, 5)), ==, 1);
	saved_clear(&saved);

	host_free(&host);
	gsurf_webkit_test_spin(50);
	gsurf_container_registry_undefine(registry, "work", NULL);
}

/* The startup view is in another container than the first saved tab:
 * it is replaced, and goes away. A parent cycle restores as roots. */
static void
test_restore_replaces_initial(void)
{
	g_autofree gchar *yaml = NULL;
	Host host;
	GsurfView *initial;
	gpointer weak;

	if (!gsurf_webkit_test_init())
		return;
	initial = gui_host_init(&host, GSURF_CONTAINER_TEMPORARY);
	weak = initial;
	g_object_add_weak_pointer(G_OBJECT(initial), &weak);
	g_assert_true(g_file_set_contents(host.file,
		"windows:\n  - tabs:\n"
		"      - uri: http://127.0.0.1:9/a\n        parent: 1\n"
		"      - uri: http://127.0.0.1:9/b\n        parent: 0\n"
		"      - uri: http://127.0.0.1:9/c\n        parent: 7\n", -1, NULL));
	yaml = section(&host, TRUE, NULL);
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*tab 2: parent 7 is not another tab*");
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*tab 0 is its own ancestor*");
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_test_assert_expected_messages();

	g_assert_cmpuint(gsurf_window_get_n_views(host.window), ==, 3);
	g_assert_cmpint(gsurf_window_get_view_index(host.window, initial), ==, -1);
	g_assert_null(gsurf_view_get_container_id(gsurf_window_get_nth_view(host.window, 0)));
	g_assert_cmpstr(uri_of(gsurf_window_get_nth_view(host.window, 1)), ==,
		"http://127.0.0.1:9/b");
	/* The cycle a <-> b lost one edge: a is a root, b its child. */
	g_assert_null(gsurf_view_get_opener(gsurf_window_get_nth_view(host.window, 0)));
	g_assert_true(gsurf_view_get_opener(gsurf_window_get_nth_view(host.window, 1)) ==
		gsurf_window_get_nth_view(host.window, 0));
	g_assert_true(gsurf_window_get_active_view(host.window) ==
		gsurf_window_get_nth_view(host.window, 0));
	gsurf_webkit_test_spin(100);
	g_assert_null(weak);
	host_free(&host);
}

/* A temporary startup view takes a temporary group (and the group's
 * other tabs join its container); the old plain-text file migrates and
 * restores in one go. */
static void
test_restore_temporary_initial_and_legacy(void)
{
	g_autofree gchar *yaml = NULL;
	Host host;
	GsurfView *initial;

	if (!gsurf_webkit_test_init())
		return;
	initial = gui_host_init(&host, GSURF_CONTAINER_TEMPORARY);
	g_assert_true(g_file_set_contents(host.file,
		"windows:\n  - tabs:\n"
		"      - uri: http://127.0.0.1:9/x\n        container: temporary:5\n"
		"      - uri: http://127.0.0.1:9/y\n        container: temporary:5\n", -1, NULL));
	yaml = section(&host, TRUE, NULL);
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_assert_cmpuint(gsurf_window_get_n_views(host.window), ==, 2);
	g_assert_true(gsurf_window_get_nth_view(host.window, 0) == initial);
	g_assert_cmpstr(gsurf_view_get_container_id(gsurf_window_get_nth_view(host.window, 1)),
		==, gsurf_view_get_container_id(initial));
	host_free(&host);

	/* Legacy only: migrated, then restored into the default startup view. */
	initial = gui_host_init(&host, NULL);
	{
		g_autofree gchar *legacy = g_build_filename(host.dir, "session", NULL);

		g_assert_true(g_file_set_contents(legacy,
			"http://127.0.0.1:9/l1\nhttp://127.0.0.1:9/l2\n", -1, NULL));
	}
	g_clear_pointer(&yaml, g_free);
	yaml = section(&host, TRUE, NULL);
	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*migrated*");
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_test_assert_expected_messages();
	g_assert_true(g_file_test(host.file, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(gsurf_window_get_n_views(host.window), ==, 2);
	g_assert_true(gsurf_window_get_nth_view(host.window, 0) == initial);
	g_assert_cmpstr(uri_of(gsurf_window_get_nth_view(host.window, 1)), ==,
		"http://127.0.0.1:9/l2");
	host_free(&host);
}

/* ===== GUI: restoring a window layout ===== */

/* Four saved tabs, the third without a URI (dropped), the last active;
 * the layout names file indices. */
#define LAYOUT_FIXTURE \
	"version: 1\n" \
	"windows:\n" \
	"  - active: 3\n" \
	"    tabs:\n" \
	"      - uri: http://127.0.0.1:9/l0\n" \
	"      - uri: http://127.0.0.1:9/l1\n" \
	"        parent: 0\n" \
	"      - container: lost\n" \
	"      - uri: http://127.0.0.1:9/l3\n" \
	"    layout: 'v(3,h(0,1))'\n"

/* A real window (main.c's startup view in it) with a fake layout whose
 * root is a real widget, installed before the module activates (as
 * wmux, priority -10, does before session). */
static FakeLayout *
gui_layout_host_init(Host *host)
{
	gui_host_init(host, NULL);
	return install_fake_layout(host->window,
		G_OBJECT(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)));
}

/* Round trip: restore() gets the saved string and the views by file
 * index (NULL for the dropped tab), once, after the tabs exist and the
 * active tab is set; saving again maps the views to the new indices. */
static void
test_restore_layout(void)
{
	g_autofree gchar *yaml = NULL;
	Host host;
	Saved saved = { 0 };
	FakeLayout *layout;
	GsurfView *v[3];
	guint i;

	if (!gsurf_webkit_test_init())
		return;
	layout = gui_layout_host_init(&host);
	g_assert_true(g_file_set_contents(host.file, LAYOUT_FIXTURE, -1, NULL));
	yaml = section(&host, TRUE, NULL);
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*window 0 tab 2 has no `uri`*");
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_test_assert_expected_messages();

	g_assert_cmpuint(gsurf_window_get_n_views(host.window), ==, 3);
	for (i = 0; i < 3; i++)
		v[i] = gsurf_window_get_nth_view(host.window, i);
	g_assert_cmpstr(uri_of(v[2]), ==, "http://127.0.0.1:9/l3");
	g_assert_cmpuint(layout->restore_calls, ==, 1);
	g_assert_cmpstr(layout->restored_state, ==, "v(3,h(0,1))");
	g_assert_cmpuint(layout->restored_views->len, ==, 4);
	g_assert_true(g_ptr_array_index(layout->restored_views, 0) == v[0]);
	g_assert_true(g_ptr_array_index(layout->restored_views, 1) == v[1]);
	g_assert_null(g_ptr_array_index(layout->restored_views, 2));
	g_assert_true(g_ptr_array_index(layout->restored_views, 3) == v[2]);
	g_assert_true(layout->active_at_restore == v[2]);

	/* Restoring did not save (the layout's visible-views-changed came
	 * while restoring): the file is still the fixture. */
	drain_idles();
	saved_load(&saved, host.file);
	g_assert_cmpstr(tab_string(saved_window(&saved, 0), "layout"), ==, "v(3,h(0,1))");
	saved_clear(&saved);

	/* Saving again writes the current arrangement with the new indices. */
	layout->generation = 7;
	g_assert_true(press_save(&host));
	saved_load(&saved, host.file);
	g_assert_cmpuint(saved_n_tabs(&saved, 0), ==, 3);
	g_assert_cmpstr(tab_string(saved_window(&saved, 0), "layout"), ==, "g7:0,1,2");
	saved_clear(&saved);
	host_free(&host);
}

/* The layout refuses the string, there is no layout at all, a later
 * saved window has one, or the file predates layouts: the tabs are
 * restored either way, with one warning per dropped layout and none for
 * an old file. */
static void
test_restore_layout_fallbacks(void)
{
	g_autofree gchar *yaml = NULL;
	Host host;
	FakeLayout *layout;

	if (!gsurf_webkit_test_init())
		return;

	/* 1. restore() returns FALSE. */
	layout = gui_layout_host_init(&host);
	layout->restore_result = FALSE;
	g_assert_true(g_file_set_contents(host.file,
		"windows:\n  - tabs:\n      - uri: http://127.0.0.1:9/a\n"
		"      - uri: http://127.0.0.1:9/b\n    layout: 'h(0,5)'\n", -1, NULL));
	yaml = section(&host, TRUE, NULL);
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
		"*window 0: the window layout cannot restore*");
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_test_assert_expected_messages();
	g_assert_cmpuint(layout->restore_calls, ==, 1);
	g_assert_cmpuint(layout->restored_views->len, ==, 2);
	g_assert_cmpuint(gsurf_window_get_n_views(host.window), ==, 2);
	host_free(&host);

	/* 2. No layout installed (wmux disabled): warned and dropped. */
	gui_host_init(&host, NULL);
	g_assert_true(g_file_set_contents(host.file,
		"windows:\n  - tabs:\n      - uri: http://127.0.0.1:9/a\n"
		"    layout: 'h(0)'\n", -1, NULL));
	g_clear_pointer(&yaml, g_free);
	yaml = section(&host, TRUE, NULL);
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
		"*window 0: no window layout is installed*");
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_test_assert_expected_messages();
	g_assert_cmpuint(gsurf_window_get_n_views(host.window), ==, 1);
	host_free(&host);

	/* 3. A second saved window's layout: its tabs join the first window,
	 *    its layout is dropped; the first window's still applies. */
	layout = gui_layout_host_init(&host);
	g_assert_true(g_file_set_contents(host.file,
		"windows:\n"
		"  - tabs:\n      - uri: http://127.0.0.1:9/a\n    layout: first\n"
		"  - tabs:\n      - uri: http://127.0.0.1:9/b\n    layout: second\n", -1, NULL));
	g_clear_pointer(&yaml, g_free);
	yaml = section(&host, TRUE, NULL);
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*window 1: its tabs join*");
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_test_assert_expected_messages();
	g_assert_cmpuint(layout->restore_calls, ==, 1);
	g_assert_cmpstr(layout->restored_state, ==, "first");
	g_assert_cmpuint(layout->restored_views->len, ==, 1);
	g_assert_cmpuint(gsurf_window_get_n_views(host.window), ==, 2);
	host_free(&host);

	/* 4. An older file without `layout:`: restore() is not called and
	 *    nothing is reported. */
	layout = gui_layout_host_init(&host);
	g_assert_true(g_file_set_contents(host.file,
		"version: 1\nwindows:\n  - tabs:\n      - uri: http://127.0.0.1:9/a\n", -1, NULL));
	g_clear_pointer(&yaml, g_free);
	yaml = section(&host, TRUE, NULL);
	if (!host_load(&host, yaml)) {
		host_free(&host);
		return;
	}
	g_assert_cmpuint(layout->restore_calls, ==, 0);
	g_assert_cmpuint(gsurf_window_get_n_views(host.window), ==, 1);
	host_free(&host);
}

int
main(int argc, char **argv)
{
	g_autofree gchar *home = g_dir_make_tmp("gsurf-session-test-XXXXXX", NULL);
	g_autofree gchar *data = g_build_filename(home, "data", NULL);
	g_autofree gchar *cache = g_build_filename(home, "cache", NULL);
	gint result;

	/* Keep the default session file and container profiles out of the
	 * user's real directories. (G_TEST_OPTION_ISOLATE_DIRS would also
	 * move XDG_RUNTIME_DIR and hide the display from the GUI tests.) */
	g_setenv("XDG_DATA_HOME", data, TRUE);
	g_setenv("XDG_CACHE_HOME", cache, TRUE);
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/gsurf/session/save-format", test_save_format);
	g_test_add_func("/gsurf/session/save-triggers", test_save_triggers);
	g_test_add_func("/gsurf/session/migration", test_migration);
	g_test_add_func("/gsurf/session/bad-files", test_bad_files);
	g_test_add_func("/gsurf/session/cli-precedence", test_cli_precedence);
	g_test_add_func("/gsurf/session/reconfigure", test_reconfigure);
	g_test_add_func("/gsurf/session/save-layout", test_save_layout);
	g_test_add_func("/gsurf/session/layout-field-errors", test_layout_field_errors);
	g_test_add_func("/gsurf/session/webkit/restore-round-trip", test_restore_round_trip);
	g_test_add_func("/gsurf/session/webkit/restore-replaces-initial",
		test_restore_replaces_initial);
	g_test_add_func("/gsurf/session/webkit/restore-temporary-and-legacy",
		test_restore_temporary_initial_and_legacy);
	g_test_add_func("/gsurf/session/webkit/restore-layout", test_restore_layout);
	g_test_add_func("/gsurf/session/webkit/restore-layout-fallbacks",
		test_restore_layout_fallbacks);
	result = g_test_run();
	gsurf_test_rmtree(home);
	return result;
}
