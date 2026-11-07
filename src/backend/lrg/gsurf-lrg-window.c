/*
 * gsurf-lrg-window.c - libregnum (LRG) standalone window
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Owns a graylib #GrlWindow and a per-frame render loop on the default GLib main
 * context. Each tick it captures the active #GsurfLrgView, uploads it to a GL
 * texture, draws it to fill the window (2D mode), and forwards raylib input to
 * the focused view. Chrome (address/find/status overlays) and full keybind/
 * modal routing are layered on in a later pass; this provides page rendering +
 * mouse + basic keyboard so @command{gsurf --lrg} is usable.
 *
 * For the WebKitGTK engine the page is rendered by GTK to CPU pixels, so the
 * GL context is only needed when uploading the texture inside the loop; the
 * #GrlWindow is therefore created lazily at present() (after the caller has set
 * the default size). A future WPE engine that imports an EGLImage will need the
 * context before the first view and should realize eagerly.
 */

#include "util/gsurf-kiosk.h"
#include "backend/lrg/gsurf-lrg-window.h"
#include "backend/lrg/gsurf-lrg-view.h"
#include "backend/lrg/gsurf-lrg-engine.h"
#include "module/gsurf-module-manager.h"
#ifdef GSURF_HAVE_LRG_3D
#include "backend/lrg/gsurf-lrg-scene.h"
#endif

#include <graylib.h>
#include <math.h>

/* X keysyms for the non-text keys we forward (avoids pulling in X/GDK headers;
 * the values are part of the stable keysym ABI). */
#define LRG_XK_BackSpace 0xFF08
#define LRG_XK_Tab       0xFF09
#define LRG_XK_Return    0xFF0D
#define LRG_XK_Escape    0xFF1B
#define LRG_XK_Home      0xFF50
#define LRG_XK_Left      0xFF51
#define LRG_XK_Up        0xFF52
#define LRG_XK_Right     0xFF53
#define LRG_XK_Down      0xFF54
#define LRG_XK_Prior     0xFF55  /* Page Up */
#define LRG_XK_Next      0xFF56  /* Page Down */
#define LRG_XK_End       0xFF57
#define LRG_XK_Insert    0xFF63
#define LRG_XK_Delete    0xFFFF
#define LRG_XK_KP_Enter  0xFF8D

#define LRG_DEFAULT_WIDTH  1024
#define LRG_DEFAULT_HEIGHT 768
#define LRG_CHROME_H       (gsurf_kiosk_is_enabled() ? 0 : 28)    /* top address-bar height in pixels */
#define LRG_CHROME_FONT    16
#define LRG_FONT_BASE      32    /* glyph-atlas size; downscaled when drawn */

/* 3D pointer gestures (same feel as cmacs lrgterm): a press becomes a drag
 * after this many pixels; orbit/look degrees per pixel; a second click within
 * this window is a double click; one wheel notch dollies by this factor. */
#define LRG_3D_DRAG_THRESHOLD 3.0f
#define LRG_3D_ORBIT_DEG_PX   0.4f
#define LRG_3D_LOOK_DEG_PX    0.25f
#define LRG_3D_DOUBLE_US      GSURF_LRG_SCENE_DOUBLE_CLICK_US
#define LRG_3D_DOLLY_STEP     0.9f
/* Keyboard camera steps. */
#define LRG_3D_ORBIT_STEP     15.0f
#define LRG_3D_ZOOM_STEP      0.8f

enum {
	SIGNAL_SCENE_CHANGED,
	N_SIGNALS
};

static guint lrg_window_signals[N_SIGNALS];

/* What a held pointer button is doing in 3D. */
typedef enum {
	LRG_GESTURE_NONE = 0,
	LRG_GESTURE_PENDING,  /* pressed, not yet moved past the threshold */
	LRG_GESTURE_ORBIT,    /* middle-drag: orbit the room */
	LRG_GESTURE_LOOK,     /* Ctrl+left-drag: turn the view in place */
	LRG_GESTURE_GRAB      /* Ctrl/Super+middle-drag: move a tab's panel */
} LrgGesture;

struct _GsurfLrgWindow
{
	GsurfWindow parent_instance;

	GrlWindow *win;             /* owned; created at present() */
	gchar     *title;
	int        default_width;
	int        default_height;
	gboolean   fullscreen;
	guint      tick_source;     /* render-loop GSource id */
	GsurfLrgRenderMode render_mode;

	/* tracked button state so we synthesize press/release edges */
	gboolean   btn_down[3];   /* button is currently held */
	gboolean   page_btn[3];   /* the press was forwarded to the page (so the
	                             matching release must go to the page too) */

	/* chrome state, tracked from the active view's signals */
	gchar     *chrome_url;      /* current URI shown in the address bar */
	gchar     *chrome_hover;    /* hovered link URI (bottom HUD), or NULL */
	gdouble    chrome_progress; /* 0..1 load progress (underline) */

	/* LRG-native address bar (open-prompt under --lrg, since the GTK
	   chromebar/omnibar modules cannot render here) */
	gboolean   url_editing;
	gboolean   url_fresh;   /* pre-filled URI not yet edited: "selected" */
	GString   *url_input;

	/* Chrome UI font (same family as the GTK client; raylib's default font
	   is ugly).  Loaded lazily once the GL context exists. */
	GrlFont   *font;
	gboolean   font_tried;

	/* 3D scene options (copied from gsurf_lrg_window_set_scene_options). */
	gchar     *scene_arrangement;
	gchar     *scene_environment;
	gdouble    scene_background_fps;
	guint      scene_history_limit;
	gboolean   scene_reading_single_tab;
	gboolean   scene_started;     /* first-frame setup done */
	gboolean   scene_auto_reading; /* reading was entered for a lone tab */
	gint64     last_tick_us;

#ifdef GSURF_HAVE_LRG_3D
	GsurfLrgScene *scene;         /* owned; 3D mode only */
#endif

	/* 3D pointer state. */
	LrgGesture gesture;
	int        gesture_btn;       /* index into the button table */
	guint      gesture_mods;
	gfloat     gesture_x0, gesture_y0, gesture_lx, gesture_ly;
	GsurfView *gesture_view;      /* borrowed: the tab under the press */
	GsurfView *page_view;         /* borrowed: the tab a forwarded press went to */
	GsurfView *focused_view;      /* borrowed: the tab holding page focus */
	int        page_x, page_y;    /* last page pixel sent to page_view */
	gint64     last_click_us;
	GsurfView *last_click_view;   /* compared only, never dereferenced */

	/* Plain left clicks, for the double click that reads a tab. */
	gint64     plain_click_us;
	GsurfView *plain_click_view;  /* compared only, never dereferenced */
	gfloat     plain_click_x, plain_click_y;
};

G_DEFINE_FINAL_TYPE(GsurfLrgWindow, gsurf_lrg_window, GSURF_TYPE_WINDOW)

/* GrlKey -> X keysym for non-text keys. */
static guint
lrg_special_keysym(GrlKey key)
{
	switch (key) {
	case GRL_KEY_BACKSPACE:  return LRG_XK_BackSpace;
	case GRL_KEY_TAB:        return LRG_XK_Tab;
	case GRL_KEY_ENTER:      return LRG_XK_Return;
	case GRL_KEY_KP_ENTER:   return LRG_XK_KP_Enter;
	case GRL_KEY_ESCAPE:     return LRG_XK_Escape;
	case GRL_KEY_HOME:       return LRG_XK_Home;
	case GRL_KEY_LEFT:       return LRG_XK_Left;
	case GRL_KEY_UP:         return LRG_XK_Up;
	case GRL_KEY_RIGHT:      return LRG_XK_Right;
	case GRL_KEY_DOWN:       return LRG_XK_Down;
	case GRL_KEY_PAGE_UP:    return LRG_XK_Prior;
	case GRL_KEY_PAGE_DOWN:  return LRG_XK_Next;
	case GRL_KEY_END:        return LRG_XK_End;
	case GRL_KEY_INSERT:     return LRG_XK_Insert;
	case GRL_KEY_DELETE:     return LRG_XK_Delete;
	default:                 return 0;
	}
}

static guint
lrg_current_mods(void)
{
	guint mods = 0;
	if (grl_input_is_key_down(GRL_KEY_LEFT_SHIFT) ||
	    grl_input_is_key_down(GRL_KEY_RIGHT_SHIFT))
		mods |= GSURF_MOD_SHIFT;
	if (grl_input_is_key_down(GRL_KEY_LEFT_CONTROL) ||
	    grl_input_is_key_down(GRL_KEY_RIGHT_CONTROL))
		mods |= GSURF_MOD_CTRL;
	if (grl_input_is_key_down(GRL_KEY_LEFT_ALT) ||
	    grl_input_is_key_down(GRL_KEY_RIGHT_ALT))
		mods |= GSURF_MOD_ALT;
	if (grl_input_is_key_down(GRL_KEY_LEFT_SUPER) ||
	    grl_input_is_key_down(GRL_KEY_RIGHT_SUPER))
		mods |= GSURF_MOD_SUPER;
	return mods;
}

/* Set GSURF_LRG_DEBUG=1 to trace input routing on stderr. */
static gboolean
lrg_dbg(void)
{
	static int d = -1;
	if (d < 0)
		d = (g_getenv("GSURF_LRG_DEBUG") != NULL) ? 1 : 0;
	return d;
}

/* Load the chrome UI font once the GL context exists (raylib's bundled font is
 * a low-res bitmap that looks terrible). Uses the same family as the GTK client
 * (see gsurf_lrg_engine_get_ui_font_path()), loaded at a large base size and
 * bilinear-filtered so downscaled draws stay crisp. Falls back to the raylib
 * default if no font file can be resolved or loaded. Must be called with the
 * GL context current (i.e. inside begin/swap). */
static void
lrg_window_ensure_font(GsurfLrgWindow *self)
{
	const char *path;

	if (self->font_tried)
		return;
	self->font_tried = TRUE;

	path = gsurf_lrg_engine_get_ui_font_path();
	if (path == NULL)
		return;
	self->font = grl_font_new_from_file_ex(path, LRG_FONT_BASE, NULL, 0);
	if (self->font != NULL)
		grl_font_set_filter(self->font, GRL_TEXTURE_FILTER_BILINEAR);
}

/* Draw chrome text with the loaded UI font (fallback: raylib default). */
static void
lrg_window_text(GsurfLrgWindow *self, const char *text, int x, int y, int size,
                const GrlColor *color)
{
	if (text == NULL)
		return;
	if (self->font != NULL) {
		g_autoptr(GrlVector2) pos = grl_vector2_new((gfloat)x, (gfloat)y);
		grl_draw_text_ex(self->font, text, pos, (gfloat)size, 0.0f, color);
	} else {
		grl_draw_text(text, x, y, size, color);
	}
}

/* Width of chrome text in the loaded UI font (fallback: raylib default). */
static int
lrg_window_text_width(GsurfLrgWindow *self, const char *text, int size)
{
	if (text == NULL)
		return 0;
	if (self->font != NULL) {
		g_autoptr(GrlVector2) m =
			grl_font_measure_text(self->font, text, (gfloat)size, 0.0f);
		return (int)(grl_vector2_get_x(m) + 0.5f);
	}
	return grl_measure_text(text, size);
}

static GsurfLrgView *
lrg_window_active_lrg_view(GsurfLrgWindow *self)
{
	GsurfView *v = gsurf_window_get_active_view(GSURF_WINDOW(self));
	if (v != NULL && GSURF_IS_LRG_VIEW(v))
		return GSURF_LRG_VIEW(v);
	return NULL;
}

/* Map window coords to page coords (page fills the window in 2D mode). */
static void
lrg_window_to_page(GsurfLrgWindow *self, GsurfLrgView *view, int wx, int wy,
                   int *px, int *py)
{
	int ww = grl_window_get_width(self->win);
	int wh = grl_window_get_height(self->win) - LRG_CHROME_H;  /* body height */
	int by = wy - LRG_CHROME_H;                                 /* body-local y */
	int tw = 0, th = 0;

	gsurf_lrg_view_get_texture(view, &tw, &th);
	if (tw <= 0 || th <= 0 || ww <= 0 || wh <= 0) {
		*px = wx;
		*py = (by > 0) ? by : 0;
		return;
	}
	if (by < 0) by = 0;
	*px = (int)((gint64)wx * tw / ww);
	*py = (int)((gint64)by * th / wh);
}

/* Open or close the LRG address editor. While it is open every key goes
 * to the URL field: the window is typing (the published mode reads
 * INSERT), as with a focused GTK address bar. */
static void
lrg_set_url_editing(GsurfLrgWindow *self, gboolean editing)
{
	self->url_editing = editing;
	gsurf_window_set_entry_focus(GSURF_WINDOW(self), editing);
}

/* Load the typed address (URI-handler modules rewrite it -> search etc.). */
static void
lrg_window_url_commit(GsurfLrgWindow *self)
{
	GsurfView *v = gsurf_window_get_active_view(GSURF_WINDOW(self));
	const char *input = self->url_input ? self->url_input->str : NULL;

	if (v != NULL && input != NULL && *input != '\0') {
		GsurfModuleManager *mgr = gsurf_module_manager_get_default();
		gchar *rewritten = gsurf_module_manager_dispatch_rewrite_uri(mgr, input);
		gsurf_view_load_uri(v, rewritten ? rewritten : input);
		g_free(rewritten);
	}
	lrg_set_url_editing(self, FALSE);
}

/* Type a character into the address field. The pre-filled URI acts as
 * selected text (as in a GTK entry): the first keystroke replaces it. */
static void
lrg_url_type(GsurfLrgWindow *self, gint ch)
{
	if (ch < 0x20)
		return;
	if (self->url_fresh) {
		g_string_truncate(self->url_input, 0);
		self->url_fresh = FALSE;
	}
	g_string_append_unichar(self->url_input, (gunichar)ch);
}

/* Delete the last UTF-8 character from the address field (all of it while
 * the pre-filled URI is still "selected"). */
static void
lrg_url_backspace(GsurfLrgWindow *self)
{
	glong n;

	if (self->url_input == NULL || self->url_input->len == 0)
		return;
	if (self->url_fresh) {
		g_string_truncate(self->url_input, 0);
		self->url_fresh = FALSE;
		return;
	}
	n = g_utf8_strlen(self->url_input->str, -1);
	if (n > 0) {
		gchar *p = g_utf8_offset_to_pointer(self->url_input->str, n - 1);
		g_string_truncate(self->url_input, (gsize)(p - self->url_input->str));
	}
}

/* Map a non-text GrlKey to a keysym and route it: modules/keybinds first, then
 * the focused page.  Shared by the initial-press queue and the auto-repeat
 * pass.  Returns TRUE if the key produced a keysym worth dispatching. */
static gboolean
lrg_page_dispatch_special(GsurfLrgWindow *self, GsurfLrgView *view,
                          GrlKey key, guint mods)
{
	GsurfWindow *win = GSURF_WINDOW(self);
	guint keysym = lrg_special_keysym(key);
	gboolean printable = (key >= GRL_KEY_SPACE && key <= GRL_KEY_GRAVE);
	gboolean kcm = (mods & (GSURF_MOD_CTRL | GSURF_MOD_ALT | GSURF_MOD_SUPER)) != 0;
	guint kv = 0;

	if (keysym != 0)
		kv = keysym;
	else if (printable && kcm)
		kv = (key >= GRL_KEY_A && key <= GRL_KEY_Z)
		   ? (guint)key + 32 : (guint)key;
	if (lrg_dbg())
		g_printerr("[lrg-input] KEY grl=%d keysym=0x%x mods=0x%x kv=0x%x\n",
		           (int)key, keysym, mods, kv);
	if (kv == 0)
		return FALSE;
	if (!gsurf_window_emit_key_press(win, kv, 0, mods)) {
		gsurf_lrg_view_send_key(view, kv, 0, mods, TRUE);
		gsurf_lrg_view_send_key(view, kv, 0, mods, FALSE);
	}
	return TRUE;
}

/* Non-text keys a user expects to auto-repeat while held (editing/navigation).
 * raylib's get_key_pressed() queue only fires on the initial press, so held
 * repeats are picked up via is_key_pressed_repeat(). */
static const GrlKey lrg_repeat_keys[] = {
	GRL_KEY_BACKSPACE, GRL_KEY_DELETE, GRL_KEY_LEFT, GRL_KEY_RIGHT,
	GRL_KEY_UP, GRL_KEY_DOWN, GRL_KEY_PAGE_UP, GRL_KEY_PAGE_DOWN,
};

#ifdef GSURF_HAVE_LRG_3D
static void lrg_window_forward_pointer_3d(GsurfLrgWindow *self, guint mods,
                                          int wx, int wy);
#endif

static void
lrg_window_forward_input(GsurfLrgWindow *self, GsurfLrgView *view,
                         gboolean spatial)
{
	GsurfWindow *win = GSURF_WINDOW(self);
	guint mods = lrg_current_mods();
	int wx = grl_input_get_mouse_x();
	int wy = grl_input_get_mouse_y();
	int px, py;
	gfloat wheel;
	GrlKey key;
	gint ch;
	static const struct { GrlMouseButton grl; guint gsurf; int idx; } buttons[] = {
		{ GRL_MOUSE_BUTTON_LEFT,   GSURF_BUTTON_PRIMARY,   0 },
		{ GRL_MOUSE_BUTTON_RIGHT,  GSURF_BUTTON_SECONDARY, 1 },
		{ GRL_MOUSE_BUTTON_MIDDLE, GSURF_BUTTON_MIDDLE,    2 },
	};
	guint i;
	/* Pointer events only reach the page in the body area (below the address
	 * bar); the chrome is the window's own UI. */
	gboolean in_body = wy >= LRG_CHROME_H;

	if (lrg_dbg()) {
		static int fc = 0;
		if ((fc++ % 60) == 0)
			g_printerr("[lrg-input] tick: mouse(%d,%d) lbtn=%d in_body=%d "
			           "url_edit=%d focus=%d\n", wx, wy,
			           grl_input_is_mouse_button_down(GRL_MOUSE_BUTTON_LEFT),
			           in_body, self->url_editing,
			           grl_window_is_focused(self->win));
	}

	/* --- LRG address bar: while editing, all keys go to the URL field --- */
	if (self->url_editing) {
		while ((ch = grl_input_get_char_pressed()) != 0)
			lrg_url_type(self, ch);
		while ((key = grl_input_get_key_pressed()) != 0) {
			if (key == GRL_KEY_ENTER || key == GRL_KEY_KP_ENTER)
				lrg_window_url_commit(self);
			else if (key == GRL_KEY_ESCAPE)
				lrg_set_url_editing(self, FALSE);
			else if (key == GRL_KEY_BACKSPACE)
				lrg_url_backspace(self);
		}
		/* Auto-repeat held backspace (the press queue doesn't repeat). */
		if (self->url_editing
		    && grl_input_is_key_pressed_repeat(GRL_KEY_BACKSPACE))
			lrg_url_backspace(self);
		return;
	}

#ifdef GSURF_HAVE_LRG_3D
	/* 3D: the pointer is ray-cast onto the tab panels instead. */
	if (spatial) {
		lrg_window_forward_pointer_3d(self, mods, wx, wy);
		goto keys;
	}
#else
	(void)spatial;
#endif

	lrg_window_to_page(self, view, wx, wy, &px, &py);

	/* Motion -> page hover (no module dispatch for motion). */
	if (in_body)
		gsurf_lrg_view_send_motion(view, px, py, mods);

	/* Buttons.  A press on the chrome (address bar) opens the URL editor; a
	 * press in the body goes to gsurf modules + mousebinds first and, if
	 * unconsumed, to the page (so links work).  The matching release follows
	 * the press: only forwarded to the page if the press was. */
	for (i = 0; i < G_N_ELEMENTS(buttons); i++) {
		gboolean raw = grl_input_is_mouse_button_down(buttons[i].grl);
		if (raw && !self->btn_down[buttons[i].idx]) {
			self->btn_down[buttons[i].idx] = TRUE;
			self->page_btn[buttons[i].idx] = FALSE;
			if (!in_body) {
				/* Click the address bar -> edit the URL (like clicking a
				 * browser's location bar). */
				if (buttons[i].grl == GRL_MOUSE_BUTTON_LEFT)
					gsurf_lrg_window_begin_url_edit(self);
			} else {
				gboolean consumed =
					gsurf_window_emit_button_press(win, buttons[i].gsurf, mods);
				if (lrg_dbg())
					g_printerr("[lrg-input] BTN%u press win(%d,%d) page(%d,%d) "
					           "module_consumed=%d -> %s\n", buttons[i].gsurf,
					           wx, wy, px, py, consumed,
					           consumed ? "module" : "page");
				if (!consumed) {
					self->page_btn[buttons[i].idx] = TRUE;
					gsurf_lrg_view_send_button(view, px, py, buttons[i].gsurf,
					                           TRUE, mods);
				}
			}
		} else if (!raw && self->btn_down[buttons[i].idx]) {
			self->btn_down[buttons[i].idx] = FALSE;
			if (self->page_btn[buttons[i].idx]) {
				gsurf_lrg_view_send_button(view, px, py, buttons[i].gsurf,
				                           FALSE, mods);
				self->page_btn[buttons[i].idx] = FALSE;
			}
		}
	}

	/* Wheel: positive grl wheel = up, which is a negative scroll delta.  Pass
	 * raw wheel ticks; the engine converts each tick to one discrete scroll
	 * step (a px multiplier here over-scrolled badly). */
	wheel = grl_input_get_mouse_wheel_move();
	/* The window sees the notch first (wheel buttons 4-7), so a mousebind
	 * consumes it: Ctrl+wheel zooms the page by default. */
	if (in_body && wheel != 0.0f &&
	    !gsurf_window_emit_wheel(GSURF_WINDOW(self), GSURF_VIEW(view), 0.0, -wheel, mods))
		gsurf_lrg_view_send_axis(view, px, py, 0.0, -wheel, mods);

#ifdef GSURF_HAVE_LRG_3D
keys:
#endif
	/* Text keys: modules/keybinds first (modal `f`, etc.); the keymap layer
	 * (main.c on_key_press) returns FALSE while the page is editing so bare
	 * keys then reach the focused form field. */
	while ((ch = grl_input_get_char_pressed()) != 0) {
		gboolean consumed;

		/* A key earlier in this frame (`o`) opened the address bar: the
		 * rest of the frame's typing belongs to it, not to the page. */
		if (self->url_editing) {
			lrg_url_type(self, ch);
			continue;
		}
		consumed = gsurf_window_emit_key_press(win, (guint)ch, 0, mods);
		if (lrg_dbg())
			g_printerr("[lrg-input] CHAR %d ('%c') module/keybind_consumed=%d "
			           "-> %s\n", ch, (ch >= 32 && ch < 127) ? ch : '?',
			           consumed, consumed ? "module" : "page");
		if (!consumed) {
			guint keysym = (ch <= 0xFF) ? (guint)ch : (0x01000000u | (guint)ch);
			gsurf_lrg_view_send_key(view, keysym, 0, mods, TRUE);
			gsurf_lrg_view_send_key(view, keysym, 0, mods, FALSE);
		}
	}

	/* The address bar just opened: its own branch handles keys next tick. */
	if (self->url_editing)
		return;

	/* Non-text keys + Ctrl/Alt-modified keys (no char from GLFW for those). */
	while ((key = grl_input_get_key_pressed()) != 0)
		lrg_page_dispatch_special(self, view, key, mods);

	/* Auto-repeat held editing/navigation keys (backspace, arrows, etc.): the
	 * get_key_pressed() queue only fires on the initial press. */
	for (i = 0; i < G_N_ELEMENTS(lrg_repeat_keys); i++)
		if (grl_input_is_key_pressed_repeat(lrg_repeat_keys[i]))
			lrg_page_dispatch_special(self, view, lrg_repeat_keys[i], mods);

	/* Auto-repeat held Ctrl/Alt/Super-modified printable keys (e.g. C-e/C-y
	 * line-scroll keybinds): GLFW emits no char for those, so they arrive via
	 * the (non-repeating) key queue.  Re-dispatch any that are repeating while
	 * a modifier is held.  (Bare printable keys auto-repeat via the char
	 * queue, so they are excluded here.) */
	if (mods & (GSURF_MOD_CTRL | GSURF_MOD_ALT | GSURF_MOD_SUPER)) {
		GrlKey k;
		for (k = GRL_KEY_SPACE; k <= GRL_KEY_GRAVE; k++)
			if (grl_input_is_key_pressed_repeat(k))
				lrg_page_dispatch_special(self, view, k, mods);
	}
}

/* The window's own 2D chrome: address bar (or the URL editor), load
 * progress and the hover-URL readout.  Drawn over the page in 2D and as the
 * HUD over the scene in 3D.  Needs the GL context current. */
static void
lrg_window_draw_chrome(GsurfLrgWindow *self, int ww, int wh)
{
	lrg_window_ensure_font(self);

	/* Chrome is omitted entirely in kiosk mode, including hover URLs. */
	if (!gsurf_kiosk_is_enabled()) {
		g_autoptr(GrlColor) fg  = grl_color_new(220, 220, 225, 255);
		g_autoptr(GrlColor) accent = grl_color_new(90, 160, 250, 255);
		int ty = (LRG_CHROME_H - LRG_CHROME_FONT) / 2;

		if (self->url_editing) {
			g_autoptr(GrlColor) ebar = grl_color_new(28, 34, 52, 255);
			g_autofree gchar *line =
				g_strdup_printf("Open: %s_",
				                self->url_input ? self->url_input->str : "");
			grl_draw_rectangle(0, 0, ww, LRG_CHROME_H, ebar);
			grl_draw_rectangle(0, LRG_CHROME_H - 2, ww, 2, accent);
			/* Highlight the pre-filled URI while typing would replace it. */
			if (self->url_fresh) {
				g_autoptr(GrlColor) sel = grl_color_new(52, 78, 128, 255);
				int px0 = 8 + lrg_window_text_width(self, "Open: ", LRG_CHROME_FONT);
				int sw = lrg_window_text_width(self, self->url_input->str,
				                               LRG_CHROME_FONT);

				grl_draw_rectangle(px0, ty - 2, sw, LRG_CHROME_FONT + 4, sel);
			}
			lrg_window_text(self, line, 8, ty, LRG_CHROME_FONT, fg);
		} else {
			g_autoptr(GrlColor) bar = grl_color_new(40, 42, 48, 255);
			grl_draw_rectangle(0, 0, ww, LRG_CHROME_H, bar);
			lrg_window_text(self, self->chrome_url ? self->chrome_url : "",
			                8, ty, LRG_CHROME_FONT, fg);
			if (self->chrome_progress > 0.0 && self->chrome_progress < 1.0)
				grl_draw_rectangle(0, LRG_CHROME_H - 2,
				                   (int)(ww * self->chrome_progress), 2, accent);

			if (self->chrome_hover != NULL) {
				int tw = lrg_window_text_width(self, self->chrome_hover, 13) + 12;
				g_autoptr(GrlColor) hbg = grl_color_new(20, 20, 24, 230);
				grl_draw_rectangle(0, wh - 20, tw, 20, hbg);
				lrg_window_text(self, self->chrome_hover, 6, wh - 18, 13, fg);
			}
		}
	}
}

#ifdef GSURF_HAVE_LRG_3D
/* --- 3D pointer ------------------------------------------------------------ */

/* Forward a press or release to @view at a page pixel, remembering where a
 * press went so its release follows it. */
static void
lrg3d_page_button(GsurfLrgWindow *self, GsurfView *view, int x, int y,
                  guint button, gboolean pressed, guint mods)
{
	if (view == NULL || !GSURF_IS_LRG_VIEW(view))
		return;
	gsurf_lrg_view_send_button(GSURF_LRG_VIEW(view), x, y, button, pressed, mods);
	self->page_view = pressed ? view : NULL;
	self->page_x = x;
	self->page_y = y;
}

/* Read @view: make it the active tab and fly head-on to it, full-frame;
 * once the camera settles the flat path draws it pixel-exact. */
static void
lrg3d_read_view(GsurfLrgWindow *self, GsurfView *view)
{
	gsurf_window_set_active_view(GSURF_WINDOW(self), view);
	gsurf_lrg_scene_note_active(self->scene, view);
	gsurf_lrg_scene_set_reading(self->scene, TRUE);
	gsurf_lrg_window_notify_scene(self, "reading");
}

/* A press that ended without moving: the click actions of the 3D gestures. */
static void
lrg3d_gesture_click(GsurfLrgWindow *self, guint button, GsurfView *view,
                    int px, int py)
{
	GsurfWindow *win = GSURF_WINDOW(self);
	gint64 now = g_get_monotonic_time();
	gboolean ctrl = (self->gesture_mods & GSURF_MOD_CTRL) != 0;
	gboolean twice = (view != NULL && view == self->last_click_view
	                  && now - self->last_click_us < LRG_3D_DOUBLE_US);

	self->last_click_us = now;
	self->last_click_view = view;
	if (view == NULL)
		return;

	if (button == GSURF_BUTTON_PRIMARY && ctrl) {
		/* Ctrl+click: fly to the tab and make it active; Ctrl+double-click:
		 * read it (head-on, then pixel-exact). */
		if (twice) {
			lrg3d_read_view(self, view);
		} else {
			gsurf_window_set_active_view(win, view);
			gsurf_lrg_scene_focus_view(self->scene, view);
			gsurf_lrg_window_notify_scene(self, "focus");
		}
	} else if (button == GSURF_BUTTON_MIDDLE && !ctrl
	           && (self->gesture_mods & GSURF_MOD_SUPER) == 0) {
		/* A plain middle click (open link in a new tab, paste) was held back
		 * by the drag check: deliver it now, to mousebinds/modules first
		 * (Button2: open-new-view-background) and then to the page. */
		if (gsurf_window_emit_button_press(win, GSURF_BUTTON_MIDDLE,
		                                   self->gesture_mods))
			return;
		lrg3d_page_button(self, view, px, py, GSURF_BUTTON_MIDDLE, TRUE, 0);
		lrg3d_page_button(self, view, px, py, GSURF_BUTTON_MIDDLE, FALSE, 0);
	}
}

/*
 * Route the pointer in the 3D scene:
 * - motion, plain clicks and the wheel go to the tab under the pointer at the
 *   page pixel the ray hits (clicking a background tab first activates it);
 * - clicking a history thumbnail jumps there;
 * - middle-drag orbits the room, Ctrl+left-drag looks around, Ctrl/Super+
 *   middle-drag moves (and pins) a panel, Ctrl+wheel dollies;
 * - the address bar keeps its 2D behaviour.
 */
static void
lrg_window_forward_pointer_3d(GsurfLrgWindow *self, guint mods, int wx, int wy)
{
	static const struct { GrlMouseButton grl; guint gsurf; } buttons[] = {
		{ GRL_MOUSE_BUTTON_LEFT,   GSURF_BUTTON_PRIMARY },
		{ GRL_MOUSE_BUTTON_RIGHT,  GSURF_BUTTON_SECONDARY },
		{ GRL_MOUSE_BUTTON_MIDDLE, GSURF_BUTTON_MIDDLE },
	};
	GsurfWindow *win = GSURF_WINDOW(self);
	GsurfView *active = gsurf_window_get_active_view(win);
	GsurfLrgPick pick;
	gboolean in_body = wy >= LRG_CHROME_H;
	gboolean hit = FALSE;
	gfloat mx = (gfloat)wx, my = (gfloat)wy, wheel;
	guint i;

	if (in_body)
		hit = gsurf_lrg_scene_pick(self->scene, mx, my, &pick);

	/* 1. Drags in progress. */
	if (self->gesture != LRG_GESTURE_NONE) {
		gfloat dx = mx - self->gesture_lx, dy = my - self->gesture_ly;

		if (self->gesture == LRG_GESTURE_PENDING
		    && fabsf(mx - self->gesture_x0) + fabsf(my - self->gesture_y0)
		       > LRG_3D_DRAG_THRESHOLD) {
			gboolean ctrl = (self->gesture_mods & GSURF_MOD_CTRL) != 0;
			gboolean super = (self->gesture_mods & GSURF_MOD_SUPER) != 0;

			if (buttons[self->gesture_btn].gsurf == GSURF_BUTTON_PRIMARY)
				self->gesture = LRG_GESTURE_LOOK;
			else if ((ctrl || super) && self->gesture_view != NULL
			         && gsurf_lrg_scene_grab(self->scene, self->gesture_view,
			                                 self->gesture_x0, self->gesture_y0))
				self->gesture = LRG_GESTURE_GRAB;
			else
				self->gesture = LRG_GESTURE_ORBIT;
		}
		switch (self->gesture) {
		case LRG_GESTURE_ORBIT:
			gsurf_lrg_scene_orbit(self->scene, -dx * LRG_3D_ORBIT_DEG_PX,
			                      dy * LRG_3D_ORBIT_DEG_PX, FALSE);
			break;
		case LRG_GESTURE_LOOK:
			gsurf_lrg_scene_look(self->scene, -dx * LRG_3D_LOOK_DEG_PX,
			                     -dy * LRG_3D_LOOK_DEG_PX);
			break;
		case LRG_GESTURE_GRAB:
			gsurf_lrg_scene_drag(self->scene, mx, my);
			break;
		case LRG_GESTURE_NONE:
		case LRG_GESTURE_PENDING:
		default:
			break;
		}
		self->gesture_lx = mx;
		self->gesture_ly = my;
	} else if (hit && pick.kind == GSURF_LRG_PICK_TAB) {
		/* 2. Hover goes to the tab under the pointer. */
		gsurf_lrg_view_send_motion(GSURF_LRG_VIEW(pick.view), pick.x, pick.y,
		                           mods);
	}

	/* 3. Button edges. */
	for (i = 0; i < G_N_ELEMENTS(buttons); i++) {
		gboolean raw = grl_input_is_mouse_button_down(buttons[i].grl);
		guint b = buttons[i].gsurf;

		if (raw && !self->btn_down[i]) {
			gboolean nav;

			self->btn_down[i] = TRUE;
			self->page_btn[i] = FALSE;

			if (!in_body) {
				if (b == GSURF_BUTTON_PRIMARY)
					gsurf_lrg_window_begin_url_edit(self);
				continue;
			}

			/* Navigation gestures: any middle press, or Ctrl/Super + left. */
			nav = (b == GSURF_BUTTON_MIDDLE)
			   || (b == GSURF_BUTTON_PRIMARY
			       && (mods & (GSURF_MOD_CTRL | GSURF_MOD_SUPER)) != 0);
			if (nav && self->gesture == LRG_GESTURE_NONE) {
				self->gesture = LRG_GESTURE_PENDING;
				self->gesture_btn = (int)i;
				self->gesture_mods = mods;
				self->gesture_x0 = self->gesture_lx = mx;
				self->gesture_y0 = self->gesture_ly = my;
				self->gesture_view = (hit && pick.kind == GSURF_LRG_PICK_TAB)
				                   ? pick.view : NULL;
				continue;
			}

			/* Modules and mousebinds see the press before the page. */
			if (gsurf_window_emit_button_press(win, b, mods))
				continue;

			if (!hit)
				continue;
			if (pick.kind == GSURF_LRG_PICK_HISTORY) {
				if (b == GSURF_BUTTON_PRIMARY && active != NULL
				    && GSURF_IS_LRG_VIEW(active)) {
					gsurf_lrg_view_go_history(GSURF_LRG_VIEW(active),
					                          pick.history_offset);
					gsurf_lrg_scene_set_history(self->scene, FALSE);
					gsurf_lrg_window_notify_scene(self, "history");
				}
				continue;
			}
			/* A double click on a panel reads that tab: it becomes the
			 * active one and the camera re-snaps head-on, full-frame, then
			 * hands over to the pixel-exact flat view.  (The first click
			 * already did its usual thing; the second is not forwarded.) */
			if (b == GSURF_BUTTON_PRIMARY) {
				gint64 now = g_get_monotonic_time();
				gboolean twice = gsurf_lrg_scene_is_double_click(
					self->plain_click_view, self->plain_click_us,
					self->plain_click_x, self->plain_click_y,
					pick.view, now, mx, my);

				if (twice) {
					self->plain_click_view = NULL;
					lrg3d_read_view(self, pick.view);
					continue;
				}
				self->plain_click_us = now;
				self->plain_click_view = pick.view;
				self->plain_click_x = mx;
				self->plain_click_y = my;
			}
			/* A background tab: the first click brings it forward. */
			if (pick.view != active && b == GSURF_BUTTON_PRIMARY) {
				gsurf_window_set_active_view(win, pick.view);
				gsurf_lrg_window_notify_scene(self, "focus");
				continue;
			}
			self->page_btn[i] = TRUE;
			lrg3d_page_button(self, pick.view, pick.x, pick.y, b, TRUE, mods);
		} else if (!raw && self->btn_down[i]) {
			self->btn_down[i] = FALSE;

			if (self->gesture != LRG_GESTURE_NONE
			    && self->gesture_btn == (int)i) {
				if (self->gesture == LRG_GESTURE_PENDING)
					lrg3d_gesture_click(self, b, self->gesture_view,
						(hit && pick.kind == GSURF_LRG_PICK_TAB) ? pick.x : 0,
						(hit && pick.kind == GSURF_LRG_PICK_TAB) ? pick.y : 0);
				else if (self->gesture == LRG_GESTURE_GRAB) {
					gsurf_lrg_scene_release(self->scene);
					gsurf_lrg_window_notify_scene(self, "pin");
				} else
					gsurf_lrg_window_notify_scene(self, "camera");
				self->gesture = LRG_GESTURE_NONE;
				self->gesture_view = NULL;
				continue;
			}
			/* The release follows its press to the same tab; at the pixel
			 * under the pointer if it is still over that tab. */
			if (self->page_btn[i] && self->page_view != NULL) {
				gboolean same = hit && pick.kind == GSURF_LRG_PICK_TAB
				             && pick.view == self->page_view;

				gsurf_lrg_view_send_button(GSURF_LRG_VIEW(self->page_view),
					same ? pick.x : self->page_x, same ? pick.y : self->page_y,
					b, FALSE, mods);
				self->page_view = NULL;
			}
			self->page_btn[i] = FALSE;
		}
	}

	/* 4. Wheel: Ctrl dollies the camera, otherwise the tab under the pointer
	 *    scrolls (a background tab too). */
	wheel = grl_input_get_mouse_wheel_move();
	if (wheel != 0.0f && in_body) {
		if (mods & GSURF_MOD_CTRL)
			gsurf_lrg_scene_zoom(self->scene,
			                     powf(LRG_3D_DOLLY_STEP, wheel), FALSE);
		else if (hit && pick.kind == GSURF_LRG_PICK_TAB)
			gsurf_lrg_view_send_axis(GSURF_LRG_VIEW(pick.view), pick.x, pick.y,
			                         0.0, -wheel, mods);
	}
}

/* The scene's HUD: the usual chrome plus a 3D status line (bottom right). */
static void
lrg3d_draw_hud(GsurfLrgScene *scene, gpointer user_data)
{
	GsurfLrgWindow *self = user_data;
	int ww = grl_window_get_width(self->win);
	int wh = grl_window_get_height(self->win);

	lrg_window_draw_chrome(self, ww, wh);

	if (!gsurf_kiosk_is_enabled()) {
		g_autoptr(GrlColor) bg = grl_color_new(20, 20, 24, 200);
		g_autoptr(GrlColor) fg = grl_color_new(170, 176, 190, 255);
		/* ASCII only: the chrome font is loaded with the default glyph set. */
		g_autofree gchar *status = g_strdup_printf("3D  %s | %s%s",
			gsurf_lrg_scene_get_history(scene) ? "history"
			                                   : gsurf_lrg_scene_get_arrangement(scene),
			gsurf_lrg_scene_get_environment(scene),
			gsurf_lrg_scene_get_reading(scene) ? " | reading" : "");
		int tw = lrg_window_text_width(self, status, 13) + 12;

		grl_draw_rectangle(ww - tw, wh - 20, tw, 20, bg);
		lrg_window_text(self, status, ww - tw + 6, wh - 18, 13, fg);
	}
}
#endif /* GSURF_HAVE_LRG_3D */

static gboolean
lrg_window_tick(gpointer user_data)
{
	GsurfLrgWindow *self = user_data;
	GsurfLrgView *view;
	g_autoptr(GrlColor) bg = grl_color_new(24, 24, 24, 255);

	if (self->win == NULL)
		return G_SOURCE_REMOVE;

	/* Update raylib input state for THIS frame (mirrors cmacs lrgterm, which
	 * polls explicitly and swaps without EndDrawing -- EndDrawing's WaitTime
	 * would block the GLib main loop and its poll timing differs). */
	grl_window_poll_events(self->win);

	if (grl_window_should_close(self->win)) {
		self->tick_source = 0;
		gsurf_window_emit_close_request(GSURF_WINDOW(self));
		return G_SOURCE_REMOVE;
	}

	{
		int ww = grl_window_get_width(self->win);
		int wh = grl_window_get_height(self->win);
		int body_h = wh - LRG_CHROME_H;

		view = lrg_window_active_lrg_view(self);

#ifdef GSURF_HAVE_LRG_3D
		if (self->scene != NULL) {
			/* Seconds since the previous frame, for the scene's easing. */
			gint64 now = g_get_monotonic_time();
			gfloat dt = self->last_tick_us > 0
				? (gfloat)(now - self->last_tick_us) / (gfloat)G_USEC_PER_SEC
				: (1.0f / 60.0f);

			self->last_tick_us = now;

			/* First frame with tabs: open a lone tab in reading mode. */
			if (!self->scene_started
			    && gsurf_window_get_n_views(GSURF_WINDOW(self)) > 0) {
				self->scene_started = TRUE;
				if (self->scene_reading_single_tab
				    && gsurf_window_get_n_views(GSURF_WINDOW(self)) == 1) {
					gsurf_lrg_scene_set_reading(self->scene, TRUE);
					self->scene_auto_reading = TRUE;
					gsurf_lrg_window_notify_scene(self, "reading");
				}
			}
			/* That automatic reading mode lasts while the tab is alone: a
			 * second tab brings the space back (a reading mode the user
			 * chose stays until they leave it). */
			if (self->scene_auto_reading
			    && gsurf_window_get_n_views(GSURF_WINDOW(self)) > 1) {
				self->scene_auto_reading = FALSE;
				if (gsurf_lrg_scene_get_reading(self->scene)) {
					/* There was no camera before this reading mode worth
					 * returning to: aim at the new layout instead. */
					gsurf_lrg_scene_set_reading(self->scene, FALSE);
					gsurf_lrg_scene_reframe(self->scene);
					gsurf_lrg_window_notify_scene(self, "reading");
				}
			}
			if (!gsurf_lrg_scene_get_reading(self->scene))
				self->scene_auto_reading = FALSE;

			/* The spatial frame, unless reading mode has settled on the
			 * active tab: then the flat path below draws it pixel-exact.
			 * (A tab switch while flat flies to the new tab first.) */
			gsurf_lrg_scene_note_active(self->scene,
				gsurf_window_get_active_view(GSURF_WINDOW(self)));
			if (!gsurf_lrg_scene_wants_flat(self->scene)) {
				if (view != NULL)
					lrg_window_forward_input(self, view, TRUE);
				gsurf_lrg_scene_frame(self->scene,
					gsurf_window_get_views(GSURF_WINDOW(self)),
					gsurf_window_get_active_view(GSURF_WINDOW(self)),
					ww, MAX(body_h, 1), MIN(dt, 0.1f));
				return G_SOURCE_CONTINUE;
			}
		}
#endif

		/* Keep the page laid out at the body size (below the address bar). */
		if (view != NULL && body_h > 0)
			gsurf_lrg_view_resize(view, ww, body_h, 1.0);

		if (view != NULL)
			lrg_window_forward_input(self, view, FALSE);

		grl_window_begin_drawing(self->win);
		grl_window_clear_background(self->win, bg);

		/* Page texture, drawn into the body rect (below the chrome). */
		if (view != NULL && body_h > 0) {
			int tw = 0, th = 0;
			GrlTexture *tex;

			gsurf_lrg_view_capture(view);
			tex = gsurf_lrg_view_get_texture(view, &tw, &th);
#ifdef GSURF_HAVE_LRG_3D
			/* Reading flat: keep the history thumbnails current. */
			if (self->scene != NULL)
				gsurf_lrg_scene_note_flat_frame(self->scene, GSURF_VIEW(view));
#endif
			if (tex != NULL && tw > 0 && th > 0) {
				g_autoptr(GrlRectangle) src = grl_rectangle_new(0.0f, 0.0f,
					(gfloat)tw, (gfloat)th);
				g_autoptr(GrlRectangle) dst = grl_rectangle_new(0.0f,
					(gfloat)LRG_CHROME_H, (gfloat)ww, (gfloat)body_h);
				g_autoptr(GrlVector2) origin = grl_vector2_new(0.0f, 0.0f);
				g_autoptr(GrlColor) white = grl_color_new(255, 255, 255, 255);

				grl_draw_texture_pro(tex, src, dst, origin, 0.0f, white);
			}
		}

		lrg_window_draw_chrome(self, ww, wh);
#ifdef GSURF_HAVE_LRG_3D
		/* Reading flat in 3D mode: screenshots come from this frame. */
		if (self->scene != NULL)
			gsurf_lrg_scene_flush_screenshots(self->scene);
#endif

		grl_window_swap_buffers(self->win);
	}
	return G_SOURCE_CONTINUE;
}

/* --- chrome state (tracked from the active view's signals) --- */

/* Every tab is connected, but the chrome shows the active one: a background
 * tab loading (or a link hovered in a 3D background panel) must not repaint
 * the address bar. */
static gboolean
lrg_chrome_is_active(GsurfLrgWindow *self, GsurfView *view)
{
	return view == gsurf_window_get_active_view(GSURF_WINDOW(self));
}

static void
on_chrome_uri(GsurfView *view, const gchar *uri, gpointer user)
{
	GsurfLrgWindow *self = user;
	if (!lrg_chrome_is_active(self, view))
		return;
	g_free(self->chrome_url);
	self->chrome_url = g_strdup(uri ? uri : "");
}

static void
on_chrome_hover(GsurfView *view, const gchar *uri, gpointer user)
{
	GsurfLrgWindow *self = user;
	if (!lrg_chrome_is_active(self, view))
		return;
	g_clear_pointer(&self->chrome_hover, g_free);
	if (uri != NULL && *uri != '\0')
		self->chrome_hover = g_strdup(uri);
}

static void
on_chrome_progress(GsurfView *view, gdouble progress, gpointer user)
{
	GsurfLrgWindow *self = user;
	if (!lrg_chrome_is_active(self, view))
		return;
	self->chrome_progress = progress;
}

static void
on_chrome_title(GsurfView *view, const gchar *title, gpointer user)
{
	GsurfLrgWindow *self = user;
	g_autofree gchar *full = NULL;
	if (!lrg_chrome_is_active(self, view))
		return;
	full = g_strdup_printf("%s%sgsurf", (title && *title) ? title : "",
	                       (title && *title) ? " \xe2\x80\x94 " : "");
	gsurf_window_set_title(GSURF_WINDOW(self), full);
}

/* --- GsurfWindow vfuncs --- */

/* The base class calls realize from constructed(), before the host has set
 * the default size or the render mode -- so the raylib window is NOT made
 * here (it would open at the built-in size, and a 3D window would already be
 * a 2D one).  present() makes it, via gsurf_lrg_window_realize(). */
static void
gsurf_lrg_window_realize_deferred(GsurfWindow *window)
{
	(void)window;
}

static void
gsurf_lrg_window_realize(GsurfWindow *window)
{
	GsurfLrgWindow *self = GSURF_LRG_WINDOW(window);

	if (self->win != NULL)
		return;

#ifdef GSURF_HAVE_LRG_3D
	/* 3D: the scene's surface owns the raylib window (one per process);
	 * borrow it for the chrome and input. */
	if (self->render_mode == GSURF_LRG_RENDER_MODE_3D) {
		self->scene = gsurf_lrg_scene_new(self->default_width,
		                                  self->default_height,
		                                  self->title ? self->title : "gsurf",
		                                  self->scene_arrangement,
		                                  self->scene_environment);
		if (self->scene_background_fps > 0.0)
			gsurf_lrg_scene_set_background_fps(self->scene,
			                                   self->scene_background_fps);
		if (self->scene_history_limit > 0)
			gsurf_lrg_scene_set_history_limit(self->scene,
			                                  self->scene_history_limit);
		gsurf_lrg_scene_set_hud_func(self->scene, lrg3d_draw_hud, self, NULL);
		if (gsurf_lrg_scene_get_window(self->scene) != NULL)
			self->win = g_object_ref(gsurf_lrg_scene_get_window(self->scene));
	} else
#endif
	self->win = grl_window_new(self->default_width, self->default_height,
	                           self->title ? self->title : "gsurf");
	if (self->win == NULL) {
		g_warning("gsurf LRG: failed to create a raylib window (no display?)");
		return;
	}
	grl_window_set_target_fps(self->win, 60);
	/* Let the user resize the window (the page + chrome relayout each frame
	 * from grl_window_get_width/height, so this just works). */
	grl_window_set_state(self->win, GRL_FLAG_WINDOW_RESIZABLE);
	/* Don't let raylib's default exit key (Escape) close the window -- Escape
	 * must reach the page / modal module (exit insert mode, cancel the URL
	 * bar, etc.).  Matches cmacs lrgterm. */
	grl_input_set_exit_key(GRL_KEY_NULL);
}

static void
gsurf_lrg_window_insert_view(GsurfWindow *window, GsurfView *view)
{
	/* No native packing; the active view's texture is drawn each frame.  Track
	   the view's state for the address bar / status HUD. */
	g_signal_connect(view, "uri-changed", G_CALLBACK(on_chrome_uri), window);
	g_signal_connect(view, "hovered-uri-changed",
	                 G_CALLBACK(on_chrome_hover), window);
	g_signal_connect(view, "progress-changed",
	                 G_CALLBACK(on_chrome_progress), window);
	g_signal_connect(view, "title-changed", G_CALLBACK(on_chrome_title), window);
}

static void
gsurf_lrg_window_remove_view(GsurfWindow *window, GsurfView *view)
{
	GsurfLrgWindow *self = GSURF_LRG_WINDOW(window);

	g_signal_handlers_disconnect_by_data(view, window);
	/* Forget borrowed pointers to the closing tab. */
	if (self->focused_view == view)
		self->focused_view = NULL;
	if (self->page_view == view)
		self->page_view = NULL;
	if (self->gesture_view == view) {
		self->gesture_view = NULL;
		self->gesture = LRG_GESTURE_NONE;
	}
}

static void
gsurf_lrg_window_set_active_view(GsurfWindow *window, GsurfView *view)
{
	GsurfLrgWindow *lw = GSURF_LRG_WINDOW(window);
	/* The base class has already made @view active, so the previous tab is
	 * tracked here rather than read back from the window. */
	GsurfView *prev = lw->focused_view;

	if (prev != NULL && prev != view && GSURF_IS_LRG_VIEW(prev))
		gsurf_lrg_view_set_focus(GSURF_LRG_VIEW(prev), FALSE);
	if (view != NULL && GSURF_IS_LRG_VIEW(view))
		gsurf_lrg_view_set_focus(GSURF_LRG_VIEW(view), TRUE);
	lw->focused_view = view;

	/* The chrome now shows the newly active tab. */
	{
		GsurfLrgWindow *self = GSURF_LRG_WINDOW(window);
		const gchar *uri = view != NULL ? gsurf_view_get_uri(view) : NULL;
		const gchar *title = view != NULL ? gsurf_view_get_title(view) : NULL;
		g_autofree gchar *full = NULL;

		g_free(self->chrome_url);
		self->chrome_url = g_strdup(uri != NULL ? uri : "");
		g_clear_pointer(&self->chrome_hover, g_free);
		self->chrome_progress = view != NULL
			? gsurf_view_get_estimated_load_progress(view) : 0.0;
		full = g_strdup_printf("%s%sgsurf", (title && *title) ? title : "",
		                       (title && *title) ? " \xe2\x80\x94 " : "");
		gsurf_window_set_title(window, full);
	}
}

static void
gsurf_lrg_window_present(GsurfWindow *window)
{
	GsurfLrgWindow *self = GSURF_LRG_WINDOW(window);

	gsurf_lrg_window_realize(window);
	if (self->win == NULL)
		return;

	/* Focus the active view so caret/IME work. */
	{
		GsurfLrgView *v = lrg_window_active_lrg_view(self);
		if (v != NULL) {
			gsurf_lrg_view_set_focus(v, TRUE);
			self->focused_view = GSURF_VIEW(v);
		}
	}

	if (self->tick_source == 0)
		self->tick_source = g_timeout_add(16, lrg_window_tick, self);
}

static void
gsurf_lrg_window_set_title(GsurfWindow *window, const gchar *title)
{
	GsurfLrgWindow *self = GSURF_LRG_WINDOW(window);

	g_free(self->title);
	self->title = g_strdup(title);
	if (self->win != NULL)
		grl_window_set_title(self->win, title ? title : "gsurf");
}

static void
gsurf_lrg_window_set_default_size(GsurfWindow *window, gint width, gint height)
{
	GsurfLrgWindow *self = GSURF_LRG_WINDOW(window);

	if (width > 0)
		self->default_width = width;
	if (height > 0)
		self->default_height = height;
}

static void
gsurf_lrg_window_set_fullscreen(GsurfWindow *window, gboolean fullscreen)
{
	GsurfLrgWindow *self = GSURF_LRG_WINDOW(window);
	self->fullscreen = fullscreen;
	/* graylib fullscreen toggle is applied in a later pass. */
}

static gpointer
gsurf_lrg_window_get_native_widget(GsurfWindow *window)
{
	return GSURF_LRG_WINDOW(window)->win;
}

static void
gsurf_lrg_window_add_chrome_widget(GsurfWindow *window, gpointer widget,
                                   gboolean top)
{
	/* No native widgets; LRG chrome is drawn by the window in a later pass. */
	(void)window;
	(void)widget;
	(void)top;
}

/* --- public API --- */

void
gsurf_lrg_window_set_render_mode(GsurfLrgWindow *self, GsurfLrgRenderMode mode)
{
	g_return_if_fail(GSURF_IS_LRG_WINDOW(self));

	if (!gsurf_lrg_render_mode_is_implemented(mode)) {
		g_warning("gsurf LRG: render mode '%s' is not available in this "
		          "build; using 2d", gsurf_lrg_render_mode_to_string(mode));
		mode = GSURF_LRG_RENDER_MODE_2D;
	}
	if (self->win != NULL && mode != self->render_mode) {
		g_warning("gsurf LRG: the render mode is fixed once the window is "
		          "shown");
		return;
	}
	self->render_mode = mode;
}

void
gsurf_lrg_window_set_scene_options(GsurfLrgWindow *self,
                                   const GsurfLrgSceneOptions *options)
{
	g_return_if_fail(GSURF_IS_LRG_WINDOW(self));
	g_return_if_fail(options != NULL);

	g_free(self->scene_arrangement);
	g_free(self->scene_environment);
	self->scene_arrangement = g_strdup(options->arrangement);
	self->scene_environment = g_strdup(options->environment);
	self->scene_background_fps = options->background_fps;
	self->scene_history_limit = options->history_limit;
	self->scene_reading_single_tab = options->reading_single_tab;
}

gpointer
gsurf_lrg_window_get_scene(GsurfLrgWindow *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_WINDOW(self), NULL);
#ifdef GSURF_HAVE_LRG_3D
	return self->scene;
#else
	return NULL;
#endif
}

void
gsurf_lrg_window_notify_scene(GsurfLrgWindow *self, const gchar *what)
{
	g_return_if_fail(GSURF_IS_LRG_WINDOW(self));
	g_signal_emit(self, lrg_window_signals[SIGNAL_SCENE_CHANGED], 0, what);
}

gboolean
gsurf_lrg_window_run_scene_action(GsurfLrgWindow *self, GsurfAction action)
{
#ifdef GSURF_HAVE_LRG_3D
	GsurfLrgScene *scene;
	GsurfView *active;
	const gchar *what = "camera";

	g_return_val_if_fail(GSURF_IS_LRG_WINDOW(self), FALSE);

	scene = self->scene;
	if (scene == NULL)
		return FALSE;
	active = gsurf_window_get_active_view(GSURF_WINDOW(self));

	switch (action) {
	case GSURF_ACTION_LRG_CYCLE_ARRANGEMENT:
		gsurf_lrg_scene_cycle_arrangement(scene);
		what = "arrangement";
		break;
	case GSURF_ACTION_LRG_CYCLE_ENVIRONMENT:
		gsurf_lrg_scene_cycle_environment(scene);
		what = "environment";
		break;
	case GSURF_ACTION_LRG_READING:
		gsurf_lrg_scene_set_reading(scene, !gsurf_lrg_scene_get_reading(scene));
		what = "reading";
		break;
	case GSURF_ACTION_LRG_HISTORY:
		gsurf_lrg_scene_set_history(scene, !gsurf_lrg_scene_get_history(scene));
		what = "history";
		break;
	case GSURF_ACTION_LRG_FOCUS:
		if (active == NULL || !gsurf_lrg_scene_focus_view(scene, active))
			return TRUE;
		what = "focus";
		break;
	case GSURF_ACTION_LRG_PIN:
		if (active == NULL)
			return TRUE;
		if (gsurf_lrg_scene_is_view_pinned(scene, active))
			gsurf_lrg_scene_unpin_view(scene, active);
		else
			gsurf_lrg_scene_pin_view(scene, active);
		what = "pin";
		break;
	case GSURF_ACTION_LRG_UNPIN_ALL:
		gsurf_lrg_scene_unpin_view(scene, NULL);
		what = "pin";
		break;
	case GSURF_ACTION_LRG_CAMERA_RESET:
		gsurf_lrg_scene_reset_camera(scene);
		break;
	case GSURF_ACTION_LRG_OVERVIEW:
		gsurf_lrg_scene_overview(scene);
		break;
	case GSURF_ACTION_LRG_ORBIT_LEFT:
		gsurf_lrg_scene_orbit(scene, -LRG_3D_ORBIT_STEP, 0.0f, TRUE);
		break;
	case GSURF_ACTION_LRG_ORBIT_RIGHT:
		gsurf_lrg_scene_orbit(scene, LRG_3D_ORBIT_STEP, 0.0f, TRUE);
		break;
	case GSURF_ACTION_LRG_ORBIT_UP:
		gsurf_lrg_scene_orbit(scene, 0.0f, LRG_3D_ORBIT_STEP, TRUE);
		break;
	case GSURF_ACTION_LRG_ORBIT_DOWN:
		gsurf_lrg_scene_orbit(scene, 0.0f, -LRG_3D_ORBIT_STEP, TRUE);
		break;
	case GSURF_ACTION_LRG_ZOOM_IN:
		gsurf_lrg_scene_zoom(scene, LRG_3D_ZOOM_STEP, TRUE);
		break;
	case GSURF_ACTION_LRG_ZOOM_OUT:
		gsurf_lrg_scene_zoom(scene, 1.0f / LRG_3D_ZOOM_STEP, TRUE);
		break;
	default:
		return FALSE;
	}
	gsurf_lrg_window_notify_scene(self, what);
	return TRUE;
#else
	g_return_val_if_fail(GSURF_IS_LRG_WINDOW(self), FALSE);
	(void)action;
	return FALSE;
#endif
}

GsurfLrgRenderMode
gsurf_lrg_window_get_render_mode(GsurfLrgWindow *self)
{
	g_return_val_if_fail(GSURF_IS_LRG_WINDOW(self), GSURF_LRG_RENDER_MODE_2D);
	return self->render_mode;
}

void
gsurf_lrg_window_begin_url_edit(GsurfLrgWindow *self)
{
	g_return_if_fail(GSURF_IS_LRG_WINDOW(self));

	if (gsurf_kiosk_is_enabled())
		return;
	if (self->url_input == NULL)
		self->url_input = g_string_new(NULL);
	else
		g_string_truncate(self->url_input, 0);
	/* Pre-fill with the current URI so the user can edit it. */
	if (self->chrome_url != NULL)
		g_string_assign(self->url_input, self->chrome_url);
	self->url_fresh = self->url_input->len > 0;
	lrg_set_url_editing(self, TRUE);
}

/* --- GObject lifecycle --- */

static void
gsurf_lrg_window_dispose(GObject *object)
{
	GsurfLrgWindow *self = GSURF_LRG_WINDOW(object);

	if (self->tick_source != 0) {
		g_source_remove(self->tick_source);
		self->tick_source = 0;
	}
	/* GL objects first, while the window (and its context) still exists; the
	 * scene's surface owns the window in 3D, so it goes last. */
	g_clear_object(&self->font);
	g_clear_object(&self->win);
#ifdef GSURF_HAVE_LRG_3D
	g_clear_object(&self->scene);
#endif
	g_clear_pointer(&self->scene_arrangement, g_free);
	g_clear_pointer(&self->scene_environment, g_free);
	g_clear_pointer(&self->title, g_free);
	g_clear_pointer(&self->chrome_url, g_free);
	g_clear_pointer(&self->chrome_hover, g_free);
	if (self->url_input != NULL)
		g_string_free(self->url_input, TRUE);

	G_OBJECT_CLASS(gsurf_lrg_window_parent_class)->dispose(object);
}

static void
gsurf_lrg_window_class_init(GsurfLrgWindowClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfWindowClass *window_class = GSURF_WINDOW_CLASS(klass);

	object_class->dispose = gsurf_lrg_window_dispose;

	window_class->realize = gsurf_lrg_window_realize_deferred;
	window_class->insert_view = gsurf_lrg_window_insert_view;
	window_class->remove_view = gsurf_lrg_window_remove_view;
	window_class->set_active_view = gsurf_lrg_window_set_active_view;
	window_class->present = gsurf_lrg_window_present;
	window_class->set_title = gsurf_lrg_window_set_title;
	window_class->set_default_size = gsurf_lrg_window_set_default_size;
	window_class->set_fullscreen = gsurf_lrg_window_set_fullscreen;
	window_class->get_native_widget = gsurf_lrg_window_get_native_widget;
	window_class->add_chrome_widget = gsurf_lrg_window_add_chrome_widget;

	/**
	 * GsurfLrgWindow::scene-changed:
	 * @self: the window
	 * @what: what changed ("arrangement", "environment", "reading",
	 *   "history", "focus", "pin", "camera")
	 *
	 * Emitted when the 3D scene changes through an action, a pointer
	 * gesture or the control socket.  The control server turns it into
	 * `lrg.*` events.
	 */
	lrg_window_signals[SIGNAL_SCENE_CHANGED] =
		g_signal_new("scene-changed", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gsurf_lrg_window_init(GsurfLrgWindow *self)
{
	self->default_width = LRG_DEFAULT_WIDTH;
	self->default_height = LRG_DEFAULT_HEIGHT;
	self->render_mode = GSURF_LRG_RENDER_MODE_2D;
	self->scene_reading_single_tab = TRUE;
}

GsurfWindow *
gsurf_lrg_window_new(void)
{
	return g_object_new(GSURF_TYPE_LRG_WINDOW, NULL);
}
