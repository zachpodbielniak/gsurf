/*
 * gsurf-certificate.c - GSURF TLS Certificate Boxed Type Implementation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of the GsurfCertificate boxed type capturing the
 * subject/issuer, validity window, fingerprint, PEM data, verification
 * flags, and trust verdict for a TLS certificate.
 */

#include "gsurf-certificate.h"

G_DEFINE_BOXED_TYPE(GsurfCertificate, gsurf_certificate, gsurf_certificate_copy, gsurf_certificate_free)

/*
 * gsurf_certificate_new:
 *
 * Creates a new, empty certificate. All string fields are %NULL,
 * @flags is 0, and @trusted is %FALSE.
 *
 * Returns: (transfer full): a newly allocated GsurfCertificate
 */
GsurfCertificate *
gsurf_certificate_new(void)
{
	GsurfCertificate *certificate;

	certificate = g_new0(GsurfCertificate, 1);
	certificate->subject = NULL;
	certificate->issuer = NULL;
	certificate->not_before = NULL;
	certificate->not_after = NULL;
	certificate->fingerprint = NULL;
	certificate->pem = NULL;
	certificate->flags = 0;
	certificate->trusted = FALSE;

	return certificate;
}

/*
 * gsurf_certificate_copy:
 * @certificate: a GsurfCertificate to copy
 *
 * Creates a deep copy of the certificate, duplicating all strings.
 *
 * Returns: (transfer full): a copy of @certificate
 */
GsurfCertificate *
gsurf_certificate_copy(const GsurfCertificate *certificate)
{
	GsurfCertificate *copy;

	g_return_val_if_fail(certificate != NULL, NULL);

	copy = g_new0(GsurfCertificate, 1);
	copy->subject = g_strdup(certificate->subject);
	copy->issuer = g_strdup(certificate->issuer);
	copy->not_before = g_strdup(certificate->not_before);
	copy->not_after = g_strdup(certificate->not_after);
	copy->fingerprint = g_strdup(certificate->fingerprint);
	copy->pem = g_strdup(certificate->pem);
	copy->flags = certificate->flags;
	copy->trusted = certificate->trusted;

	return copy;
}

/*
 * gsurf_certificate_free:
 * @certificate: a GsurfCertificate to free
 *
 * Frees the memory allocated for the certificate, including all
 * duplicated strings.
 */
void
gsurf_certificate_free(GsurfCertificate *certificate)
{
	if (certificate == NULL) {
		return;
	}

	g_free(certificate->subject);
	g_free(certificate->issuer);
	g_free(certificate->not_before);
	g_free(certificate->not_after);
	g_free(certificate->fingerprint);
	g_free(certificate->pem);
	g_free(certificate);
}

/*
 * gsurf_certificate_get_subject:
 * @certificate: a GsurfCertificate
 *
 * Returns: (nullable): the subject DN, or %NULL
 */
const gchar *
gsurf_certificate_get_subject(const GsurfCertificate *certificate)
{
	g_return_val_if_fail(certificate != NULL, NULL);

	return certificate->subject;
}

/*
 * gsurf_certificate_get_issuer:
 * @certificate: a GsurfCertificate
 *
 * Returns: (nullable): the issuer DN, or %NULL
 */
const gchar *
gsurf_certificate_get_issuer(const GsurfCertificate *certificate)
{
	g_return_val_if_fail(certificate != NULL, NULL);

	return certificate->issuer;
}

/*
 * gsurf_certificate_get_not_before:
 * @certificate: a GsurfCertificate
 *
 * Returns: (nullable): the validity start timestamp, or %NULL
 */
const gchar *
gsurf_certificate_get_not_before(const GsurfCertificate *certificate)
{
	g_return_val_if_fail(certificate != NULL, NULL);

	return certificate->not_before;
}

/*
 * gsurf_certificate_get_not_after:
 * @certificate: a GsurfCertificate
 *
 * Returns: (nullable): the validity end timestamp, or %NULL
 */
const gchar *
gsurf_certificate_get_not_after(const GsurfCertificate *certificate)
{
	g_return_val_if_fail(certificate != NULL, NULL);

	return certificate->not_after;
}

/*
 * gsurf_certificate_get_fingerprint:
 * @certificate: a GsurfCertificate
 *
 * Returns: (nullable): the fingerprint, or %NULL
 */
const gchar *
gsurf_certificate_get_fingerprint(const GsurfCertificate *certificate)
{
	g_return_val_if_fail(certificate != NULL, NULL);

	return certificate->fingerprint;
}

/*
 * gsurf_certificate_get_pem:
 * @certificate: a GsurfCertificate
 *
 * Returns: (nullable): the PEM-encoded certificate data, or %NULL
 */
const gchar *
gsurf_certificate_get_pem(const GsurfCertificate *certificate)
{
	g_return_val_if_fail(certificate != NULL, NULL);

	return certificate->pem;
}

/*
 * gsurf_certificate_get_flags:
 * @certificate: a GsurfCertificate
 *
 * Returns: the TLS verification flags
 */
guint
gsurf_certificate_get_flags(const GsurfCertificate *certificate)
{
	g_return_val_if_fail(certificate != NULL, 0);

	return certificate->flags;
}

/*
 * gsurf_certificate_get_trusted:
 * @certificate: a GsurfCertificate
 *
 * Returns: %TRUE if the certificate is trusted
 */
gboolean
gsurf_certificate_get_trusted(const GsurfCertificate *certificate)
{
	g_return_val_if_fail(certificate != NULL, FALSE);

	return certificate->trusted;
}

/*
 * gsurf_certificate_set_subject:
 * @certificate: a GsurfCertificate
 * @subject: (nullable): the subject DN, or %NULL
 *
 * Sets the subject, freeing any previous value.
 */
void
gsurf_certificate_set_subject(
	GsurfCertificate    *certificate,
	const gchar         *subject
){
	g_return_if_fail(certificate != NULL);

	g_free(certificate->subject);
	certificate->subject = g_strdup(subject);
}

/*
 * gsurf_certificate_set_issuer:
 * @certificate: a GsurfCertificate
 * @issuer: (nullable): the issuer DN, or %NULL
 *
 * Sets the issuer, freeing any previous value.
 */
void
gsurf_certificate_set_issuer(
	GsurfCertificate    *certificate,
	const gchar         *issuer
){
	g_return_if_fail(certificate != NULL);

	g_free(certificate->issuer);
	certificate->issuer = g_strdup(issuer);
}

/*
 * gsurf_certificate_set_not_before:
 * @certificate: a GsurfCertificate
 * @not_before: (nullable): the validity start timestamp, or %NULL
 *
 * Sets the validity start, freeing any previous value.
 */
void
gsurf_certificate_set_not_before(
	GsurfCertificate    *certificate,
	const gchar         *not_before
){
	g_return_if_fail(certificate != NULL);

	g_free(certificate->not_before);
	certificate->not_before = g_strdup(not_before);
}

/*
 * gsurf_certificate_set_not_after:
 * @certificate: a GsurfCertificate
 * @not_after: (nullable): the validity end timestamp, or %NULL
 *
 * Sets the validity end, freeing any previous value.
 */
void
gsurf_certificate_set_not_after(
	GsurfCertificate    *certificate,
	const gchar         *not_after
){
	g_return_if_fail(certificate != NULL);

	g_free(certificate->not_after);
	certificate->not_after = g_strdup(not_after);
}

/*
 * gsurf_certificate_set_fingerprint:
 * @certificate: a GsurfCertificate
 * @fingerprint: (nullable): the fingerprint, or %NULL
 *
 * Sets the fingerprint, freeing any previous value.
 */
void
gsurf_certificate_set_fingerprint(
	GsurfCertificate    *certificate,
	const gchar         *fingerprint
){
	g_return_if_fail(certificate != NULL);

	g_free(certificate->fingerprint);
	certificate->fingerprint = g_strdup(fingerprint);
}

/*
 * gsurf_certificate_set_pem:
 * @certificate: a GsurfCertificate
 * @pem: (nullable): the PEM-encoded certificate data, or %NULL
 *
 * Sets the PEM data, freeing any previous value.
 */
void
gsurf_certificate_set_pem(
	GsurfCertificate    *certificate,
	const gchar         *pem
){
	g_return_if_fail(certificate != NULL);

	g_free(certificate->pem);
	certificate->pem = g_strdup(pem);
}

/*
 * gsurf_certificate_set_flags:
 * @certificate: a GsurfCertificate
 * @flags: the TLS verification flags
 *
 * Sets the verification flags.
 */
void
gsurf_certificate_set_flags(
	GsurfCertificate    *certificate,
	guint               flags
){
	g_return_if_fail(certificate != NULL);

	certificate->flags = flags;
}

/*
 * gsurf_certificate_set_trusted:
 * @certificate: a GsurfCertificate
 * @trusted: the trust verdict
 *
 * Sets whether the certificate is trusted.
 */
void
gsurf_certificate_set_trusted(
	GsurfCertificate    *certificate,
	gboolean            trusted
){
	g_return_if_fail(certificate != NULL);

	certificate->trusted = trusted;
}
