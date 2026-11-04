/*
 * test-form-autofill-webkit.c - form_autofill in a real WebKit view
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the form_autofill module and a page with several forms from a
 * loopback HTTP server (and an HTTPS one for card data), then checks end
 * to end, on both backends:
 *   - the fill key with one profile fills the focused field's form only:
 *     autocomplete-tagged and heuristic-only fields, selects (country by
 *     code or name, region), address lines; input/change events reach
 *     the page; hidden, off-screen and other-section fields are skipped;
 *   - the form-autofill action does the same;
 *   - focusing a recognised field opens the suggestion list with labels
 *     only; arrow keys move, Enter picks, Escape closes; picking fills;
 *   - the page's main world never sees the module, its message handler,
 *     the labels or any value before a fill;
 *   - card fields are refused on http://127.0.0.1 (the user is told) and
 *     filled on https;
 *   - an excluded host gets no effect at all.
 * Needs a display and a web process: GSURF_TEST_GUI=1 (make test-gui).
 */

#include <gsurf.h>
#include "webkit-test-util.h"
#include <glib/gstdio.h>
#include <string.h>

#define WORLD "gsurf-autofill"

static const gchar PAGE[] =
	"<!doctype html><html lang='en'><head><meta charset='utf-8'><title>forms</title>"
	"<style>input,select,textarea{display:block;width:12em;height:1.6em}</style>"
	"</head><body>"
	/* A: autocomplete-tagged, with traps that must stay empty. */
	"<form id='A'>"
	"<input id='a-given' autocomplete='given-name'>"
	"<input id='a-family' autocomplete='family-name'>"
	"<input id='a-name' autocomplete='name'>"
	"<input id='a-email' autocomplete='shipping email'>"
	"<input id='a-tel' autocomplete='tel'>"
	"<input id='a-org' autocomplete='organization'>"
	"<input id='a-line1' autocomplete='shipping address-line1'>"
	"<input id='a-line2' autocomplete='shipping address-line2'>"
	"<input id='a-city' autocomplete='shipping address-level2'>"
	"<select id='a-region' autocomplete='shipping address-level1'>"
	"<option value=''>-</option><option value='CA'>California</option>"
	"<option value='IL'>Illinois</option></select>"
	"<input id='a-zip' autocomplete='shipping postal-code'>"
	"<select id='a-country' autocomplete='shipping country'>"
	"<option value=''>-</option><option value='FR'>France</option>"
	"<option value='US'>United States</option></select>"
	"<input id='a-bill-city' autocomplete='billing address-level2'>"
	"<input id='a-hidden' type='hidden' autocomplete='email'>"
	"<input id='a-offscreen' autocomplete='tel' style='position:absolute;left:-9999px'>"
	"<input id='a-invisible' autocomplete='email' style='opacity:0'>"
	"<div style='display:none'><input id='a-nodisplay' autocomplete='email'></div>"
	"<input id='a-cc' autocomplete='cc-number'>"
	"</form>"
	/* B: heuristics only. */
	"<form id='B'>"
	"<input id='b-first' name='firstName'>"
	"<input id='b-last' name='billing_lastName'>"
	"<label for='b-mail'>E-mail</label><input id='b-mail' name='f3'>"
	"<input id='b-phone' placeholder='Phone number'>"
	"<input id='b-company' name='company'>"
	"<input id='b-addr' name='address1'>"
	"<input id='b-city' name='city'>"
	"<input id='b-zip' aria-label='ZIP code'>"
	"<select id='b-country' name='country'><option>Canada</option>"
	"<option>United States</option></select>"
	"<input id='b-user' name='username'>"
	"<input id='b-search' name='q' placeholder='Search'>"
	"</form>"
	/* C: card fields. */
	"<form id='C'>"
	"<input id='c-name' autocomplete='cc-name'>"
	"<input id='c-number' name='cardNumber'>"
	"<select id='c-month' autocomplete='cc-exp-month'><option value=''>MM</option>"
	"<option value='1'>01</option><option value='12'>12</option></select>"
	"<select id='c-year' autocomplete='cc-exp-year'><option value=''>YY</option>"
	"<option value='2030'>2030</option><option value='2031'>2031</option></select>"
	"<input id='c-exp' placeholder='MM / YY'>"
	"<input id='c-csc' autocomplete='cc-csc'>"
	"<input id='c-email' autocomplete='email'>"
	"</form>"
	"<script>"
	/* What the page (main world) observes: events and the values its
	 * listeners saw, like a framework's onChange would. */
	"window.seen = {};"
	"document.addEventListener('input', function (e) {"
	"  var s = seen[e.target.id] || (seen[e.target.id] = {input: 0, change: 0, value: null});"
	"  s.input++; s.value = e.target.value; }, true);"
	"document.addEventListener('change', function (e) {"
	"  var s = seen[e.target.id] || (seen[e.target.id] = {input: 0, change: 0, value: null});"
	"  s.change++; }, true);"
	/* A React-style instance value tracker on one field: the module must
	 * set through the prototype, so this setter never runs. */
	"window.trackerCalls = 0;"
	"(function () { var el = document.getElementById('b-first');"
	"  var d = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');"
	"  Object.defineProperty(el, 'value', { configurable: true,"
	"    get: function () { return d.get.call(this); },"
	"    set: function (v) { window.trackerCalls++; d.set.call(this, v); } }); })();"
	"</script></body></html>";

/* Values the page world may only ever see in filled fields. */
static const gchar PROFILES[] =
	"profiles:\n"
	"  - label: home\n"
	"    given_name: Zach\n"
	"    family_name: Tester\n"
	"    email: zach.secret@example.org\n"
	"    tel: '+1 555 0100'\n"
	"    organization: Hearth Ltd\n"
	"    street: ['742 Evergreen Terrace', 'Apt 9']\n"
	"    city: Springfield\n"
	"    region: IL\n"
	"    postal_code: '62704'\n"
	"    country: US\n"
	"    cc_name: Zach Q Cardholder\n"
	"    cc_number: '4111 1111 1111 1111'\n"
	"    cc_exp: '12/31'\n";

static const gchar PROFILE_WORK[] =
	"  - label: work\n"
	"    given_name: Zed\n"
	"    email: zed.work@example.com\n"
	"    organization: Example Corp\n";

typedef struct {
	SoupServer *server;
	guint16     port;
} TestServer;

static void
serve(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query,
      gpointer data)
{
	(void)server;
	(void)query;
	(void)data;
	if (!g_str_equal(path, "/form.html")) {
		soup_server_message_set_status(msg, 404, NULL);
		return;
	}
	soup_server_message_set_status(msg, 200, NULL);
	soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_STATIC,
		PAGE, strlen(PAGE));
}

/* A file under the source tree (walking up from the test binary). */
static gchar *
source_file(const gchar *relative)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);
	guint up;

	for (up = 0; up < 6; up++) {
		gchar *path = g_build_filename(dir, relative, NULL);
		gchar *parent;

		if (g_file_test(path, G_FILE_TEST_EXISTS))
			return path;
		g_free(path);
		parent = g_path_get_dirname(dir);
		g_free(dir);
		dir = parent;
	}
	return NULL;
}

static void
server_start(TestServer *ts, gboolean https)
{
	g_autoptr(GError) error = NULL;
	GSList *uris;

	if (https) {
		g_autofree gchar *pem = source_file("tests/fixtures/gemini-server.pem");
		g_autoptr(GTlsCertificate) cert = NULL;

		g_assert_nonnull(pem);
		cert = g_tls_certificate_new_from_file(pem, &error);
		g_assert_no_error(error);
		ts->server = soup_server_new("tls-certificate", cert, NULL);
	} else {
		ts->server = soup_server_new(NULL, NULL);
	}
	soup_server_add_handler(ts->server, NULL, serve, ts, NULL);
	g_assert_true(soup_server_listen_local(ts->server, 0,
		SOUP_SERVER_LISTEN_IPV4_ONLY | (https ? SOUP_SERVER_LISTEN_HTTPS : 0), &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(ts->server);
	ts->port = (guint16)g_uri_get_port(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
}

/* ------------------------------------------------------------------ */

typedef struct {
	GsurfModuleManager *mgr;
	GsurfView          *view;
	WebKitWebView      *native;
	TestServer          ts;
	gchar              *dir;
} Fixture;

static gchar *
eval_main(Fixture *f, const gchar *expr)
{
	return gsurf_webkit_test_eval(f->native, expr);
}

static gchar *
eval_world(Fixture *f, const gchar *expr)
{
	return gsurf_webkit_test_eval_in(f->native, WORLD, expr);
}

/* Poll (bounded) until @expr is "true" in @world (NULL = main). */
static void
wait_true(Fixture *f, const gchar *world, const gchar *expr)
{
	guint i;

	for (i = 0; i < 200; i++) {
		g_autofree gchar *v = gsurf_webkit_test_eval_in(f->native, world, expr);

		if (g_strcmp0(v, "true") == 0)
			return;
		gsurf_webkit_test_spin(25);
	}
	g_error("timed out waiting for %s in %s", expr, world != NULL ? world : "main");
}

static void
assert_main(Fixture *f, const gchar *expr, const gchar *want)
{
	g_autofree gchar *got = eval_main(f, expr);

	if (g_strcmp0(got, want) != 0)
		g_error("%s = %s, want %s", expr, got, want);
}

/* Main-world value of field @id. */
static void
assert_value(Fixture *f, const gchar *id, const gchar *want)
{
	g_autofree gchar *expr = g_strdup_printf("document.getElementById('%s').value", id);

	assert_main(f, expr, want);
}

/* The page saw at least one input and one change event on @id, and its
 * input listener saw the final value. */
static void
assert_events(Fixture *f, const gchar *id)
{
	g_autofree gchar *expr = g_strdup_printf(
		"(function(){var s=seen['%s'],e=document.getElementById('%s');"
		"return !!s&&s.input>0&&s.change>0&&s.value===e.value;})()", id, id);

	assert_main(f, expr, "true");
}

static gboolean
fixture_setup(Fixture *f, const gchar *profiles, const gchar *extra, gboolean https)
{
	g_autofree gchar *so = gsurf_webkit_test_module_path("form_autofill");
	g_autofree gchar *file = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr(GsurfConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	memset(f, 0, sizeof(*f));
	if (!gsurf_webkit_test_init())
		return FALSE;
	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("form_autofill.so not built");
		return FALSE;
	}

	/* A private profile file in a temp dir: card data allowed. */
	f->dir = g_dir_make_tmp("gsurf-autofill-XXXXXX", &error);
	g_assert_no_error(error);
	file = g_build_filename(f->dir, "autofill.yaml", NULL);
	g_assert_true(g_file_set_contents(file, profiles, -1, &error));
	g_assert_cmpint(g_chmod(file, 0600), ==, 0);
	yaml = g_strdup_printf("modules:\n  form_autofill:\n    enabled: true\n"
		"    file: %s\n%s", file, extra != NULL ? extra : "");

	config = gsurf_config_new();
	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	f->mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(f->mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(f->mgr, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(f->mgr);

	server_start(&f->ts, https);
	f->view = gsurf_view_new();
	f->native = gsurf_webkit_test_native(f->view);
	gsurf_module_manager_dispatch_inject_scripts(f->mgr, f->view, NULL);
	uri = g_strdup_printf("%s://127.0.0.1:%u/form.html", https ? "https" : "http",
		f->ts.port);
	gsurf_webkit_test_load(f->native, uri);
	return TRUE;
}

static void
fixture_teardown(Fixture *f)
{
	gpointer weak_view = f->view;

	if (f->view != NULL) {
		g_object_add_weak_pointer(G_OBJECT(f->view), &weak_view);
		g_object_unref(f->view);
		g_assert_null(weak_view);
	}
	g_clear_object(&f->mgr);
	g_clear_object(&f->ts.server);
	if (f->dir != NULL) {
		g_autofree gchar *file = g_build_filename(f->dir, "autofill.yaml", NULL);

		g_unlink(file);
		g_rmdir(f->dir);
		g_free(f->dir);
	}
}

static void
focus(Fixture *f, const gchar *id)
{
	g_autofree gchar *expr = g_strdup_printf(
		"document.getElementById('%s').focus();document.activeElement.id", id);
	g_autofree gchar *got = eval_main(f, expr);

	g_assert_cmpstr(got, ==, id);
}

static gboolean
press_fill_key(Fixture *f)
{
	return gsurf_module_manager_dispatch_key_event(f->mgr, f->view, GDK_KEY_f, 0,
		GSURF_MOD_CTRL | GSURF_MOD_ALT, GSURF_MODE_NORMAL);
}

/* No profile value (nor label) anywhere the page world can look. */
static void
assert_page_has_no_profile_data(Fixture *f)
{
	assert_main(f,
		"(function(){var t=document.documentElement.outerHTML+'\\n'+"
		"document.documentElement.innerText+'\\n'+document.documentElement.textContent;"
		"var bad=['Zach','zach.secret','Evergreen','Springfield','62704','4111',"
		"'Cardholder','Hearth','Zed','Example Corp'];"
		"for(var i=0;i<bad.length;i++)if(t.indexOf(bad[i])>=0)return bad[i];"
		"var f=document.querySelectorAll('input,select,textarea');"
		"for(i=0;i<f.length;i++)if(f[i].value&&f[i].tagName!=='SELECT')return f[i].id;"
		"return 'clean';})()", "clean");
}

/* ------------------------------------------------------------------ */

static void
test_key_fill_single_profile(void)
{
	Fixture f;

	if (!fixture_setup(&f, PROFILES, NULL, FALSE))
		return;
	wait_true(&f, WORLD, "__gsurfAutofill.state().armed");

	/* The page world sees neither the module nor its handler. */
	assert_main(&f, "typeof window.__gsurfAutofill", "undefined");
	assert_main(&f, "String(!!(window.webkit&&window.webkit.messageHandlers&&"
		"window.webkit.messageHandlers.gsurfAutofill))", "false");
	assert_page_has_no_profile_data(&f);

	/* Form A, autocomplete-tagged: one profile -> filled at once. */
	focus(&f, "a-given");
	g_assert_true(press_fill_key(&f));
	wait_true(&f, NULL, "document.getElementById('a-given').value==='Zach'");
	assert_value(&f, "a-family", "Tester");
	assert_value(&f, "a-name", "Zach Tester");
	assert_value(&f, "a-email", "zach.secret@example.org");
	assert_value(&f, "a-tel", "+1 555 0100");
	assert_value(&f, "a-org", "Hearth Ltd");
	assert_value(&f, "a-line1", "742 Evergreen Terrace");
	assert_value(&f, "a-line2", "Apt 9");
	assert_value(&f, "a-city", "Springfield");
	assert_value(&f, "a-region", "IL");
	assert_value(&f, "a-zip", "62704");
	assert_value(&f, "a-country", "US");
	assert_events(&f, "a-given");
	assert_events(&f, "a-country");
	assert_events(&f, "a-line2");
	/* Untagged-section focus fills every section of the form. */
	assert_value(&f, "a-bill-city", "Springfield");
	/* Hidden, off-screen and invisible fields stay empty; so does the
	 * card field of an address fill, and every other form. */
	assert_value(&f, "a-hidden", "");
	assert_value(&f, "a-offscreen", "");
	assert_value(&f, "a-invisible", "");
	assert_value(&f, "a-nodisplay", "");
	assert_value(&f, "a-cc", "");
	assert_value(&f, "b-first", "");
	assert_value(&f, "c-email", "");

	/* Form B, heuristics only, via the form-autofill action. */
	focus(&f, "b-mail");
	g_assert_true(gsurf_module_manager_dispatch_action(f.mgr, f.view,
		GSURF_ACTION_FORM_AUTOFILL));
	wait_true(&f, NULL, "document.getElementById('b-mail').value!==''");
	assert_value(&f, "b-first", "Zach");
	assert_value(&f, "b-last", "Tester");
	assert_value(&f, "b-mail", "zach.secret@example.org");
	assert_value(&f, "b-phone", "+1 555 0100");
	assert_value(&f, "b-company", "Hearth Ltd");
	/* No line-2 field: line 1 gets the whole street. */
	assert_value(&f, "b-addr", "742 Evergreen Terrace, Apt 9");
	assert_value(&f, "b-city", "Springfield");
	assert_value(&f, "b-zip", "62704");
	/* A select with names only matches the country code by name. */
	assert_value(&f, "b-country", "United States");
	assert_value(&f, "b-user", "");
	assert_value(&f, "b-search", "");
	/* React-style: the prototype setter was used, the page still saw
	 * the change through its event listeners. */
	assert_main(&f, "String(window.trackerCalls)", "0");
	assert_events(&f, "b-first");

	/* Form C on http: card fields refused, the user is told. */
	focus(&f, "c-number");
	g_assert_true(press_fill_key(&f));
	wait_true(&f, WORLD, "__gsurfAutofill.state().lastNotice==='insecure'");
	assert_value(&f, "c-name", "");
	assert_value(&f, "c-number", "");
	assert_value(&f, "c-month", "");
	assert_value(&f, "c-exp", "");
	assert_value(&f, "c-email", "");

	/* No focused field: nothing happens, the user is told. */
	eval_main(&f, "document.activeElement.blur()");
	g_assert_true(press_fill_key(&f));
	wait_true(&f, WORLD, "__gsurfAutofill.state().lastNotice==='nofield'");

	/* Other chords are not ours. */
	g_assert_false(gsurf_module_manager_dispatch_key_event(f.mgr, f.view, GDK_KEY_f, 0,
		GSURF_MOD_CTRL, GSURF_MODE_NORMAL));

	fixture_teardown(&f);
}

static void
test_suggest_and_pick(void)
{
	g_autofree gchar *profiles = g_strconcat(PROFILES, PROFILE_WORK, NULL);
	g_autofree gchar *state = NULL;
	Fixture f;

	if (!fixture_setup(&f, profiles, NULL, FALSE))
		return;
	wait_true(&f, WORLD, "__gsurfAutofill.state().armed");

	/* Focus opens the list: labels only, nothing selected. */
	focus(&f, "a-email");
	wait_true(&f, WORLD, "__gsurfAutofill.state().open");
	state = eval_world(&f, "JSON.stringify(__gsurfAutofill.state())");
	g_assert_nonnull(strstr(state, "\"list\":[\"home\",\"work\"]"));
	g_assert_nonnull(strstr(state, "\"sel\":-1"));
	/* The isolated world holds labels, never values. */
	g_assert_null(strstr(state, "zach.secret"));
	g_assert_null(strstr(state, "Evergreen"));

	/* The page sees an opaque host element: closed shadow root, no text. */
	assert_main(&f, "(function(){var h=document.querySelector('[data-gsurf-autofill]');"
		"return !!h&&h.shadowRoot===null&&h.childNodes.length===0;})()", "true");
	assert_page_has_no_profile_data(&f);
	assert_main(&f, "String(document.documentElement.innerText.indexOf('work'))", "-1");

	/* Keyboard: Down, Down, Up moves; Escape closes. */
	g_free(eval_world(&f, "__gsurfAutofill.key('ArrowDown')"));
	g_free(eval_world(&f, "__gsurfAutofill.key('ArrowDown')"));
	g_free(eval_world(&f, "__gsurfAutofill.key('ArrowDown')"));
	{
		g_autofree gchar *sel = eval_world(&f, "String(__gsurfAutofill.state().sel)");

		g_assert_cmpstr(sel, ==, "0");   /* wrapped around */
	}
	g_free(eval_world(&f, "__gsurfAutofill.key('ArrowUp')"));
	{
		g_autofree gchar *sel = eval_world(&f, "String(__gsurfAutofill.state().sel)");

		g_assert_cmpstr(sel, ==, "1");
	}
	{
		g_autofree gchar *esc = eval_world(&f, "String(__gsurfAutofill.key('Escape'))");

		g_assert_cmpstr(esc, ==, "true");
	}
	wait_true(&f, WORLD, "!__gsurfAutofill.state().open");
	assert_main(&f, "String(document.querySelector('[data-gsurf-autofill]'))", "null");

	/* Refocus another field: open again; Down, Down, Enter picks 'work'. */
	focus(&f, "a-org");
	wait_true(&f, WORLD, "__gsurfAutofill.state().open");
	g_free(eval_world(&f, "__gsurfAutofill.key('ArrowDown')"));
	g_free(eval_world(&f, "__gsurfAutofill.key('ArrowDown')"));
	g_free(eval_world(&f, "__gsurfAutofill.key('Enter')"));
	wait_true(&f, NULL, "document.getElementById('a-org').value==='Example Corp'");
	assert_value(&f, "a-email", "zed.work@example.com");
	assert_value(&f, "a-given", "Zed");
	/* 'work' has no street: those fields stay empty. */
	assert_value(&f, "a-line1", "");
	assert_events(&f, "a-org");

	/* Fill key with two profiles: the picker opens, first one selected;
	 * a click-equivalent choose(0) fills 'home' into form B. */
	focus(&f, "b-city");
	g_free(eval_world(&f, "__gsurfAutofill.key('Escape')"));
	g_assert_true(press_fill_key(&f));
	wait_true(&f, WORLD, "__gsurfAutofill.state().open&&__gsurfAutofill.state().sel===0");
	assert_value(&f, "b-city", "");
	g_free(eval_world(&f, "__gsurfAutofill.choose(0)"));
	wait_true(&f, NULL, "document.getElementById('b-city').value==='Springfield'");

	/* Card fields: only profiles with card data are offered. */
	focus(&f, "c-name");
	wait_true(&f, WORLD, "__gsurfAutofill.state().open&&"
		"JSON.stringify(__gsurfAutofill.state().list)==='[\"home\"]'");

	fixture_teardown(&f);
}

/* suggest_on_focus: false keeps the list closed; the key still works. */
static void
test_no_suggest(void)
{
	Fixture f;

	if (!fixture_setup(&f, PROFILES, "    suggest_on_focus: false\n", FALSE))
		return;
	wait_true(&f, WORLD, "__gsurfAutofill.state().armed");
	focus(&f, "a-city");
	gsurf_webkit_test_spin(300);
	assert_main(&f, "String(document.querySelector('[data-gsurf-autofill]'))", "null");
	g_assert_true(press_fill_key(&f));
	wait_true(&f, NULL, "document.getElementById('a-city').value==='Springfield'");
	fixture_teardown(&f);
}

/* An excluded host: never armed, no DOM, the key passes through. */
static void
test_excluded_host(void)
{
	g_autofree gchar *armed = NULL;
	Fixture f;

	if (!fixture_setup(&f, PROFILES, "    exclude: ['^127\\.0\\.0\\.1$']\n", FALSE))
		return;
	gsurf_webkit_test_spin(500);
	armed = eval_world(&f, "String(__gsurfAutofill.state().armed)");
	g_assert_cmpstr(armed, ==, "false");
	focus(&f, "a-given");
	gsurf_webkit_test_spin(300);
	assert_main(&f, "String(document.querySelector('[data-gsurf-autofill]'))", "null");
	g_assert_false(press_fill_key(&f));
	g_assert_false(gsurf_module_manager_dispatch_action(f.mgr, f.view,
		GSURF_ACTION_FORM_AUTOFILL));
	gsurf_webkit_test_spin(300);
	assert_page_has_no_profile_data(&f);
	fixture_teardown(&f);
}

/* https: card fields are filled (selects and a combined MM/YY field),
 * the security code never. */
static void
test_https_card(void)
{
	Fixture f;

	if (!fixture_setup(&f, PROFILES, NULL, TRUE))
		return;
	wait_true(&f, NULL, "location.protocol==='https:'&&window.isSecureContext");
	wait_true(&f, WORLD, "__gsurfAutofill.state().armed");
	focus(&f, "c-number");
	g_assert_true(press_fill_key(&f));
	wait_true(&f, NULL, "document.getElementById('c-number').value!==''");
	assert_value(&f, "c-number", "4111111111111111");
	assert_value(&f, "c-name", "Zach Q Cardholder");
	assert_value(&f, "c-month", "12");
	assert_value(&f, "c-year", "2031");
	assert_value(&f, "c-exp", "12/31");
	assert_value(&f, "c-csc", "");
	/* A card fill carries no address data. */
	assert_value(&f, "c-email", "");
	assert_events(&f, "c-number");
	fixture_teardown(&f);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/* The https fixture uses a self-signed certificate: the session (made
	 * with the first view) must not fail on it. */
	{
		g_autoptr(GsurfConfig) config = gsurf_config_new();

		config->tls_strict = FALSE;
		gsurf_config_set_default(config);
	}

	g_test_add_func("/form-autofill/webkit/key-fill", test_key_fill_single_profile);
	g_test_add_func("/form-autofill/webkit/suggest-pick", test_suggest_and_pick);
	g_test_add_func("/form-autofill/webkit/no-suggest", test_no_suggest);
	g_test_add_func("/form-autofill/webkit/excluded-host", test_excluded_host);
	g_test_add_func("/form-autofill/webkit/https-card", test_https_card);
	return g_test_run();
}
