/*
 * gsurf-wmux-module.c - wmux: a tmux-style web multiplexer
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The module core:
 *
 *   - configuration (modules.wmux), validated key by key; every bad value
 *     warns and keeps its default, every bad binding warns and is skipped;
 *   - keys: the prefix (C-b) sets the window's key grab so the next key
 *     reaches us even while the user types in a page or the address bar;
 *     that key runs its prefix-table command and clears the grab;
 *     repeatable bindings work again without the prefix for repeat_time
 *     ms; the root table (C-h/j/k/l) works without the prefix;
 *   - the command interpreter for the wmux command language (bindings and
 *     the command prompt);
 *   - the pane-split-right/-down actions (tabs context menu, core
 *     keybinds) and the keybind help listing;
 *   - the life cycle: activate() installs the module as the active
 *     window's #GsurfWindowLayout, deactivate() puts the plain view stack
 *     back with every view alive.
 *
 * Implements #GsurfWindowLayout (gsurf-wmux-session.c), #GsurfInputHandler,
 * #GsurfActionHandler and #GsurfKeybindProvider. Builds on GTK3 and GTK4.
 */

#include <gmodule.h>
#include <yaml-glib.h>

#include "gsurf-wmux-private.h"

static void gsurf_wmux_input_init(GsurfInputHandlerInterface *iface);
static void gsurf_wmux_keybind_init(GsurfKeybindProviderInterface *iface);
static void gsurf_wmux_action_init(GsurfActionHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfWmuxModule, gsurf_wmux_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_WINDOW_LAYOUT, gsurf_wmux_layout_iface_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_wmux_input_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_KEYBIND_PROVIDER, gsurf_wmux_keybind_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_ACTION_HANDLER, gsurf_wmux_action_init))

/* The dispatch priority: before modal (GSURF_PRIORITY_HIGHEST = 0) and
 * find_bar, so the prefix and C-h/j/k/l win. */
#define WMUX_PRIORITY (-10)

/* Every key modules.wmux may hold (anything else warns). */
static const gchar *const known_keys[] = {
	"enabled", "prefix", "prefix_timeout", "repeat_time", "display_panes_time",
	"bindings", "root_bindings", "repeatable", "split_ratio", "on_pane_kill",
	"on_tab_close", "confirm_kill", "mouse", "remember_layout", "min_pane_pct", "gap",
	"border_width", "active_border_width", "border_color", "active_border_color",
	"container_colors", "inactive_opacity", "active_brighten", "status",
	"status_position", "status_bg", "status_fg", "status_current_fg",
	"window_name_max", "mode_indicator", "mode_hide_normal", "mode_normal_color",
	"mode_insert_color", "mode_follow_color", NULL
};

/* ------------------------------------------------------------------ */
/* Key grab                                                            */
/* ------------------------------------------------------------------ */

/* The window's key grab is on while we wait for a key: after the
 * prefix, while display-panes shows numbers, while the chooser is open. */
void
gsurf_wmux_update_grab(GsurfWmuxModule *self)
{
	if (self->window == NULL || !self->installed)
		return;
	gsurf_window_set_key_grab(self->window,
		self->prefix_active || self->displaying || self->chooser_open);
}

static gboolean
on_prefix_timeout(gpointer user_data)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(user_data);

	self->prefix_timeout_id = 0;
	self->prefix_active = FALSE;
	gsurf_wmux_update_grab(self);
	return G_SOURCE_REMOVE;
}

static void
set_prefix_active(GsurfWmuxModule *self, gboolean active)
{
	if (self->prefix_timeout_id != 0) {
		g_source_remove(self->prefix_timeout_id);
		self->prefix_timeout_id = 0;
	}
	self->prefix_active = active;
	if (active && self->prefix_timeout > 0)
		self->prefix_timeout_id = g_timeout_add((guint)self->prefix_timeout,
			on_prefix_timeout, self);
	gsurf_wmux_update_grab(self);
}

/* Drop what waits for a key — a pending prefix, a repeat window, the pane
 * numbers — when the user's attention moved: a click (into a page field,
 * a pane, the address bar), another view becoming active, or the window
 * losing focus. Otherwise the grab would stay on (prefix_timeout 0 has
 * no timer) and the next key typed into a field, possibly in another
 * window first, would run as a wmux command. The chooser stays: it is an
 * on-screen panel with its own keys and closes on Escape or q. */
static void
cancel_pending_keys(GsurfWmuxModule *self, gboolean end_repeat)
{
	if (end_repeat)
		self->repeat_until = 0;
	if (self->displaying)
		gsurf_wmux_ui_display_panes(self, FALSE);
	if (self->prefix_active)
		set_prefix_active(self, FALSE);
}

static gboolean
on_window_button_press(GsurfWindow *window, guint button, guint state, gpointer user_data)
{
	(void)window;
	(void)button;
	(void)state;
	cancel_pending_keys(GSURF_WMUX_MODULE(user_data), TRUE);
	return FALSE;
}

static void
on_window_active_view_changed(GsurfWindow *window, GsurfView *view, gpointer user_data)
{
	(void)window;
	(void)view;
	/* A repeatable select-pane changes the active view itself: keep its
	 * repeat window. */
	cancel_pending_keys(GSURF_WMUX_MODULE(user_data), FALSE);
}

/* The toplevel (GtkWindow, both toolkits) gained or lost focus. */
static void
on_native_is_active(GObject *native, GParamSpec *pspec, gpointer user_data)
{
	gboolean active = TRUE;

	(void)pspec;
	g_object_get(native, "is-active", &active, NULL);
	if (!active)
		cancel_pending_keys(GSURF_WMUX_MODULE(user_data), TRUE);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

/* "#W" in @text replaced by the current window's name. */
static gchar *
expand_formats(GsurfWmuxModule *self, const gchar *text)
{
	g_autofree gchar *name = gsurf_wmux_win_display_name(self, self->current);
	g_auto(GStrv) parts = NULL;

	if (text == NULL)
		return g_strdup("");
	parts = g_strsplit(text, "#W", -1);
	return g_strjoinv(name, parts);
}

static GsurfWmuxDirection
direction_of(gchar letter)
{
	switch (letter) {
	case 'L': return GSURF_WMUX_DIR_LEFT;
	case 'R': return GSURF_WMUX_DIR_RIGHT;
	case 'U': return GSURF_WMUX_DIR_UP;
	default:  return GSURF_WMUX_DIR_DOWN;
	}
}

/* The keybind help, built from our own tables. */
static void
show_keys(GsurfWmuxModule *self)
{
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gsurf_keybind_help_free);

	gsurf_keybind_provider_list_keybinds(GSURF_KEYBIND_PROVIDER(self), entries);
	if (self->window != NULL)
		gsurf_window_show_keybind_help(self->window, entries);
}

/* select-pane / select-window -t: an absolute number, or +N / -N from
 * @current, wrapping over @n. */
static gint
resolve_target(const GsurfWmuxCommand *cmd, gint current, gint n)
{
	if (n <= 0)
		return -1;
	if (cmd->relative)
		return (((current + cmd->number) % n) + n) % n;
	return cmd->number >= 0 && cmd->number < n ? cmd->number : -1;
}

/* Whether a kill command may skip its confirm-before (confirm_kill off). */
static gboolean
is_kill(const GsurfWmuxCommand *cmd)
{
	return cmd != NULL &&
	       (cmd->id == GSURF_WMUX_CMD_KILL_PANE || cmd->id == GSURF_WMUX_CMD_KILL_WINDOW);
}

/*
 * gsurf_wmux_run:
 * @cmd: a parsed command
 * @error: (nullable): where a command that cannot be carried out says why
 *
 * Runs @cmd on the current window. Unknown targets (select-pane -t 7 with
 * two panes, select-window -t 9, last-pane without a last pane, ...) and
 * refused operations (a split below min_pane_pct, break-pane of a lone
 * pane) fail with a message as tmux prints it; a direction without a
 * neighbour (select-pane -L at the left edge) is not an error.
 *
 * Returns: whether @cmd ran
 */
gboolean
gsurf_wmux_run(GsurfWmuxModule *self, const GsurfWmuxCommand *cmd, GError **error)
{
	GsurfWmuxWin *win = gsurf_wmux_current(self);
	guint active;
	gint target;

	if (cmd == NULL)
		return TRUE;
	if (win == NULL || !self->attached) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
			"wmux is not attached to a window");
		return FALSE;
	}
	active = gsurf_wmux_layout_get_active(win->layout);

	switch (cmd->id) {
	case GSURF_WMUX_CMD_SPLIT_WINDOW:
		return gsurf_wmux_split(self, active,
			gsurf_wmux_command_has_flag(cmd, 'h') ? GSURF_WMUX_SPLIT_H : GSURF_WMUX_SPLIT_V,
			gsurf_wmux_command_has_flag(cmd, 'b'), gsurf_wmux_command_has_flag(cmd, 'd'),
			cmd->has_number ? (guint)cmd->number : 0, gsurf_wmux_command_has_flag(cmd, 'c'),
			gsurf_wmux_command_get_arg(cmd, 0), NULL, error);
	case GSURF_WMUX_CMD_SELECT_PANE:
		if (gsurf_wmux_command_has_flag(cmd, 'l')) {
			if (gsurf_wmux_layout_get_last(win->layout) == GSURF_WMUX_PANE_NONE) {
				g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
					"select-pane: no last pane");
				return FALSE;
			}
			gsurf_wmux_select_pane(self, gsurf_wmux_layout_get_last(win->layout));
		} else if (gsurf_wmux_command_has_flag(cmd, 't')) {
			target = resolve_target(cmd, gsurf_wmux_layout_get_pane_index(win->layout, active),
				(gint)gsurf_wmux_layout_get_n_panes(win->layout));
			if (target < 0) {
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
					"select-pane: can't find pane %d (the window has %u)", cmd->number,
					gsurf_wmux_layout_get_n_panes(win->layout));
				return FALSE;
			}
			gsurf_wmux_select_pane(self,
				gsurf_wmux_layout_get_nth_pane(win->layout, (guint)target));
		} else {
			gsurf_wmux_select_pane(self, gsurf_wmux_layout_neighbor(win->layout, active,
				direction_of(gsurf_wmux_command_direction(cmd)), TRUE));
		}
		break;
	case GSURF_WMUX_CMD_LAST_PANE:
		if (gsurf_wmux_layout_get_last(win->layout) == GSURF_WMUX_PANE_NONE) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
				"last-pane: no last pane");
			return FALSE;
		}
		gsurf_wmux_select_pane(self, gsurf_wmux_layout_get_last(win->layout));
		break;
	case GSURF_WMUX_CMD_KILL_PANE:
		gsurf_wmux_kill_pane(self, self->current, active);
		break;
	case GSURF_WMUX_CMD_RESIZE_PANE:
		if (gsurf_wmux_command_has_flag(cmd, 'Z'))
			gsurf_wmux_layout_toggle_zoom(win->layout);
		else
			gsurf_wmux_layout_resize(win->layout, active,
				direction_of(gsurf_wmux_command_direction(cmd)),
				cmd->has_number ? (guint)cmd->number : 1);
		gsurf_wmux_show_active(self);
		break;
	case GSURF_WMUX_CMD_SWAP_PANE:
		gsurf_wmux_layout_swap(win->layout, active,
			gsurf_wmux_command_has_flag(cmd, 'U') ? gsurf_wmux_layout_prev(win->layout, active)
			: gsurf_wmux_layout_next(win->layout, active));
		gsurf_wmux_show_active(self);
		break;
	case GSURF_WMUX_CMD_ROTATE_WINDOW:
		gsurf_wmux_layout_rotate(win->layout, gsurf_wmux_command_has_flag(cmd, 'D'));
		gsurf_wmux_show_active(self);
		break;
	case GSURF_WMUX_CMD_SELECT_LAYOUT:
		if (gsurf_wmux_command_has_flag(cmd, 'E')) {
			gsurf_wmux_layout_spread_evenly(win->layout, active);
		} else {
			GsurfWmuxPreset preset;

			if (!gsurf_wmux_preset_from_string(gsurf_wmux_command_get_arg(cmd, 0), &preset)) {
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
					"select-layout: unknown layout '%s'",
					gsurf_wmux_command_get_arg(cmd, 0));
				return FALSE;
			}
			gsurf_wmux_layout_apply_preset(win->layout, preset);
		}
		gsurf_wmux_show_active(self);
		break;
	case GSURF_WMUX_CMD_NEXT_LAYOUT:
		gsurf_wmux_layout_next_preset(win->layout);
		gsurf_wmux_show_active(self);
		break;
	case GSURF_WMUX_CMD_BREAK_PANE:
		return gsurf_wmux_break_pane(self, error);
	case GSURF_WMUX_CMD_DISPLAY_PANES:
		gsurf_wmux_ui_display_panes(self, TRUE);
		break;
	case GSURF_WMUX_CMD_NEW_WINDOW:
		gsurf_wmux_new_window(self, gsurf_wmux_command_get_value(cmd, 'n'),
			gsurf_wmux_command_get_arg(cmd, 0));
		break;
	case GSURF_WMUX_CMD_NEXT_WINDOW:
		gsurf_wmux_select_window(self, (self->current + 1) % self->wins->len);
		break;
	case GSURF_WMUX_CMD_PREVIOUS_WINDOW:
		gsurf_wmux_select_window(self, (self->current + self->wins->len - 1) % self->wins->len);
		break;
	case GSURF_WMUX_CMD_LAST_WINDOW:
		if (self->last_win < 0 || (guint)self->last_win >= self->wins->len) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
				"last-window: no last window");
			return FALSE;
		}
		gsurf_wmux_select_window(self, (guint)self->last_win);
		break;
	case GSURF_WMUX_CMD_SELECT_WINDOW:
		target = resolve_target(cmd, (gint)self->current, (gint)self->wins->len);
		if (target < 0) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
				"select-window: can't find window %d (there are %u)", cmd->number,
				self->wins->len);
			return FALSE;
		}
		gsurf_wmux_select_window(self, (guint)target);
		break;
	case GSURF_WMUX_CMD_KILL_WINDOW:
		gsurf_wmux_kill_window(self, self->current);
		break;
	case GSURF_WMUX_CMD_RENAME_WINDOW:
		gsurf_wmux_rename_window(self, gsurf_wmux_command_get_arg(cmd, 0));
		break;
	case GSURF_WMUX_CMD_CHOOSE_TREE:
		gsurf_wmux_ui_chooser_open(self);
		break;
	case GSURF_WMUX_CMD_COMMAND_PROMPT: {
		const gchar *prompt = gsurf_wmux_command_get_value(cmd, 'p');
		g_autofree gchar *initial = expand_formats(self, gsurf_wmux_command_get_value(cmd, 'I'));

		gsurf_wmux_ui_prompt(self, GSURF_WMUX_PROMPT_COMMAND,
			prompt != NULL ? prompt : ":", initial);
		g_free(self->prompt_template);
		self->prompt_template = g_strdup(cmd->template_str);
		break;
	}
	case GSURF_WMUX_CMD_CONFIRM_BEFORE: {
		g_autofree gchar *text = NULL;
		g_autofree gchar *prompt = NULL;

		if (cmd->nested == NULL)
			break;
		if (!self->confirm_kill && is_kill(cmd->nested))
			return gsurf_wmux_run(self, cmd->nested, error);
		text = gsurf_wmux_command_to_string(cmd->nested);
		prompt = gsurf_wmux_command_get_value(cmd, 'p') != NULL
			? expand_formats(self, gsurf_wmux_command_get_value(cmd, 'p'))
			: g_strdup_printf("%s? (y/n)", text);
		gsurf_wmux_ui_prompt(self, GSURF_WMUX_PROMPT_CONFIRM, prompt, "");
		g_free(self->prompt_command);
		self->prompt_command = g_steal_pointer(&text);
		break;
	}
	case GSURF_WMUX_CMD_SEND_PREFIX:
		self->send_prefix = TRUE;
		break;
	case GSURF_WMUX_CMD_LIST_KEYS:
		show_keys(self);
		break;
	case GSURF_WMUX_CMD_COUNT:
	default:
		break;
	}
	return TRUE;
}

/* Run @cmd from a key or the prompt: a failure goes to the status line
 * (and the log), as tmux shows it. */
static void
run_and_report(GsurfWmuxModule *self, const GsurfWmuxCommand *cmd)
{
	g_autoptr(GError) error = NULL;

	if (!gsurf_wmux_run(self, cmd, &error) && error != NULL) {
		g_debug("wmux: %s", error->message);
		gsurf_wmux_ui_message(self, error->message);
	}
}

gboolean
gsurf_wmux_run_line(GsurfWmuxModule *self, const gchar *line)
{
	g_autoptr(GsurfWmuxCommand) cmd = NULL;
	g_autoptr(GError) error = NULL;

	cmd = gsurf_wmux_command_parse(line, NULL, &error);
	if (cmd == NULL) {
		gsurf_wmux_ui_message(self, error->message);
		return FALSE;
	}
	run_and_report(self, cmd);
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Keys                                                                */
/* ------------------------------------------------------------------ */

/* Run a binding's command (parsed once and cached by the table). */
static void
run_binding(GsurfWmuxModule *self, GsurfWmuxBindings *table, const gchar *key)
{
	run_and_report(self, gsurf_wmux_bindings_get_command(table, key));
}

/* Keys while the chooser is open: j/k (or arrows) move, Enter picks, x
 * kills, Escape or q closes; everything else is swallowed. */
static gboolean
chooser_key(GsurfWmuxModule *self, const gchar *name)
{
	if (g_strcmp0(name, "j") == 0 || g_strcmp0(name, "Down") == 0)
		gsurf_wmux_ui_chooser_move(self, 1);
	else if (g_strcmp0(name, "k") == 0 || g_strcmp0(name, "Up") == 0)
		gsurf_wmux_ui_chooser_move(self, -1);
	else if (g_strcmp0(name, "Enter") == 0 || g_strcmp0(name, "KPEnter") == 0)
		gsurf_wmux_ui_chooser_choose(self);
	else if (g_strcmp0(name, "x") == 0)
		gsurf_wmux_ui_chooser_kill(self);
	else if (g_strcmp0(name, "Escape") == 0 || g_strcmp0(name, "q") == 0)
		gsurf_wmux_ui_chooser_close(self);
	return TRUE;
}

static gboolean
gsurf_wmux_handle_key_event(GsurfInputHandler *handler, GsurfView *view,
                            guint keyval, guint keycode, guint state, GsurfModePolicy mode)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(handler);
	g_autofree gchar *name = NULL;
	const GsurfWmuxBinding *binding;

	(void)view;
	(void)keycode;
	(void)mode;
	if (!self->installed || !self->attached)
		return FALSE;
	/* The prompt entry types its own keys (Enter/Escape are its own). */
	if (self->prompt_kind != GSURF_WMUX_PROMPT_NONE)
		return FALSE;
	name = gsurf_wmux_keys_from_event(keyval, state);
	/* A bare modifier (the Shift of a "%") never ends a pending prefix. */
	if (name == NULL)
		return FALSE;

	/* 1. Modes that own the keyboard. */
	if (self->chooser_open)
		return chooser_key(self, name);
	if (self->displaying) {
		if (g_ascii_isdigit(name[0]) && name[1] == '\0') {
			GsurfWmuxWin *win = gsurf_wmux_current(self);
			guint pane = gsurf_wmux_layout_get_nth_pane(win->layout, (guint)(name[0] - '0'));

			gsurf_wmux_ui_display_panes(self, FALSE);
			if (pane != GSURF_WMUX_PANE_NONE)
				gsurf_wmux_select_pane(self, pane);
			return TRUE;
		}
		/* Any other key hides the numbers and is handled as usual. */
		gsurf_wmux_ui_display_panes(self, FALSE);
	}

	/* 2. The key after the prefix: its prefix-table command (an unbound
	 *    key or Escape just cancels). */
	if (self->prefix_active) {
		set_prefix_active(self, FALSE);
		binding = gsurf_wmux_bindings_lookup(self->prefix_table, name);
		if (binding == NULL)
			return TRUE;
		self->repeat_until = binding->repeatable
			? g_get_monotonic_time() + (gint64)self->repeat_time * 1000 : 0;
		run_binding(self, self->prefix_table, name);
		return TRUE;
	}

	/* 3. A repeatable key again within repeat_time: no prefix needed. */
	if (self->repeat_until != 0) {
		if (g_get_monotonic_time() <= self->repeat_until) {
			binding = gsurf_wmux_bindings_lookup(self->prefix_table, name);
			if (binding != NULL && binding->repeatable) {
				self->repeat_until = g_get_monotonic_time() + (gint64)self->repeat_time * 1000;
				run_binding(self, self->prefix_table, name);
				return TRUE;
			}
		}
		self->repeat_until = 0;
	}

	/* 4. The prefix itself (after send-prefix, once, it reaches the page). */
	if (g_strcmp0(name, self->prefix) == 0) {
		if (self->send_prefix) {
			self->send_prefix = FALSE;
			return FALSE;
		}
		set_prefix_active(self, TRUE);
		return TRUE;
	}

	/* 5. The root table. */
	if (gsurf_wmux_bindings_lookup(self->root_table, name) != NULL) {
		run_binding(self, self->root_table, name);
		return TRUE;
	}
	return FALSE;
}

static void
gsurf_wmux_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_wmux_handle_key_event;
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

/* pane-split-right / pane-split-down for @view: a view on screen gets its
 * pane split with a new tab; any other view is shown in a new pane split
 * off the active one. */
static gboolean
gsurf_wmux_handle_action(GsurfActionHandler *handler, GsurfView *view, GsurfAction action)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(handler);
	GsurfWmuxSplit split;
	GsurfWmuxWin *win;
	guint w;
	guint p;

	if (action == GSURF_ACTION_PANE_SPLIT_RIGHT)
		split = GSURF_WMUX_SPLIT_H;
	else if (action == GSURF_ACTION_PANE_SPLIT_DOWN)
		split = GSURF_WMUX_SPLIT_V;
	else
		return FALSE;
	if (!self->installed || !self->attached || gsurf_kiosk_is_enabled())
		return FALSE;
	win = gsurf_wmux_current(self);
	if (view == NULL || gsurf_window_get_view_index(self->window, view) < 0)
		view = gsurf_window_get_active_view(self->window);

	if (view != NULL && gsurf_wmux_view_visible(self, view) &&
	    gsurf_wmux_find_view(self, gsurf_view_get_id(view), &w, &p))
		gsurf_wmux_split(self, p, split, FALSE, FALSE, 0, FALSE, NULL, NULL, NULL);
	else
		gsurf_wmux_split(self, gsurf_wmux_layout_get_active(win->layout), split, FALSE,
			FALSE, 0, FALSE, NULL, view, NULL);
	return TRUE;
}

static void
gsurf_wmux_action_init(GsurfActionHandlerInterface *iface)
{
	iface->handle_action = gsurf_wmux_handle_action;
}

/* ------------------------------------------------------------------ */
/* Keybind help                                                        */
/* ------------------------------------------------------------------ */

static void
list_table(GPtrArray *entries, GsurfWmuxBindings *table, const gchar *prefix)
{
	guint n = gsurf_wmux_bindings_get_n(table);
	guint i;

	for (i = 0; i < n; i++) {
		const GsurfWmuxBinding *b = gsurf_wmux_bindings_get_nth(table, i);
		g_autofree gchar *key = prefix != NULL ? g_strdup_printf("%s %s", prefix, b->key)
			: g_strdup(b->key);
		g_autofree gchar *desc = b->repeatable
			? g_strdup_printf("%s (repeatable)", b->command) : g_strdup(b->command);

		gsurf_keybind_help_append(entries, key, desc, "wmux", b->command);
	}
}

static void
gsurf_wmux_list_keybinds(GsurfKeybindProvider *provider, GPtrArray *entries)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(provider);

	gsurf_keybind_help_append(entries, self->prefix,
		"wmux prefix: the next key runs a prefix-table command", "wmux", "prefix");
	list_table(entries, self->prefix_table, self->prefix);
	list_table(entries, self->root_table, NULL);
}

static void
gsurf_wmux_keybind_init(GsurfKeybindProviderInterface *iface)
{
	iface->list_keybinds = gsurf_wmux_list_keybinds;
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

static const gchar *gsurf_wmux_get_name(GsurfModule *m) { (void)m; return "wmux"; }
static const gchar *gsurf_wmux_get_description(GsurfModule *m)
{
	(void)m;
	return "tmux-style panes, windows and status line in one browser window";
}

static void gsurf_wmux_deactivate(GsurfModule *module);

static void
on_container_changed(gpointer instance, gpointer user_data)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(user_data);

	(void)instance;
	gsurf_wmux_ui_update_css(self);
	gsurf_wmux_ui_sync(self);
}

static void
on_container_notify(GObject *container, GParamSpec *pspec, gpointer user_data)
{
	(void)pspec;
	on_container_changed(container, user_data);
}

static void
watch_container(GsurfWmuxModule *self, GsurfContainer *container)
{
	if (g_signal_handler_find(container, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self) != 0)
		return;
	g_signal_connect(container, "notify::color", G_CALLBACK(on_container_notify), self);
}

static void
on_container_added(GsurfContainerRegistry *registry, GsurfContainer *container,
                   gpointer user_data)
{
	(void)registry;
	watch_container(GSURF_WMUX_MODULE(user_data), container);
	on_container_changed(container, user_data);
}

static void
on_container_removed(GsurfContainerRegistry *registry, GsurfContainer *container,
                     gpointer user_data)
{
	(void)registry;
	g_signal_handlers_disconnect_by_data(container, user_data);
	on_container_changed(container, user_data);
}

static void
connect_registry(GsurfWmuxModule *self)
{
	g_autoptr(GPtrArray) containers = NULL;
	guint i;

	self->registry = g_object_ref(gsurf_container_registry_get_default());
	g_signal_connect(self->registry, "container-added", G_CALLBACK(on_container_added), self);
	g_signal_connect(self->registry, "container-removed", G_CALLBACK(on_container_removed), self);
	containers = gsurf_container_registry_list(self->registry);
	for (i = 0; i < containers->len; i++)
		watch_container(self, g_ptr_array_index(containers, i));
}

static void
disconnect_registry(GsurfWmuxModule *self)
{
	g_autoptr(GPtrArray) containers = NULL;
	guint i;

	if (self->registry == NULL)
		return;
	g_signal_handlers_disconnect_by_data(self->registry, self);
	containers = gsurf_container_registry_list(self->registry);
	for (i = 0; i < containers->len; i++)
		g_signal_handlers_disconnect_by_data(g_ptr_array_index(containers, i), self);
	g_clear_object(&self->registry);
}

/* gsurf_window_reopen_view() (reopen in a container): the new view takes
 * the old one's pane. The window then activates it (it is in a pane, so
 * that pane is focused) and removes the old view (a background tab by
 * then). */
static void
on_view_replaced(GsurfWindow *window, GsurfView *old_view, GsurfView *new_view,
                 gpointer user_data)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(user_data);
	GsurfWmuxWin *win;
	guint w;
	guint p;

	(void)window;
	if (!self->attached ||
	    !gsurf_wmux_find_view(self, gsurf_view_get_id(old_view), &w, &p))
		return;
	/* The new view may already sit in a pane of its own: free that one. */
	if (gsurf_wmux_find_view(self, gsurf_view_get_id(new_view), NULL, NULL))
		return;
	win = g_ptr_array_index(self->wins, w);
	gsurf_wmux_layout_set_payload(win->layout, p, gsurf_view_get_id(new_view));
	gsurf_wmux_ui_sync(self);
}

/* Release everything but the configuration; the window's own handlers
 * are the caller's business. */
static void
release_activation(GsurfWmuxModule *self)
{
	if (self->prefix_timeout_id != 0) {
		g_source_remove(self->prefix_timeout_id);
		self->prefix_timeout_id = 0;
	}
	self->prefix_active = FALSE;
	self->repeat_until = 0;
	self->send_prefix = FALSE;
	self->dragging = FALSE;
	gsurf_wmux_session_clear(self);
	gsurf_wmux_ui_free(self);
	disconnect_registry(self);
	self->installed = FALSE;
	self->attached = FALSE;
}

/* The host destroyed the window first: its dispose removed the layout
 * (detach ran); drop what is left. */
static void
on_window_finalized(gpointer user_data, GObject *where_the_window_was)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(user_data);

	(void)where_the_window_was;
	self->window = NULL;
	release_activation(self);
}

static gboolean
gsurf_wmux_activate(GsurfModule *module)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(module);
	GsurfWindow *window = gsurf_module_manager_get_active_window(
		gsurf_module_manager_get_default());

	/* Keys before modal (0) and find_bar. */
	gsurf_module_set_priority(module, WMUX_PRIORITY);
	if (window == NULL)
		return TRUE;
	/* A second activate() starts from a clean slate. */
	gsurf_wmux_deactivate(module);

	/* Kiosk shows one view by design, and a backend without native
	 * content packing (libregnum) cannot host a layout: stay inactive,
	 * say so once (before building any widget). */
	if (gsurf_kiosk_is_enabled() || GSURF_WINDOW_GET_CLASS(window)->set_content == NULL) {
		if (!self->refusal_logged)
			g_message("wmux: %s; wmux stays inactive", gsurf_kiosk_is_enabled()
				? "kiosk mode shows a single view" : "this window cannot host panes");
		self->refusal_logged = TRUE;
		return TRUE;
	}

	self->window = window;
	g_object_weak_ref(G_OBJECT(window), on_window_finalized, self);
	connect_registry(self);
	gsurf_wmux_ui_build(self);

	/* Install: the window hands us its views (attach, add_view each,
	 * activate the active one), then packs our root. A backend that
	 * cannot host a layout (libregnum) refuses with its own one-time
	 * warning; we then stay inactive. */
	self->installed = TRUE;
	if (!gsurf_window_set_layout(window, GSURF_WINDOW_LAYOUT(self))) {
		self->installed = FALSE;
		gsurf_wmux_deactivate(module);
		return TRUE;
	}
	g_signal_connect(window, "view-replaced", G_CALLBACK(on_view_replaced), self);
	g_signal_connect(window, "button-press", G_CALLBACK(on_window_button_press), self);
	g_signal_connect(window, "active-view-changed",
		G_CALLBACK(on_window_active_view_changed), self);
	{
		GObject *native = gsurf_window_get_native_widget(window);

		if (native != NULL && g_object_class_find_property(G_OBJECT_GET_CLASS(native),
				"is-active") != NULL)
			g_signal_connect(native, "notify::is-active",
				G_CALLBACK(on_native_is_active), self);
	}
	gsurf_wmux_ui_sync(self);
	return TRUE;
}

/* Put the plain view stack back (every view alive, the active one shown)
 * and release everything; safe to call any number of times. */
static void
gsurf_wmux_deactivate(GsurfModule *module)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(module);
	g_autoptr(GsurfWindow) window = NULL;

	if (self->window != NULL) {
		GObject *native;

		window = g_object_ref(self->window);
		g_signal_handlers_disconnect_by_data(window, self);
		native = gsurf_window_get_native_widget(window);
		if (native != NULL)
			g_signal_handlers_disconnect_by_data(native, self);
		if (self->installed && gsurf_window_get_layout(window) == GSURF_WINDOW_LAYOUT(self))
			gsurf_window_set_layout(window, NULL);
		gsurf_window_set_key_grab(window, FALSE);
		g_object_weak_unref(G_OBJECT(window), on_window_finalized, self);
		self->installed = FALSE;
		release_activation(self);
		self->window = NULL;
		return;
	}
	release_activation(self);
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static void
set_string(gchar **field, const gchar *value)
{
	g_free(*field);
	*field = g_strdup(value);
}

/* The scalar text of @member, or NULL (null, a list, a mapping: the
 * caller's warning covers it). */
static const gchar *
scalar_member(YamlMapping *m, const gchar *member)
{
	YamlNode *node = yaml_mapping_get_member(m, member);

	if (node == NULL || yaml_node_is_null(node) ||
	    yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

static void
read_int(YamlMapping *m, const gchar *member, gint min, gint max, gint *field)
{
	const gchar *text;
	gint64 value;

	if (!yaml_mapping_has_member(m, member))
		return;
	text = scalar_member(m, member);
	if (text == NULL || !g_ascii_string_to_signed(text, 10, min, max, &value, NULL)) {
		g_warning("wmux.%s must be an integer from %d to %d; keeping %d",
		          member, min, max, *field);
		return;
	}
	*field = (gint)value;
}

static void
read_double(YamlMapping *m, const gchar *member, gdouble min, gdouble max, gdouble *field)
{
	const gchar *text;
	gchar *end = NULL;
	gdouble value = 0.0;

	if (!yaml_mapping_has_member(m, member))
		return;
	text = scalar_member(m, member);
	if (text != NULL && *text != '\0')
		value = g_ascii_strtod(text, &end);
	if (text == NULL || *text == '\0' || end == NULL || *end != '\0' ||
	    !(value >= min && value <= max)) {
		g_warning("wmux.%s must be a number from %g to %g; keeping %g",
		          member, min, max, *field);
		return;
	}
	*field = value;
}

static void
read_bool(YamlMapping *m, const gchar *member, gboolean *field)
{
	static const gchar *const yes[] = { "true", "yes", "on", NULL };
	static const gchar *const no[] = { "false", "no", "off", NULL };
	const gchar *text;
	guint i;

	if (!yaml_mapping_has_member(m, member))
		return;
	text = scalar_member(m, member);
	for (i = 0; text != NULL && yes[i] != NULL; i++) {
		if (g_ascii_strcasecmp(text, yes[i]) == 0) {
			*field = TRUE;
			return;
		}
		if (g_ascii_strcasecmp(text, no[i]) == 0) {
			*field = FALSE;
			return;
		}
	}
	g_warning("wmux.%s must be true or false; keeping %s", member, *field ? "true" : "false");
}

/* One of two words; anything else warns and keeps the default. */
static void
read_choice(YamlMapping *m, const gchar *member, const gchar *first,
            const gchar *second, gboolean *is_second)
{
	const gchar *text;

	if (!yaml_mapping_has_member(m, member))
		return;
	text = scalar_member(m, member);
	if (g_strcmp0(text, first) == 0)
		*is_second = FALSE;
	else if (g_strcmp0(text, second) == 0)
		*is_second = TRUE;
	else
		g_warning("wmux.%s must be '%s' or '%s', not '%s'; keeping '%s'",
		          member, first, second, text != NULL ? text : "",
		          *is_second ? second : first);
}

/* A CSS colour, validated and stored normalized. */
static void
read_color(YamlMapping *m, const gchar *member, gchar **field)
{
	const gchar *text;
	GdkRGBA rgba;

	if (!yaml_mapping_has_member(m, member))
		return;
	text = scalar_member(m, member);
	if (text == NULL || !gdk_rgba_parse(&rgba, text)) {
		g_warning("wmux.%s: '%s' is not a CSS colour; keeping %s",
		          member, text != NULL ? text : "", *field);
		return;
	}
	g_free(*field);
	*field = gdk_rgba_to_string(&rgba);
}

/* A key: tmux or gsurf notation, stored canonical. */
static void
read_key(YamlMapping *m, const gchar *member, gchar **field)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = NULL;
	const gchar *text;

	if (!yaml_mapping_has_member(m, member))
		return;
	text = scalar_member(m, member);
	key = gsurf_wmux_keys_normalize(text != NULL ? text : "", &error);
	if (key == NULL) {
		g_warning("wmux.%s: %s; keeping %s", member, error->message, *field);
		return;
	}
	g_free(*field);
	*field = g_steal_pointer(&key);
}

/* A binding table override (bindings:, root_bindings:): key -> command;
 * "" or null unbinds. A bad key or command warns and is skipped, so the
 * default for that key stays. */
static void
read_bindings(YamlMapping *m, const gchar *member, GsurfWmuxBindings *table)
{
	g_autoptr(GsurfWmuxBindings) overrides = NULL;
	g_autoptr(GPtrArray) errors = NULL;
	YamlNode *node = yaml_mapping_get_member(m, member);
	YamlMapping *map;
	guint i;

	if (node == NULL || yaml_node_is_null(node))
		return;
	if (yaml_node_get_node_type(node) != YAML_NODE_MAPPING) {
		g_warning("wmux.%s must be a mapping of key: command", member);
		return;
	}
	map = yaml_node_get_mapping(node);
	overrides = gsurf_wmux_bindings_new();
	for (i = 0; i < yaml_mapping_get_size(map); i++) {
		const gchar *key = yaml_mapping_get_key(map, i);
		YamlNode *value = yaml_mapping_get_value(map, i);
		const gchar *command = NULL;
		g_autoptr(GError) error = NULL;

		if (value != NULL && !yaml_node_is_null(value)) {
			if (yaml_node_get_node_type(value) != YAML_NODE_SCALAR) {
				g_warning("wmux.%s.%s: the command must be a string; skipped", member, key);
				continue;
			}
			command = yaml_node_get_scalar(value);
		}
		if (!gsurf_wmux_bindings_set(overrides, key, command, &error))
			g_warning("wmux.%s: %s; skipped", member, error->message);
	}
	if (!gsurf_wmux_bindings_validate(overrides, TRUE, &errors)) {
		for (i = 0; i < errors->len; i++)
			g_warning("wmux.%s: %s; skipped", member,
				((GError *)g_ptr_array_index(errors, i))->message);
	}
	gsurf_wmux_bindings_merge(table, overrides);
}

/* repeatable: a list of prefix-table keys that repeat. */
static void
read_repeatable(YamlMapping *m, GsurfWmuxBindings *table)
{
	YamlNode *node = yaml_mapping_get_member(m, "repeatable");
	YamlSequence *seq;
	guint i;

	if (node == NULL || yaml_node_is_null(node))
		return;
	if (yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE) {
		g_warning("wmux.repeatable must be a list of keys");
		return;
	}
	seq = yaml_node_get_sequence(node);
	for (i = 0; i < yaml_sequence_get_length(seq); i++) {
		YamlNode *element = yaml_sequence_get_element(seq, i);
		const gchar *key = element != NULL ? yaml_node_get_scalar(element) : NULL;
		g_autoptr(GError) error = NULL;

		if (key == NULL) {
			g_warning("wmux.repeatable[%u] must be a key", i);
			continue;
		}
		if (!gsurf_wmux_bindings_set_repeatable(table, key, TRUE, &error))
			g_warning("wmux.repeatable: %s; skipped", error->message);
	}
}

/* Every option back to its default (also the state with no wmux: block). */
static void
reset_config(GsurfWmuxModule *self)
{
	set_string(&self->prefix, GSURF_WMUX_DEFAULT_PREFIX);
	self->prefix_timeout = GSURF_WMUX_DEFAULT_PREFIX_TIMEOUT;
	self->repeat_time = GSURF_WMUX_DEFAULT_REPEAT_TIME;
	self->display_panes_time = GSURF_WMUX_DEFAULT_DISPLAY_PANES_TIME;
	g_clear_pointer(&self->prefix_table, gsurf_wmux_bindings_free);
	g_clear_pointer(&self->root_table, gsurf_wmux_bindings_free);
	self->prefix_table = gsurf_wmux_bindings_new_prefix_defaults();
	self->root_table = gsurf_wmux_bindings_new_root_defaults();
	self->split_ratio = GSURF_WMUX_DEFAULT_SPLIT_RATIO;
	self->keep_tab = FALSE;
	self->keep_pane = FALSE;
	self->confirm_kill = TRUE;
	self->mouse = TRUE;
	self->remember_layout = TRUE;
	self->min_pane_pct = GSURF_WMUX_DEFAULT_MIN_PANE_PCT;
	self->gap = GSURF_WMUX_DEFAULT_GAP;
	self->border_width = GSURF_WMUX_DEFAULT_BORDER_WIDTH;
	self->active_border_width = GSURF_WMUX_DEFAULT_ACTIVE_BORDER_WIDTH;
	set_string(&self->border_color, GSURF_WMUX_DEFAULT_BORDER_COLOR);
	set_string(&self->active_border_color, GSURF_WMUX_DEFAULT_ACTIVE_BORDER_COLOR);
	self->container_colors = TRUE;
	self->inactive_opacity = GSURF_WMUX_DEFAULT_INACTIVE_OPACITY;
	self->active_brighten = GSURF_WMUX_DEFAULT_ACTIVE_BRIGHTEN;
	self->status = TRUE;
	self->status_top = FALSE;
	set_string(&self->status_bg, GSURF_WMUX_DEFAULT_STATUS_BG);
	set_string(&self->status_fg, GSURF_WMUX_DEFAULT_STATUS_FG);
	set_string(&self->status_current_fg, GSURF_WMUX_DEFAULT_STATUS_CURRENT_FG);
	self->window_name_max = GSURF_WMUX_DEFAULT_WINDOW_NAME_MAX;
	self->mode_indicator = TRUE;
	self->mode_hide_normal = FALSE;
	set_string(&self->mode_normal_color, GSURF_WMUX_DEFAULT_MODE_NORMAL_COLOR);
	set_string(&self->mode_insert_color, GSURF_WMUX_DEFAULT_MODE_INSERT_COLOR);
	set_string(&self->mode_follow_color, GSURF_WMUX_DEFAULT_MODE_FOLLOW_COLOR);
}

/* The default table binds C-b to send-prefix; with another prefix that
 * binding moves to the new prefix key (C-a C-a sends C-a to the page). */
static void
move_send_prefix(GsurfWmuxModule *self)
{
	const GsurfWmuxBinding *b;

	if (g_strcmp0(self->prefix, GSURF_WMUX_DEFAULT_PREFIX) == 0)
		return;
	b = gsurf_wmux_bindings_lookup(self->prefix_table, GSURF_WMUX_DEFAULT_PREFIX);
	if (b == NULL || g_strcmp0(b->command, "send-prefix") != 0)
		return;
	gsurf_wmux_bindings_unbind(self->prefix_table, GSURF_WMUX_DEFAULT_PREFIX, NULL);
	gsurf_wmux_bindings_set(self->prefix_table, self->prefix, "send-prefix", NULL);
}

/* Each configure() starts from the defaults, so a second call replaces the
 * previous configuration entirely; it takes effect at the next
 * activate(). */
static void
gsurf_wmux_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(module);
	YamlNode *node;
	YamlMapping *m;
	guint i;

	reset_config(self);
	node = gsurf_config_get_module_node(config_ptr, "wmux");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
		return;
	m = yaml_node_get_mapping(node);

	/* 1. Unknown keys are typos: say so. */
	for (i = 0; i < yaml_mapping_get_size(m); i++) {
		const gchar *key = yaml_mapping_get_key(m, i);

		if (!g_strv_contains(known_keys, key))
			g_warning("wmux: unknown option '%s' (ignored)", key);
	}

	/* 2. Keys and tables: the prefix, send-prefix following it, then
	 *    the overrides and the extra repeatable keys. */
	read_key(m, "prefix", &self->prefix);
	move_send_prefix(self);
	read_bindings(m, "bindings", self->prefix_table);
	read_bindings(m, "root_bindings", self->root_table);
	read_repeatable(m, self->prefix_table);
	read_int(m, "prefix_timeout", 0, 60000, &self->prefix_timeout);
	read_int(m, "repeat_time", 0, 10000, &self->repeat_time);
	read_int(m, "display_panes_time", 100, 60000, &self->display_panes_time);

	/* 3. Behaviour. */
	read_int(m, "split_ratio", 1, 99, &self->split_ratio);
	read_choice(m, "on_pane_kill", "close_tab", "keep_tab", &self->keep_tab);
	read_choice(m, "on_tab_close", "close_pane", "keep_pane", &self->keep_pane);
	read_bool(m, "confirm_kill", &self->confirm_kill);
	read_bool(m, "mouse", &self->mouse);
	read_bool(m, "remember_layout", &self->remember_layout);
	read_int(m, "min_pane_pct", 0, 50, &self->min_pane_pct);

	/* 4. Looks. */
	read_int(m, "gap", 0, 64, &self->gap);
	read_int(m, "border_width", 0, 32, &self->border_width);
	read_int(m, "active_border_width", 0, 32, &self->active_border_width);
	read_color(m, "border_color", &self->border_color);
	read_color(m, "active_border_color", &self->active_border_color);
	read_bool(m, "container_colors", &self->container_colors);
	read_double(m, "inactive_opacity", 0.0, 1.0, &self->inactive_opacity);
	read_double(m, "active_brighten", 0.0, 1.0, &self->active_brighten);
	read_bool(m, "status", &self->status);
	read_choice(m, "status_position", "bottom", "top", &self->status_top);
	read_color(m, "status_bg", &self->status_bg);
	read_color(m, "status_fg", &self->status_fg);
	read_color(m, "status_current_fg", &self->status_current_fg);
	read_int(m, "window_name_max", 1, 200, &self->window_name_max);
	read_bool(m, "mode_indicator", &self->mode_indicator);
	read_bool(m, "mode_hide_normal", &self->mode_hide_normal);
	read_color(m, "mode_normal_color", &self->mode_normal_color);
	read_color(m, "mode_insert_color", &self->mode_insert_color);
	read_color(m, "mode_follow_color", &self->mode_follow_color);
}

static void
gsurf_wmux_module_dispose(GObject *object)
{
	gsurf_wmux_deactivate(GSURF_MODULE(object));
	G_OBJECT_CLASS(gsurf_wmux_module_parent_class)->dispose(object);
}

static void
gsurf_wmux_module_finalize(GObject *object)
{
	GsurfWmuxModule *self = GSURF_WMUX_MODULE(object);

	g_clear_pointer(&self->prefix, g_free);
	g_clear_pointer(&self->prefix_table, gsurf_wmux_bindings_free);
	g_clear_pointer(&self->root_table, gsurf_wmux_bindings_free);
	g_clear_pointer(&self->border_color, g_free);
	g_clear_pointer(&self->active_border_color, g_free);
	g_clear_pointer(&self->status_bg, g_free);
	g_clear_pointer(&self->status_fg, g_free);
	g_clear_pointer(&self->status_current_fg, g_free);
	g_clear_pointer(&self->mode_normal_color, g_free);
	g_clear_pointer(&self->mode_insert_color, g_free);
	g_clear_pointer(&self->mode_follow_color, g_free);
	G_OBJECT_CLASS(gsurf_wmux_module_parent_class)->finalize(object);
}

static void
gsurf_wmux_module_class_init(GsurfWmuxModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);

	object_class->dispose = gsurf_wmux_module_dispose;
	object_class->finalize = gsurf_wmux_module_finalize;
	module_class->activate = gsurf_wmux_activate;
	module_class->deactivate = gsurf_wmux_deactivate;
	module_class->get_name = gsurf_wmux_get_name;
	module_class->get_description = gsurf_wmux_get_description;
	module_class->configure = gsurf_wmux_configure;
}

static void
gsurf_wmux_module_init(GsurfWmuxModule *self)
{
	self->last_win = -1;
	reset_config(self);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_WMUX_MODULE;
}
