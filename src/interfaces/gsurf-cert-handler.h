/*
 * gsurf-cert-handler.h - Module hook: TLS certificate decisions
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Modules implementing #GsurfCertHandler decide whether to proceed past
 * a TLS certificate error for a host (cert-manager module). The first
 * decisive (non-PROMPT) verdict wins.
 */

#ifndef GSURF_CERT_HANDLER_H
#define GSURF_CERT_HANDLER_H

#include <glib-object.h>

#include "gsurf-enums.h"

G_BEGIN_DECLS

#define GSURF_TYPE_CERT_HANDLER (gsurf_cert_handler_get_type())

G_DECLARE_INTERFACE(GsurfCertHandler, gsurf_cert_handler, GSURF, CERT_HANDLER, GObject)

struct _GsurfCertHandlerInterface
{
	GTypeInterface parent_iface;

	GsurfTlsDecision (*verify)(GsurfCertHandler *self, const gchar *host, guint tls_errors);
};

GsurfTlsDecision gsurf_cert_handler_verify(GsurfCertHandler *self, const gchar *host, guint tls_errors);

G_END_DECLS

#endif /* GSURF_CERT_HANDLER_H */
