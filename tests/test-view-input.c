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

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/gsurf/view-input/key-names", test_key_names);
	g_test_add_func("/gsurf/view-input/locate-quotes", test_locate_script_quotes_the_selector);

	return g_test_run();
}
