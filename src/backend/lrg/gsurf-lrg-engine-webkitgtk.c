/*
 * gsurf-lrg-engine-webkitgtk.c - LRG web engine: offscreen WebKitGTK
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The engine that runs today (incl. inside cmacs and on platforms without WPE).
 * It hosts a WebKitGTK WebView in a GtkOffscreenWindow with hardware
 * acceleration disabled, so the page renders into the window's cairo surface;
 * each capture reads that surface back as RGBA for the host to upload to a GL
 * texture. Input is injected by synthesizing GDK events into the offscreen
 * widget. It deliberately uses the *same* WebKit/GTK already linked by the GTK
 * backend (webkit2gtk-4.1 with GTK3 in the cmacs build), so no second WebKit
 * library is linked (their webkit_* symbols would collide).
 *
 * Only the GTK3 / webkit2gtk-4.1 toolkit is implemented here; the GTK4 /
 * webkitgtk-6.0 offscreen path is a TODO guarded below.
 */

#include "backend/lrg/gsurf-lrg-engine.h"

#include <string.h>
#include <gtk/gtk.h>
#include <fontconfig/fontconfig.h>

#if defined(GSURF_BACKEND_GTK4)
#error "LRG WebKitGTK engine: GTK4/webkitgtk-6.0 offscreen path not yet implemented"
#else
#include <webkit2/webkit2.h>
#endif

#define LRG_DEFAULT_WIDTH  1024
#define LRG_DEFAULT_HEIGHT 768

struct _GsurfLrgEngine
{
	GsurfView     *owner;     /* borrowed (owner owns the engine) */
	GtkWidget     *offscreen; /* GtkOffscreenWindow (we hold a ref) */
	WebKitWebView *webview;   /* child of @offscreen */

	int     width;
	int     height;
	double  scale;

	guint8 *rgba;       /* width*height*4, R8G8B8A8 */
	gsize   rgba_size;
	gboolean new_frame; /* a fresh frame landed since the last capture() */
	gboolean captured;  /* at least one real frame has been read back */
	gboolean in_flight; /* a webkit_web_view_get_snapshot() is pending */
	gboolean focused;
};

const gchar *
gsurf_lrg_engine_get_engine_name(void)
{
	return "lrg-webkitgtk";
}

gboolean
gsurf_lrg_engine_backend_init(int *argc, char ***argv, GError **error)
{
	if (!gtk_init_check(argc, argv)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"gsurf LRG: failed to initialize GTK (no display?)");
		return FALSE;
	}
	return TRUE;
}

/* --- size / buffer --- */

static void
engine_alloc_buffer(GsurfLrgEngine *self)
{
	gsize need = (gsize)self->width * (gsize)self->height * 4;

	if (need != self->rgba_size) {
		g_free(self->rgba);
		self->rgba = need ? g_malloc0(need) : NULL;
		self->rgba_size = need;
	}
	self->new_frame = FALSE;
	self->captured = FALSE;
}

void
gsurf_lrg_engine_set_size(GsurfLrgEngine *self, int width, int height,
                          double scale)
{
	if (width <= 0 || height <= 0)
		return;
	if (width == self->width && height == self->height && scale == self->scale)
		return;

	self->width = width;
	self->height = height;
	self->scale = scale > 0.0 ? scale : 1.0;

	gtk_window_resize(GTK_WINDOW(self->offscreen), width, height);
	gtk_widget_set_size_request(GTK_WIDGET(self->webview), width, height);
	engine_alloc_buffer(self);
}

/* --- pixel readback via webkit_web_view_get_snapshot ---
 *
 * gtk_offscreen_window_get_surface() does NOT capture WebKit2GTK's content
 * (the page is rendered in a separate web process and composited via a path
 * the offscreen widget surface never sees -> a blank/white grab).  The
 * reliable way to get the rendered page as a cairo surface is to ask WebKit
 * for a snapshot; it forces an actual render of the visible region.  It is
 * async, so we keep at most one request in flight and copy the result into
 * the RGBA buffer on completion.  capture() (called once per host frame)
 * reports whether a fresh frame landed and re-arms the next snapshot.  */

static void
engine_snapshot_ready(GObject *source, GAsyncResult *res, gpointer data)
{
	GsurfLrgEngine *self = data;
	cairo_surface_t *surface;
	const guchar *src;
	int sw, sh, stride, y, x, cw, ch;

	self->in_flight = FALSE;

	surface = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source),
	                                              res, NULL);
	if (surface == NULL)
		return;
	if (cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE
	    || self->rgba == NULL) {
		cairo_surface_destroy(surface);
		return;
	}

	cairo_surface_flush(surface);
	src = cairo_image_surface_get_data(surface);
	sw = cairo_image_surface_get_width(surface);
	sh = cairo_image_surface_get_height(surface);
	stride = cairo_image_surface_get_stride(surface);
	if (src != NULL && sw > 0 && sh > 0) {
		cw = sw < self->width ? sw : self->width;
		ch = sh < self->height ? sh : self->height;
		for (y = 0; y < ch; y++) {
			const guint32 *in = (const guint32 *)(src + (gsize)y * stride);
			guint8 *out = self->rgba + (gsize)y * self->width * 4;
			for (x = 0; x < cw; x++) {
				/* Cairo ARGB32 little-endian byte order is B,G,R,A. */
				guint32 p = in[x];
				out[x * 4 + 0] = (guint8)((p >> 16) & 0xFF); /* R */
				out[x * 4 + 1] = (guint8)((p >> 8)  & 0xFF); /* G */
				out[x * 4 + 2] = (guint8)((p)       & 0xFF); /* B */
				out[x * 4 + 3] = 0xFF;                       /* opaque */
			}
		}
		self->captured = TRUE;
		self->new_frame = TRUE;
	}
	cairo_surface_destroy(surface);
}

gboolean
gsurf_lrg_engine_capture(GsurfLrgEngine *self)
{
	gboolean fresh = self->new_frame;

	self->new_frame = FALSE;

	/* Re-arm a snapshot if none is pending (one in flight at a time). */
	if (!self->in_flight && self->webview != NULL && self->rgba != NULL) {
		self->in_flight = TRUE;
		webkit_web_view_get_snapshot(self->webview,
			WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
			NULL, engine_snapshot_ready, self);
	}
	return fresh;
}

const guint8 *
gsurf_lrg_engine_get_pixels(GsurfLrgEngine *self, int *out_width,
                            int *out_height)
{
	if (out_width != NULL)
		*out_width = self->width;
	if (out_height != NULL)
		*out_height = self->height;
	/* No texture until a real frame has been read back, so the host never
	 * uploads a blank buffer (and never touches the GPU before there is a
	 * GL context). */
	return self->captured ? self->rgba : NULL;
}

gpointer
gsurf_lrg_engine_get_webview(GsurfLrgEngine *self)
{
	return self->webview;
}

/* --- input injection via synthesized GDK events --- */

static guint
engine_gdk_state(guint mods)
{
	guint state = 0;
	if (mods & GSURF_MOD_SHIFT)
		state |= GDK_SHIFT_MASK;
	if (mods & GSURF_MOD_CTRL)
		state |= GDK_CONTROL_MASK;
	if (mods & GSURF_MOD_ALT)
		state |= GDK_MOD1_MASK;
	if (mods & GSURF_MOD_SUPER)
		state |= GDK_SUPER_MASK;
	return state;
}

static GdkWindow *
engine_event_window(GsurfLrgEngine *self)
{
	return gtk_widget_get_window(GTK_WIDGET(self->webview));
}

static GdkDevice *
engine_device(GsurfLrgEngine *self, gboolean keyboard)
{
	GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(self->webview));
	GdkSeat *seat = gdk_display_get_default_seat(display);

	return keyboard ? gdk_seat_get_keyboard(seat)
	                : gdk_seat_get_pointer(seat);
}

/* Resolve a hardware keycode (and keyboard group) for a keysym from the
 * display keymap.  Real key events carry a hardware_keycode; WebKit's handling
 * of editing keys (Backspace/Delete/arrows) is unreliable with keycode 0 even
 * though printable chars resolve from the keyval alone.  Returns 0 if the
 * keysym isn't on the keymap. */
static guint16
engine_keycode_for_keysym(GsurfLrgEngine *self, guint keysym, guint8 *out_group)
{
	GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(self->webview));
	GdkKeymap *keymap = gdk_keymap_get_for_display(display);
	GdkKeymapKey *keys = NULL;
	gint n = 0;
	guint16 code = 0;

	if (out_group != NULL)
		*out_group = 0;
	if (keymap != NULL
	    && gdk_keymap_get_entries_for_keyval(keymap, keysym, &keys, &n)
	    && n > 0) {
		code = (guint16)keys[0].keycode;
		if (out_group != NULL)
			*out_group = (guint8)keys[0].group;
	}
	g_free(keys);
	return code;
}

void
gsurf_lrg_engine_send_key(GsurfLrgEngine *self, guint keysym, guint keycode,
                          guint mods, gboolean pressed)
{
	GdkWindow *win = engine_event_window(self);
	GdkEvent *ev;
	guint8 group = 0;

	if (win == NULL)
		return;

	/* Synthesized events must look real enough for WebKit: derive the
	 * hardware keycode + group from the keymap when the caller didn't supply
	 * one (the raylib path always passes 0). */
	if (keycode == 0)
		keycode = engine_keycode_for_keysym(self, keysym, &group);

	ev = gdk_event_new(pressed ? GDK_KEY_PRESS : GDK_KEY_RELEASE);
	ev->key.window = g_object_ref(win);
	ev->key.send_event = TRUE;
	ev->key.time = GDK_CURRENT_TIME;
	ev->key.state = engine_gdk_state(mods);
	ev->key.keyval = keysym;
	ev->key.hardware_keycode = (guint16)keycode;
	ev->key.group = group;
	gdk_event_set_device(ev, engine_device(self, TRUE));

	gtk_main_do_event(ev);
	gdk_event_free(ev);
}

void
gsurf_lrg_engine_send_motion(GsurfLrgEngine *self, int x, int y, guint mods)
{
	GdkWindow *win = engine_event_window(self);
	GdkEvent *ev;

	if (win == NULL)
		return;

	ev = gdk_event_new(GDK_MOTION_NOTIFY);
	ev->motion.window = g_object_ref(win);
	ev->motion.send_event = TRUE;
	ev->motion.time = GDK_CURRENT_TIME;
	ev->motion.x = x;
	ev->motion.y = y;
	ev->motion.state = engine_gdk_state(mods);
	gdk_event_set_device(ev, engine_device(self, FALSE));

	gtk_main_do_event(ev);
	gdk_event_free(ev);
}

void
gsurf_lrg_engine_send_button(GsurfLrgEngine *self, int x, int y, guint button,
                             gboolean pressed, guint mods)
{
	GdkWindow *win = engine_event_window(self);
	GdkEvent *ev;

	if (win == NULL)
		return;

	ev = gdk_event_new(pressed ? GDK_BUTTON_PRESS : GDK_BUTTON_RELEASE);
	ev->button.window = g_object_ref(win);
	ev->button.send_event = TRUE;
	ev->button.time = GDK_CURRENT_TIME;
	ev->button.x = x;
	ev->button.y = y;
	ev->button.button = button;
	ev->button.state = engine_gdk_state(mods);
	gdk_event_set_device(ev, engine_device(self, FALSE));

	gtk_main_do_event(ev);
	gdk_event_free(ev);
}

/* One discrete wheel "click" in DIR -- WebKit scrolls by its standard step,
 * exactly like the GTK client gets from a real wheel (smooth deltas were wildly
 * over-scaled). */
static void
engine_scroll_click(GsurfLrgEngine *self, int x, int y, GdkScrollDirection dir,
                    guint mods)
{
	GdkWindow *win = engine_event_window(self);
	GdkEvent *ev;

	if (win == NULL)
		return;
	ev = gdk_event_new(GDK_SCROLL);
	ev->scroll.window = g_object_ref(win);
	ev->scroll.send_event = TRUE;
	ev->scroll.time = GDK_CURRENT_TIME;
	ev->scroll.x = x;
	ev->scroll.y = y;
	ev->scroll.state = engine_gdk_state(mods);
	ev->scroll.direction = dir;
	gdk_event_set_device(ev, engine_device(self, FALSE));
	gtk_main_do_event(ev);
	gdk_event_free(ev);
}

void
gsurf_lrg_engine_send_axis(GsurfLrgEngine *self, int x, int y, double dx,
                           double dy, guint mods)
{
	/* dx/dy are wheel notches (positive dy = scroll content down).  Emit one
	 * discrete scroll per notch. */
	int v = (dy > 0.0) ? (int)(dy + 0.5) : (dy < 0.0) ? (int)(-dy + 0.5) : 0;
	int h = (dx > 0.0) ? (int)(dx + 0.5) : (dx < 0.0) ? (int)(-dx + 0.5) : 0;
	int i;

	if (v == 0 && dy != 0.0)
		v = 1;
	if (h == 0 && dx != 0.0)
		h = 1;
	for (i = 0; i < v; i++)
		engine_scroll_click(self, x, y,
		                    dy > 0.0 ? GDK_SCROLL_DOWN : GDK_SCROLL_UP, mods);
	for (i = 0; i < h; i++)
		engine_scroll_click(self, x, y,
		                    dx > 0.0 ? GDK_SCROLL_RIGHT : GDK_SCROLL_LEFT, mods);
}

/* Resolve a UI font file for the LRG chrome: $GSURF_LRG_FONT override, else the
 * GTK theme font family (so it matches the GTK client), else "sans-serif",
 * resolved to a file via fontconfig. */
static char *
engine_fc_match(const char *family)
{
	FcPattern *pat, *match;
	FcResult res;
	FcChar8 *file = NULL;
	char *out = NULL;

	if (!FcInit())
		return NULL;
	pat = FcPatternCreate();
	FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	match = FcFontMatch(NULL, pat, &res);
	if (match != NULL) {
		if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch
		    && file != NULL
		    /* Reject OpenType *variable* fonts (e.g. "NotoSans[wght].ttf",
		     * which fontconfig hands back for "sans-serif" on Fedora): raylib's
		     * stb_truetype loader can't handle them and returns no font, so the
		     * chrome would fall back to the ugly built-in bitmap font.  The
		     * "[axis]" in the filename is the reliable marker (FC_VARIABLE is
		     * not set on the match).  Static families load fine. */
		    && strchr((const char *)file, '[') == NULL)
			out = g_strdup((const char *)file);
		FcPatternDestroy(match);
	}
	FcPatternDestroy(pat);
	return out;
}

const char *
gsurf_lrg_engine_get_ui_font_path(void)
{
	static char *cached = NULL;
	static gboolean done = FALSE;
	const gchar *env;
	g_autofree gchar *family = NULL;
	GtkSettings *settings;

	if (done)
		return cached;
	done = TRUE;

	env = g_getenv("GSURF_LRG_FONT");
	if (env != NULL && *env != '\0' && g_file_test(env, G_FILE_TEST_EXISTS)) {
		cached = g_strdup(env);
		return cached;
	}

	/* "Cantarell 11" -> family "Cantarell" (drop trailing point size). */
	settings = gtk_settings_get_default();
	if (settings != NULL) {
		g_autofree gchar *fontname = NULL;
		g_object_get(settings, "gtk-font-name", &fontname, NULL);
		if (fontname != NULL && *fontname != '\0') {
			gchar *sp;
			family = g_strdup(fontname);
			sp = strrchr(family, ' ');
			if (sp != NULL && g_ascii_isdigit((guchar)sp[1]))
				*sp = '\0';
		}
	}

	/* Prefer the GTK theme family; engine_fc_match() rejects variable fonts, so
	 * fall back through known *static* sans families until one resolves to a
	 * loadable file (on Fedora "sans-serif" maps to a variable font). */
	{
		static const char *const statics[] = {
			"DejaVu Sans", "Liberation Sans", "Cantarell",
			"FreeSans", "Noto Sans", "sans-serif"
		};
		gsize i;

		if (family != NULL)
			cached = engine_fc_match(family);
		for (i = 0; cached == NULL && i < G_N_ELEMENTS(statics); i++)
			cached = engine_fc_match(statics[i]);
	}
	return cached;
}

void
gsurf_lrg_engine_set_focus(GsurfLrgEngine *self, gboolean focused)
{
	GdkWindow *win;
	GdkEvent *ev;

	if (self->focused == focused)
		return;
	self->focused = focused;

	if (focused) {
		/* NB: never gtk_window_present() a GtkOffscreenWindow -- it has no
		 * presentable GdkWindow and crashes.  Designating the focus widget
		 * plus the synthesized GDK_FOCUS_CHANGE below is enough for WebKit. */
		gtk_window_set_focus(GTK_WINDOW(self->offscreen),
		                     GTK_WIDGET(self->webview));
		gtk_widget_grab_focus(GTK_WIDGET(self->webview));
	}

	win = engine_event_window(self);
	if (win == NULL)
		return;

	ev = gdk_event_new(GDK_FOCUS_CHANGE);
	ev->focus_change.window = g_object_ref(win);
	ev->focus_change.send_event = TRUE;
	ev->focus_change.in = focused;
	/* A real focus event carries the keyboard device; without one GDK warns
	 * ("Event with type 12 not holding a GdkDevice") and may drop it. */
	gdk_event_set_device(ev, engine_device(self, TRUE));
	gtk_main_do_event(ev);
	gdk_event_free(ev);
}

/* --- lifecycle --- */

GsurfLrgEngine *
gsurf_lrg_engine_new(GsurfView *owner, GError **error)
{
	GsurfLrgEngine *self;
	WebKitSettings *ws;

	self = g_new0(GsurfLrgEngine, 1);
	self->owner = owner;
	self->width = LRG_DEFAULT_WIDTH;
	self->height = LRG_DEFAULT_HEIGHT;
	self->scale = 1.0;

	self->offscreen = gtk_offscreen_window_new();
	g_object_ref_sink(self->offscreen);

	self->webview = WEBKIT_WEB_VIEW(webkit_web_view_new());

	/* Render into the cairo/system surface (not a GL compositor) so the
	 * offscreen window's surface contains the page pixels. */
	ws = webkit_web_view_get_settings(self->webview);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	webkit_settings_set_hardware_acceleration_policy(ws,
		WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
	G_GNUC_END_IGNORE_DEPRECATIONS

	gtk_container_add(GTK_CONTAINER(self->offscreen),
	                  GTK_WIDGET(self->webview));
	gtk_widget_set_size_request(GTK_WIDGET(self->webview),
	                            self->width, self->height);
	gtk_window_set_default_size(GTK_WINDOW(self->offscreen),
	                            self->width, self->height);
	gtk_widget_show_all(self->offscreen);

	engine_alloc_buffer(self);

	(void)error;
	return self;
}

void
gsurf_lrg_engine_free(GsurfLrgEngine *self)
{
	if (self == NULL)
		return;

	if (self->offscreen != NULL) {
		gtk_widget_destroy(self->offscreen);
		g_clear_object(&self->offscreen);
	}
	/* @webview is owned by @offscreen; destroyed with it. */
	self->webview = NULL;

	g_clear_pointer(&self->rgba, g_free);
	g_free(self);
}
