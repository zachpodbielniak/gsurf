/*
 * test-mcp.c - The mcp module's tools, headless
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the real mcp.so (built with MCP=1; skipped otherwise) into the
 * default module manager and calls its tools in-process through the
 * module's exported gsurf_mcp_module_call_tool(), i.e. through the same
 * registration (enabled flags, schemas, handlers) the socket server uses.
 *
 * Without a display, the window, views, window layout and tab tree are
 * fakes that record what the tools ask of the core seams:
 *   - a recording GsurfWindowLayout with two groups ("web": views a, b;
 *     "docs": view c) logs every run_command line, rename_group and
 *     show_view_at, so the wmux convenience tools are checked down to the
 *     exact command strings they send, and its errors come back as the
 *     tool's error text;
 *   - a module implementing GsurfTabTreeProvider and GsurfActionHandler
 *     answers the tree tools and run_action.
 * Configuration (every tool off by default, per-tool enabling,
 * reconfiguring), argument leniency ("3", 3.0), unknown views and panes,
 * missing layouts, kiosk refusals and the action executor are covered.
 * test-mcp-webkit (make test-gui) drives the same tools against real
 * windows with wmux, tabs, containers and modal.
 */

#include <gsurf.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* The module's in-process entry points (G_MODULE_EXPORT in mcp.so). */
typedef gchar *(*CallToolFunc) (GsurfModule *module, const gchar *name, const gchar *args,
                                gboolean *is_error, GError **error);
typedef gchar *(*ListToolsFunc) (GsurfModule *module);

/* Every tool, in the module's table order. */
static const gchar *const all_tools[] = {
	"navigate", "get_current_uri", "get_title", "go_back", "go_forward", "reload", "stop",
	"get_page_text", "get_page_html", "get_links", "execute_javascript", "scroll",
	"find_text", "click_link", "screenshot", "save_screenshot", "get_history",
	"get_cookies", "get_config", "set_config", "set_setting", "list_modules",
	"toggle_module",
	"list_views", "switch_view", "open_view", "close_view", "open_child_view",
	"move_view", "set_collapsed", "get_tab_tree", "list_containers",
	"reopen_in_container", "new_temporary_container_view", "get_mode", "set_mode",
	"follow_hints", "run_action", "list_actions",
	"wmux_status", "list_wmux_windows", "list_panes", "wmux_command", "split_pane",
	"select_pane", "kill_pane", "zoom_pane", "select_layout", "show_view_in_pane",
	"select_window", "new_window", "rename_window", "kill_window",
	NULL
};

static GModule      *so_handle;
static CallToolFunc  call_tool;
static ListToolsFunc list_tools;

/* ------------------------------------------------------------------ */
/* Headless view and window                                            */
/* ------------------------------------------------------------------ */

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

static void w_noop(GsurfWindow *w, GsurfView *v) { (void)w; (void)v; }
static void w_content(GsurfWindow *w, gpointer widget) { (void)w; (void)widget; }

static void
test_window_class_init(TestWindowClass *klass)
{
	GsurfWindowClass *wc = GSURF_WINDOW_CLASS(klass);

	wc->insert_view = w_noop;
	wc->remove_view = w_noop;
	wc->set_active_view = w_noop;
	wc->set_content = w_content;
}

static void test_window_init(TestWindow *self) { (void)self; }

/* A view opened from @opener (NULL for none). */
static GsurfView *
new_view(GsurfView *opener)
{
	return g_object_new(test_view_get_type(), "opener-id",
		opener != NULL ? gsurf_view_get_id(opener) : G_GUINT64_CONSTANT(0), NULL);
}

/* ------------------------------------------------------------------ */
/* Recording layout: group 0 "web" (a, b), group 1 "docs" (c)          */
/* ------------------------------------------------------------------ */

typedef struct {
	GObject    parent;
	GObject   *root;
	GString   *log;        /* "cmd;" per run_command, rename/drop entries */
	guint64    ids[3];     /* a, b, c */
	guint      current;
} TestLayout;
typedef struct { GObjectClass parent; } TestLayoutClass;

static void test_layout_iface_init(GsurfWindowLayoutInterface *iface);
GType test_layout_get_type(void);
G_DEFINE_TYPE_WITH_CODE(TestLayout, test_layout, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_WINDOW_LAYOUT, test_layout_iface_init))

static gpointer l_root(GsurfWindowLayout *self) { return ((TestLayout *)self)->root; }

static gboolean
l_visible(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view)
{
	TestLayout *l = (TestLayout *)self;
	guint64 id = gsurf_view_get_id(view);

	(void)window;
	return l->current == 0 ? (id == l->ids[0] || id == l->ids[1]) : id == l->ids[2];
}

static GPtrArray *
l_list_groups(GsurfWindowLayout *self, GsurfWindow *window)
{
	TestLayout *l = (TestLayout *)self;
	GPtrArray *groups = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_layout_group_unref);
	GsurfLayoutGroup *web = gsurf_layout_group_new(0, "web", l->current == 0);
	GsurfLayoutGroup *docs = gsurf_layout_group_new(1, "docs", l->current == 1);

	(void)window;
	gsurf_layout_group_add_view_id(web, l->ids[0]);
	gsurf_layout_group_add_view_id(web, l->ids[1]);
	gsurf_layout_group_add_view_id(docs, l->ids[2]);
	gsurf_layout_group_set_last(docs, TRUE);
	gsurf_layout_group_set_zoomed(docs, TRUE);
	g_ptr_array_add(groups, web);
	g_ptr_array_add(groups, docs);
	return groups;
}

static GPtrArray *
l_list_panes(GsurfWindowLayout *self, GsurfWindow *window, guint group)
{
	TestLayout *l = (TestLayout *)self;
	GPtrArray *panes;
	GsurfLayoutPane *p;

	(void)window;
	if (group > 1)
		return NULL;
	panes = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_layout_pane_unref);
	if (group == 0) {
		p = gsurf_layout_pane_new(0, 5, l->ids[0], TRUE);
		gsurf_layout_pane_set_rect(p, 0, 0, 400, 300);
		gsurf_layout_pane_set_fraction(p, 0.0, 0.0, 0.5, 1.0);
		g_ptr_array_add(panes, p);
		p = gsurf_layout_pane_new(1, 6, l->ids[1], FALSE);
		gsurf_layout_pane_set_rect(p, 402, 0, 398, 300);
		gsurf_layout_pane_set_fraction(p, 0.5, 0.0, 0.5, 1.0);
		g_ptr_array_add(panes, p);
	} else {
		p = gsurf_layout_pane_new(0, 9, l->ids[2], TRUE);
		gsurf_layout_pane_set_zoomed(p, TRUE);
		g_ptr_array_add(panes, p);
	}
	return panes;
}

/* Records every line; "select-window -t 1" switches the current group;
 * lines starting with "bad" or "select-pane -t 9" fail with a message. */
static gboolean
l_run_command(GsurfWindowLayout *self, GsurfWindow *window, const gchar *command,
              GError **error)
{
	TestLayout *l = (TestLayout *)self;

	(void)window;
	g_string_append_printf(l->log, "%s;", command);
	if (g_str_has_prefix(command, "bad") || g_strcmp0(command, "select-pane -t 9") == 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "refused (%s)", command);
		return FALSE;
	}
	if (g_strcmp0(command, "select-window -t 1") == 0)
		l->current = 1;
	if (g_strcmp0(command, "select-window -t 0") == 0)
		l->current = 0;
	return TRUE;
}

static gboolean
l_rename_group(GsurfWindowLayout *self, GsurfWindow *window, guint index, const gchar *name)
{
	(void)window;
	if (index > 1)
		return FALSE;
	g_string_append_printf(((TestLayout *)self)->log, "rename %u %s;", index,
		name != NULL ? name : "(null)");
	return TRUE;
}

static gboolean
l_show_view_at(GsurfWindowLayout *self, GsurfWindow *window, GsurfView *view,
               GsurfView *target, GsurfLayoutDropZone zone)
{
	GEnumClass *klass = g_type_class_ref(GSURF_TYPE_LAYOUT_DROP_ZONE);

	(void)window;
	g_string_append_printf(((TestLayout *)self)->log, "drop %" G_GUINT64_FORMAT " %"
		G_GUINT64_FORMAT " %s;", gsurf_view_get_id(view), gsurf_view_get_id(target),
		g_enum_get_value(klass, zone)->value_nick);
	g_type_class_unref(klass);
	return view != target;
}

static GVariant *
l_describe(GsurfWindowLayout *self, GsurfWindow *window)
{
	GVariantBuilder b;

	(void)self;
	(void)window;
	g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&b, "{sv}", "name", g_variant_new_string("fake"));
	g_variant_builder_add(&b, "{sv}", "prefix", g_variant_new_string("Ctrl+b"));
	g_variant_builder_add(&b, "{sv}", "mouse", g_variant_new_boolean(TRUE));
	g_variant_builder_add(&b, "{sv}", "width", g_variant_new_int32(800));
	g_variant_builder_add(&b, "{sv}", "windows", g_variant_new_uint32(2));
	return g_variant_builder_end(&b);
}

static void
test_layout_iface_init(GsurfWindowLayoutInterface *iface)
{
	iface->get_root = l_root;
	iface->is_view_visible = l_visible;
	iface->list_groups = l_list_groups;
	iface->list_panes = l_list_panes;
	iface->run_command = l_run_command;
	iface->rename_group = l_rename_group;
	iface->show_view_at = l_show_view_at;
	iface->describe = l_describe;
}

static void
test_layout_finalize(GObject *object)
{
	TestLayout *self = (TestLayout *)object;

	g_clear_object(&self->root);
	g_string_free(self->log, TRUE);
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
	self->root = g_object_new(G_TYPE_OBJECT, NULL);
	self->log = g_string_new(NULL);
}

/* ------------------------------------------------------------------ */
/* A tree provider + action handler module                             */
/* ------------------------------------------------------------------ */

#define TEST_TYPE_TREE_MODULE (test_tree_module_get_type())
G_DECLARE_FINAL_TYPE(TestTreeModule, test_tree_module, TEST, TREE_MODULE, GsurfModule)

struct _TestTreeModule
{
	GsurfModule parent_instance;
	GString    *log;
	guint64     parent_of_c;   /* what get_parent answers for every view */
	gboolean    collapsed;
};

static void test_tree_provider_init(GsurfTabTreeProviderInterface *iface);
static void test_tree_action_init(GsurfActionHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(TestTreeModule, test_tree_module, GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_TAB_TREE_PROVIDER, test_tree_provider_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_ACTION_HANDLER, test_tree_action_init))

static guint64
t_parent(GsurfTabTreeProvider *self, GsurfWindow *window, GsurfView *view)
{
	(void)window;
	/* Only the opener-less third view gets a (made-up) parent. */
	return gsurf_view_get_opener_id(view) == 0 &&
	       gsurf_window_get_view_index(window, view) == 2
		? TEST_TREE_MODULE(self)->parent_of_c : gsurf_view_get_opener_id(view);
}

static gboolean
t_get_collapsed(GsurfTabTreeProvider *self, GsurfWindow *window, GsurfView *view)
{
	(void)window;
	(void)view;
	return TEST_TREE_MODULE(self)->collapsed;
}

static gboolean
t_set_collapsed(GsurfTabTreeProvider *self, GsurfWindow *window, GsurfView *view,
                gboolean collapsed)
{
	(void)window;
	/* Only the first view "has children". */
	if (gsurf_window_get_view_index(window, view) != 0)
		return FALSE;
	TEST_TREE_MODULE(self)->collapsed = collapsed;
	return TRUE;
}

static gboolean
t_move_view(GsurfTabTreeProvider *self, GsurfWindow *window, GsurfView *view,
            GsurfView *target, GsurfTabTreePosition position)
{
	GEnumClass *klass = g_type_class_ref(GSURF_TYPE_TAB_TREE_POSITION);

	(void)window;
	g_string_append_printf(TEST_TREE_MODULE(self)->log, "move %" G_GUINT64_FORMAT " %"
		G_GUINT64_FORMAT " %s;", gsurf_view_get_id(view),
		target != NULL ? gsurf_view_get_id(target) : G_GUINT64_CONSTANT(0),
		g_enum_get_value(klass, position)->value_nick);
	g_type_class_unref(klass);
	return TRUE;
}

static void
test_tree_provider_init(GsurfTabTreeProviderInterface *iface)
{
	iface->get_parent = t_parent;
	iface->get_collapsed = t_get_collapsed;
	iface->set_collapsed = t_set_collapsed;
	iface->move_view = t_move_view;
}

/* Performs "find" only. */
static gboolean
t_action(GsurfActionHandler *handler, GsurfView *view, GsurfAction action)
{
	(void)view;
	g_string_append_printf(TEST_TREE_MODULE(handler)->log, "action %s;",
		gsurf_action_to_string(action));
	return action == GSURF_ACTION_FIND;
}

static void
test_tree_action_init(GsurfActionHandlerInterface *iface)
{
	iface->handle_action = t_action;
}

static const gchar *
t_name(GsurfModule *module)
{
	(void)module;
	return "test-tree";
}

static void
test_tree_module_finalize(GObject *object)
{
	g_string_free(TEST_TREE_MODULE(object)->log, TRUE);
	G_OBJECT_CLASS(test_tree_module_parent_class)->finalize(object);
}

static void
test_tree_module_class_init(TestTreeModuleClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = test_tree_module_finalize;
	GSURF_MODULE_CLASS(klass)->get_name = t_name;
}

static void
test_tree_module_init(TestTreeModule *self)
{
	self->log = g_string_new(NULL);
}

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
	GsurfConfig      *config;
	GsurfModule      *mcp;     /* owned by the default manager */
	GsurfApplication *app;
	GsurfWindow      *window;
	GsurfView        *a;       /* borrowed from the window */
	GsurfView        *b;       /* opened from a */
	GsurfView        *c;       /* no opener */
	TestLayout       *layout;
	TestTreeModule   *tree;
} H;

static gchar *
module_so_path(const gchar *name)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);
	g_autofree gchar *base = g_strconcat(name, ".so", NULL);

	return g_build_filename(dir, "modules", base, NULL);
}

/* YAML enabling every tool (or none with @tools FALSE), plus @extra. */
static gchar *
config_yaml(gboolean tools, const gchar *extra)
{
	GString *yaml = g_string_new("modules:\n  mcp:\n    enabled: true\n"
		"    socket_name: test-mcp-headless\n    tools:\n");
	guint i;

	for (i = 0; all_tools[i] != NULL; i++)
		g_string_append_printf(yaml, "      %s: %s\n", all_tools[i], tools ? "true" : "false");
	if (extra != NULL)
		g_string_append(yaml, extra);
	return g_string_free(yaml, FALSE);
}

/* Load (once) or reconfigure mcp.so in the default manager, without
 * activating it (no socket server is needed for in-process calls). */
static gboolean
h_mcp(H *h, const gchar *yaml)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	g_autofree gchar *so = module_so_path("mcp");
	g_autoptr(GError) error = NULL;

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("mcp.so not built (make MCP=1)");
		return FALSE;
	}
	g_clear_object(&h->config);
	h->config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(h->config, yaml, -1, &error));
	g_assert_no_error(error);
	gsurf_config_set_default(h->config);
	gsurf_module_manager_set_config(mgr, h->config);

	h->mcp = gsurf_module_manager_get_module(mgr, "mcp");
	if (h->mcp == NULL) {
		h->mcp = gsurf_module_manager_load_module(mgr, so, &error);
		g_assert_no_error(error);
		g_assert_nonnull(h->mcp);
		so_handle = g_module_open(so, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
		g_assert_nonnull(so_handle);
		g_assert_true(g_module_symbol(so_handle, "gsurf_mcp_module_call_tool",
			(gpointer *)&call_tool));
		g_assert_true(g_module_symbol(so_handle, "gsurf_mcp_module_list_tools",
			(gpointer *)&list_tools));
	} else {
		gsurf_module_configure(h->mcp, h->config);
	}
	return TRUE;
}

/* An application with one fake window holding a, b (child of a), c. */
static void
h_window(H *h)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	g_autoptr(GsurfView) a = new_view(NULL);
	g_autoptr(GsurfView) b = new_view(a);
	g_autoptr(GsurfView) c = new_view(NULL);

	h->app = gsurf_application_new(h->config);
	h->window = g_object_new(test_window_get_type(), NULL);
	gsurf_window_add_view(h->window, a);
	gsurf_window_add_view(h->window, b);
	gsurf_window_add_view(h->window, c);
	h->a = a;
	h->b = b;
	h->c = c;
	gsurf_window_set_active_view(h->window, a);
	gsurf_application_add_window(h->app, h->window);
	gsurf_module_manager_set_application(mgr, h->app);
}

/* Install the recording layout in the window. */
static void
h_layout(H *h)
{
	h->layout = g_object_new(test_layout_get_type(), NULL);
	h->layout->ids[0] = gsurf_view_get_id(h->a);
	h->layout->ids[1] = gsurf_view_get_id(h->b);
	h->layout->ids[2] = gsurf_view_get_id(h->c);
	g_assert_true(gsurf_window_set_layout(h->window, GSURF_WINDOW_LAYOUT(h->layout)));
}

/* Add the tree provider module (active) to the default manager. */
static void
h_tree(H *h)
{
	h->tree = g_object_new(TEST_TYPE_TREE_MODULE, NULL);
	gsurf_module_set_enabled(GSURF_MODULE(h->tree), TRUE);
	gsurf_module_manager_add_module(gsurf_module_manager_get_default(), GSURF_MODULE(h->tree));
	gsurf_module_activate(GSURF_MODULE(h->tree));
}

static void
h_free(H *h)
{
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();

	if (h->tree != NULL) {
		/* The manager keeps it; deactivate so the next case has no tree. */
		gsurf_module_deactivate(GSURF_MODULE(h->tree));
		gsurf_module_set_enabled(GSURF_MODULE(h->tree), FALSE);
		g_clear_object(&h->tree);
	}
	if (h->window != NULL)
		gsurf_window_set_layout(h->window, NULL);
	g_clear_object(&h->layout);
	gsurf_module_manager_set_application(mgr, NULL);
	g_clear_object(&h->window);
	g_clear_object(&h->app);
	gsurf_config_set_default(NULL);
	g_clear_object(&h->config);
}

/* Call @tool with @args (JSON text or NULL); assert whether it failed and
 * return its text. */
static gchar *
call(H *h, const gchar *tool, const gchar *args, gboolean expect_error)
{
	g_autoptr(GError) error = NULL;
	gboolean is_error = !expect_error;
	gchar *text = call_tool(h->mcp, tool, args, &is_error, &error);

	g_assert_no_error(error);
	g_assert_nonnull(text);
	if (is_error != expect_error)
		g_error("%s(%s): %s: %s", tool, args != NULL ? args : "", expect_error
			? "expected an error" : "unexpected error", text);
	return text;
}

/* call() with a printf-built argument object. */
static gchar *
callf(H *h, const gchar *tool, gboolean expect_error, const gchar *format, ...) G_GNUC_PRINTF(4, 5);

static gchar *
callf(H *h, const gchar *tool, gboolean expect_error, const gchar *format, ...)
{
	g_autofree gchar *args = NULL;
	va_list ap;

	va_start(ap, format);
	args = g_strdup_vprintf(format, ap);
	va_end(ap);
	return call(h, tool, args, expect_error);
}

/* Parse a tool's JSON text (asserting it parses). */
static JsonNode *
parse(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(json_parser_load_from_data(parser, text, -1, &error));
	g_assert_no_error(error);
	return json_node_copy(json_parser_get_root(parser));
}

/* Take the layout's log. */
static gchar *
take_log(GString *log)
{
	gchar *out = g_strdup(log->str);

	g_string_truncate(log, 0);
	return out;
}

#define ASSERT_LOG(log, expected) \
	G_STMT_START { \
		g_autofree gchar *got__ = take_log(log); \
		g_assert_cmpstr(got__, ==, (expected)); \
	} G_STMT_END

#define ASSERT_HAS(text, needle) \
	G_STMT_START { \
		if (strstr((text), (needle)) == NULL) \
			g_error("'%s' not in: %s", (needle), (text)); \
	} G_STMT_END

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* Every tool is off until enabled; enabled tools are announced with a
 * description and an object schema; a reconfigure applies at once. */
static void
test_registration(void)
{
	H h = { 0 };
	g_autofree gchar *none = config_yaml(FALSE, NULL);
	g_autofree gchar *all = config_yaml(TRUE, NULL);
	g_autofree gchar *listed = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GError) error = NULL;
	JsonArray *tools;
	gboolean is_error = FALSE;
	gchar *text;
	guint i;
	guint j;

	if (!h_mcp(&h, none))
		return;

	/* 1. Nothing enabled: nothing listed, calls refused. */
	listed = list_tools(h.mcp);
	g_assert_cmpstr(listed, ==, "[]");
	text = call_tool(h.mcp, "wmux_status", NULL, &is_error, &error);
	g_assert_null(text);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "wmux_status"));
	g_clear_error(&error);

	/* 2. Everything enabled: every tool, in table order, with a schema. */
	g_assert_true(h_mcp(&h, all));
	g_clear_pointer(&listed, g_free);
	listed = list_tools(h.mcp);
	root = parse(listed);
	tools = json_node_get_array(root);
	g_assert_cmpuint(json_array_get_length(tools), ==, g_strv_length((gchar **)all_tools));
	for (i = 0; all_tools[i] != NULL; i++) {
		gboolean found = FALSE;

		for (j = 0; j < json_array_get_length(tools) && !found; j++) {
			JsonObject *tool = json_array_get_object_element(tools, j);
			JsonObject *schema;

			if (g_strcmp0(json_object_get_string_member(tool, "name"), all_tools[i]) != 0)
				continue;
			found = TRUE;
			g_assert_cmpuint(strlen(json_object_get_string_member(tool, "description")), >, 10);
			schema = json_object_get_object_member(tool, "inputSchema");
			g_assert_cmpstr(json_object_get_string_member(schema, "type"), ==, "object");
		}
		if (!found)
			g_error("tool %s not listed", all_tools[i]);
	}

	/* 3. Bad arguments JSON. */
	text = call_tool(h.mcp, "list_views", "[1]", &is_error, &error);
	g_assert_null(text);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	text = call_tool(h.mcp, "list_views", "{", &is_error, &error);
	g_assert_null(text);
	g_assert_nonnull(error);
	h_free(&h);
}

/* Without a window: clear errors, and the tools that need none work. */
static void
test_no_window(void)
{
	H h = { 0 };
	g_autofree gchar *yaml = config_yaml(TRUE, NULL);
	g_autofree gchar *text = NULL;

	if (!h_mcp(&h, yaml))
		return;
#define NO_WINDOW(tool, args) G_STMT_START { \
		g_autofree gchar *t__ = call(&h, tool, args, TRUE); \
		g_assert_cmpstr(t__, ==, "no active window"); \
	} G_STMT_END
	NO_WINDOW("list_views", NULL);
	NO_WINDOW("get_tab_tree", NULL);
	NO_WINDOW("list_panes", NULL);
	NO_WINDOW("wmux_command", "{\"command\":\"kill-pane\"}");
	NO_WINDOW("split_pane", NULL);
	NO_WINDOW("run_action", "{\"action\":\"zoom-in\"}");
	NO_WINDOW("navigate", "{\"uri\":\"about:blank\"}");
	NO_WINDOW("open_child_view", NULL);
#undef NO_WINDOW

	/* Status, containers, modes and actions need no window. */
	text = call(&h, "wmux_status", NULL, FALSE);
	ASSERT_HAS(text, "\"loaded\":false");
	ASSERT_HAS(text, "\"active\":false");
	g_free(text);
	text = call(&h, "get_mode", NULL, FALSE);
	ASSERT_HAS(text, "\"mode\":\"normal\"");
	ASSERT_HAS(text, "\"label\":\"NORMAL\"");
	ASSERT_HAS(text, "\"modal_active\":false");
	g_free(text);
	text = call(&h, "list_actions", NULL, FALSE);
	ASSERT_HAS(text, "{\"name\":\"zoom-in\",\"allowed\":true}");
	ASSERT_HAS(text, "{\"name\":\"pane-split-right\",\"allowed\":true}");
	g_free(text);
	text = call(&h, "list_containers", NULL, FALSE);
	ASSERT_HAS(text, "\"id\":\"default\"");
	ASSERT_HAS(text, "\"n_views\":0");
	h_free(&h);
}

/* list_views, view addressing by id / index, lenient numbers, the opener
 * tree without a provider, and close/switch errors. */
static void
test_views(void)
{
	H h = { 0 };
	g_autofree gchar *yaml = config_yaml(TRUE, NULL);
	g_autoptr(JsonNode) root = NULL;
	gchar *text;
	JsonArray *views;
	JsonObject *v;
	guint64 a;
	guint64 b;
	guint64 c;

	if (!h_mcp(&h, yaml))
		return;
	h_window(&h);
	a = gsurf_view_get_id(h.a);
	b = gsurf_view_get_id(h.b);
	c = gsurf_view_get_id(h.c);

	/* 1. list_views: ids, opener, tree parent (derived from openers),
	 *    no layout so no pane, only the active view visible. */
	text = call(&h, "list_views", NULL, FALSE);
	root = parse(text);
	g_free(text);
	views = json_node_get_array(root);
	g_assert_cmpuint(json_array_get_length(views), ==, 3);
	v = json_array_get_object_element(views, 1);
	g_assert_cmpint(json_object_get_int_member(v, "index"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(v, "id"), ==, (gint64)b);
	g_assert_cmpint(json_object_get_int_member(v, "opener_id"), ==, (gint64)a);
	g_assert_cmpint(json_object_get_int_member(v, "parent_id"), ==, (gint64)a);
	g_assert_false(json_object_get_boolean_member(v, "active"));
	g_assert_false(json_object_get_boolean_member(v, "visible"));
	g_assert_true(json_object_get_null_member(v, "container"));
	g_assert_true(json_object_get_null_member(v, "pane"));
	g_assert_true(json_object_get_boolean_member(json_array_get_object_element(views, 0),
		"visible"));
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(views, 2),
		"parent_id"), ==, 0);

	/* 2. switch_view by id, by index, by a numeric string, and errors. */
	g_free(callf(&h, "switch_view", FALSE, "{\"view\":%" G_GUINT64_FORMAT "}", c));
	g_assert_true(gsurf_window_get_active_view(h.window) == h.c);
	g_free(call(&h, "switch_view", "{\"index\":1}", FALSE));
	g_assert_true(gsurf_window_get_active_view(h.window) == h.b);
	g_free(callf(&h, "switch_view", FALSE, "{\"view\":\"%" G_GUINT64_FORMAT "\"}", a));
	g_assert_true(gsurf_window_get_active_view(h.window) == h.a);
	g_free(callf(&h, "switch_view", FALSE, "{\"view\":%" G_GUINT64_FORMAT ".0}", b));
	g_assert_true(gsurf_window_get_active_view(h.window) == h.b);
	text = call(&h, "switch_view", "{\"view\":999999}", TRUE);
	ASSERT_HAS(text, "unknown view 999999");
	g_free(text);
	text = call(&h, "switch_view", "{\"index\":7}", TRUE);
	ASSERT_HAS(text, "index 7 out of range (the window has 3 views)");
	g_free(text);
	text = call(&h, "switch_view", "{\"view\":\"x\"}", TRUE);
	ASSERT_HAS(text, "'view' must be an integer");
	g_free(text);
	text = call(&h, "switch_view", NULL, TRUE);
	ASSERT_HAS(text, "missing 'view' (or 'index')");
	g_free(text);

	/* 3. The page tools take a view id too (an unknown one fails). */
	text = call(&h, "get_title", "{\"view\":424242}", TRUE);
	ASSERT_HAS(text, "unknown view 424242");
	g_free(text);
	g_free(callf(&h, "get_title", FALSE, "{\"view\":%" G_GUINT64_FORMAT "}", c));

	/* 4. close_view by id; never the last view. */
	g_free(callf(&h, "close_view", FALSE, "{\"view\":%" G_GUINT64_FORMAT "}", c));
	g_assert_cmpuint(gsurf_window_get_n_views(h.window), ==, 2);
	g_free(call(&h, "close_view", "{\"index\":1}", FALSE));
	text = call(&h, "close_view", "{\"index\":0}", TRUE);
	g_assert_cmpstr(text, ==, "cannot close the last view");
	g_free(text);
	h_free(&h);
}

/* The tab tree: opener-derived without a provider, the provider's answers
 * with one; move_view and set_collapsed through the provider, and their
 * errors without one. */
static void
test_tree(void)
{
	H h = { 0 };
	g_autofree gchar *yaml = config_yaml(TRUE, NULL);
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *want = NULL;
	gchar *text;
	JsonArray *tree;
	JsonObject *node;
	guint64 a;
	guint64 b;
	guint64 c;

	if (!h_mcp(&h, yaml))
		return;
	h_window(&h);
	a = gsurf_view_get_id(h.a);
	b = gsurf_view_get_id(h.b);
	c = gsurf_view_get_id(h.c);

	/* 1. No provider: roots a (with child b) and c; not grouped. */
	text = call(&h, "get_tab_tree", NULL, FALSE);
	root = parse(text);
	g_free(text);
	node = json_node_get_object(root);
	g_assert_false(json_object_get_boolean_member(node, "has_tree_provider"));
	g_assert_false(json_object_get_boolean_member(node, "grouped"));
	g_assert_true(json_object_get_null_member(node, "groups"));
	tree = json_object_get_array_member(node, "tree");
	g_assert_cmpuint(json_array_get_length(tree), ==, 2);
	node = json_array_get_object_element(tree, 0);
	g_assert_cmpint(json_object_get_int_member(node, "id"), ==, (gint64)a);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(node, "children")), ==, 1);
	g_clear_pointer(&root, json_node_unref);

	/* Without a provider: re-parenting and collapsing need the tabs
	 * module; sibling moves are window reorders. */
	text = callf(&h, "move_view", TRUE, "{\"view\":%" G_GUINT64_FORMAT ",\"parent\":%"
		G_GUINT64_FORMAT "}", c, a);
	ASSERT_HAS(text, "enable the tabs module");
	g_free(text);
	text = callf(&h, "set_collapsed", TRUE, "{\"view\":%" G_GUINT64_FORMAT "}", a);
	ASSERT_HAS(text, "enable the tabs module");
	g_free(text);
	g_free(callf(&h, "move_view", FALSE, "{\"view\":%" G_GUINT64_FORMAT ",\"before\":%"
		G_GUINT64_FORMAT "}", c, a));
	g_assert_true(gsurf_window_get_nth_view(h.window, 0) == h.c);
	g_free(callf(&h, "move_view", FALSE, "{\"view\":%" G_GUINT64_FORMAT ",\"after\":%"
		G_GUINT64_FORMAT "}", c, b));
	g_assert_true(gsurf_window_get_nth_view(h.window, 2) == h.c);
	g_free(callf(&h, "move_view", FALSE, "{\"view\":%" G_GUINT64_FORMAT ",\"index\":0}", b));
	g_assert_true(gsurf_window_get_nth_view(h.window, 0) == h.b);
	g_free(callf(&h, "move_view", FALSE, "{\"view\":%" G_GUINT64_FORMAT ",\"index\":1}", b));

	/* Destination errors. */
	text = callf(&h, "move_view", TRUE, "{\"view\":%" G_GUINT64_FORMAT "}", a);
	ASSERT_HAS(text, "missing destination");
	g_free(text);
	text = callf(&h, "move_view", TRUE, "{\"view\":%" G_GUINT64_FORMAT ",\"before\":%"
		G_GUINT64_FORMAT ",\"index\":0}", a, b);
	ASSERT_HAS(text, "give only one of");
	g_free(text);
	text = callf(&h, "move_view", TRUE, "{\"view\":%" G_GUINT64_FORMAT ",\"after\":%"
		G_GUINT64_FORMAT "}", a, a);
	ASSERT_HAS(text, "relative to itself");
	g_free(text);
	text = callf(&h, "move_view", TRUE, "{\"view\":%" G_GUINT64_FORMAT ",\"after\":77777}", a);
	ASSERT_HAS(text, "unknown view 77777");
	g_free(text);

	/* 2. With a provider: its parents and collapse state. */
	h_tree(&h);
	h.tree->parent_of_c = a;
	h.tree->collapsed = TRUE;
	text = call(&h, "get_tab_tree", NULL, FALSE);
	root = parse(text);
	g_free(text);
	node = json_node_get_object(root);
	g_assert_true(json_object_get_boolean_member(node, "has_tree_provider"));
	tree = json_object_get_array_member(node, "tree");
	g_assert_cmpuint(json_array_get_length(tree), ==, 1);
	node = json_array_get_object_element(tree, 0);
	g_assert_true(json_object_get_boolean_member(node, "collapsed"));
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(node, "children")), ==, 2);

	/* move_view / set_collapsed go to the provider. */
	g_free(callf(&h, "move_view", FALSE, "{\"view\":%" G_GUINT64_FORMAT ",\"parent\":%"
		G_GUINT64_FORMAT "}", c, b));
	g_free(callf(&h, "move_view", FALSE, "{\"view\":%" G_GUINT64_FORMAT ",\"parent\":0}", c));
	g_free(callf(&h, "move_view", FALSE, "{\"view\":%" G_GUINT64_FORMAT ",\"before\":%"
		G_GUINT64_FORMAT "}", b, a));
	g_free(callf(&h, "move_view", FALSE, "{\"view\":%" G_GUINT64_FORMAT ",\"after\":%"
		G_GUINT64_FORMAT "}", b, c));
	want = g_strdup_printf("move %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " child;"
		"move %" G_GUINT64_FORMAT " 0 child;move %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
		" before;move %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " after;",
		c, b, c, b, a, b, c);
	ASSERT_LOG(h.tree->log, want);
	g_free(callf(&h, "set_collapsed", FALSE, "{\"view\":%" G_GUINT64_FORMAT
		",\"collapsed\":false}", a));
	g_assert_false(h.tree->collapsed);
	text = callf(&h, "set_collapsed", TRUE, "{\"view\":%" G_GUINT64_FORMAT "}", b);
	ASSERT_HAS(text, "has no children to collapse");
	g_free(text);
	text = callf(&h, "set_collapsed", TRUE, "{\"view\":%" G_GUINT64_FORMAT
		",\"collapsed\":\"maybe\"}", a);
	ASSERT_HAS(text, "'collapsed' must be a boolean");
	g_free(text);
	h_free(&h);
}

/* The wmux tools against the recording layout: exact command strings,
 * the layout's errors as the tool's error, the groups and panes as JSON,
 * rename and drops through their own seams. */
static void
test_wmux(void)
{
	H h = { 0 };
	g_autofree gchar *yaml = config_yaml(TRUE, NULL);
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *want = NULL;
	gchar *text;
	JsonArray *list;
	JsonObject *obj;
	JsonObject *pane;
	guint64 a;
	guint64 b;
	guint64 c;

	if (!h_mcp(&h, yaml))
		return;
	h_window(&h);
	a = gsurf_view_get_id(h.a);
	b = gsurf_view_get_id(h.b);
	c = gsurf_view_get_id(h.c);

	/* 1. No layout: every wmux tool says how to get one. */
	text = call(&h, "list_panes", NULL, TRUE);
	ASSERT_HAS(text, "enable the wmux module");
	g_free(text);
	text = call(&h, "wmux_command", "{\"command\":\"kill-pane\"}", TRUE);
	ASSERT_HAS(text, "enable the wmux module");
	g_free(text);

	h_layout(&h);

	/* 2. Status from describe(); windows and panes. */
	text = call(&h, "wmux_status", NULL, FALSE);
	ASSERT_HAS(text, "\"active\":true");
	ASSERT_HAS(text, "\"name\":\"fake\"");
	ASSERT_HAS(text, "\"prefix\":\"Ctrl+b\"");
	ASSERT_HAS(text, "\"mouse\":true");
	ASSERT_HAS(text, "\"width\":800");
	ASSERT_HAS(text, "\"windows\":2");
	g_free(text);
	text = call(&h, "list_wmux_windows", NULL, FALSE);
	root = parse(text);
	g_free(text);
	list = json_node_get_array(root);
	g_assert_cmpuint(json_array_get_length(list), ==, 2);
	obj = json_array_get_object_element(list, 1);
	g_assert_cmpstr(json_object_get_string_member(obj, "name"), ==, "docs");
	g_assert_true(json_object_get_boolean_member(obj, "last"));
	g_assert_true(json_object_get_boolean_member(obj, "zoomed"));
	g_assert_false(json_object_get_boolean_member(obj, "current"));
	pane = json_array_get_object_element(json_object_get_array_member(obj, "panes"), 0);
	g_assert_cmpint(json_object_get_int_member(pane, "view"), ==, (gint64)c);
	g_assert_false(json_object_get_boolean_member(pane, "visible"));
	g_clear_pointer(&root, json_node_unref);

	text = call(&h, "list_panes", NULL, FALSE);
	root = parse(text);
	g_free(text);
	obj = json_node_get_object(root);
	g_assert_cmpint(json_object_get_int_member(obj, "window"), ==, 0);
	list = json_object_get_array_member(obj, "panes");
	g_assert_cmpuint(json_array_get_length(list), ==, 2);
	pane = json_array_get_object_element(list, 1);
	g_assert_cmpint(json_object_get_int_member(pane, "index"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(pane, "id"), ==, 6);
	g_assert_cmpint(json_object_get_int_member(pane, "view"), ==, (gint64)b);
	g_assert_false(json_object_get_boolean_member(pane, "active"));
	g_assert_true(json_object_get_boolean_member(pane, "visible"));
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(pane, "rect"),
		"x"), ==, 402);
	g_assert_cmpfloat(json_object_get_double_member(json_object_get_object_member(pane,
		"fraction"), "x"), ==, 0.5);
	g_clear_pointer(&root, json_node_unref);
	text = call(&h, "list_panes", "{\"window\":5}", TRUE);
	ASSERT_HAS(text, "unknown wmux window 5");
	g_free(text);

	/* list_views now reports panes and visibility. */
	text = call(&h, "list_views", NULL, FALSE);
	ASSERT_HAS(text, "\"pane\":{\"window\":1,\"index\":0}");
	ASSERT_HAS(text, "\"pane\":{\"window\":0,\"index\":1}");
	g_free(text);

	/* 3. wmux_command: the line as given; a failure is the error text. */
	g_free(call(&h, "wmux_command", "{\"command\":\"rotate-window -D\"}", FALSE));
	ASSERT_LOG(h.layout->log, "rotate-window -D;");
	text = call(&h, "wmux_command", "{\"command\":\"bad-command\"}", TRUE);
	g_assert_cmpstr(text, ==, "refused (bad-command)");
	g_free(text);
	g_string_truncate(h.layout->log, 0);
	text = call(&h, "wmux_command", "{\"command\":\"\"}", TRUE);
	ASSERT_HAS(text, "missing 'command'");
	g_free(text);

	/* 4. split_pane: direction, ratio (fraction or percent), duplicate,
	 *    background, a quoted URI, a target pane. */
	g_free(call(&h, "split_pane", NULL, FALSE));
	ASSERT_LOG(h.layout->log, "split-window -h;");
	g_free(call(&h, "split_pane", "{\"direction\":\"left\",\"ratio\":0.3,\"duplicate\":true,"
		"\"background\":true}", FALSE));
	ASSERT_LOG(h.layout->log, "split-window -h -b -d -c -p 30;");
	g_free(call(&h, "split_pane", "{\"direction\":\"down\",\"ratio\":25,\"pane\":1,"
		"\"uri\":\"https://example.org/a b?q=\\\"x\\\"\"}", FALSE));
	ASSERT_LOG(h.layout->log, "select-pane -t 1;split-window -v -p 25 "
		"\"https://example.org/a b?q=\\\"x\\\"\";");
	g_free(call(&h, "split_pane", "{\"direction\":\"up\",\"uri\":\"https://example.org/\"}",
		FALSE));
	ASSERT_LOG(h.layout->log, "split-window -v -b https://example.org/;");
	text = call(&h, "split_pane", "{\"direction\":\"sideways\"}", TRUE);
	ASSERT_HAS(text, "'direction' must be");
	g_free(text);
	text = call(&h, "split_pane", "{\"ratio\":120}", TRUE);
	ASSERT_HAS(text, "'ratio' must be");
	g_free(text);
	text = call(&h, "split_pane", "{\"pane\":9}", TRUE);
	g_assert_cmpstr(text, ==, "select-pane -t 9: refused (select-pane -t 9)");
	g_free(text);
	g_string_truncate(h.layout->log, 0);

	/* 5. select_pane: direction, last, pane, view (in another window:
	 *    selects the window first). */
	g_free(call(&h, "select_pane", "{\"direction\":\"right\"}", FALSE));
	g_free(call(&h, "select_pane", "{\"last\":true}", FALSE));
	g_free(call(&h, "select_pane", "{\"pane\":1}", FALSE));
	g_free(callf(&h, "select_pane", FALSE, "{\"view\":%" G_GUINT64_FORMAT "}", c));
	ASSERT_LOG(h.layout->log, "select-pane -R;last-pane;select-pane -t 1;select-window -t 1;"
		"select-pane -t 0;");
	text = call(&h, "select_pane", NULL, TRUE);
	ASSERT_HAS(text, "give 'pane', 'view', 'direction' or last=true");
	g_free(text);
	h.layout->current = 0;

	/* 6. kill_pane, zoom_pane (toggle vs. requested state). */
	g_free(callf(&h, "kill_pane", FALSE, "{\"view\":%" G_GUINT64_FORMAT "}", b));
	g_free(call(&h, "zoom_pane", NULL, FALSE));
	g_free(call(&h, "zoom_pane", "{\"zoomed\":false}", FALSE));
	ASSERT_LOG(h.layout->log, "select-pane -t 1;kill-pane;resize-pane -Z;");

	/* A background tab cannot be addressed as a pane. */
	gsurf_window_set_layout(h.window, NULL);
	h.layout->ids[1] = 0;
	g_assert_true(gsurf_window_set_layout(h.window, GSURF_WINDOW_LAYOUT(h.layout)));
	text = callf(&h, "kill_pane", TRUE, "{\"view\":%" G_GUINT64_FORMAT "}", b);
	ASSERT_HAS(text, "is not shown in any pane");
	g_free(text);
	/* ... but split_pane puts it in a new pane beside the active one. */
	g_free(callf(&h, "split_pane", FALSE, "{\"view\":%" G_GUINT64_FORMAT
		",\"direction\":\"down\"}", b));
	want = g_strdup_printf("drop %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " bottom;", b, a);
	ASSERT_LOG(h.layout->log, want);
	g_clear_pointer(&want, g_free);
	text = callf(&h, "split_pane", TRUE, "{\"view\":%" G_GUINT64_FORMAT
		",\"uri\":\"about:blank\"}", b);
	ASSERT_HAS(text, "do not apply");
	g_free(text);
	h.layout->ids[1] = b;

	/* 7. Layouts, windows, names (quoted). */
	g_free(call(&h, "select_layout", "{\"name\":\"tiled\"}", FALSE));
	g_free(call(&h, "select_layout", "{\"name\":\"next\"}", FALSE));
	g_free(call(&h, "select_layout", "{\"name\":\"even\"}", FALSE));
	g_free(call(&h, "select_window", "{\"which\":\"next\"}", FALSE));
	g_free(call(&h, "select_window", "{\"which\":\"last\"}", FALSE));
	g_free(call(&h, "select_window", "{\"index\":0}", FALSE));
	g_free(call(&h, "new_window", "{\"name\":\"my docs\",\"uri\":\"about:blank\"}", FALSE));
	g_free(call(&h, "kill_window", NULL, FALSE));
	ASSERT_LOG(h.layout->log, "select-layout tiled;next-layout;select-layout -E;next-window;"
		"last-window;select-window -t 0;new-window -n \"my docs\" about:blank;kill-window;");
	text = call(&h, "select_window", NULL, TRUE);
	ASSERT_HAS(text, "give 'index' or 'which'");
	g_free(text);
	text = call(&h, "select_layout", NULL, TRUE);
	ASSERT_HAS(text, "missing 'name'");
	g_free(text);

	/* rename_window: through rename_group, current by default. */
	g_free(call(&h, "rename_window", "{\"index\":1,\"name\":\"x y\"}", FALSE));
	g_free(call(&h, "rename_window", "{\"name\":\"\"}", FALSE));
	ASSERT_LOG(h.layout->log, "rename 1 x y;rename 0 ;");
	text = call(&h, "rename_window", "{\"index\":4,\"name\":\"z\"}", TRUE);
	ASSERT_HAS(text, "unknown wmux window 4");
	g_free(text);
	text = call(&h, "rename_window", "{\"index\":1}", TRUE);
	ASSERT_HAS(text, "missing 'name'");
	g_free(text);

	/* 8. show_view_in_pane: by pane (with its window) or target view. */
	g_free(callf(&h, "show_view_in_pane", FALSE, "{\"view\":%" G_GUINT64_FORMAT
		",\"pane\":1,\"zone\":\"left\"}", c));
	want = g_strdup_printf("drop %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " left;", c, b);
	ASSERT_LOG(h.layout->log, want);
	g_clear_pointer(&want, g_free);
	g_free(callf(&h, "show_view_in_pane", FALSE, "{\"view\":%" G_GUINT64_FORMAT
		",\"target\":%" G_GUINT64_FORMAT "}", c, a));
	want = g_strdup_printf("drop %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " center;", c, a);
	ASSERT_LOG(h.layout->log, want);
	g_clear_pointer(&want, g_free);
	text = callf(&h, "show_view_in_pane", TRUE, "{\"view\":%" G_GUINT64_FORMAT
		",\"target\":%" G_GUINT64_FORMAT "}", c, c);
	ASSERT_HAS(text, "is not on screen");
	g_free(text);
	text = callf(&h, "show_view_in_pane", TRUE, "{\"view\":%" G_GUINT64_FORMAT
		",\"pane\":4}", c);
	ASSERT_HAS(text, "no pane 4 in the current wmux window");
	g_free(text);
	text = callf(&h, "show_view_in_pane", TRUE, "{\"view\":%" G_GUINT64_FORMAT
		",\"pane\":0,\"zone\":\"middle\"}", c);
	ASSERT_HAS(text, "'zone' must be");
	g_free(text);
	text = callf(&h, "show_view_in_pane", TRUE, "{\"view\":%" G_GUINT64_FORMAT
		",\"target\":%" G_GUINT64_FORMAT "}", a, a);
	ASSERT_HAS(text, "could not show view");
	g_free(text);
	g_string_truncate(h.layout->log, 0);

	/* 9. The grouped tab tree: group "web" shows a (with no background
	 *    child: b is in a pane) and b; "docs" shows c; no background. */
	g_clear_pointer(&root, json_node_unref);
	text = call(&h, "get_tab_tree", NULL, FALSE);
	root = parse(text);
	g_free(text);
	obj = json_node_get_object(root);
	g_assert_true(json_object_get_boolean_member(obj, "grouped"));
	list = json_object_get_array_member(obj, "groups");
	g_assert_cmpuint(json_array_get_length(list), ==, 2);
	obj = json_array_get_object_element(list, 0);
	g_assert_cmpstr(json_object_get_string_member(obj, "name"), ==, "web");
	list = json_object_get_array_member(obj, "tabs");
	g_assert_cmpuint(json_array_get_length(list), ==, 2);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(list, 1),
		"pane_index"), ==, 1);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
		json_array_get_object_element(list, 0), "children")), ==, 0);
	h_free(&h);
}

/* run_action, modes and hints: unknown names, the module path (no host
 * executor here), the host executor, kiosk refusals; and the kiosk
 * refusals of the view-opening tools. */
static void
test_actions_and_kiosk(void)
{
	H h = { 0 };
	g_autofree gchar *yaml = config_yaml(TRUE, NULL);
	gchar *text;

	if (!h_mcp(&h, yaml))
		return;
	h_window(&h);
	h_tree(&h);

	/* 1. Actions by name (underscores accepted); performed by a module,
	 *    or reported as not performed. */
	text = call(&h, "run_action", "{\"action\":\"find\"}", FALSE);
	g_assert_cmpstr(text, ==, "{\"action\":\"find\",\"performed\":true}");
	g_free(text);
	text = call(&h, "run_action", "{\"action\":\"zoom_in\"}", TRUE);
	ASSERT_HAS(text, "nothing performed action 'zoom-in'");
	g_free(text);
	ASSERT_LOG(h.tree->log, "action find;action zoom-in;");
	text = call(&h, "run_action", "{\"action\":\"fly\"}", TRUE);
	ASSERT_HAS(text, "unknown action 'fly'");
	g_free(text);
	text = call(&h, "run_action", NULL, TRUE);
	ASSERT_HAS(text, "missing 'action'");
	g_free(text);

	/* 2. Modes need the modal module. */
	text = call(&h, "set_mode", "{\"mode\":\"insert\"}", TRUE);
	ASSERT_HAS(text, "need the modal module");
	g_free(text);
	text = call(&h, "set_mode", "{\"mode\":\"visual\"}", TRUE);
	ASSERT_HAS(text, "\"normal\" or \"insert\"");
	g_free(text);
	text = call(&h, "follow_hints", NULL, TRUE);
	ASSERT_HAS(text, "need the modal module");
	g_free(text);

	/* 3. Kiosk: forbidden actions and view-opening tools are refused
	 *    before anything runs; allowed actions still go through. */
	h.config->kiosk = TRUE;
	g_string_truncate(h.tree->log, 0);
	text = call(&h, "run_action", "{\"action\":\"open-prompt\"}", TRUE);
	ASSERT_HAS(text, "not allowed in kiosk mode");
	g_free(text);
	g_free(call(&h, "run_action", "{\"action\":\"find\"}", FALSE));
	ASSERT_LOG(h.tree->log, "action find;");
	text = call(&h, "open_view", NULL, TRUE);
	ASSERT_HAS(text, "kiosk mode");
	g_free(text);
	text = call(&h, "open_child_view", NULL, TRUE);
	ASSERT_HAS(text, "kiosk mode");
	g_free(text);
	text = call(&h, "new_temporary_container_view", NULL, TRUE);
	ASSERT_HAS(text, "kiosk mode");
	g_free(text);
	text = call(&h, "reopen_in_container", "{\"container\":\"temporary\"}", TRUE);
	ASSERT_HAS(text, "kiosk mode");
	g_free(text);
	text = call(&h, "list_actions", NULL, FALSE);
	ASSERT_HAS(text, "{\"name\":\"open-prompt\",\"allowed\":false}");
	ASSERT_HAS(text, "{\"name\":\"zoom-in\",\"allowed\":true}");
	g_free(text);
	text = call(&h, "list_panes", NULL, TRUE);
	ASSERT_HAS(text, "kiosk mode shows a single view");
	g_free(text);
	h.config->kiosk = FALSE;

	/* 4. Containers: unknown ids are named; the argument is required. */
	text = call(&h, "reopen_in_container", "{\"container\":\"nope\"}", TRUE);
	ASSERT_HAS(text, "unknown container 'nope'");
	g_free(text);
	text = call(&h, "reopen_in_container", NULL, TRUE);
	ASSERT_HAS(text, "missing 'container'");
	g_free(text);
	text = call(&h, "open_child_view", "{\"container\":\"nope\"}", TRUE);
	ASSERT_HAS(text, "unknown container 'nope'");
	g_free(text);
	h_free(&h);
}

/* list_containers: registry containers with the containers module's
 * configured host rules (matched by id, or by name without one). */
static void
test_containers(void)
{
	H h = { 0 };
	g_autofree gchar *yaml = config_yaml(TRUE,
		"  containers:\n    enabled: false\n    containers:\n"
		"      - name: Media\n        color: \"#f9e2af\"\n"
		"        hosts: ['(^|\\.)youtube\\.com$', 'twitch\\.tv$']\n"
		"      - name: Work\n        id: job\n        hosts: ['corp\\.example$']\n");
	GsurfContainerRegistry *registry = gsurf_container_registry_get_default();
	g_autoptr(GError) error = NULL;
	gchar *text;

	if (!h_mcp(&h, yaml))
		return;
	g_assert_nonnull(gsurf_container_registry_define(registry, "media", "Media", "#f9e2af",
		&error));
	g_assert_no_error(error);
	g_assert_nonnull(gsurf_container_registry_define(registry, "job", "Work", NULL, &error));
	g_assert_no_error(error);

	text = call(&h, "list_containers", NULL, FALSE);
	ASSERT_HAS(text, "{\"id\":\"media\",\"name\":\"Media\",\"color\":\"#f9e2af\","
		"\"temporary\":false,\"default\":false,\"n_views\":0,"
		"\"rules\":[\"(^|\\\\.)youtube\\\\.com$\",\"twitch\\\\.tv$\"],\"rules_active\":false}");
	ASSERT_HAS(text, "{\"id\":\"job\",\"name\":\"Work\",\"color\":null,\"temporary\":false,"
		"\"default\":false,\"n_views\":0,\"rules\":[\"corp\\\\.example$\"]");
	g_free(text);

	g_assert_true(gsurf_container_registry_undefine(registry, "media", &error));
	g_assert_true(gsurf_container_registry_undefine(registry, "job", &error));
	h_free(&h);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *tmp = NULL;
	int ret;

	g_test_init(&argc, &argv, NULL);
	tmp = g_dir_make_tmp("gsurf-test-mcp-XXXXXX", NULL);
	g_assert_nonnull(tmp);
	g_setenv("XDG_RUNTIME_DIR", tmp, TRUE);
	g_setenv("XDG_DATA_HOME", tmp, TRUE);

	g_test_add_func("/mcp/registration", test_registration);
	g_test_add_func("/mcp/no-window", test_no_window);
	g_test_add_func("/mcp/views", test_views);
	g_test_add_func("/mcp/tree", test_tree);
	g_test_add_func("/mcp/wmux", test_wmux);
	g_test_add_func("/mcp/actions-and-kiosk", test_actions_and_kiosk);
	g_test_add_func("/mcp/containers", test_containers);

	ret = g_test_run();
	if (so_handle != NULL)
		g_module_close(so_handle);
	g_rmdir(tmp);
	return ret;
}
