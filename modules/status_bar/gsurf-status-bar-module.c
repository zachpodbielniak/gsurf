/*
 * gsurf-status-bar-module.c - Bottom status bar
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A GtkLabel packed below the view area showing the active view's URI
 * and load progress. GTK3.
 */

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <gtk/gtk.h>

#define GSURF_TYPE_STATUS_BAR_MODULE (gsurf_status_bar_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfStatusBarModule, gsurf_status_bar_module,
                     GSURF, STATUS_BAR_MODULE, GsurfModule)

struct _GsurfStatusBarModule
{
	GsurfModule parent_instance;
	GtkWidget *label;
	GsurfView *tracked;
	gulong     uri_id;
	gulong     progress_id;
	gdouble    progress;
};

G_DEFINE_FINAL_TYPE(GsurfStatusBarModule, gsurf_status_bar_module, GSURF_TYPE_MODULE)

static void
update_label(GsurfStatusBarModule *self)
{
	GsurfView *v = self->tracked;
	const gchar *uri;
	g_autofree gchar *text = NULL;

	if (self->label == NULL)
		return;
	uri = v ? gsurf_view_get_uri(v) : NULL;
	if (self->progress > 0.0 && self->progress < 1.0)
		text = g_strdup_printf("%s  [%d%%]", uri ? uri : "",
			(int)(self->progress * 100));
	else
		text = g_strdup(uri ? uri : "");
	gtk_label_set_text(GTK_LABEL(self->label), text);
}

static void
on_uri_changed(GsurfView *v, const gchar *uri, gpointer user_data)
{
	update_label(GSURF_STATUS_BAR_MODULE(user_data));
}

static void
on_progress_changed(GsurfView *v, gdouble p, gpointer user_data)
{
	GsurfStatusBarModule *self = GSURF_STATUS_BAR_MODULE(user_data);
	self->progress = p;
	update_label(self);
}

static void
track_view(GsurfStatusBarModule *self, GsurfView *view)
{
	if (self->tracked == view)
		return;
	if (self->tracked != NULL) {
		if (self->uri_id) g_signal_handler_disconnect(self->tracked, self->uri_id);
		if (self->progress_id) g_signal_handler_disconnect(self->tracked, self->progress_id);
		g_object_remove_weak_pointer(G_OBJECT(self->tracked), (gpointer *)&self->tracked);
	}
	self->tracked = view;
	self->uri_id = self->progress_id = 0;
	if (view != NULL) {
		g_object_add_weak_pointer(G_OBJECT(view), (gpointer *)&self->tracked);
		self->uri_id = g_signal_connect(view, "uri-changed", G_CALLBACK(on_uri_changed), self);
		self->progress_id = g_signal_connect(view, "progress-changed", G_CALLBACK(on_progress_changed), self);
		update_label(self);
	}
}

static void
on_active_view_changed(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	track_view(GSURF_STATUS_BAR_MODULE(user_data), view);
}

static const gchar *gsurf_status_bar_get_name(GsurfModule *m) { return "status_bar"; }
static const gchar *gsurf_status_bar_get_description(GsurfModule *m)
{
	return "Bottom status bar showing URI and load progress";
}

static gboolean
gsurf_status_bar_activate(GsurfModule *module)
{
	GsurfStatusBarModule *self = GSURF_STATUS_BAR_MODULE(module);
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());

	if (window == NULL)
		return TRUE;

	self->label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(self->label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(self->label), PANGO_ELLIPSIZE_MIDDLE);
	gsurf_window_add_bottom_widget(window, self->label);

	g_signal_connect(window, "active-view-changed",
		G_CALLBACK(on_active_view_changed), self);
	track_view(self, gsurf_window_get_active_view(window));
	return TRUE;
}

static void
gsurf_status_bar_module_finalize(GObject *object)
{
	track_view(GSURF_STATUS_BAR_MODULE(object), NULL);
	G_OBJECT_CLASS(gsurf_status_bar_module_parent_class)->finalize(object);
}

static void
gsurf_status_bar_module_class_init(GsurfStatusBarModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->finalize = gsurf_status_bar_module_finalize;
	module_class->activate = gsurf_status_bar_activate;
	module_class->get_name = gsurf_status_bar_get_name;
	module_class->get_description = gsurf_status_bar_get_description;
}

static void
gsurf_status_bar_module_init(GsurfStatusBarModule *self)
{
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_STATUS_BAR_MODULE;
}
