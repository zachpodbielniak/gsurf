/*
 * gsurf-frames-module.c - reach a page's frames, cross-origin ones too
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * WebKit's UI-process API reads and scripts a page's main frame only, so
 * a form inside an iframe from another site -- a payment provider's card
 * fields -- is out of reach of page.text, page.eval, page.click and
 * page.type.  This module ships the other half: a web-process extension
 * (webext/) that lists a page's frames and runs a script in one of them,
 * answering gsurf_view_list_frames_async() and
 * gsurf_view_run_javascript_in_frame_async().  The module itself only
 * registers it (gsurf_web_process_extension_register()); it has no
 * options beyond `enabled:` and `web_process_extension:` (a path, for an
 * install that keeps the extension elsewhere).
 *
 * The extension reaches the web processes started after it is
 * registered, so the module belongs in the config at startup.
 */

#define _GNU_SOURCE
#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <dlfcn.h>

/* Where the web-process extension is built/installed, relative to the
 * directory holding frames.so. */
#define GSURF_FRAMES_WEBEXT_SUBDIR "frames-webext"
#define GSURF_FRAMES_WEBEXT_NAME   "gsurf-frames-webext.so"

/* The name the extension is registered under. */
#define GSURF_FRAMES_WEBEXT_KEY "frames"

G_MODULE_EXPORT GType gsurf_module_register(void);

#define GSURF_TYPE_FRAMES_MODULE (gsurf_frames_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfFramesModule, gsurf_frames_module, GSURF, FRAMES_MODULE, GsurfModule)

struct _GsurfFramesModule
{
	GsurfModule  parent_instance;
	gchar       *webext_path;       /* NULL = locate next to frames.so */
	gboolean     webext_registered;
};

G_DEFINE_FINAL_TYPE(GsurfFramesModule, gsurf_frames_module, GSURF_TYPE_MODULE)

/* The extension ships next to frames.so ("frames-webext/"), wherever the
 * module was loaded from: the build tree, an install, an AppImage. */
static gchar *
find_webext(GsurfFramesModule *self)
{
	g_autofree gchar *dir = NULL;
	gchar *path;
	Dl_info info;

	if (self->webext_path != NULL)
		return g_file_test(self->webext_path, G_FILE_TEST_IS_REGULAR)
			? g_strdup(self->webext_path) : NULL;
	if (dladdr((void *)gsurf_module_register, &info) == 0 || info.dli_fname == NULL)
		return NULL;
	dir = g_path_get_dirname(info.dli_fname);
	path = g_build_filename(dir, GSURF_FRAMES_WEBEXT_SUBDIR, GSURF_FRAMES_WEBEXT_NAME, NULL);
	if (!g_path_is_absolute(path)) {
		g_autofree gchar *cwd = gsurf_get_launch_dir();
		gchar *absolute = g_build_filename(cwd, path, NULL);

		g_free(path);
		path = absolute;
	}
	if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
		g_free(path);
		return NULL;
	}
	return path;
}

static void
unregister_webext(GsurfFramesModule *self)
{
	if (self->webext_registered)
		gsurf_web_process_extension_unregister(GSURF_FRAMES_WEBEXT_KEY);
	self->webext_registered = FALSE;
}

static void
gsurf_frames_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfFramesModule *self = GSURF_FRAMES_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m = NULL;

	/* a reconfigure starts from nothing */
	g_clear_pointer(&self->webext_path, g_free);
	node = config != NULL ? gsurf_config_get_module_node(config, "frames") : NULL;
	if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING)
		m = yaml_node_get_mapping(node);
	if (m != NULL && yaml_mapping_has_member(m, "web_process_extension")) {
		const gchar *path = yaml_mapping_get_string_member(m, "web_process_extension");

		if (path != NULL && *path != '\0')
			self->webext_path = gsurf_resolve_path(path);
	}
}

static gboolean
gsurf_frames_activate(GsurfModule *module)
{
	GsurfFramesModule *self = GSURF_FRAMES_MODULE(module);
	g_autofree gchar *webext = find_webext(self);

	unregister_webext(self);
	if (webext == NULL) {
		g_message("gsurf frames: web-process extension not found%s%s; "
			"page frames stay out of reach",
			self->webext_path != NULL ? " at " : " next to frames.so",
			self->webext_path != NULL ? self->webext_path : "");
		return TRUE;
	}
	gsurf_web_process_extension_register(GSURF_FRAMES_WEBEXT_KEY, webext, NULL);
	self->webext_registered = TRUE;
	return TRUE;
}

static void
gsurf_frames_deactivate(GsurfModule *module)
{
	unregister_webext(GSURF_FRAMES_MODULE(module));
}

static const gchar *gsurf_frames_get_name(GsurfModule *m) { return "frames"; }
static const gchar *gsurf_frames_get_description(GsurfModule *m)
{
	return "Read and drive a page's frames, cross-origin ones included";
}

static void
gsurf_frames_module_finalize(GObject *object)
{
	GsurfFramesModule *self = GSURF_FRAMES_MODULE(object);

	unregister_webext(self);
	g_clear_pointer(&self->webext_path, g_free);
	G_OBJECT_CLASS(gsurf_frames_module_parent_class)->finalize(object);
}

static void
gsurf_frames_module_class_init(GsurfFramesModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_frames_module_finalize;
	module_class->activate = gsurf_frames_activate;
	module_class->deactivate = gsurf_frames_deactivate;
	module_class->configure = gsurf_frames_configure;
	module_class->get_name = gsurf_frames_get_name;
	module_class->get_description = gsurf_frames_get_description;
}

static void
gsurf_frames_module_init(GsurfFramesModule *self)
{
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_FRAMES_MODULE;
}
