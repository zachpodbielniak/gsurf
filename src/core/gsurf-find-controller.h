/*
 * gsurf-find-controller.h - Backend-agnostic find/search state holder
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GsurfFindController is a plain data-holder GObject describing the state of
 * an in-page text search. It carries no GTK/WebKit state; the backend mirrors
 * the engine's find controller onto this type so modules and embedders can
 * observe match counts and search state through GObject signals.
 */

#ifndef GSURF_FIND_CONTROLLER_H
#define GSURF_FIND_CONTROLLER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GSURF_TYPE_FIND_CONTROLLER (gsurf_find_controller_get_type())

G_DECLARE_FINAL_TYPE(GsurfFindController, gsurf_find_controller, GSURF, FIND_CONTROLLER, GObject)

/**
 * gsurf_find_controller_new:
 *
 * Creates a new find controller data holder.
 *
 * Returns: (transfer full): a new #GsurfFindController
 */
GsurfFindController *gsurf_find_controller_new(void);

/**
 * gsurf_find_controller_get_search_text:
 * @self: a #GsurfFindController
 *
 * Returns: (transfer none) (nullable): the current search text
 */
const gchar *gsurf_find_controller_get_search_text(GsurfFindController *self);

/**
 * gsurf_find_controller_set_search_text:
 * @self: a #GsurfFindController
 * @search_text: (nullable): the search text
 */
void gsurf_find_controller_set_search_text(GsurfFindController *self,
	const gchar *search_text);

/**
 * gsurf_find_controller_get_match_count:
 * @self: a #GsurfFindController
 *
 * Returns: the number of matches found
 */
guint gsurf_find_controller_get_match_count(GsurfFindController *self);

/**
 * gsurf_find_controller_set_match_count:
 * @self: a #GsurfFindController
 * @match_count: the number of matches found
 *
 * Sets the match count and emits #GsurfFindController::matches-found.
 */
void gsurf_find_controller_set_match_count(GsurfFindController *self,
	guint match_count);

/**
 * gsurf_find_controller_get_current_match:
 * @self: a #GsurfFindController
 *
 * Returns: the index of the currently highlighted match
 */
guint gsurf_find_controller_get_current_match(GsurfFindController *self);

/**
 * gsurf_find_controller_set_current_match:
 * @self: a #GsurfFindController
 * @current_match: the index of the currently highlighted match
 */
void gsurf_find_controller_set_current_match(GsurfFindController *self,
	guint current_match);

/**
 * gsurf_find_controller_get_case_sensitive:
 * @self: a #GsurfFindController
 *
 * Returns: %TRUE if the search is case-sensitive
 */
gboolean gsurf_find_controller_get_case_sensitive(GsurfFindController *self);

/**
 * gsurf_find_controller_set_case_sensitive:
 * @self: a #GsurfFindController
 * @case_sensitive: whether the search is case-sensitive
 */
void gsurf_find_controller_set_case_sensitive(GsurfFindController *self,
	gboolean case_sensitive);

/**
 * gsurf_find_controller_get_wrap_around:
 * @self: a #GsurfFindController
 *
 * Returns: %TRUE if the search wraps around at the document boundary
 */
gboolean gsurf_find_controller_get_wrap_around(GsurfFindController *self);

/**
 * gsurf_find_controller_set_wrap_around:
 * @self: a #GsurfFindController
 * @wrap_around: whether the search wraps around
 */
void gsurf_find_controller_set_wrap_around(GsurfFindController *self,
	gboolean wrap_around);

/**
 * gsurf_find_controller_failed:
 * @self: a #GsurfFindController
 *
 * Emits #GsurfFindController::search-failed to indicate the search found no
 * matches.
 */
void gsurf_find_controller_failed(GsurfFindController *self);

G_END_DECLS

#endif /* GSURF_FIND_CONTROLLER_H */
