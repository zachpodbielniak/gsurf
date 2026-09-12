/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef GSURF_KIOSK_H
#define GSURF_KIOSK_H

#include "gsurf-enums.h"

G_BEGIN_DECLS

/**
 * gsurf_kiosk_is_enabled:
 *
 * Returns: whether the default configuration requests kiosk UI restrictions
 */
gboolean gsurf_kiosk_is_enabled(void);

/**
 * gsurf_kiosk_allows_action:
 * @action: a browser action
 *
 * Checks the kiosk action policy, independently of the current configuration.
 * Page navigation remains available; arbitrary URL entry and new views do not.
 *
 * Returns: whether @action is allowed in kiosk mode
 */
gboolean gsurf_kiosk_allows_action(GsurfAction action);

G_END_DECLS
#endif
