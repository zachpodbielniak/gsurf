/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "util/gsurf-kiosk.h"
#include "config/gsurf-config.h"

gboolean
gsurf_kiosk_is_enabled(void)
{
	GsurfConfig *config;

	/* Embedders without a default config retain ordinary browser behavior. */
	config = gsurf_config_get_default();
	return config != NULL && config->kiosk;
}

gboolean
gsurf_kiosk_allows_action(GsurfAction action)
{
	/* URL controls must remain disabled even when users rebind their keys. */
	switch (action) {
	case GSURF_ACTION_OPEN_PROMPT:
	case GSURF_ACTION_PASTE_URL:
	case GSURF_ACTION_HOME:
	case GSURF_ACTION_OPEN_NEW_VIEW:
	case GSURF_ACTION_OPEN_NEW_VIEW_BACKGROUND:
	case GSURF_ACTION_TAB_NEW:
	case GSURF_ACTION_TAB_REOPEN:
	case GSURF_ACTION_FOLLOW_HINTS_NEW_VIEW:
	/* These open another view, which kiosk mode never allows. */
	case GSURF_ACTION_CONTAINER_NEW_TEMP:
	case GSURF_ACTION_CONTAINER_REOPEN_NEXT:
	case GSURF_ACTION_PANE_SPLIT_RIGHT:
	case GSURF_ACTION_PANE_SPLIT_DOWN:
		return FALSE;
	default:
		return TRUE;
	}
}
