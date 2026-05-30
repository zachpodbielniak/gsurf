/*
 * gsurf-hit-test.c - GSURF Hit Test Result Boxed Type Implementation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of the GsurfHitTest boxed type describing the element
 * under the pointer (link, image, media, editable, selection, scrollbar)
 * and its associated URIs and labels.
 */

#include "gsurf-hit-test.h"

G_DEFINE_BOXED_TYPE(GsurfHitTest, gsurf_hit_test, gsurf_hit_test_copy, gsurf_hit_test_free)

/*
 * gsurf_hit_test_new:
 *
 * Creates a new, empty hit-test result. The context is 0 (no flags set)
 * and all URI/label fields are %NULL.
 *
 * Returns: (transfer full): a newly allocated GsurfHitTest
 */
GsurfHitTest *
gsurf_hit_test_new(void)
{
	GsurfHitTest *hit_test;

	hit_test = g_new0(GsurfHitTest, 1);
	hit_test->context = 0;
	hit_test->link_uri = NULL;
	hit_test->image_uri = NULL;
	hit_test->media_uri = NULL;
	hit_test->link_label = NULL;

	return hit_test;
}

/*
 * gsurf_hit_test_copy:
 * @hit_test: a GsurfHitTest to copy
 *
 * Creates a deep copy of the hit-test result, duplicating all strings.
 *
 * Returns: (transfer full): a copy of @hit_test
 */
GsurfHitTest *
gsurf_hit_test_copy(const GsurfHitTest *hit_test)
{
	GsurfHitTest *copy;

	g_return_val_if_fail(hit_test != NULL, NULL);

	copy = g_new0(GsurfHitTest, 1);
	copy->context = hit_test->context;
	copy->link_uri = g_strdup(hit_test->link_uri);
	copy->image_uri = g_strdup(hit_test->image_uri);
	copy->media_uri = g_strdup(hit_test->media_uri);
	copy->link_label = g_strdup(hit_test->link_label);

	return copy;
}

/*
 * gsurf_hit_test_free:
 * @hit_test: a GsurfHitTest to free
 *
 * Frees the memory allocated for the hit-test result, including all
 * duplicated strings.
 */
void
gsurf_hit_test_free(GsurfHitTest *hit_test)
{
	if (hit_test == NULL) {
		return;
	}

	g_free(hit_test->link_uri);
	g_free(hit_test->image_uri);
	g_free(hit_test->media_uri);
	g_free(hit_test->link_label);
	g_free(hit_test);
}

/*
 * gsurf_hit_test_get_context:
 * @hit_test: a GsurfHitTest
 *
 * Gets the raw context bitmask.
 *
 * Returns: the bitmask of GSURF_HIT_TEST_* flags
 */
guint
gsurf_hit_test_get_context(const GsurfHitTest *hit_test)
{
	g_return_val_if_fail(hit_test != NULL, 0);

	return hit_test->context;
}

/*
 * gsurf_hit_test_get_link_uri:
 * @hit_test: a GsurfHitTest
 *
 * Gets the hovered link URI.
 *
 * Returns: (nullable): the link URI, or %NULL
 */
const gchar *
gsurf_hit_test_get_link_uri(const GsurfHitTest *hit_test)
{
	g_return_val_if_fail(hit_test != NULL, NULL);

	return hit_test->link_uri;
}

/*
 * gsurf_hit_test_get_image_uri:
 * @hit_test: a GsurfHitTest
 *
 * Gets the hovered image URI.
 *
 * Returns: (nullable): the image URI, or %NULL
 */
const gchar *
gsurf_hit_test_get_image_uri(const GsurfHitTest *hit_test)
{
	g_return_val_if_fail(hit_test != NULL, NULL);

	return hit_test->image_uri;
}

/*
 * gsurf_hit_test_get_media_uri:
 * @hit_test: a GsurfHitTest
 *
 * Gets the hovered media URI.
 *
 * Returns: (nullable): the media URI, or %NULL
 */
const gchar *
gsurf_hit_test_get_media_uri(const GsurfHitTest *hit_test)
{
	g_return_val_if_fail(hit_test != NULL, NULL);

	return hit_test->media_uri;
}

/*
 * gsurf_hit_test_get_link_label:
 * @hit_test: a GsurfHitTest
 *
 * Gets the hovered link's visible label text.
 *
 * Returns: (nullable): the link label, or %NULL
 */
const gchar *
gsurf_hit_test_get_link_label(const GsurfHitTest *hit_test)
{
	g_return_val_if_fail(hit_test != NULL, NULL);

	return hit_test->link_label;
}

/*
 * gsurf_hit_test_context_is_link:
 * @hit_test: a GsurfHitTest
 *
 * Checks whether the GSURF_HIT_TEST_LINK flag is set.
 *
 * Returns: %TRUE if the pointer is over a link
 */
gboolean
gsurf_hit_test_context_is_link(const GsurfHitTest *hit_test)
{
	g_return_val_if_fail(hit_test != NULL, FALSE);

	return (hit_test->context & GSURF_HIT_TEST_LINK) != 0;
}

/*
 * gsurf_hit_test_context_is_image:
 * @hit_test: a GsurfHitTest
 *
 * Checks whether the GSURF_HIT_TEST_IMAGE flag is set.
 *
 * Returns: %TRUE if the pointer is over an image
 */
gboolean
gsurf_hit_test_context_is_image(const GsurfHitTest *hit_test)
{
	g_return_val_if_fail(hit_test != NULL, FALSE);

	return (hit_test->context & GSURF_HIT_TEST_IMAGE) != 0;
}

/*
 * gsurf_hit_test_context_is_media:
 * @hit_test: a GsurfHitTest
 *
 * Checks whether the GSURF_HIT_TEST_MEDIA flag is set.
 *
 * Returns: %TRUE if the pointer is over a media element
 */
gboolean
gsurf_hit_test_context_is_media(const GsurfHitTest *hit_test)
{
	g_return_val_if_fail(hit_test != NULL, FALSE);

	return (hit_test->context & GSURF_HIT_TEST_MEDIA) != 0;
}

/*
 * gsurf_hit_test_context_is_editable:
 * @hit_test: a GsurfHitTest
 *
 * Checks whether the GSURF_HIT_TEST_EDITABLE flag is set.
 *
 * Returns: %TRUE if the pointer is over editable content
 */
gboolean
gsurf_hit_test_context_is_editable(const GsurfHitTest *hit_test)
{
	g_return_val_if_fail(hit_test != NULL, FALSE);

	return (hit_test->context & GSURF_HIT_TEST_EDITABLE) != 0;
}

/*
 * gsurf_hit_test_context_is_selection:
 * @hit_test: a GsurfHitTest
 *
 * Checks whether the GSURF_HIT_TEST_SELECTION flag is set.
 *
 * Returns: %TRUE if the pointer is over a text selection
 */
gboolean
gsurf_hit_test_context_is_selection(const GsurfHitTest *hit_test)
{
	g_return_val_if_fail(hit_test != NULL, FALSE);

	return (hit_test->context & GSURF_HIT_TEST_SELECTION) != 0;
}

/*
 * gsurf_hit_test_set_context:
 * @hit_test: a GsurfHitTest
 * @context: bitmask of GSURF_HIT_TEST_* flags
 *
 * Sets the raw context bitmask.
 */
void
gsurf_hit_test_set_context(
	GsurfHitTest    *hit_test,
	guint           context
){
	g_return_if_fail(hit_test != NULL);

	hit_test->context = context;
}

/*
 * gsurf_hit_test_set_link_uri:
 * @hit_test: a GsurfHitTest
 * @link_uri: (nullable): the link URI, or %NULL
 *
 * Sets the hovered link URI, freeing any previous value.
 */
void
gsurf_hit_test_set_link_uri(
	GsurfHitTest    *hit_test,
	const gchar     *link_uri
){
	g_return_if_fail(hit_test != NULL);

	g_free(hit_test->link_uri);
	hit_test->link_uri = g_strdup(link_uri);
}

/*
 * gsurf_hit_test_set_image_uri:
 * @hit_test: a GsurfHitTest
 * @image_uri: (nullable): the image URI, or %NULL
 *
 * Sets the hovered image URI, freeing any previous value.
 */
void
gsurf_hit_test_set_image_uri(
	GsurfHitTest    *hit_test,
	const gchar     *image_uri
){
	g_return_if_fail(hit_test != NULL);

	g_free(hit_test->image_uri);
	hit_test->image_uri = g_strdup(image_uri);
}

/*
 * gsurf_hit_test_set_media_uri:
 * @hit_test: a GsurfHitTest
 * @media_uri: (nullable): the media URI, or %NULL
 *
 * Sets the hovered media URI, freeing any previous value.
 */
void
gsurf_hit_test_set_media_uri(
	GsurfHitTest    *hit_test,
	const gchar     *media_uri
){
	g_return_if_fail(hit_test != NULL);

	g_free(hit_test->media_uri);
	hit_test->media_uri = g_strdup(media_uri);
}

/*
 * gsurf_hit_test_set_link_label:
 * @hit_test: a GsurfHitTest
 * @link_label: (nullable): the link label, or %NULL
 *
 * Sets the hovered link's visible label, freeing any previous value.
 */
void
gsurf_hit_test_set_link_label(
	GsurfHitTest    *hit_test,
	const gchar     *link_label
){
	g_return_if_fail(hit_test != NULL);

	g_free(hit_test->link_label);
	hit_test->link_label = g_strdup(link_label);
}
