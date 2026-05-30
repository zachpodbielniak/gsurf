/*
 * gsurf-find-controller.c - Backend-agnostic find/search state holder
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/gsurf-find-controller.h"

struct _GsurfFindController
{
	GObject parent_instance;

	gchar   *search_text;
	guint    match_count;
	guint    current_match;
	gboolean case_sensitive;
	gboolean wrap_around;
};

enum {
	SIG_MATCHES_FOUND,
	SIG_SEARCH_FAILED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GsurfFindController, gsurf_find_controller, G_TYPE_OBJECT)

const gchar *
gsurf_find_controller_get_search_text(GsurfFindController *self)
{
	g_return_val_if_fail(GSURF_IS_FIND_CONTROLLER(self), NULL);
	return self->search_text;
}

void
gsurf_find_controller_set_search_text(GsurfFindController *self,
	const gchar *search_text)
{
	g_return_if_fail(GSURF_IS_FIND_CONTROLLER(self));

	g_clear_pointer(&self->search_text, g_free);
	self->search_text = g_strdup(search_text);
}

guint
gsurf_find_controller_get_match_count(GsurfFindController *self)
{
	g_return_val_if_fail(GSURF_IS_FIND_CONTROLLER(self), 0);
	return self->match_count;
}

void
gsurf_find_controller_set_match_count(GsurfFindController *self, guint match_count)
{
	g_return_if_fail(GSURF_IS_FIND_CONTROLLER(self));

	self->match_count = match_count;
	g_signal_emit(self, signals[SIG_MATCHES_FOUND], 0, match_count);
}

guint
gsurf_find_controller_get_current_match(GsurfFindController *self)
{
	g_return_val_if_fail(GSURF_IS_FIND_CONTROLLER(self), 0);
	return self->current_match;
}

void
gsurf_find_controller_set_current_match(GsurfFindController *self,
	guint current_match)
{
	g_return_if_fail(GSURF_IS_FIND_CONTROLLER(self));
	self->current_match = current_match;
}

gboolean
gsurf_find_controller_get_case_sensitive(GsurfFindController *self)
{
	g_return_val_if_fail(GSURF_IS_FIND_CONTROLLER(self), FALSE);
	return self->case_sensitive;
}

void
gsurf_find_controller_set_case_sensitive(GsurfFindController *self,
	gboolean case_sensitive)
{
	g_return_if_fail(GSURF_IS_FIND_CONTROLLER(self));
	self->case_sensitive = case_sensitive;
}

gboolean
gsurf_find_controller_get_wrap_around(GsurfFindController *self)
{
	g_return_val_if_fail(GSURF_IS_FIND_CONTROLLER(self), FALSE);
	return self->wrap_around;
}

void
gsurf_find_controller_set_wrap_around(GsurfFindController *self,
	gboolean wrap_around)
{
	g_return_if_fail(GSURF_IS_FIND_CONTROLLER(self));
	self->wrap_around = wrap_around;
}

void
gsurf_find_controller_failed(GsurfFindController *self)
{
	g_return_if_fail(GSURF_IS_FIND_CONTROLLER(self));
	g_signal_emit(self, signals[SIG_SEARCH_FAILED], 0);
}

static void
gsurf_find_controller_finalize(GObject *object)
{
	GsurfFindController *self = GSURF_FIND_CONTROLLER(object);

	g_clear_pointer(&self->search_text, g_free);

	G_OBJECT_CLASS(gsurf_find_controller_parent_class)->finalize(object);
}

static void
gsurf_find_controller_class_init(GsurfFindControllerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gsurf_find_controller_finalize;

	/**
	 * GsurfFindController::matches-found:
	 * @self: the #GsurfFindController
	 * @count: the number of matches found
	 *
	 * Emitted when the match count is updated.
	 */
	signals[SIG_MATCHES_FOUND] = g_signal_new(
		"matches-found", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_UINT);

	/**
	 * GsurfFindController::search-failed:
	 * @self: the #GsurfFindController
	 *
	 * Emitted when the search finds no matches.
	 */
	signals[SIG_SEARCH_FAILED] = g_signal_new(
		"search-failed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

static void
gsurf_find_controller_init(GsurfFindController *self)
{
	self->search_text = NULL;
	self->match_count = 0;
	self->current_match = 0;
	self->case_sensitive = FALSE;
	self->wrap_around = FALSE;
}

GsurfFindController *
gsurf_find_controller_new(void)
{
	return g_object_new(GSURF_TYPE_FIND_CONTROLLER, NULL);
}
