/*
 * test-modal-webkit.c - vim modes and typing in page fields, real WebKit
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the real modal.so into a module manager, gives it a real backend
 * window (GTK3 or GTK4) with a WebKit view on a loopback page, and sends
 * keys through the backend's key handling (GTK3: the toplevel's
 * key-press-event; GTK4: the window's capture-phase key controller), then
 * GsurfWindow::key-press, then a copy of the gsurf binary's key routing
 * (main.c is not linked into tests): the typing guard, the modules, the
 * core Escape blur, and the core keymap with `o` bound to open-prompt
 * (counted here instead of opening a bar).
 *
 * Checks, for a plain <input>, an <input> in an open shadow root and an
 * <input> in an <iframe>: focusing the field is detected, stays detected
 * (an iframe that loads later cannot clear it), and `o`, `a`, `i` are
 * typing (never a command, never the prompt); Escape blurs. Then the
 * modes: `a` and `i` enter INSERT, where `o` is the page's; Escape
 * returns to NORMAL and blurs; `o` with nothing focused is the prompt;
 * `gi` focuses the first field and enters INSERT.
 *
 * Runs only with GSURF_TEST_GUI=1 (make test-gui), on both toolkits.
 */

#include <gsurf.h>
#include <string.h>

#include "test-util.h"
#include "webkit-test-util.h"

static const gchar MAIN_PAGE[] =
	"<!doctype html><html><body>"
	"<input id=plain>"
	"<x-host id=host></x-host>"
	"<iframe id=fr src='/frame'></iframe>"
	"<script>"
	"customElements.define('x-host',class extends HTMLElement{constructor(){super();"
	"this.attachShadow({mode:'open'}).innerHTML='<input id=inner>';}});"
	"</script></body></html>";

static const gchar FRAME_PAGE[] =
	"<!doctype html><html><body><input id=framed></body></html>";

static void
serve(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query,
      gpointer data)
{
	const gchar *body = g_str_equal(path, "/frame") ? FRAME_PAGE : MAIN_PAGE;

	(void)server;
	(void)query;
	(void)data;
	soup_server_message_set_status(msg, 200, NULL);
	soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_STATIC,
		body, strlen(body));
}

typedef struct {
	GsurfConfig *config;
	GsurfApplication *app;
	GsurfWindow *window;
	GsurfView *view;
	GtkWidget *toplevel;
	guint prompts;        /* open-prompt reached the core keymap */
	gboolean consumed;    /* what the key routing returned last */
} H;

/* A copy of main.c's on_key_press routing (see the header). */
static gboolean
on_key_press(GsurfWindow *window, guint keyval, guint keycode, guint state,
             gpointer user_data)
{
	H *h = user_data;
	GsurfModuleManager *mgr = gsurf_module_manager_get_default();
	GsurfView *view = gsurf_window_get_active_view(window);
	gboolean has_mod = (state & (GSURF_MOD_CTRL | GSURF_MOD_ALT | GSURF_MOD_SUPER)) != 0;
	gboolean is_escape = (keyval == GDK_KEY_Escape);
	g_autofree gchar *keystr = NULL;
	gboolean typing;

	h->consumed = FALSE;
	typing = (gsurf_module_manager_get_input_passthrough(mgr) ||
	          (view != NULL && gsurf_view_get_editing(view))) &&
	         !has_mod && !is_escape;
	if (typing && !gsurf_window_get_key_grab(window))
		return FALSE;
	if (gsurf_module_manager_dispatch_key_event(mgr, view, keyval, keycode, state,
			GSURF_MODE_NORMAL))
		return h->consumed = TRUE;
	if (typing)
		return FALSE;
	if (is_escape && !has_mod && view != NULL && gsurf_view_get_editing(view)) {
		gsurf_view_blur_editable(view);
		return h->consumed = TRUE;
	}
	keystr = gsurf_keys_to_string(keyval, state);
	if (gsurf_config_get_keybind_action(h->config, keystr) == GSURF_ACTION_OPEN_PROMPT) {
		h->prompts++;
		return h->consumed = TRUE;
	}
	return FALSE;
}

/* A key press through the backend's key handling. */
static void
press(H *h, guint keyval)
{
#ifdef GSURF_BACKEND_GTK4
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(h->toplevel);
	GtkEventController *found = NULL;
	gboolean handled = FALSE;
	guint i;

	for (i = 0; i < g_list_model_get_n_items(controllers) && found == NULL; i++) {
		g_autoptr(GtkEventController) c = g_list_model_get_item(controllers, i);

		if (GTK_IS_EVENT_CONTROLLER_KEY(c) &&
		    gtk_event_controller_get_propagation_phase(c) == GTK_PHASE_CAPTURE)
			found = c; /* still owned by the toplevel */
	}
	g_assert_nonnull(found);
	g_signal_emit_by_name(found, "key-pressed", keyval, 0u, (GdkModifierType)0, &handled);
#else
	GdkEvent *event = gdk_event_new(GDK_KEY_PRESS);
	GdkDisplay *display = gtk_widget_get_display(h->toplevel);
	GdkKeymapKey *keys = NULL;
	gint n_keys = 0;
	gunichar ch = gdk_keyval_to_unicode(keyval);
	gchar text[8] = { 0 };

	if (ch != 0)
		g_unichar_to_utf8(ch, text);
	event->key.window = g_object_ref(gtk_widget_get_window(h->toplevel));
	event->key.send_event = TRUE;
	event->key.time = GDK_CURRENT_TIME;
	event->key.keyval = keyval;
	event->key.string = g_strdup(text);
	event->key.length = (gint)strlen(text);
	if (gdk_keymap_get_entries_for_keyval(gdk_keymap_get_for_display(display),
			keyval, &keys, &n_keys) && n_keys > 0)
		event->key.hardware_keycode = (guint16)keys[0].keycode;
	g_free(keys);
	gdk_event_set_device(event, gdk_seat_get_keyboard(gdk_display_get_default_seat(display)));
	gtk_widget_event(h->toplevel, event);
	gdk_event_free(event);
#endif
	gsurf_webkit_test_spin(30);
}

static gboolean
inserting(void)
{
	return gsurf_module_manager_get_input_passthrough(gsurf_module_manager_get_default());
}

static gchar *
eval(H *h, const gchar *js)
{
	return gsurf_webkit_test_eval(gsurf_webkit_test_native(h->view), js);
}

/* Poll (bounded) until the editing state is @want. */
static void
wait_editing(H *h, gboolean want)
{
	guint i;

	for (i = 0; i < 100 && gsurf_view_get_editing(h->view) != want; i++)
		gsurf_webkit_test_spin(30);
	g_assert_true(gsurf_view_get_editing(h->view) == want);
}

/* The deepest active element's id, through shadow roots and iframes. */
#define DEEP_ACTIVE_JS \
	"(function(){var d=document,a=d.activeElement;for(;;){" \
	"if(a&&a.shadowRoot&&a.shadowRoot.activeElement){a=a.shadowRoot.activeElement;continue;}" \
	"if(a&&a.tagName==='IFRAME'&&a.contentDocument){a=a.contentDocument.activeElement;continue;}" \
	"break;}return a?(a.id||a.tagName):'none';})()"

/* A field: how to focus it, how to read it, and its id. */
typedef struct {
	const gchar *focus;
	const gchar *value;
	const gchar *id;
} Field;

static const Field fields[] = {
	{ "document.getElementById('plain').focus();'f'",
	  "document.getElementById('plain').value", "plain" },
	{ "document.getElementById('host').shadowRoot.getElementById('inner').focus();'f'",
	  "document.getElementById('host').shadowRoot.getElementById('inner').value", "inner" },
	{ "var f=document.getElementById('fr');f.contentWindow.focus();"
	  "f.contentDocument.getElementById('framed').focus();'f'",
	  "document.getElementById('fr').contentDocument.getElementById('framed').value", "framed" },
};

static void
check_field(H *h, const Field *field)
{
	const guint keys[] = { GDK_KEY_o, GDK_KEY_a, GDK_KEY_i };
	g_autofree gchar *active = NULL;
	g_autofree gchar *after = NULL;
	guint i;

	g_free(eval(h, field->focus));
	wait_editing(h, TRUE);
	active = eval(h, DEEP_ACTIVE_JS);
	g_assert_cmpstr(active, ==, field->id);
	/* Other frames' later reports (the iframe's own focusout settling,
	 * the top frame seeing an IFRAME) must not clear it. */
	gsurf_webkit_test_spin(300);
	g_assert_true(gsurf_view_get_editing(h->view));

	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		press(h, keys[i]);
		g_assert_false(h->consumed);
		g_assert_cmpuint(h->prompts, ==, 0);
		g_assert_false(inserting());
	}
#ifdef GSURF_BACKEND_GTK3
	/* GTK3 delivers the unconsumed synthetic key to the page. */
	{
		g_autofree gchar *value = eval(h, field->value);

		g_assert_cmpstr(value, ==, "oai");
	}
#endif

	/* Escape leaves the field: the next key is a command. */
	press(h, GDK_KEY_Escape);
	g_assert_true(h->consumed);
	g_assert_false(gsurf_view_get_editing(h->view));
	gsurf_webkit_test_spin(200);
	after = eval(h, DEEP_ACTIVE_JS);
	g_assert_cmpstr(after, !=, field->id);
	g_assert_false(gsurf_view_get_editing(h->view));
}

static void
test_modes(void)
{
	g_autoptr(SoupServer) server = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *modal = gsurf_webkit_test_module_path("modal");
	g_autofree gchar *uri = NULL;
	GsurfModuleManager *mgr;
	H h = { 0 };
	guint i;

	if (!gsurf_webkit_test_init())
		return;
	if (!g_file_test(modal, G_FILE_TEST_EXISTS)) {
		g_test_skip("modal.so not built");
		return;
	}
	server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, serve, NULL, NULL);
	uri = g_strdup_printf("http://127.0.0.1:%u/", gsurf_webkit_test_listen(server));

	/* Config, app, manager and modal wired as the gsurf binary does. */
	h.config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(h.config,
		"keybinds:\n  o: open-prompt\n"
		"modules:\n  modal:\n    enabled: true\n", -1, &error));
	g_assert_no_error(error);
	gsurf_config_set_default(h.config);
	h.app = gsurf_application_new(h.config);
	mgr = gsurf_module_manager_get_default();
	gsurf_module_manager_set_application(mgr, h.app);
	gsurf_module_manager_set_config(mgr, h.config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, modal, &error));
	g_assert_no_error(error);

	h.window = gsurf_window_new();
	g_signal_connect(h.window, "key-press", G_CALLBACK(on_key_press), &h);
	h.view = gsurf_view_new();
	gsurf_window_add_view(h.window, h.view);
	gsurf_application_add_window(h.app, h.window);
	gsurf_module_manager_activate_all(mgr);
	gsurf_window_present(h.window);
	h.toplevel = gsurf_window_get_native_widget(h.window);
	gsurf_webkit_test_load(gsurf_webkit_test_native(h.view), uri);
	/* The iframe and the custom element are ready. */
	for (i = 0; i < 100; i++) {
		g_autofree gchar *ready = eval(&h,
			"String(!!(document.getElementById('host').shadowRoot&&"
			"document.getElementById('fr').contentDocument&&"
			"document.getElementById('fr').contentDocument.getElementById('framed')))");

		if (g_strcmp0(ready, "true") == 0)
			break;
		gsurf_webkit_test_spin(30);
	}
	gsurf_view_focus(h.view);
	gsurf_webkit_test_spin(100);
	g_assert_false(gsurf_view_get_editing(h.view));

	/* 1. Every kind of field takes o/a/i as text; Escape leaves it. */
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
		check_field(&h, &fields[i]);

	/* 2. An iframe that loads while the user types in the page must not
	 *    clear the editing state (its start-up report says "nothing
	 *    focused here"). */
	g_free(eval(&h, "document.getElementById('plain').focus();"
		"window.__late=0;var f=document.createElement('iframe');"
		"f.onload=function(){window.__late=1;};f.src='/frame';"
		"document.body.appendChild(f);'f'"));
	wait_editing(&h, TRUE);
	for (i = 0; i < 100; i++) {
		g_autofree gchar *late = eval(&h, "String(window.__late)");

		if (g_strcmp0(late, "1") == 0)
			break;
		gsurf_webkit_test_spin(30);
	}
	gsurf_webkit_test_spin(300);
	g_assert_true(gsurf_view_get_editing(h.view));
	press(&h, GDK_KEY_o);
	g_assert_false(h.consumed);
	g_assert_cmpuint(h.prompts, ==, 0);
	press(&h, GDK_KEY_Escape);
	g_assert_false(gsurf_view_get_editing(h.view));
	gsurf_webkit_test_spin(200);

	/* 3. Nothing focused: `o` is the prompt (the binding is live). */
	press(&h, GDK_KEY_o);
	g_assert_true(h.consumed);
	g_assert_cmpuint(h.prompts, ==, 1);

	/* 4. `a` and `i` enter INSERT; there `o` is the page's; Escape
	 *    returns to NORMAL. */
	press(&h, GDK_KEY_a);
	g_assert_true(h.consumed);
	g_assert_true(inserting());
	press(&h, GDK_KEY_o);
	g_assert_false(h.consumed);
	g_assert_cmpuint(h.prompts, ==, 1);
	press(&h, GDK_KEY_Escape);
	g_assert_true(h.consumed);
	g_assert_false(inserting());
	press(&h, GDK_KEY_i);
	g_assert_true(inserting());
	press(&h, GDK_KEY_Escape);
	g_assert_false(inserting());

	/* 5. `gi` focuses the first field and enters INSERT; Escape leaves
	 *    INSERT and the field. */
	g_free(eval(&h, "document.getElementById('plain').value='';'c'"));
	press(&h, GDK_KEY_g);
	press(&h, GDK_KEY_i);
	g_assert_true(inserting());
	wait_editing(&h, TRUE);
	{
		g_autofree gchar *active = eval(&h, DEEP_ACTIVE_JS);

		g_assert_cmpstr(active, ==, "plain");
	}
	press(&h, GDK_KEY_o);
	g_assert_false(h.consumed);
	g_assert_cmpuint(h.prompts, ==, 1);
	press(&h, GDK_KEY_Escape);
	g_assert_true(h.consumed);
	g_assert_false(inserting());
	g_assert_false(gsurf_view_get_editing(h.view));
	gsurf_webkit_test_spin(200);
	{
		g_autofree gchar *active = eval(&h, DEEP_ACTIVE_JS);

		g_assert_cmpstr(active, !=, "plain");
	}

	gsurf_module_manager_deactivate_all(mgr);
	gsurf_module_manager_set_application(mgr, NULL);
	gsurf_module_manager_set_config(mgr, NULL);
	gsurf_config_set_default(NULL);
	g_signal_handlers_disconnect_by_data(h.window, &h);
	gtk_widget_set_visible(h.toplevel, FALSE);
	g_clear_object(&h.view);
	g_clear_object(&h.window);
	g_clear_object(&h.app);
	g_clear_object(&h.config);
	gsurf_webkit_test_spin(50);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/modal/webkit/modes-and-fields", test_modes);
	return g_test_run();
}
