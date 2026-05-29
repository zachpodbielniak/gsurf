/*
 * gsurf-cert-handler.c - Module hook: TLS certificate decisions
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "interfaces/gsurf-cert-handler.h"

G_DEFINE_INTERFACE(GsurfCertHandler, gsurf_cert_handler, G_TYPE_OBJECT)

static void
gsurf_cert_handler_default_init(GsurfCertHandlerInterface *iface)
{
}

GsurfTlsDecision
gsurf_cert_handler_verify(GsurfCertHandler *self, const gchar *host, guint tls_errors)
{
	GsurfCertHandlerInterface *iface;

	g_return_val_if_fail(GSURF_IS_CERT_HANDLER(self), GSURF_TLS_PROMPT);

	iface = GSURF_CERT_HANDLER_GET_IFACE(self);
	if (iface->verify != NULL)
		return iface->verify(self, host, tls_errors);
	return GSURF_TLS_PROMPT;
}
