/*
 * gsurf-types.h - GSURF Type Forward Declarations
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Forward declarations for manually-defined GSURF types and common
 * aliases. Types using G_DECLARE_FINAL_TYPE / G_DECLARE_DERIVABLE_TYPE /
 * G_DECLARE_INTERFACE generate their own typedefs in their own headers
 * and are NOT listed here.
 */

#ifndef GSURF_TYPES_H
#define GSURF_TYPES_H

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

/*
 * Forward declarations - Boxed types
 *
 * These are defined with G_DEFINE_BOXED_TYPE in the boxed sources and
 * declared fully in their own headers; the bare typedefs are surfaced
 * here so that core headers can reference them without a hard include.
 */
typedef struct _GsurfHitTest          GsurfHitTest;
typedef struct _GsurfNavigationAction GsurfNavigationAction;
typedef struct _GsurfCertificate      GsurfCertificate;
typedef struct _GsurfUriParameters    GsurfUriParameters;
typedef struct _GsurfKeybind          GsurfKeybind;
typedef struct _GsurfMousebind        GsurfMousebind;
typedef struct _GsurfMenuItem         GsurfMenuItem;

/*
 * Default values
 */
#define GSURF_DEFAULT_WIDTH    (1024)
#define GSURF_DEFAULT_HEIGHT   (768)
#define GSURF_DEFAULT_ZOOM     (1.0)
#define GSURF_DEFAULT_HOMEPAGE "about:blank"

/* Maximum length sanity caps */
#define GSURF_MAX_URI_LEN      (65536)

G_END_DECLS

#endif /* GSURF_TYPES_H */
