/*
 * test-view-input.c - The parts of agent input that need no web engine
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Key names and the element-locating script are shared by page.key,
 * page.click and page.type on the control socket and by the MCP module's
 * click, type_text and press_key -- one table and one script, so the two
 * surfaces cannot disagree about what "Enter" or "the element" means.
 * The events themselves are tested against WebKit in test-control-webkit.c.
 */

#include <gsurf.h>
#include <glib.h>
#include <string.h>

static void
test_key_names(void)
{
	/* the DOM's names and the X ones, any case */
	g_assert_cmpuint(gsurf_view_keyval_from_name("Enter"), ==, 0xff0d);
	g_assert_cmpuint(gsurf_view_keyval_from_name("return"), ==, 0xff0d);
	g_assert_cmpuint(gsurf_view_keyval_from_name("ArrowLeft"), ==, 0xff51);
	g_assert_cmpuint(gsurf_view_keyval_from_name("left"), ==, 0xff51);
	g_assert_cmpuint(gsurf_view_keyval_from_name("BACKSPACE"), ==, 0xff08);
	g_assert_cmpuint(gsurf_view_keyval_from_name("PageDown"), ==, 0xff56);
	g_assert_cmpuint(gsurf_view_keyval_from_name("space"), ==, 0x20);

	/* one character is that character: Latin-1 itself, the rest 0x01000000 + code */
	g_assert_cmpuint(gsurf_view_keyval_from_name("a"), ==, 'a');
	g_assert_cmpuint(gsurf_view_keyval_from_name("A"), ==, 'A');
	g_assert_cmpuint(gsurf_view_keyval_from_name("\xc3\xa9"), ==, 0xe9);
	g_assert_cmpuint(gsurf_view_keyval_from_name("\xe2\x82\xac"), ==, 0x010020ac);

	/* not a key: nothing, never a guess */
	g_assert_cmpuint(gsurf_view_keyval_from_name("NotAKey"), ==, 0);
	g_assert_cmpuint(gsurf_view_keyval_from_name("ab"), ==, 0);
	g_assert_cmpuint(gsurf_view_keyval_from_name(""), ==, 0);
	g_assert_cmpuint(gsurf_view_keyval_from_name(NULL), ==, 0);
}

static void
test_locate_script_quotes_the_selector(void)
{
	/* a selector is data: quoted as a JavaScript string, never spliced */
	g_autofree gchar *script = gsurf_view_locate_script("a[title=\"x\"]');alert(1);//");

	g_assert_nonnull(strstr(script, "document.querySelector(\"a[title=\\\"x\\\"]');alert(1);//\")"));
	g_assert_nonnull(strstr(script, "scrollIntoView"));
	g_assert_nonnull(strstr(script, "getBoundingClientRect"));
}

/* A view with no engine: enough for the hold, which lives in the base. */
typedef struct { GsurfView parent; } HoldView;
typedef struct { GsurfViewClass parent; } HoldViewClass;

GType hold_view_get_type(void);
G_DEFINE_TYPE(HoldView, hold_view, GSURF_TYPE_VIEW)

static void hold_view_class_init(HoldViewClass *klass) { (void)klass; }
static void hold_view_init(HoldView *self) { (void)self; }

static void
count_notify(GObject *object, GParamSpec *pspec, gpointer user_data)
{
	(void)object;
	(void)pspec;
	(*(guint *)user_data)++;
}

/*
 * The secret hold: set once, notified once, refused through the one
 * gate; only a committed navigation ends it, and NULL fails closed.
 */
static void
test_secret_hold(void)
{
	g_autoptr(GsurfView) view = g_object_new(hold_view_get_type(), NULL);
	g_autoptr(GError) error = NULL;
	guint notified = 0;
	gboolean prop = FALSE;

	g_signal_connect(view, "notify::secret-hold", G_CALLBACK(count_notify), &notified);
	g_assert_false(gsurf_view_get_secret_hold(view));
	g_assert_true(gsurf_view_check_secret_hold(view, "page.eval", &error));
	g_assert_no_error(error);

	/* 1. Set: notified once however often it is set. */
	gsurf_view_set_secret_hold(view);
	gsurf_view_set_secret_hold(view);
	g_assert_cmpuint(notified, ==, 1);
	g_object_get(view, "secret-hold", &prop, NULL);
	g_assert_true(prop);
	g_assert_false(gsurf_view_check_secret_hold(view, "page.eval", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "page.eval refused"));
	g_clear_error(&error);

	/* 2. Loads that keep the document keep the hold. */
	gsurf_view_emit_load_changed(view, GSURF_LOAD_STARTED);
	gsurf_view_emit_load_changed(view, GSURF_LOAD_REDIRECTED);
	gsurf_view_emit_load_changed(view, GSURF_LOAD_FINISHED);
	gsurf_view_emit_uri_changed(view, "https://example.test/#after");
	g_assert_true(gsurf_view_get_secret_hold(view));

	/* 3. A committed navigation is a new document: the hold ends. */
	gsurf_view_emit_load_changed(view, GSURF_LOAD_COMMITTED);
	g_assert_false(gsurf_view_get_secret_hold(view));
	g_assert_cmpuint(notified, ==, 2);
	g_assert_true(gsurf_view_check_secret_hold(view, NULL, NULL));

	/* 4. Fail closed: no view is a held view. */
	g_assert_false(gsurf_view_check_secret_hold(NULL, NULL, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/gsurf/view-input/key-names", test_key_names);
	g_test_add_func("/gsurf/view-input/locate-quotes", test_locate_script_quotes_the_selector);
	g_test_add_func("/gsurf/view-input/secret-hold", test_secret_hold);

	return g_test_run();
}
