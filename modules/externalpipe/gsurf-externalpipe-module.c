/*
 * gsurf-externalpipe-module.c - Pipe page content to external commands
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler: configurable keys feed the page HTML,
 * text, selection, or URI to a shell command (link extraction, editing).
 * Ports surf's externalpipe patch.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <gio/gio.h>
#include <unistd.h>

#define GSURF_TYPE_EXTERNALPIPE_MODULE (gsurf_externalpipe_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfExternalpipeModule, gsurf_externalpipe_module,
                     GSURF, EXTERNALPIPE_MODULE, GsurfModule)

typedef struct { gchar *key; gchar *command; gchar *input; } PipeCmd;

struct _GsurfExternalpipeModule
{
	GsurfModule parent_instance;
	GPtrArray *commands;  /* PipeCmd* */
};

static void gsurf_externalpipe_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfExternalpipeModule, gsurf_externalpipe_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_externalpipe_input_init))

/* --- synchronous JS via a nested main loop --- */
typedef struct { GMainLoop *loop; GsurfView *view; gchar *result; } JsCtx;

static void
js_done(GObject *src, GAsyncResult *res, gpointer data)
{
	JsCtx *c = data;
	c->result = gsurf_view_run_javascript_finish(c->view, res, NULL);
	g_main_loop_quit(c->loop);
}

static gchar *
run_js_sync(GsurfView *view, const gchar *script)
{
	JsCtx c = { 0 };
	c.view = view;
	c.loop = g_main_loop_new(NULL, FALSE);
	gsurf_view_run_javascript_async(view, script, NULL, js_done, &c);
	g_main_loop_run(c.loop);
	g_main_loop_unref(c.loop);
	return c.result;
}

static void
pipe_to_command(const gchar *command, const gchar *data)
{
	g_autofree gchar *tmpl = g_build_filename(g_get_tmp_dir(), "gsurf-pipe-XXXXXX", NULL);
	g_autoptr(GError) error = NULL;
	gint fd;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *cmd = NULL;
	gchar *argv[] = { "sh", "-c", NULL, NULL };

	fd = g_mkstemp(tmpl);
	if (fd < 0)
		return;
	close(fd);
	if (!g_file_set_contents(tmpl, data ? data : "", -1, &error)) {
		g_warning("gsurf externalpipe: %s", error->message);
		return;
	}

	quoted = g_shell_quote(tmpl);
	cmd = g_strdup_printf("%s < %s; rm -f %s", command, quoted, quoted);
	argv[2] = cmd;
	g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
	if (error != NULL)
		g_warning("gsurf externalpipe: %s", error->message);
}

static gboolean
gsurf_externalpipe_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                                    guint keyval, guint keycode, guint state, GsurfModePolicy mode)
{
	GsurfExternalpipeModule *self = GSURF_EXTERNALPIPE_MODULE(handler);
	guint i;

	if (view == NULL)
		return FALSE;

	for (i = 0; i < self->commands->len; i++) {
		PipeCmd *pc = g_ptr_array_index(self->commands, i);
		g_autofree gchar *data = NULL;

		if (pc->key == NULL || !gsurf_keys_match(keyval, state, pc->key))
			continue;

		if (g_strcmp0(pc->input, "uri") == 0)
			data = g_strdup(gsurf_view_get_uri(view));
		else if (g_strcmp0(pc->input, "text") == 0)
			data = run_js_sync(view, "document.body?document.body.innerText:''");
		else if (g_strcmp0(pc->input, "selection") == 0)
			data = run_js_sync(view, "window.getSelection().toString()");
		else /* html */
			data = run_js_sync(view, "document.documentElement.outerHTML");

		pipe_to_command(pc->command, data);
		return TRUE;
	}
	return FALSE;
}

static void
gsurf_externalpipe_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_externalpipe_handle_key_event;
}

static const gchar *gsurf_externalpipe_get_name(GsurfModule *m) { return "externalpipe"; }
static const gchar *gsurf_externalpipe_get_description(GsurfModule *m)
{
	return "Pipe page HTML/text/selection/URI to external commands";
}

static void
pipe_cmd_free(gpointer p)
{
	PipeCmd *c = p;
	g_free(c->key);
	g_free(c->command);
	g_free(c->input);
	g_free(c);
}

static void
gsurf_externalpipe_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfExternalpipeModule *self = GSURF_EXTERNALPIPE_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	YamlSequence *cmds;
	guint i, n;

	node = gsurf_config_get_module_node(config, "externalpipe");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	cmds = yaml_mapping_get_sequence_member(m, "commands");
	if (cmds == NULL)
		return;
	n = yaml_sequence_get_length(cmds);
	for (i = 0; i < n; i++) {
		YamlNode *en = yaml_sequence_get_element(cmds, i);
		YamlMapping *cm;
		PipeCmd *pc;

		if (en == NULL || yaml_node_get_node_type(en) != YAML_NODE_MAPPING)
			continue;
		cm = yaml_node_get_mapping(en);
		pc = g_new0(PipeCmd, 1);
		pc->key = g_strdup(yaml_mapping_get_string_member(cm, "key"));
		pc->command = g_strdup(yaml_mapping_get_string_member(cm, "command"));
		pc->input = g_strdup(yaml_mapping_get_string_member(cm, "input"));
		if (pc->key != NULL && pc->command != NULL)
			g_ptr_array_add(self->commands, pc);
		else
			pipe_cmd_free(pc);
	}
}

static gboolean gsurf_externalpipe_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_externalpipe_module_finalize(GObject *object)
{
	GsurfExternalpipeModule *self = GSURF_EXTERNALPIPE_MODULE(object);
	g_clear_pointer(&self->commands, g_ptr_array_unref);
	G_OBJECT_CLASS(gsurf_externalpipe_module_parent_class)->finalize(object);
}

static void
gsurf_externalpipe_module_class_init(GsurfExternalpipeModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_externalpipe_module_finalize;
	module_class->activate = gsurf_externalpipe_activate;
	module_class->get_name = gsurf_externalpipe_get_name;
	module_class->get_description = gsurf_externalpipe_get_description;
	module_class->configure = gsurf_externalpipe_configure;
}

static void
gsurf_externalpipe_module_init(GsurfExternalpipeModule *self)
{
	self->commands = g_ptr_array_new_with_free_func(pipe_cmd_free);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_EXTERNALPIPE_MODULE;
}
