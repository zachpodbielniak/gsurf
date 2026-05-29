/*
 * gsurf-enums.c - GSURF enum/flags GType registration
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gsurf-enums.h"

/*
 * Variadic so the brace-enclosed value list (which contains commas) is
 * captured whole as __VA_ARGS__ and used directly as an array initializer.
 */
#define GSURF_DEFINE_ENUM_TYPE(TypeName, type_name, ...)               \
GType                                                                  \
type_name##_get_type(void)                                             \
{                                                                      \
	static gsize g_define_id = 0;                                  \
	if (g_once_init_enter(&g_define_id)) {                         \
		static const GEnumValue v[] = __VA_ARGS__;             \
		GType id = g_enum_register_static(#TypeName, v);       \
		g_once_init_leave(&g_define_id, id);                   \
	}                                                              \
	return (GType)g_define_id;                                     \
}

#define GSURF_DEFINE_FLAGS_TYPE(TypeName, type_name, ...)              \
GType                                                                  \
type_name##_get_type(void)                                             \
{                                                                      \
	static gsize g_define_id = 0;                                  \
	if (g_once_init_enter(&g_define_id)) {                         \
		static const GFlagsValue v[] = __VA_ARGS__;            \
		GType id = g_flags_register_static(#TypeName, v);      \
		g_once_init_leave(&g_define_id, id);                   \
	}                                                              \
	return (GType)g_define_id;                                     \
}

#define EV(value, nick) { value, #value, nick }
#define END_EV { 0, NULL, NULL }

GSURF_DEFINE_ENUM_TYPE(GsurfBackendType, gsurf_backend_type, {
	EV(GSURF_BACKEND_GTK3_WEBKIT2, "gtk3-webkit2"),
	EV(GSURF_BACKEND_GTK4_WEBKIT6, "gtk4-webkit6"),
	END_EV
})

GSURF_DEFINE_FLAGS_TYPE(GsurfKeyMod, gsurf_key_mod, {
	EV(GSURF_MOD_NONE, "none"),
	EV(GSURF_MOD_SHIFT, "shift"),
	EV(GSURF_MOD_CTRL, "ctrl"),
	EV(GSURF_MOD_ALT, "alt"),
	EV(GSURF_MOD_SUPER, "super"),
	END_EV
})

GSURF_DEFINE_ENUM_TYPE(GsurfModePolicy, gsurf_mode_policy, {
	EV(GSURF_MODE_NORMAL, "normal"),
	EV(GSURF_MODE_INSERT, "insert"),
	EV(GSURF_MODE_FOLLOW, "follow"),
	EV(GSURF_MODE_FIND, "find"),
	EV(GSURF_MODE_PASSTHROUGH, "passthrough"),
	END_EV
})

GSURF_DEFINE_ENUM_TYPE(GsurfLoadEvent, gsurf_load_event, {
	EV(GSURF_LOAD_STARTED, "started"),
	EV(GSURF_LOAD_REDIRECTED, "redirected"),
	EV(GSURF_LOAD_COMMITTED, "committed"),
	EV(GSURF_LOAD_FINISHED, "finished"),
	END_EV
})

GSURF_DEFINE_ENUM_TYPE(GsurfPolicyDecision, gsurf_policy_decision, {
	EV(GSURF_POLICY_USE, "use"),
	EV(GSURF_POLICY_IGNORE, "ignore"),
	EV(GSURF_POLICY_DOWNLOAD, "download"),
	END_EV
})

GSURF_DEFINE_ENUM_TYPE(GsurfFilterVerdict, gsurf_filter_verdict, {
	EV(GSURF_FILTER_ALLOW, "allow"),
	EV(GSURF_FILTER_BLOCK, "block"),
	EV(GSURF_FILTER_REDIRECT, "redirect"),
	END_EV
})

GSURF_DEFINE_ENUM_TYPE(GsurfTlsDecision, gsurf_tls_decision, {
	EV(GSURF_TLS_REJECT, "reject"),
	EV(GSURF_TLS_PROCEED, "proceed"),
	EV(GSURF_TLS_PROMPT, "prompt"),
	END_EV
})

GSURF_DEFINE_ENUM_TYPE(GsurfPermissionVerdict, gsurf_permission_verdict, {
	EV(GSURF_PERMISSION_DENY, "deny"),
	EV(GSURF_PERMISSION_ALLOW, "allow"),
	EV(GSURF_PERMISSION_PROMPT, "prompt"),
	END_EV
})

GSURF_DEFINE_ENUM_TYPE(GsurfModuleState, gsurf_module_state, {
	EV(GSURF_MODULE_STATE_LOADED, "loaded"),
	EV(GSURF_MODULE_STATE_ACTIVE, "active"),
	EV(GSURF_MODULE_STATE_INACTIVE, "inactive"),
	EV(GSURF_MODULE_STATE_ERROR, "error"),
	END_EV
})

GSURF_DEFINE_ENUM_TYPE(GsurfHookPoint, gsurf_hook_point, {
	EV(GSURF_HOOK_KEY_PRESS, "key-press"),
	EV(GSURF_HOOK_KEY_RELEASE, "key-release"),
	EV(GSURF_HOOK_BUTTON_PRESS, "button-press"),
	EV(GSURF_HOOK_BUTTON_RELEASE, "button-release"),
	EV(GSURF_HOOK_SCROLL, "scroll"),
	EV(GSURF_HOOK_URI_REWRITE, "uri-rewrite"),
	EV(GSURF_HOOK_PRE_NAVIGATE, "pre-navigate"),
	EV(GSURF_HOOK_POST_NAVIGATE, "post-navigate"),
	EV(GSURF_HOOK_DECIDE_POLICY, "decide-policy"),
	EV(GSURF_HOOK_LOAD_STARTED, "load-started"),
	EV(GSURF_HOOK_LOAD_COMMITTED, "load-committed"),
	EV(GSURF_HOOK_LOAD_FINISHED, "load-finished"),
	EV(GSURF_HOOK_LOAD_FAILED, "load-failed"),
	EV(GSURF_HOOK_TLS_ERROR, "tls-error"),
	EV(GSURF_HOOK_RESOURCE_REQUEST, "resource-request"),
	EV(GSURF_HOOK_RESOURCE_RESPONSE, "resource-response"),
	EV(GSURF_HOOK_INJECT_SCRIPT, "inject-script"),
	EV(GSURF_HOOK_INJECT_STYLE, "inject-style"),
	EV(GSURF_HOOK_CONTEXT_MENU, "context-menu"),
	EV(GSURF_HOOK_DOWNLOAD_REQUESTED, "download-requested"),
	EV(GSURF_HOOK_DOWNLOAD_FINISHED, "download-finished"),
	EV(GSURF_HOOK_PERMISSION_REQUEST, "permission-request"),
	EV(GSURF_HOOK_TITLE_CHANGE, "title-change"),
	EV(GSURF_HOOK_STATUS_CHANGE, "status-change"),
	EV(GSURF_HOOK_PROGRESS_CHANGE, "progress-change"),
	EV(GSURF_HOOK_FAVICON_CHANGE, "favicon-change"),
	EV(GSURF_HOOK_RENDER_OVERLAY, "render-overlay"),
	EV(GSURF_HOOK_VIEW_CREATE, "view-create"),
	EV(GSURF_HOOK_VIEW_DESTROY, "view-destroy"),
	EV(GSURF_HOOK_WINDOW_CREATE, "window-create"),
	EV(GSURF_HOOK_FULLSCREEN, "fullscreen"),
	EV(GSURF_HOOK_WEB_PROCESS_CRASHED, "web-process-crashed"),
	EV(GSURF_HOOK_LAST, "last"),
	END_EV
})

GSURF_DEFINE_ENUM_TYPE(GsurfAction, gsurf_action, {
	EV(GSURF_ACTION_NONE, "none"),
	EV(GSURF_ACTION_BACK, "back"),
	EV(GSURF_ACTION_FORWARD, "forward"),
	EV(GSURF_ACTION_RELOAD, "reload"),
	EV(GSURF_ACTION_RELOAD_NOCACHE, "reload-nocache"),
	EV(GSURF_ACTION_STOP, "stop"),
	EV(GSURF_ACTION_HOME, "home"),
	EV(GSURF_ACTION_OPEN_PROMPT, "open-prompt"),
	EV(GSURF_ACTION_OPEN_NEW_VIEW, "open-new-view"),
	EV(GSURF_ACTION_SCROLL_UP, "scroll-up"),
	EV(GSURF_ACTION_SCROLL_DOWN, "scroll-down"),
	EV(GSURF_ACTION_SCROLL_LEFT, "scroll-left"),
	EV(GSURF_ACTION_SCROLL_RIGHT, "scroll-right"),
	EV(GSURF_ACTION_SCROLL_TOP, "scroll-top"),
	EV(GSURF_ACTION_SCROLL_BOTTOM, "scroll-bottom"),
	EV(GSURF_ACTION_PAGE_UP, "page-up"),
	EV(GSURF_ACTION_PAGE_DOWN, "page-down"),
	EV(GSURF_ACTION_HALF_PAGE_UP, "half-page-up"),
	EV(GSURF_ACTION_HALF_PAGE_DOWN, "half-page-down"),
	EV(GSURF_ACTION_ZOOM_IN, "zoom-in"),
	EV(GSURF_ACTION_ZOOM_OUT, "zoom-out"),
	EV(GSURF_ACTION_ZOOM_RESET, "zoom-reset"),
	EV(GSURF_ACTION_FIND, "find"),
	EV(GSURF_ACTION_FIND_NEXT, "find-next"),
	EV(GSURF_ACTION_FIND_PREV, "find-prev"),
	EV(GSURF_ACTION_COPY_URL, "copy-url"),
	EV(GSURF_ACTION_PASTE_URL, "paste-url"),
	EV(GSURF_ACTION_ENTER_NORMAL_MODE, "enter-normal-mode"),
	EV(GSURF_ACTION_ENTER_INSERT_MODE, "enter-insert-mode"),
	EV(GSURF_ACTION_FOLLOW_HINTS, "follow-hints"),
	EV(GSURF_ACTION_FOLLOW_HINTS_NEW_VIEW, "follow-hints-new-view"),
	EV(GSURF_ACTION_TAB_NEW, "tab-new"),
	EV(GSURF_ACTION_TAB_CLOSE, "tab-close"),
	EV(GSURF_ACTION_TAB_NEXT, "tab-next"),
	EV(GSURF_ACTION_TAB_PREV, "tab-prev"),
	EV(GSURF_ACTION_TAB_REOPEN, "tab-reopen"),
	EV(GSURF_ACTION_TOGGLE_FULLSCREEN, "toggle-fullscreen"),
	EV(GSURF_ACTION_QUIT, "quit"),
	EV(GSURF_ACTION_TOGGLE_SETTING, "toggle-setting"),
	EV(GSURF_ACTION_MODULE, "module"),
	END_EV
})

GsurfAction
gsurf_action_from_string(const gchar *str)
{
	GEnumClass *klass;
	GEnumValue *value;
	gchar *nick;
	GsurfAction action = GSURF_ACTION_NONE;

	if (str == NULL || *str == '\0')
		return GSURF_ACTION_NONE;

	/* Accept underscores as a convenience (config style). */
	nick = g_strdelimit(g_strdup(str), "_", '-');

	klass = g_type_class_ref(GSURF_TYPE_ACTION);
	value = g_enum_get_value_by_nick(klass, nick);
	if (value != NULL)
		action = (GsurfAction)value->value;
	g_type_class_unref(klass);

	g_free(nick);
	return action;
}

const gchar *
gsurf_action_to_string(GsurfAction action)
{
	GEnumClass *klass;
	GEnumValue *value;
	const gchar *nick = NULL;

	klass = g_type_class_ref(GSURF_TYPE_ACTION);
	value = g_enum_get_value(klass, action);
	if (value != NULL)
		nick = value->value_nick;
	g_type_class_unref(klass);

	return nick;
}
