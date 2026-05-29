/*
 * gsurf-modal-module.c - Vim-style modal navigation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements #GsurfInputHandler at high priority and tracks its own
 * mode. In NORMAL mode, bare keys are navigation commands (hjkl, gg, G,
 * d/u, H/L, r) and are consumed so they never reach the page; `i` enters
 * INSERT mode where keys pass through; Escape returns to NORMAL. Ports
 * surf's modal patch and adds vim navigation.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <string.h>

#define GSURF_TYPE_MODAL_MODULE (gsurf_modal_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfModalModule, gsurf_modal_module, GSURF, MODAL_MODULE, GsurfModule)

struct _GsurfModalModule
{
	GsurfModule parent_instance;
	GsurfModePolicy mode;
	gboolean pending_g;   /* saw 'g', waiting for the second */
	gint scroll_step;
	gchar *key_insert;
	gchar *key_normal;
};

static void gsurf_modal_input_init(GsurfInputHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfModalModule, gsurf_modal_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_modal_input_init))

static void
scroll(GsurfView *view, const gchar *js)
{
	gsurf_view_run_javascript_async(view, js, NULL, NULL, NULL);
}

static gboolean
gsurf_modal_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                             guint keyval, guint keycode, guint state,
                             GsurfModePolicy mode_hint)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(handler);
	g_autofree gchar *name = gsurf_keys_to_string(keyval, 0); /* bare name */
	gboolean has_mod = (state & (GSURF_MOD_CTRL | GSURF_MOD_ALT | GSURF_MOD_SUPER)) != 0;
	g_autofree gchar *js = NULL;

	if (name == NULL || view == NULL)
		return FALSE;

	/* INSERT mode: only Escape is ours; everything else goes to the page. */
	if (self->mode == GSURF_MODE_INSERT) {
		if (g_strcmp0(name, "Escape") == 0) {
			self->mode = GSURF_MODE_NORMAL;
			return TRUE;
		}
		return FALSE;
	}

	/* NORMAL mode: let modified chords (Ctrl+...) fall through to core. */
	if (has_mod)
		return FALSE;

	/* Chord: gg -> top. */
	if (self->pending_g) {
		self->pending_g = FALSE;
		if (g_strcmp0(name, "g") == 0) {
			scroll(view, "window.scrollTo(0,0)");
			return TRUE;
		}
		/* fall through: 'g' then other key — treat the new key normally */
	}

	if (g_strcmp0(name, "i") == 0) { self->mode = GSURF_MODE_INSERT; return TRUE; }
	if (g_strcmp0(name, "Escape") == 0) { self->pending_g = FALSE; return TRUE; }
	if (g_strcmp0(name, "g") == 0) { self->pending_g = TRUE; return TRUE; }
	if (g_strcmp0(name, "G") == 0) { scroll(view, "window.scrollTo(0,document.body.scrollHeight)"); return TRUE; }
	if (g_strcmp0(name, "H") == 0) { gsurf_view_go_back(view); return TRUE; }
	if (g_strcmp0(name, "L") == 0) { gsurf_view_go_forward(view); return TRUE; }
	if (g_strcmp0(name, "r") == 0) { gsurf_view_reload(view, FALSE); return TRUE; }

	if (g_strcmp0(name, "j") == 0) js = g_strdup_printf("window.scrollBy(0,%d)", self->scroll_step);
	else if (g_strcmp0(name, "k") == 0) js = g_strdup_printf("window.scrollBy(0,-%d)", self->scroll_step);
	else if (g_strcmp0(name, "h") == 0) js = g_strdup_printf("window.scrollBy(-%d,0)", self->scroll_step);
	else if (g_strcmp0(name, "l") == 0) js = g_strdup_printf("window.scrollBy(%d,0)", self->scroll_step);
	else if (g_strcmp0(name, "d") == 0) js = g_strdup("window.scrollBy(0,window.innerHeight/2)");
	else if (g_strcmp0(name, "u") == 0) js = g_strdup("window.scrollBy(0,-window.innerHeight/2)");

	if (js != NULL) {
		scroll(view, js);
		return TRUE;
	}

	return FALSE;
}

static void
gsurf_modal_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_modal_handle_key_event;
}

static const gchar *gsurf_modal_get_name(GsurfModule *m) { return "modal"; }
static const gchar *gsurf_modal_get_description(GsurfModule *m)
{
	return "Vim-style modal navigation (normal/insert modes)";
}

static gboolean
gsurf_modal_activate(GsurfModule *module)
{
	/* Run before other input handlers so modal commands win. */
	gsurf_module_set_priority(module, GSURF_PRIORITY_HIGHEST);
	return TRUE;
}

static void
gsurf_modal_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(module);
	GsurfConfig *config = config_ptr;
	YamlNode *node;
	YamlMapping *m;
	const gchar *start;

	node = gsurf_config_get_module_node(config, "modal");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	if (yaml_mapping_has_member(m, "scroll_step"))
		self->scroll_step = (gint)yaml_mapping_get_int_member(m, "scroll_step");
	if (yaml_mapping_has_member(m, "start_mode")) {
		start = yaml_mapping_get_string_member(m, "start_mode");
		self->mode = g_strcmp0(start, "insert") == 0
			? GSURF_MODE_INSERT : GSURF_MODE_NORMAL;
	}
}

static void
gsurf_modal_module_finalize(GObject *object)
{
	GsurfModalModule *self = GSURF_MODAL_MODULE(object);

	g_clear_pointer(&self->key_insert, g_free);
	g_clear_pointer(&self->key_normal, g_free);

	G_OBJECT_CLASS(gsurf_modal_module_parent_class)->finalize(object);
}

static void
gsurf_modal_module_class_init(GsurfModalModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_modal_module_finalize;
	module_class->activate = gsurf_modal_activate;
	module_class->get_name = gsurf_modal_get_name;
	module_class->get_description = gsurf_modal_get_description;
	module_class->configure = gsurf_modal_configure;
}

static void
gsurf_modal_module_init(GsurfModalModule *self)
{
	self->mode = GSURF_MODE_NORMAL;
	self->pending_g = FALSE;
	self->scroll_step = 40;
	self->key_insert = g_strdup("i");
	self->key_normal = g_strdup("Escape");
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_MODAL_MODULE;
}
