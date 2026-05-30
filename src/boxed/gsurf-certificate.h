/*
 * gsurf-certificate.h - GSURF TLS Certificate Boxed Type
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Represents a TLS certificate associated with a page load, capturing
 * the subject/issuer, validity window, fingerprint, PEM data, and the
 * verification flags and trust verdict.
 */

#ifndef GSURF_CERTIFICATE_H
#define GSURF_CERTIFICATE_H

#include <glib-object.h>
#include "../gsurf-types.h"

G_BEGIN_DECLS

#define GSURF_TYPE_CERTIFICATE (gsurf_certificate_get_type())

/**
 * GsurfCertificate:
 * @subject: Certificate subject (distinguished name), or %NULL
 * @issuer: Certificate issuer (distinguished name), or %NULL
 * @not_before: Validity start timestamp (string), or %NULL
 * @not_after: Validity end timestamp (string), or %NULL
 * @fingerprint: Certificate fingerprint, or %NULL
 * @pem: PEM-encoded certificate data, or %NULL
 * @flags: TLS verification flags (e.g. GTlsCertificateFlags)
 * @trusted: Whether the certificate is trusted
 *
 * Describes a TLS certificate and its verification status.
 */
struct _GsurfCertificate {
	gchar     *subject;         /* Subject DN, or NULL */
	gchar     *issuer;          /* Issuer DN, or NULL */
	gchar     *not_before;      /* Validity start, or NULL */
	gchar     *not_after;       /* Validity end, or NULL */
	gchar     *fingerprint;     /* Fingerprint, or NULL */
	gchar     *pem;             /* PEM-encoded data, or NULL */
	guint      flags;           /* TLS verification flags */
	gboolean   trusted;         /* Trust verdict */
};

/**
 * gsurf_certificate_get_type:
 *
 * Gets the GType for the GsurfCertificate boxed type.
 *
 * Returns: the GType
 */
GType gsurf_certificate_get_type(void) G_GNUC_CONST;

/**
 * gsurf_certificate_new:
 *
 * Creates a new, empty certificate with all strings %NULL, no flags,
 * and untrusted.
 *
 * Returns: (transfer full): a new GsurfCertificate
 */
GsurfCertificate *gsurf_certificate_new(void);

/**
 * gsurf_certificate_copy:
 * @certificate: a GsurfCertificate
 *
 * Creates a deep copy of the certificate, duplicating all strings.
 *
 * Returns: (transfer full): a copy of @certificate
 */
GsurfCertificate *gsurf_certificate_copy(const GsurfCertificate *certificate);

/**
 * gsurf_certificate_free:
 * @certificate: a GsurfCertificate
 *
 * Frees the memory allocated for the certificate.
 */
void gsurf_certificate_free(GsurfCertificate *certificate);

/* Getters */
const gchar *gsurf_certificate_get_subject(const GsurfCertificate *certificate);
const gchar *gsurf_certificate_get_issuer(const GsurfCertificate *certificate);
const gchar *gsurf_certificate_get_not_before(const GsurfCertificate *certificate);
const gchar *gsurf_certificate_get_not_after(const GsurfCertificate *certificate);
const gchar *gsurf_certificate_get_fingerprint(const GsurfCertificate *certificate);
const gchar *gsurf_certificate_get_pem(const GsurfCertificate *certificate);
guint        gsurf_certificate_get_flags(const GsurfCertificate *certificate);
gboolean     gsurf_certificate_get_trusted(const GsurfCertificate *certificate);

/* Setters */
void gsurf_certificate_set_subject(GsurfCertificate *certificate, const gchar *subject);
void gsurf_certificate_set_issuer(GsurfCertificate *certificate, const gchar *issuer);
void gsurf_certificate_set_not_before(GsurfCertificate *certificate, const gchar *not_before);
void gsurf_certificate_set_not_after(GsurfCertificate *certificate, const gchar *not_after);
void gsurf_certificate_set_fingerprint(GsurfCertificate *certificate, const gchar *fingerprint);
void gsurf_certificate_set_pem(GsurfCertificate *certificate, const gchar *pem);
void gsurf_certificate_set_flags(GsurfCertificate *certificate, guint flags);
void gsurf_certificate_set_trusted(GsurfCertificate *certificate, gboolean trusted);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfCertificate, gsurf_certificate_free)

G_END_DECLS

#endif /* GSURF_CERTIFICATE_H */
