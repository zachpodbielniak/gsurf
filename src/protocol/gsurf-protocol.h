/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef GSURF_PROTOCOL_H
#define GSURF_PROTOCOL_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Private library contract shared by WebKit backends and hermetic tests.
 * A result owns its bytes and strings. Gemini status/meta are retained for
 * native input and redirect handling; Gopher content uses status 20. */
typedef struct {
	GBytes *body;
	gchar *mime;
	gchar *meta;
	gchar *uri;
	gint status;
} GsurfProtocolResponse;

void gsurf_protocol_response_free(GsurfProtocolResponse *response);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsurfProtocolResponse, gsurf_protocol_response_free)

/* Captures the thread-default context; all socket I/O runs on a GTask worker.
 * trust_directory is a private pin directory, or NULL for the user default.
 * proxy_uri overrides GIO's system proxy resolver; direct:// disables proxies.
 * Cancellation interrupts connects, handshakes, reads and writes. */
void gsurf_protocol_fetch_async(const gchar *uri, const gchar *trust_directory, const gchar *proxy_uri,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
GsurfProtocolResponse *gsurf_protocol_fetch_finish(GAsyncResult *result, GError **error);

/* Pure parsers/renderers, also used by the transport regression suite. */
gchar *gsurf_protocol_request(const gchar *uri, gchar **host, guint16 *port,
	gchar *item_type, gboolean *gemini, GError **error);
gboolean gsurf_protocol_parse_header(const gchar *header, gint *status,
	gchar **meta, GError **error);
gchar *gsurf_protocol_render(const gchar *text, const gchar *uri, gboolean menu);
gchar *gsurf_protocol_input_page(const gchar *uri, const gchar *prompt, gboolean sensitive);
gchar *gsurf_protocol_message_page(const gchar *title, const gchar *message,
	const gchar *link);
gchar *gsurf_protocol_input_uri(const gchar *uri, GError **error);
gboolean gsurf_protocol_check_pin(GTlsCertificate *certificate,
	const gchar *host, guint16 port, const gchar *directory, GError **error);

G_END_DECLS
#endif
