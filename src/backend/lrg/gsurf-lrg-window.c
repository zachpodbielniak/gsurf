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

#include "backend/lrg/gsurf-lrg-window.h"
#include "backend/lrg/gsurf-lrg-view.h"
#include "backend/lrg/gsurf-lrg-engine.h"
#include "module/gsurf-module-manager.h"

#include <graylib.h>

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
#define LRG_CHROME_H       28    /* top address-bar height in pixels */
#define LRG_CHROME_FONT    16
#define LRG_FONT_BASE      32    /* glyph-atlas size; downscaled when drawn */

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
	GString   *url_input;

	/* Chrome UI font (same family as the GTK client; raylib's default font
	   is ugly).  Loaded lazily once the GL context exists. */
	GrlFont   *font;
	gboolean   font_tried;
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
	self->url_editing = FALSE;
}

/* Delete the last UTF-8 character from the address field. */
static void
lrg_url_backspace(GsurfLrgWindow *self)
{
	glong n;

	if (self->url_input == NULL || self->url_input->len == 0)
		return;
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

static void
lrg_window_forward_input(GsurfLrgWindow *self, GsurfLrgView *view)
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
			if (ch >= 0x20)
				g_string_append_unichar(self->url_input, (gunichar)ch);
		while ((key = grl_input_get_key_pressed()) != 0) {
			if (key == GRL_KEY_ENTER || key == GRL_KEY_KP_ENTER)
				lrg_window_url_commit(self);
			else if (key == GRL_KEY_ESCAPE)
				self->url_editing = FALSE;
			else if (key == GRL_KEY_BACKSPACE)
				lrg_url_backspace(self);
		}
		/* Auto-repeat held backspace (the press queue doesn't repeat). */
		if (self->url_editing
		    && grl_input_is_key_pressed_repeat(GRL_KEY_BACKSPACE))
			lrg_url_backspace(self);
		return;
	}

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
	if (in_body && wheel != 0.0f)
		gsurf_lrg_view_send_axis(view, px, py, 0.0, -wheel, mods);

	/* Text keys: modules/keybinds first (modal `f`, etc.); the keymap layer
	 * (main.c on_key_press) returns FALSE while the page is editing so bare
	 * keys then reach the focused form field. */
	while ((ch = grl_input_get_char_pressed()) != 0) {
		gboolean consumed = gsurf_window_emit_key_press(win, (guint)ch, 0, mods);
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

		/* Keep the page laid out at the body size (below the address bar). */
		if (view != NULL && body_h > 0)
			gsurf_lrg_view_resize(view, ww, body_h, 1.0);

		if (view != NULL)
			lrg_window_forward_input(self, view);

		grl_window_begin_drawing(self->win);
		grl_window_clear_background(self->win, bg);

		/* Load the chrome font now the GL context is current (no-op after the
		 * first frame). */
		lrg_window_ensure_font(self);

		/* Page texture, drawn into the body rect (below the chrome). */
		if (view != NULL && body_h > 0) {
			int tw = 0, th = 0;
			GrlTexture *tex;

			gsurf_lrg_view_capture(view);
			tex = gsurf_lrg_view_get_texture(view, &tw, &th);
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

		/* Chrome: address bar (URL + load-progress underline) + hover HUD.
		 * In URL-edit mode the bar becomes an editable address field. */
		{
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

		grl_window_swap_buffers(self->win);
	}
	return G_SOURCE_CONTINUE;
}

/* --- chrome state (tracked from the active view's signals) --- */

static void
on_chrome_uri(GsurfView *view, const gchar *uri, gpointer user)
{
	GsurfLrgWindow *self = user;
	(void)view;
	g_free(self->chrome_url);
	self->chrome_url = g_strdup(uri ? uri : "");
}

static void
on_chrome_hover(GsurfView *view, const gchar *uri, gpointer user)
{
	GsurfLrgWindow *self = user;
	(void)view;
	g_clear_pointer(&self->chrome_hover, g_free);
	if (uri != NULL && *uri != '\0')
		self->chrome_hover = g_strdup(uri);
}

static void
on_chrome_progress(GsurfView *view, gdouble progress, gpointer user)
{
	GsurfLrgWindow *self = user;
	(void)view;
	self->chrome_progress = progress;
}

static void
on_chrome_title(GsurfView *view, const gchar *title, gpointer user)
{
	GsurfLrgWindow *self = user;
	g_autofree gchar *full = NULL;
	(void)view;
	full = g_strdup_printf("%s%sgsurf", (title && *title) ? title : "",
	                       (title && *title) ? " \xe2\x80\x94 " : "");
	gsurf_window_set_title(GSURF_WINDOW(self), full);
}

/* --- GsurfWindow vfuncs --- */

static void
gsurf_lrg_window_realize(GsurfWindow *window)
{
	GsurfLrgWindow *self = GSURF_LRG_WINDOW(window);

	if (self->win != NULL)
		return;

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
	g_signal_handlers_disconnect_by_data(view, window);
}

static void
gsurf_lrg_window_set_active_view(GsurfWindow *window, GsurfView *view)
{
	GsurfView *prev = gsurf_window_get_active_view(window);

	if (prev != NULL && prev != view && GSURF_IS_LRG_VIEW(prev))
		gsurf_lrg_view_set_focus(GSURF_LRG_VIEW(prev), FALSE);
	if (view != NULL && GSURF_IS_LRG_VIEW(view))
		gsurf_lrg_view_set_focus(GSURF_LRG_VIEW(view), TRUE);
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
		if (v != NULL)
			gsurf_lrg_view_set_focus(v, TRUE);
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
		g_warning("gsurf LRG: render mode '%s' is not yet implemented; "
		          "using 2d", gsurf_lrg_render_mode_to_string(mode));
		mode = GSURF_LRG_RENDER_MODE_2D;
	}
	self->render_mode = mode;
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

	if (self->url_input == NULL)
		self->url_input = g_string_new(NULL);
	else
		g_string_truncate(self->url_input, 0);
	/* Pre-fill with the current URI so the user can edit it. */
	if (self->chrome_url != NULL)
		g_string_assign(self->url_input, self->chrome_url);
	self->url_editing = TRUE;
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
	g_clear_object(&self->font);
	g_clear_object(&self->win);
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

	window_class->realize = gsurf_lrg_window_realize;
	window_class->insert_view = gsurf_lrg_window_insert_view;
	window_class->remove_view = gsurf_lrg_window_remove_view;
	window_class->set_active_view = gsurf_lrg_window_set_active_view;
	window_class->present = gsurf_lrg_window_present;
	window_class->set_title = gsurf_lrg_window_set_title;
	window_class->set_default_size = gsurf_lrg_window_set_default_size;
	window_class->set_fullscreen = gsurf_lrg_window_set_fullscreen;
	window_class->get_native_widget = gsurf_lrg_window_get_native_widget;
	window_class->add_chrome_widget = gsurf_lrg_window_add_chrome_widget;
}

static void
gsurf_lrg_window_init(GsurfLrgWindow *self)
{
	self->default_width = LRG_DEFAULT_WIDTH;
	self->default_height = LRG_DEFAULT_HEIGHT;
	self->render_mode = GSURF_LRG_RENDER_MODE_2D;
}

GsurfWindow *
gsurf_lrg_window_new(void)
{
	return g_object_new(GSURF_TYPE_LRG_WINDOW, NULL);
}
