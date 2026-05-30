/*
 * gsurf-hit-test.h - GSURF Hit Test Result Boxed Type
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Represents the result of a hit-test over page content, describing
 * what kind of element is under the pointer (link, image, media, etc.)
 * and any associated URIs or labels.
 */

#ifndef GSURF_HIT_TEST_H
#define GSURF_HIT_TEST_H

#include <glib-object.h>
#include "../gsurf-types.h"

G_BEGIN_DECLS

#define GSURF_TYPE_HIT_TEST (gsurf_hit_test_get_type())

/*
 * Hit-test context bit-flags. Combine with bitwise OR in the
 * GsurfHitTest.context field.
 */
#define GSURF_HIT_TEST_LINK      (1 << 0)   /* Pointer is over a hyperlink */
#define GSURF_HIT_TEST_IMAGE     (1 << 1)   /* Pointer is over an image */
#define GSURF_HIT_TEST_MEDIA     (1 << 2)   /* Pointer is over audio/video */
#define GSURF_HIT_TEST_EDITABLE  (1 << 3)   /* Pointer is over editable content */
#define GSURF_HIT_TEST_SELECTION (1 << 4)   /* Pointer is over a text selection */
#define GSURF_HIT_TEST_SCROLLBAR (1 << 5)   /* Pointer is over a scrollbar */

/**
 * GsurfHitTest:
 * @context: Bitmask of GSURF_HIT_TEST_* flags
 * @link_uri: URI of the hovered link, or %NULL
 * @image_uri: URI of the hovered image, or %NULL
 * @media_uri: URI of the hovered media element, or %NULL
 * @link_label: Visible text label of the hovered link, or %NULL
 *
 * Describes the element under the pointer at the time of a hit-test.
 */
struct _GsurfHitTest {
	guint   context;        /* Bitmask of GSURF_HIT_TEST_* flags */
	gchar  *link_uri;       /* Hovered link URI, or NULL */
	gchar  *image_uri;      /* Hovered image URI, or NULL */
	gchar  *media_uri;      /* Hovered media URI, or NULL */
	gchar  *link_label;     /* Hovered link label, or NULL */
};

/**
 * gsurf_hit_test_get_type:
 *
 * Gets the GType for the GsurfHitTest boxed type.
 *
 * Returns: the GType
 */
GType gsurf_hit_test_get_type(void) G_GNUC_CONST;

/**
 * gsurf_hit_test_new:
 *
 * Creates a new, empty hit-test result with no context flags set
 * and all URIs %NULL.
 *
 * Returns: (transfer full): a new GsurfHitTest
 */
GsurfHitTest *gsurf_hit_test_new(void);

/**
 * gsurf_hit_test_copy:
 * @hit_test: a GsurfHitTest
 *
 * Creates a deep copy of the hit-test result, duplicating all strings.
 *
 * Returns: (transfer full): a copy of @hit_test
 */
GsurfHitTest *gsurf_hit_test_copy(const GsurfHitTest *hit_test);

/**
 * gsurf_hit_test_free:
 * @hit_test: a GsurfHitTest
 *
 * Frees the memory allocated for the hit-test result.
 */
void gsurf_hit_test_free(GsurfHitTest *hit_test);

/* Getters */
guint        gsurf_hit_test_get_context(const GsurfHitTest *hit_test);
const gchar *gsurf_hit_test_get_link_uri(const GsurfHitTest *hit_test);
const gchar *gsurf_hit_test_get_image_uri(const GsurfHitTest *hit_test);
const gchar *gsurf_hit_test_get_media_uri(const GsurfHitTest *hit_test);
const gchar *gsurf_hit_test_get_link_label(const GsurfHitTest *hit_test);

/* Convenience context predicates */
gboolean gsurf_hit_test_context_is_link(const GsurfHitTest *hit_test);
gboolean gsurf_hit_test_context_is_image(const GsurfHitTest *hit_test);
gboolean gsurf_hit_test_context_is_media(const GsurfHitTest *hit_test);
gboolean gsurf_hit_test_context_is_editable(const GsurfHitTest *hit_test);
gboolean gsurf_hit_test_context_is_selection(const GsurfHitTest *hit_test);

/* Setters */
void gsurf_hit_test_set_context(GsurfHitTest *hit_test, guint context);
void gsurf_hit_test_set_link_uri(GsurfHitTest *hit_test, const gchar *link_uri);
void gsurf_hit_test_set_image_uri(GsurfHitTest *hit_test, const gchar *image_uri);
void gsurf_hit_test_set_media_uri(GsurfHitTest *hit_test, const gchar *media_uri);
void gsurf_hit_test_set_link_label(GsurfHitTest *hit_test, const gchar *link_label);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfHitTest, gsurf_hit_test_free)

G_END_DECLS

#endif /* GSURF_HIT_TEST_H */
