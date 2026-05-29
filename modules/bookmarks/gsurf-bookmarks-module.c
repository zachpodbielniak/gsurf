/*
 * gsurf-bookmarks-module.c - Save and recall bookmarks
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler: a key appends the current URI to a
 * bookmarks file; another runs dmenu over it and loads the selection.
 * Ports surf's bookmarking patch.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <stdio.h>
#include <string.h>

#define GSURF_TYPE_BOOKMARKS_MODULE (gsurf_bookmarks_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfBookmarksModule, gsurf_bookmarks_module,
                     GSURF, BOOKMARKS_MODULE, GsurfModule)

struct _GsurfBookmarksModule
{
	GsurfModule parent_instance;
	gchar *file;
	gchar *dmenu_cmd;
	gchar *key_add;
	gchar *key_open;
};

static void gsurf_bookmarks_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfBookmarksModule, gsurf_bookmarks_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_bookmarks_input_init))

/* Run "dmenu_cmd < file" and return the selected line, or NULL. */
static gchar *
dmenu_pick(GsurfBookmarksModule *self)
{
	g_autofree gchar *quoted = g_shell_quote(self->file);
	g_autofree gchar *cmd = g_strdup_printf("%s < %s", self->dmenu_cmd, quoted);
	gchar *argv[] = { "sh", "-c", cmd, NULL };
	g_autofree gchar *out = NULL;
	g_autoptr(GError) error = NULL;

	if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			&out, NULL, NULL, &error)) {
		g_warning("gsurf bookmarks: %s", error->message);
		return NULL;
	}
	if (out != NULL)
		g_strstrip(out);
	if (out == NULL || *out == '\0')
		return NULL;
	return g_steal_pointer(&out);
}

static gboolean
gsurf_bookmarks_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                                 guint keyval, guint keycode, guint state, GsurfModePolicy mode)
{
	GsurfBookmarksModule *self = GSURF_BOOKMARKS_MODULE(handler);

	if (view == NULL || self->file == NULL)
		return FALSE;

	if (self->key_add && gsurf_keys_match(keyval, state, self->key_add)) {
		const gchar *uri = gsurf_view_get_uri(view);
		FILE *f;
		if (uri != NULL && (f = fopen(self->file, "a")) != NULL) {
			fprintf(f, "%s\n", uri);
			fclose(f);
		}
		return TRUE;
	}
	if (self->key_open && gsurf_keys_match(keyval, state, self->key_open)) {
		g_autofree gchar *sel = dmenu_pick(self);
		if (sel != NULL)
			gsurf_view_load_uri(view, sel);
		return TRUE;
	}
	return FALSE;
}

static void
gsurf_bookmarks_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_bookmarks_handle_key_event;
}

static const gchar *gsurf_bookmarks_get_name(GsurfModule *m) { return "bookmarks"; }
static const gchar *gsurf_bookmarks_get_description(GsurfModule *m)
{
	return "Save and recall bookmarks via dmenu";
}

static gchar *
expand(const gchar *p)
{
	return (p && p[0] == '~' && p[1] == '/')
		? g_build_filename(g_get_home_dir(), p + 2, NULL) : g_strdup(p);
}

static void
gsurf_bookmarks_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfBookmarksModule *self = GSURF_BOOKMARKS_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;

	node = gsurf_config_get_module_node(config, "bookmarks");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "file")) {
		g_free(self->file);
		self->file = expand(yaml_mapping_get_string_member(m, "file"));
		g_autofree gchar *dir = g_path_get_dirname(self->file);
		g_mkdir_with_parents(dir, 0755);
	}
	if (yaml_mapping_has_member(m, "dmenu_cmd")) {
		g_free(self->dmenu_cmd);
		self->dmenu_cmd = g_strdup(yaml_mapping_get_string_member(m, "dmenu_cmd"));
	}
	if (yaml_mapping_has_member(m, "key_add")) {
		g_free(self->key_add);
		self->key_add = g_strdup(yaml_mapping_get_string_member(m, "key_add"));
	}
	if (yaml_mapping_has_member(m, "key_open")) {
		g_free(self->key_open);
		self->key_open = g_strdup(yaml_mapping_get_string_member(m, "key_open"));
	}
}

static gboolean gsurf_bookmarks_activate(GsurfModule *m) { return TRUE; }

static void
gsurf_bookmarks_module_finalize(GObject *object)
{
	GsurfBookmarksModule *self = GSURF_BOOKMARKS_MODULE(object);
	g_clear_pointer(&self->file, g_free);
	g_clear_pointer(&self->dmenu_cmd, g_free);
	g_clear_pointer(&self->key_add, g_free);
	g_clear_pointer(&self->key_open, g_free);
	G_OBJECT_CLASS(gsurf_bookmarks_module_parent_class)->finalize(object);
}

static void
gsurf_bookmarks_module_class_init(GsurfBookmarksModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_bookmarks_module_finalize;
	module_class->activate = gsurf_bookmarks_activate;
	module_class->get_name = gsurf_bookmarks_get_name;
	module_class->get_description = gsurf_bookmarks_get_description;
	module_class->configure = gsurf_bookmarks_configure;
}

static void
gsurf_bookmarks_module_init(GsurfBookmarksModule *self)
{
	self->file = g_build_filename(g_get_user_data_dir(), "gsurf", "bookmarks", NULL);
	self->dmenu_cmd = g_strdup("dmenu -l 20 -i");
	self->key_add = g_strdup("Ctrl+d");
	self->key_open = g_strdup("Ctrl+Shift+d");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_BOOKMARKS_MODULE;
}
