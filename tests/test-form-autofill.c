/*
 * test-form-autofill.c - form_autofill module, headless
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Loads the real form_autofill.so and covers:
 *   - config defaults, every key, unknown keys, a second configure()
 *     (replaces state, including back to the defaults);
 *   - the profile file: default XDG path, explicit path, missing file,
 *     merge with inline profiles, duplicate labels, card data refused
 *     from a non-private file (mode, symlink) and from config.yaml;
 *   - validation: unknown keys, malformed values (Luhn, expiry, e-mail,
 *     country, ...) warn and drop only that field; cc_csc is refused;
 *   - exclude rules (and a bad rule failing closed);
 *   - the host <-> page protocol, through a headless GsurfView that
 *     records the scripts the module runs: arm carries labels only, a
 *     pick carries only the chosen part of one profile, card data is
 *     refused on http, excluded hosts and a disabled module do nothing;
 *   - the fill key (only its own binding) and the form-autofill action;
 *   - no profile value ever reaches the log;
 *   - the page-side classifier (js/form-autofill.js), evaluated in a
 *     bare JavaScriptCore context: autocomplete tokens first (sections,
 *     shipping/billing), then the heuristic table.
 */

#include <gsurf.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <jsc/jsc.h>
#include <string.h>
#include <unistd.h>

#define DOMAIN "gsurf-form-autofill"

/* Profile values used below; none of them may ever appear in a log. */
#define V_EMAIL   "zach.secret@example.org"
#define V_TEL     "+1 555 0100"
#define V_STREET1 "742 Evergreen Terrace"
#define V_STREET2 "Apt 9"
#define V_CARD    "4111 1111 1111 1111"
#define V_CARD_DIGITS "4111111111111111"
#define V_BADCARD "4111111111111112"
#define V_CCNAME  "Zach Q Cardholder"

/* ------------------------------------------------------------------ */
/* log capture                                                         */
/* ------------------------------------------------------------------ */

static GString *captured;

static void
capture_log(const gchar *domain, GLogLevelFlags level, const gchar *message,
            gpointer data)
{
	(void)domain;
	(void)level;
	(void)data;
	g_string_append(captured, message);
	g_string_append_c(captured, '\n');
}

static void
capture_reset(void)
{
	g_string_truncate(captured, 0);
}

static void
assert_logged(const gchar *needle)
{
	if (strstr(captured->str, needle) == NULL)
		g_error("expected '%s' in the log:\n%s", needle, captured->str);
}

/* No profile value in anything the module logged. */
static void
assert_no_values_logged(void)
{
	static const gchar *const secrets[] = {
		V_EMAIL, V_TEL, V_STREET1, V_STREET2, V_CARD, V_CARD_DIGITS, V_BADCARD,
		V_CCNAME, "4111", "Evergreen", "zach.secret", "Cardholder", "12/31", "2031",
		NULL
	};
	guint i;

	for (i = 0; secrets[i] != NULL; i++) {
		if (strstr(captured->str, secrets[i]) != NULL)
			g_error("profile value '%s' leaked into the log:\n%s",
			        secrets[i], captured->str);
	}
}

/* ------------------------------------------------------------------ */
/* module helpers                                                      */
/* ------------------------------------------------------------------ */

static gchar *
module_so_path(void)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);

	return g_build_filename(dir, "modules", "form_autofill.so", NULL);
}

static GsurfConfig *
config_from(const gchar *yaml)
{
	g_autoptr(GError) error = NULL;
	GsurfConfig *config = gsurf_config_new();

	g_assert_true(gsurf_config_load_from_data(config, yaml, -1, &error));
	g_assert_no_error(error);
	return config;
}

/* A manager with form_autofill loaded (configured) and activated. */
static GsurfModuleManager *
mgr_with(const gchar *yaml)
{
	g_autofree gchar *so = module_so_path();
	g_autoptr(GError) error = NULL;
	g_autoptr(GsurfConfig) config = NULL;
	GsurfModuleManager *mgr;

	if (!g_file_test(so, G_FILE_TEST_EXISTS)) {
		g_test_skip("form_autofill.so not built");
		return NULL;
	}
	config = config_from(yaml);
	mgr = gsurf_module_manager_new();
	gsurf_module_manager_set_config(mgr, config);
	g_assert_nonnull(gsurf_module_manager_load_module(mgr, so, &error));
	g_assert_no_error(error);
	gsurf_module_manager_activate_all(mgr);
	return mgr;
}

static GsurfModule *
module_of(GsurfModuleManager *mgr)
{
	GsurfModule *m = gsurf_module_manager_get_module(mgr, "form_autofill");

	g_assert_nonnull(m);
	return m;
}

static guint
n_profiles(GsurfModule *m)
{
	guint n = 0;

	g_object_get(m, "n-profiles", &n, NULL);
	return n;
}

/* The summary line ("label: field ...") of profile @i. */
static gchar *
summary_line(GsurfModule *m, guint i)
{
	g_auto(GStrv) summary = NULL;

	g_object_get(m, "summary", &summary, NULL);
	g_assert_nonnull(summary);
	g_assert_cmpuint(i, <, g_strv_length(summary));
	return g_strdup(summary[i]);
}

static void
write_file(const gchar *path, const gchar *contents, gint mode)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_path_get_dirname(path);

	g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
	g_assert_true(g_file_set_contents(path, contents, -1, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(path, mode), ==, 0);
}

static gchar *
tmp_path(const gchar *name)
{
	return g_build_filename(g_get_user_config_dir(), "gsurf-test", name, NULL);
}

static const gchar FILE_PROFILES[] =
	"profiles:\n"
	"  - label: home\n"
	"    given_name: Zach\n"
	"    family_name: Tester\n"
	"    email: " V_EMAIL "\n"
	"    tel: \"" V_TEL "\"\n"
	"    street: [\"" V_STREET1 "\", \"" V_STREET2 "\"]\n"
	"    city: Springfield\n"
	"    region: IL\n"
	"    postal_code: \"62704\"\n"
	"    country: us\n"
	"    cc_name: " V_CCNAME "\n"
	"    cc_number: \"" V_CARD "\"\n"
	"    cc_exp: \"12/31\"\n"
	"  - label: work\n"
	"    organization: Example Corp\n"
	"    email: work@example.com\n";

/* ------------------------------------------------------------------ */
/* headless view recording the module's scripts                        */
/* ------------------------------------------------------------------ */

typedef struct {
	GsurfView  parent;
	gchar     *uri;
	GPtrArray *scripts;     /* gchar*: every run_javascript_in_world */
	gchar     *world;       /* last world a script ran in */
	guint      n_user_scripts;
	gchar     *handler;     /* registered message handler name */
	gchar     *handler_world;
} FakeView;
typedef struct { GsurfViewClass parent; } FakeViewClass;

GType fake_view_get_type(void);
G_DEFINE_TYPE(FakeView, fake_view, GSURF_TYPE_VIEW)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FakeView, g_object_unref)

static const gchar *
fake_get_uri(GsurfView *view)
{
	return ((FakeView *)view)->uri;
}

static void
fake_register(GsurfView *view, const gchar *name, const gchar *world)
{
	FakeView *self = (FakeView *)view;

	g_free(self->handler);
	g_free(self->handler_world);
	self->handler = g_strdup(name);
	self->handler_world = g_strdup(world);
}

static void
fake_add_script(GsurfView *view, const gchar *source, gboolean at_end,
                const gchar *const *allow, const gchar *world)
{
	FakeView *self = (FakeView *)view;

	(void)allow;
	g_assert_nonnull(strstr(source, "__gsurfAutofill"));
	g_assert_true(at_end);
	g_assert_cmpstr(world, ==, "gsurf-autofill");
	self->n_user_scripts++;
}

static void
fake_run_in_world(GsurfView *view, const gchar *script, const gchar *world,
                  GCancellable *cancellable, GAsyncReadyCallback callback,
                  gpointer user_data)
{
	FakeView *self = (FakeView *)view;
	GTask *task = g_task_new(view, cancellable, callback, user_data);

	g_ptr_array_add(self->scripts, g_strdup(script));
	g_free(self->world);
	self->world = g_strdup(world);
	g_task_return_pointer(task, g_strdup("{\"filled\":1}"), g_free);
	g_object_unref(task);
}

static gchar *
fake_run_finish(GsurfView *view, GAsyncResult *result, GError **error)
{
	(void)view;
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
fake_finalize(GObject *object)
{
	FakeView *self = (FakeView *)object;

	g_free(self->uri);
	g_ptr_array_unref(self->scripts);
	g_free(self->world);
	g_free(self->handler);
	g_free(self->handler_world);
	G_OBJECT_CLASS(fake_view_parent_class)->finalize(object);
}

static void
fake_view_class_init(FakeViewClass *klass)
{
	GsurfViewClass *view_class = GSURF_VIEW_CLASS(klass);

	G_OBJECT_CLASS(klass)->finalize = fake_finalize;
	view_class->get_uri = fake_get_uri;
	view_class->register_script_message = fake_register;
	view_class->add_user_script_in_world = fake_add_script;
	view_class->run_javascript_in_world_async = fake_run_in_world;
	view_class->run_javascript_in_world_finish = fake_run_finish;
}

static void
fake_view_init(FakeView *self)
{
	self->scripts = g_ptr_array_new_with_free_func(g_free);
}

static FakeView *
fake_view_new(const gchar *uri)
{
	FakeView *view = g_object_new(fake_view_get_type(), NULL);

	view->uri = g_strdup(uri);
	return view;
}

static void
drain(void)
{
	while (g_main_context_iteration(NULL, FALSE))
		;
}

#define NONCE "0123456789abcdef0123456789abcdef"

static void
post(FakeView *view, const gchar *json)
{
	gsurf_view_emit_script_message(GSURF_VIEW(view), "gsurfAutofill", json);
	drain();
}

/* The JSON argument of the last script, which must call @fn. */
static JsonObject *
last_call(FakeView *view, const gchar *fn, JsonParser *parser)
{
	g_autofree gchar *prefix = g_strdup_printf(
		"window.__gsurfAutofill&&window.__gsurfAutofill.%s(", fn);
	const gchar *script;
	g_autofree gchar *arg = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_cmpuint(view->scripts->len, >, 0);
	script = g_ptr_array_index(view->scripts, view->scripts->len - 1);
	g_assert_true(g_str_has_prefix(script, prefix));
	g_assert_cmpstr(view->world, ==, "gsurf-autofill");
	arg = g_strndup(script + strlen(prefix), strlen(script) - strlen(prefix) - 1);
	g_assert_true(json_parser_load_from_data(parser, arg, -1, &error));
	g_assert_no_error(error);
	return json_node_get_object(json_parser_get_root(parser));
}

static gboolean
scripts_contain(FakeView *view, const gchar *needle)
{
	guint i;

	for (i = 0; i < view->scripts->len; i++) {
		if (strstr(g_ptr_array_index(view->scripts, i), needle) != NULL)
			return TRUE;
	}
	return FALSE;
}

/* A manager with the FILE_PROFILES private file and @extra config. */
static GsurfModuleManager *
mgr_with_profiles(const gchar *extra)
{
	g_autofree gchar *path = tmp_path("autofill.yaml");
	g_autofree gchar *yaml = NULL;

	write_file(path, FILE_PROFILES, 0600);
	yaml = g_strdup_printf("modules:\n  form_autofill:\n    enabled: true\n"
		"    file: %s\n%s", path, extra != NULL ? extra : "");
	return mgr_with(yaml);
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

static void
test_defaults(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with("modules:\n  form_autofill:\n    enabled: true\n");
	g_autofree gchar *key = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *want = NULL;
	gboolean suggest = FALSE, broken = TRUE;
	guint n_exclude = 1;
	GsurfModule *m;

	if (mgr == NULL)
		return;
	m = module_of(mgr);
	g_assert_true(gsurf_module_is_active(m));
	g_object_get(m, "key-fill", &key, "file", &file, "suggest-on-focus", &suggest,
		"n-exclude", &n_exclude, "exclude-broken", &broken, NULL);
	g_assert_cmpstr(key, ==, "Ctrl+Alt+f");
	want = g_build_filename(g_get_user_config_dir(), "gsurf", "autofill.yaml", NULL);
	g_assert_cmpstr(file, ==, want);
	g_assert_true(suggest);
	g_assert_cmpuint(n_exclude, ==, 0);
	g_assert_false(broken);
	g_assert_cmpuint(n_profiles(m), ==, 0);
	g_assert_cmpstr(gsurf_module_get_name(m), ==, "form_autofill");
}

static void
test_options_and_reconfigure(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GsurfConfig) back = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *file = NULL;
	gboolean suggest = TRUE;
	guint n_exclude = 0;
	GsurfModule *m;

	capture_reset();
	mgr = mgr_with("modules:\n  form_autofill:\n    enabled: true\n"
		"    file: ''\n"
		"    suggest_on_focus: false\n"
		"    key_fill: alt+ctrl+A\n"
		"    exclude: ['(^|\\.)paypal\\.com$', '^bank\\.example$']\n"
		"    bogus_option: 1\n"
		"    profiles:\n"
		"      - label: home\n"
		"        given_name: Zach\n");
	if (mgr == NULL)
		return;
	m = module_of(mgr);
	assert_logged("unknown key 'bogus_option'");
	g_object_get(m, "key-fill", &key, "file", &file, "suggest-on-focus", &suggest,
		"n-exclude", &n_exclude, NULL);
	g_assert_cmpstr(key, ==, "Ctrl+Alt+Shift+a");
	g_assert_null(file);
	g_assert_false(suggest);
	g_assert_cmpuint(n_exclude, ==, 2);
	g_assert_cmpuint(n_profiles(m), ==, 1);

	/* A second configure() with no section at all is back to defaults. */
	g_clear_pointer(&key, g_free);
	back = config_from("modules: {}\n");
	gsurf_module_configure(m, back);
	g_object_get(m, "key-fill", &key, "suggest-on-focus", &suggest,
		"n-exclude", &n_exclude, NULL);
	g_assert_cmpstr(key, ==, "Ctrl+Alt+f");
	g_assert_true(suggest);
	g_assert_cmpuint(n_exclude, ==, 0);
	g_assert_cmpuint(n_profiles(m), ==, 0);
}

static void
test_bad_key_and_exclude(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GsurfConfig) fixed = NULL;
	g_autofree gchar *key = NULL;
	gboolean broken = FALSE;
	GsurfModule *m;

	capture_reset();
	mgr = mgr_with("modules:\n  form_autofill:\n    enabled: true\n"
		"    key_fill: f\n"
		"    exclude: ['(unclosed']\n");
	if (mgr == NULL)
		return;
	m = module_of(mgr);
	assert_logged("needs a Ctrl, Alt or Super modifier");
	assert_logged("autofill is disabled on every host");
	g_object_get(m, "key-fill", &key, "exclude-broken", &broken, NULL);
	g_assert_null(key);
	g_assert_true(broken);

	/* Fixing the config clears the fail-closed state. */
	fixed = config_from("modules:\n  form_autofill:\n    exclude: ['ok\\.example']\n");
	gsurf_module_configure(m, fixed);
	g_object_get(m, "exclude-broken", &broken, NULL);
	g_assert_false(broken);
}

/* ------------------------------------------------------------------ */
/* profile file, merge, validation, permissions                        */
/* ------------------------------------------------------------------ */

static void
test_default_file(void)
{
	g_autofree gchar *path = g_build_filename(g_get_user_config_dir(), "gsurf",
		"autofill.yaml", NULL);
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autofree gchar *line = NULL;

	write_file(path, FILE_PROFILES, 0600);
	capture_reset();
	mgr = mgr_with("modules:\n  form_autofill:\n    enabled: true\n");
	if (mgr == NULL)
		return;
	g_assert_cmpuint(n_profiles(module_of(mgr)), ==, 2);
	line = summary_line(module_of(mgr), 0);
	g_assert_cmpstr(line, ==, "home: given_name family_name email tel city region "
		"postal_code country cc_name cc_number street[2] cc_exp");
	g_assert_null(strstr(captured->str, "refused"));
	assert_no_values_logged();
}

static void
test_missing_file(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;

	/* The default path is optional: silence. */
	capture_reset();
	mgr = mgr_with("modules:\n  form_autofill:\n    enabled: true\n");
	if (mgr == NULL)
		return;
	g_assert_cmpstr(captured->str, ==, "");
	g_clear_object(&mgr);

	/* A configured path that is missing is reported. */
	mgr = mgr_with("modules:\n  form_autofill:\n    enabled: true\n"
		"    file: /nonexistent/gsurf/autofill.yaml\n");
	assert_logged("does not exist");
	g_assert_cmpuint(n_profiles(module_of(mgr)), ==, 0);
}

static void
test_merge(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autofree gchar *home = NULL;
	g_autofree gchar *work = NULL;
	g_autofree gchar *trip = NULL;

	capture_reset();
	mgr = mgr_with_profiles(
		"    profiles:\n"
		"      - label: work\n"            /* overlays the file's 'work' */
		"        tel: '+1 555 0199'\n"
		"        email: boss@example.com\n"
		"      - label: trip\n"            /* new: appended */
		"        city: Paris\n"
		"        country: FR\n"
		"      - label: trip\n"            /* duplicate within one source */
		"        city: Lyon\n");
	if (mgr == NULL)
		return;
	g_assert_cmpuint(n_profiles(module_of(mgr)), ==, 3);
	home = summary_line(module_of(mgr), 0);
	work = summary_line(module_of(mgr), 1);
	trip = summary_line(module_of(mgr), 2);
	g_assert_true(g_str_has_prefix(home, "home:"));
	g_assert_cmpstr(work, ==, "work: email tel organization");
	/* The later duplicate replaced the earlier one wholesale. */
	g_assert_cmpstr(trip, ==, "trip: city");
	assert_logged("duplicate profile label 'trip'");
	assert_no_values_logged();
}

static void
test_validation(void)
{
	g_autofree gchar *path = tmp_path("bad.yaml");
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *line = NULL;
	g_autoptr(GsurfModuleManager) mgr = NULL;

	write_file(path,
		"profiles:\n"
		"  - label: bad\n"
		"    given_name: Zach\n"
		"    email: not-an-email\n"
		"    tel: call me\n"
		"    country: USA\n"
		"    postal_code: '!!'\n"
		"    street: [a, b, c, d]\n"
		"    cc_number: '" V_BADCARD "'\n"
		"    cc_exp: '13/31'\n"
		"    cc_csc: '123'\n"
		"    favourite_colour: blue\n"
		"    city: {nested: map}\n"
		"  - given_name: NoLabel\n"
		"  - just a string\n"
		"  - label: ok\n"
		"    cc_number: '" V_CARD "'\n"
		"    cc_exp: '1/2031'\n"
		"bogus_top: 1\n", 0600);
	yaml = g_strdup_printf("modules:\n  form_autofill:\n    enabled: true\n"
		"    file: %s\n", path);
	capture_reset();
	mgr = mgr_with(yaml);
	if (mgr == NULL)
		return;
	g_assert_cmpuint(n_profiles(module_of(mgr)), ==, 2);
	/* Only the valid field of 'bad' survives. */
	line = summary_line(module_of(mgr), 0);
	g_assert_cmpstr(line, ==, "bad: given_name");
	g_clear_pointer(&line, g_free);
	line = summary_line(module_of(mgr), 1);
	g_assert_cmpstr(line, ==, "ok: cc_number cc_exp");

	assert_logged("email is not a valid e-mail address");
	assert_logged("tel is not a telephone number");
	assert_logged("country is not a two-letter ISO 3166-1 country code");
	assert_logged("postal_code is not a postal code");
	assert_logged("street must have one to three lines");
	assert_logged("cc_number fails the card number (Luhn) checksum");
	assert_logged("cc_exp is not MM/YY or MM/YYYY");
	assert_logged("cc_csc is never stored");
	assert_logged("unknown key 'favourite_colour'");
	assert_logged("city must be a string");
	assert_logged("profile 2 has no label");
	assert_logged("profile 3 is not a mapping");
	assert_logged("unknown key 'bogus_top'");
	assert_no_values_logged();
	g_assert_null(strstr(captured->str, "not-an-email"));
	g_assert_null(strstr(captured->str, "call me"));
}

/* Card data from a group/world-readable file (or a symlink) is refused;
 * the rest of the file is still used. */
static void
test_permissions(void)
{
	g_autofree gchar *path = tmp_path("open.yaml");
	g_autofree gchar *link = tmp_path("link.yaml");
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *line = NULL;
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autoptr(GsurfConfig) config = NULL;

	write_file(path, FILE_PROFILES, 0644);
	yaml = g_strdup_printf("modules:\n  form_autofill:\n    enabled: true\n"
		"    file: %s\n", path);
	capture_reset();
	mgr = mgr_with(yaml);
	if (mgr == NULL)
		return;
	assert_logged("card data in profile file");
	assert_logged("refused");
	assert_logged("chmod 600");
	line = summary_line(module_of(mgr), 0);
	g_assert_cmpstr(line, ==, "home: given_name family_name email tel city region "
		"postal_code country street[2]");
	assert_no_values_logged();

	/* chmod 600 + reconfigure: card data accepted. */
	g_assert_cmpint(g_chmod(path, 0600), ==, 0);
	config = config_from(yaml);
	capture_reset();
	gsurf_module_configure(module_of(mgr), config);
	g_clear_pointer(&line, g_free);
	line = summary_line(module_of(mgr), 0);
	g_assert_nonnull(strstr(line, "cc_number"));
	g_assert_null(strstr(captured->str, "refused"));

	/* A symlink to the private file is not itself a private file. */
	g_assert_cmpint(symlink(path, link), ==, 0);
	g_clear_object(&config);
	g_free(yaml);
	yaml = g_strdup_printf("modules:\n  form_autofill:\n    file: %s\n", link);
	config = config_from(yaml);
	capture_reset();
	gsurf_module_configure(module_of(mgr), config);
	assert_logged("not a regular file");
	g_clear_pointer(&line, g_free);
	line = summary_line(module_of(mgr), 0);
	g_assert_null(strstr(line, "cc_number"));
	assert_no_values_logged();
}

/* Card data never comes from config.yaml. */
static void
test_inline_card_refused(void)
{
	g_autoptr(GsurfModuleManager) mgr = NULL;
	g_autofree gchar *line = NULL;

	capture_reset();
	mgr = mgr_with("modules:\n  form_autofill:\n    enabled: true\n    file: ''\n"
		"    profiles:\n"
		"      - label: home\n"
		"        given_name: Zach\n"
		"        cc_name: " V_CCNAME "\n"
		"        cc_number: '" V_CARD "'\n"
		"        cc_exp: '12/31'\n");
	if (mgr == NULL)
		return;
	assert_logged("only read from the private profile file");
	line = summary_line(module_of(mgr), 0);
	g_assert_cmpstr(line, ==, "home: given_name");
	assert_no_values_logged();
}

/* ------------------------------------------------------------------ */
/* host <-> page protocol                                              */
/* ------------------------------------------------------------------ */

static void
test_inject(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with_profiles(NULL);
	g_autoptr(FakeView) view = NULL;

	if (mgr == NULL)
		return;
	view = fake_view_new("https://shop.example/");
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);
	/* A second inject for the same view is a no-op. */
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);
	g_assert_cmpuint(view->n_user_scripts, ==, 1);
	g_assert_cmpstr(view->handler, ==, "gsurfAutofill");
	g_assert_cmpstr(view->handler_world, ==, "gsurf-autofill");
	g_assert_cmpuint(view->scripts->len, ==, 0);
}

static void
test_arm_labels_only(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with_profiles(NULL);
	g_autoptr(FakeView) view = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *arg;
	JsonArray *labels, *card;

	if (mgr == NULL)
		return;
	view = fake_view_new("https://shop.example/cart");
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);
	post(view, "{\"type\":\"ready\",\"nonce\":\"" NONCE "\"}");

	arg = last_call(view, "arm", parser);
	g_assert_cmpstr(json_object_get_string_member(arg, "nonce"), ==, NONCE);
	g_assert_cmpstr(json_object_get_string_member(arg, "host"), ==, "shop.example");
	g_assert_true(json_object_get_boolean_member(arg, "suggest"));
	labels = json_object_get_array_member(arg, "labels");
	card = json_object_get_array_member(arg, "card");
	g_assert_cmpuint(json_array_get_length(labels), ==, 2);
	g_assert_cmpstr(json_array_get_string_element(labels, 0), ==, "home");
	g_assert_cmpstr(json_array_get_string_element(labels, 1), ==, "work");
	g_assert_true(json_array_get_boolean_element(card, 0));
	g_assert_false(json_array_get_boolean_element(card, 1));

	/* Arming sends labels, never a value. */
	g_assert_cmpuint(view->scripts->len, ==, 1);
	g_assert_false(scripts_contain(view, "Evergreen"));
	g_assert_false(scripts_contain(view, "4111"));
	g_assert_false(scripts_contain(view, "zach.secret"));
	g_assert_false(scripts_contain(view, "Springfield"));
}

static void
test_bad_messages_ignored(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with_profiles(NULL);
	g_autoptr(FakeView) view = NULL;

	if (mgr == NULL)
		return;
	view = fake_view_new("https://shop.example/");
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);
	post(view, "not json");
	post(view, "\"a string\"");
	post(view, "{\"type\":\"ready\"}");
	post(view, "{\"type\":\"ready\",\"nonce\":\"short\"}");
	post(view, "{\"type\":\"ready\",\"nonce\":\"0123456789ABCDEF0123456789ABCDEF\"}");
	post(view, "{\"type\":\"pick\",\"nonce\":\"" NONCE "\",\"label\":\"home\"}");
	post(view, "{\"type\":\"pick\",\"nonce\":\"" NONCE "\",\"label\":\"home\",\"kind\":\"x\"}");
	post(view, "{\"type\":\"pick\",\"nonce\":\"" NONCE "\",\"kind\":\"card\"}");
	post(view, "{\"type\":\"other\",\"nonce\":\"" NONCE "\"}");
	g_assert_cmpuint(view->scripts->len, ==, 0);
}

static void
test_pick_address(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with_profiles(NULL);
	g_autoptr(FakeView) view = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *arg, *values;
	JsonArray *street;

	if (mgr == NULL)
		return;
	view = fake_view_new("http://shop.example/");
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);
	capture_reset();
	post(view, "{\"type\":\"pick\",\"nonce\":\"" NONCE "\",\"label\":\"home\",\"kind\":\"address\"}");
	arg = last_call(view, "fill", parser);
	g_assert_cmpstr(json_object_get_string_member(arg, "kind"), ==, "address");
	g_assert_false(json_object_has_member(arg, "refused"));
	values = json_object_get_object_member(arg, "values");
	g_assert_cmpstr(json_object_get_string_member(values, "given_name"), ==, "Zach");
	g_assert_cmpstr(json_object_get_string_member(values, "email"), ==, V_EMAIL);
	g_assert_cmpstr(json_object_get_string_member(values, "country"), ==, "US");
	g_assert_cmpstr(json_object_get_string_member(values, "postal_code"), ==, "62704");
	street = json_object_get_array_member(values, "street");
	g_assert_cmpuint(json_array_get_length(street), ==, 2);
	g_assert_cmpstr(json_array_get_string_element(street, 0), ==, V_STREET1);
	/* An address pick carries no card data at all. */
	g_assert_false(json_object_has_member(values, "cc_number"));
	g_assert_false(json_object_has_member(values, "cc_name"));
	g_assert_false(json_object_has_member(values, "cc_exp_month"));
	assert_no_values_logged();
}

static void
test_pick_card(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with_profiles(NULL);
	g_autoptr(FakeView) view = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *arg, *values;

	if (mgr == NULL)
		return;
	view = fake_view_new("https://shop.example/pay");
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);
	capture_reset();

	/* https: only the card part, the number as bare digits. */
	post(view, "{\"type\":\"pick\",\"nonce\":\"" NONCE "\",\"label\":\"home\",\"kind\":\"card\"}");
	arg = last_call(view, "fill", parser);
	values = json_object_get_object_member(arg, "values");
	g_assert_cmpstr(json_object_get_string_member(values, "cc_number"), ==, V_CARD_DIGITS);
	g_assert_cmpstr(json_object_get_string_member(values, "cc_name"), ==, V_CCNAME);
	g_assert_cmpint(json_object_get_int_member(values, "cc_exp_month"), ==, 12);
	g_assert_cmpint(json_object_get_int_member(values, "cc_exp_year"), ==, 2031);
	g_assert_false(json_object_has_member(values, "email"));
	g_assert_false(json_object_has_member(values, "street"));

	/* A profile without card data: refused, nothing sent. */
	post(view, "{\"type\":\"pick\",\"nonce\":\"" NONCE "\",\"label\":\"work\",\"kind\":\"card\"}");
	arg = last_call(view, "fill", parser);
	g_assert_cmpstr(json_object_get_string_member(arg, "refused"), ==, "nocard");
	g_assert_false(json_object_has_member(arg, "values"));

	/* An unknown label (stale page): refused. */
	post(view, "{\"type\":\"pick\",\"nonce\":\"" NONCE "\",\"label\":\"gone\",\"kind\":\"address\"}");
	arg = last_call(view, "fill", parser);
	g_assert_cmpstr(json_object_get_string_member(arg, "refused"), ==, "unknown");

	/* http: card refused, the user is told, and the log says why. */
	g_free(view->uri);
	view->uri = g_strdup("http://shop.example/pay");
	post(view, "{\"type\":\"pick\",\"nonce\":\"" NONCE "\",\"label\":\"home\",\"kind\":\"card\"}");
	arg = last_call(view, "fill", parser);
	g_assert_cmpstr(json_object_get_string_member(arg, "refused"), ==, "insecure");
	g_assert_false(json_object_has_member(arg, "values"));
	g_assert_null(strstr(g_ptr_array_index(view->scripts, view->scripts->len - 1), "4111"));
	assert_logged("refused to fill card details on shop.example");
	assert_no_values_logged();
}

/* Excluded hosts, non-http(s) pages and an inactive module: no reply. */
static void
test_excluded_and_disabled(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with_profiles(
		"    exclude: ['(^|\\.)paypal\\.com$']\n");
	g_autoptr(GsurfModuleManager) off = NULL;
	g_autoptr(FakeView) view = NULL;
	g_autoptr(FakeView) file_view = NULL;
	g_autoptr(FakeView) off_view = NULL;
	g_autofree gchar *path = tmp_path("autofill.yaml");
	g_autofree gchar *yaml = NULL;
	GsurfModule *m;

	if (mgr == NULL)
		return;
	view = fake_view_new("https://www.paypal.com/checkout");
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);
	post(view, "{\"type\":\"ready\",\"nonce\":\"" NONCE "\"}");
	post(view, "{\"type\":\"pick\",\"nonce\":\"" NONCE "\",\"label\":\"home\",\"kind\":\"address\"}");
	g_assert_false(gsurf_module_manager_dispatch_key_event(mgr, GSURF_VIEW(view),
		'f', 0, GSURF_MOD_CTRL | GSURF_MOD_ALT, GSURF_MODE_NORMAL));
	g_assert_false(gsurf_module_manager_dispatch_action(mgr, GSURF_VIEW(view),
		GSURF_ACTION_FORM_AUTOFILL));
	g_assert_cmpuint(view->scripts->len, ==, 0);

	/* Not excluded: the subdomain rule does not match a lookalike. */
	g_free(view->uri);
	view->uri = g_strdup("https://notpaypal.com.example/");
	post(view, "{\"type\":\"ready\",\"nonce\":\"" NONCE "\"}");
	g_assert_cmpuint(view->scripts->len, ==, 1);

	/* Pages without a network host get nothing. */
	file_view = fake_view_new("file:///tmp/form.html");
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(file_view), NULL);
	post(file_view, "{\"type\":\"ready\",\"nonce\":\"" NONCE "\"}");
	g_assert_cmpuint(file_view->scripts->len, ==, 0);

	/* Deactivated module: injected views go quiet. */
	m = module_of(mgr);
	gsurf_module_deactivate(m);
	g_free(view->uri);
	view->uri = g_strdup("https://shop.example/");
	post(view, "{\"type\":\"ready\",\"nonce\":\"" NONCE "\"}");
	g_assert_cmpuint(view->scripts->len, ==, 1);

	/* A module loaded with enabled: false is never active. */
	yaml = g_strdup_printf("modules:\n  form_autofill:\n    enabled: false\n"
		"    file: %s\n", path);
	off = mgr_with(yaml);
	off_view = fake_view_new("https://shop.example/");
	gsurf_script_injector_inject(GSURF_SCRIPT_INJECTOR(module_of(off)),
		GSURF_VIEW(off_view), NULL);
	post(off_view, "{\"type\":\"ready\",\"nonce\":\"" NONCE "\"}");
	g_assert_cmpuint(off_view->scripts->len, ==, 0);
	g_assert_false(gsurf_module_manager_dispatch_key_event(off, GSURF_VIEW(off_view),
		'f', 0, GSURF_MOD_CTRL | GSURF_MOD_ALT, GSURF_MODE_NORMAL));
}

static void
test_key_and_action(void)
{
	g_autoptr(GsurfModuleManager) mgr = mgr_with_profiles(NULL);
	g_autoptr(FakeView) view = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GPtrArray) help = NULL;
	JsonObject *arg;
	GsurfKeybindHelp *entry;

	if (mgr == NULL)
		return;
	view = fake_view_new("https://shop.example/");
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);

	/* Typing (bare keys, Shift) and other chords are never consumed. */
	g_assert_false(gsurf_module_manager_dispatch_key_event(mgr, GSURF_VIEW(view),
		'f', 0, 0, GSURF_MODE_NORMAL));
	g_assert_false(gsurf_module_manager_dispatch_key_event(mgr, GSURF_VIEW(view),
		'F', 0, GSURF_MOD_SHIFT, GSURF_MODE_INSERT));
	g_assert_false(gsurf_module_manager_dispatch_key_event(mgr, GSURF_VIEW(view),
		'f', 0, GSURF_MOD_CTRL, GSURF_MODE_NORMAL));
	g_assert_cmpuint(view->scripts->len, ==, 0);

	/* Its own binding triggers the page. */
	g_assert_true(gsurf_module_manager_dispatch_key_event(mgr, GSURF_VIEW(view),
		'f', 0, GSURF_MOD_CTRL | GSURF_MOD_ALT, GSURF_MODE_NORMAL));
	drain();
	arg = last_call(view, "trigger", parser);
	g_assert_cmpstr(json_object_get_string_member(arg, "host"), ==, "shop.example");

	g_assert_true(gsurf_module_manager_dispatch_action(mgr, GSURF_VIEW(view),
		GSURF_ACTION_FORM_AUTOFILL));
	drain();
	g_assert_cmpuint(view->scripts->len, ==, 2);
	g_assert_false(gsurf_module_manager_dispatch_action(mgr, GSURF_VIEW(view),
		GSURF_ACTION_RELOAD));

	/* The keybinding overlay lists the fill key. */
	help = g_ptr_array_new_with_free_func((GDestroyNotify)gsurf_keybind_help_free);
	gsurf_keybind_provider_list_keybinds(GSURF_KEYBIND_PROVIDER(module_of(mgr)), help);
	g_assert_cmpuint(help->len, ==, 1);
	entry = g_ptr_array_index(help, 0);
	g_assert_cmpstr(gsurf_keybind_help_get_key(entry), ==, "Ctrl+Alt+f");
	g_assert_cmpstr(gsurf_keybind_help_get_action(entry), ==, "form-autofill");
}

/* Lifetime: views and the module die normally; a view outliving the
 * module stops delivering messages to it. */
static void
test_lifetime(void)
{
	GsurfModuleManager *mgr = mgr_with_profiles(NULL);
	FakeView *view;
	gpointer weak_module = NULL;
	gpointer weak_view = NULL;

	if (mgr == NULL)
		return;
	view = fake_view_new("https://shop.example/");
	gsurf_module_manager_dispatch_inject_scripts(mgr, GSURF_VIEW(view), NULL);
	weak_module = module_of(mgr);
	g_object_add_weak_pointer(G_OBJECT(weak_module), &weak_module);
	g_object_unref(mgr);
	g_assert_null(weak_module);

	post(view, "{\"type\":\"ready\",\"nonce\":\"" NONCE "\"}");
	g_assert_cmpuint(view->scripts->len, ==, 0);
	weak_view = view;
	g_object_add_weak_pointer(G_OBJECT(view), &weak_view);
	g_object_unref(view);
	g_assert_null(weak_view);
}

/* ------------------------------------------------------------------ */
/* the page-side classifier in JavaScriptCore                          */
/* ------------------------------------------------------------------ */

/* js/form-autofill.js from the source tree (walk up from the binary). */
static gchar *
read_page_script(void)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);
	gchar *contents = NULL;
	guint up;

	for (up = 0; up < 6; up++) {
		g_autofree gchar *path = g_build_filename(dir, "modules", "form_autofill",
			"js", "form-autofill.js", NULL);
		gchar *parent;

		if (g_file_get_contents(path, &contents, NULL, NULL))
			return contents;
		parent = g_path_get_dirname(dir);
		g_free(dir);
		dir = parent;
	}
	return NULL;
}

typedef struct {
	const gchar *desc;   /* JS object literal */
	const gchar *field;  /* expected field, or "null" */
	const gchar *group;
} ClassifyCase;

static const ClassifyCase CLASSIFY_CASES[] = {
	/* autocomplete is authoritative, with section/shipping/billing and
	 * contact-type tokens. */
	{ "{tag:'INPUT',autocomplete:'given-name',name:'email'}", "given_name", "" },
	{ "{tag:'INPUT',autocomplete:'section-a shipping street-address'}", "street", "section-a|shipping" },
	{ "{tag:'INPUT',autocomplete:'billing postal-code'}", "postal_code", "|billing" },
	{ "{tag:'INPUT',autocomplete:'shipping work tel'}", "tel", "|shipping" },
	{ "{tag:'INPUT',autocomplete:'section-x  BILLING   Address-Line2'}", "address_line2", "section-x|billing" },
	{ "{tag:'INPUT',autocomplete:'email webauthn'}", "email", "" },
	{ "{tag:'INPUT',autocomplete:'cc-exp-month'}", "cc_exp_month", "" },
	{ "{tag:'INPUT',autocomplete:'cc-csc',name:'card'}", "cc_csc", "" },
	{ "{tag:'SELECT',autocomplete:'country'}", "country", "" },
	{ "{tag:'INPUT',autocomplete:'country-name'}", "country_name", "" },
	/* valid tokens we never fill are final: no heuristics. */
	{ "{tag:'INPUT',autocomplete:'username',name:'email'}", "null", "" },
	{ "{tag:'INPUT',autocomplete:'one-time-code',name:'phone'}", "null", "" },
	/* off/on and junk fall back to heuristics. */
	{ "{tag:'INPUT',autocomplete:'off',name:'firstName'}", "given_name", "" },
	{ "{tag:'INPUT',autocomplete:'nonsense',name:'zip'}", "postal_code", "" },
	{ "{tag:'INPUT',autocomplete:'shipping bogus given-name',name:'zip'}", "postal_code", "" },
	/* heuristics: name, id, label, placeholder, aria-label in order. */
	{ "{tag:'INPUT',name:'billing_firstName'}", "given_name", "" },
	{ "{tag:'INPUT',name:'lname'}", "family_name", "" },
	{ "{tag:'INPUT',name:'surname'}", "family_name", "" },
	{ "{tag:'INPUT',name:'fullName'}", "name", "" },
	{ "{tag:'INPUT',name:'name'}", "name", "" },
	{ "{tag:'INPUT',name:'f7',label:'E-mail'}", "email", "" },
	{ "{tag:'INPUT',type:'email'}", "email", "" },
	{ "{tag:'INPUT',type:'tel',name:'x'}", "tel", "" },
	{ "{tag:'INPUT',id:'phone-number'}", "tel", "" },
	{ "{tag:'INPUT',name:'company_name'}", "organization", "" },
	{ "{tag:'INPUT',name:'address1'}", "address_line1", "" },
	{ "{tag:'INPUT',name:'address_line_2'}", "address_line2", "" },
	{ "{tag:'INPUT',label:'Apartment, suite, etc.'}", "address_line2", "" },
	{ "{tag:'INPUT',name:'street'}", "address_line1", "" },
	{ "{tag:'TEXTAREA',name:'address'}", "street", "" },
	{ "{tag:'INPUT',name:'city'}", "city", "" },
	{ "{tag:'INPUT',name:'billing_state'}", "region", "" },
	{ "{tag:'SELECT',name:'province'}", "region", "" },
	{ "{tag:'INPUT',placeholder:'ZIP code'}", "postal_code", "" },
	{ "{tag:'INPUT',name:'postcode'}", "postal_code", "" },
	{ "{tag:'SELECT',name:'country'}", "country", "" },
	{ "{tag:'INPUT',aria:'Name on card'}", "cc_name", "" },
	{ "{tag:'INPUT',name:'cardholderName'}", "cc_name", "" },
	{ "{tag:'INPUT',name:'cardNumber'}", "cc_number", "" },
	{ "{tag:'INPUT',type:'tel',label:'Card number'}", "cc_number", "" },
	{ "{tag:'INPUT',name:'cc-num'}", "cc_number", "" },
	{ "{tag:'INPUT',placeholder:'MM / YY'}", "cc_exp", "" },
	{ "{tag:'INPUT',name:'expiry'}", "cc_exp", "" },
	{ "{tag:'INPUT',label:'Expiration date'}", "cc_exp", "" },
	{ "{tag:'SELECT',name:'exp_month'}", "cc_exp_month", "" },
	{ "{tag:'SELECT',name:'expYear'}", "cc_exp_year", "" },
	{ "{tag:'INPUT',name:'cvv'}", "cc_csc", "" },
	{ "{tag:'INPUT',label:'Security code'}", "cc_csc", "" },
	/* negatives and non-fields. */
	{ "{tag:'INPUT',name:'username'}", "null", "" },
	{ "{tag:'INPUT',name:'login_email'}", "null", "" },
	{ "{tag:'INPUT',name:'q',label:'Search'}", "null", "" },
	{ "{tag:'INPUT',name:'coupon_code'}", "null", "" },
	{ "{tag:'INPUT',name:'giftCardNumber'}", "null", "" },
	{ "{tag:'INPUT',type:'password',name:'email'}", "null", "" },
	{ "{tag:'INPUT',type:'hidden',name:'email'}", "null", "" },
	{ "{tag:'INPUT',type:'checkbox',name:'email'}", "null", "" },
	{ "{tag:'INPUT',type:'search',name:'city'}", "null", "" },
	{ "{tag:'BUTTON',name:'email'}", "null", "" },
	{ "{tag:'INPUT',name:'comment'}", "null", "" },
	{ "{tag:'INPUT'}", "null", "" },
};

static gchar *
js_string(JSCContext *ctx, const gchar *code)
{
	g_autoptr(JSCValue) value = jsc_context_evaluate(ctx, code, -1);
	g_autoptr(JSCException) exc = jsc_context_get_exception(ctx);

	if (exc != NULL)
		g_error("JavaScript exception: %s", jsc_exception_get_message(exc));
	return jsc_value_to_string(value);
}

static void
test_classifier(void)
{
	g_autofree gchar *source = read_page_script();
	g_autoptr(JSCContext) ctx = NULL;
	g_autoptr(JSCValue) loaded = NULL;
	g_autofree gchar *type = NULL;
	guint i;

	if (source == NULL) {
		g_test_skip("modules/form_autofill/js/form-autofill.js not found");
		return;
	}
	ctx = jsc_context_new();
	loaded = jsc_context_evaluate(ctx, source, -1);
	g_assert_null(jsc_context_get_exception(ctx));
	/* Without a DOM only the core is defined; the DOM side returned. */
	type = js_string(ctx, "typeof __gsurfAutofillCore.classify + ',' + typeof __gsurfAutofill");
	g_assert_cmpstr(type, ==, "function,undefined");

	for (i = 0; i < G_N_ELEMENTS(CLASSIFY_CASES); i++) {
		g_autofree gchar *code = g_strdup_printf(
			"(function(){var r=__gsurfAutofillCore.classify(%s);"
			"return String(r.field)+'|'+r.group;})()", CLASSIFY_CASES[i].desc);
		g_autofree gchar *got = js_string(ctx, code);
		g_autofree gchar *want = g_strdup_printf("%s|%s", CLASSIFY_CASES[i].field,
			CLASSIFY_CASES[i].group);

		if (g_strcmp0(got, want) != 0)
			g_error("classify(%s) = %s, want %s", CLASSIFY_CASES[i].desc, got, want);
	}
}

static void
test_expiry_format(void)
{
	g_autofree gchar *source = read_page_script();
	g_autoptr(JSCContext) ctx = NULL;
	g_autoptr(JSCValue) loaded = NULL;
	g_autofree gchar *got = NULL;

	if (source == NULL) {
		g_test_skip("modules/form_autofill/js/form-autofill.js not found");
		return;
	}
	ctx = jsc_context_new();
	loaded = jsc_context_evaluate(ctx, source, -1);
	got = js_string(ctx, "var f=__gsurfAutofillCore.formatExpiry;"
		"[f(3,2031,'MM/YY',0),f(3,2031,'MM / YYYY',0),f(12,2031,'',7),"
		"f(1,2030,'mm-yy',5),f(1,2030,null,-1)].join(' ')");
	g_assert_cmpstr(got, ==, "03/31 03/2031 12/2031 01-30 01/30");
}

int
main(int argc, char *argv[])
{
	gint status;

	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	captured = g_string_new(NULL);
	/* Every level, debug included (handlers see it regardless of
	 * G_MESSAGES_DEBUG), so the no-values check covers all of it. */
	g_log_set_handler(DOMAIN, G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION,
		capture_log, NULL);
	/* The module's warnings are expected here: record them, do not
	 * abort. g_test_init() makes warnings always-fatal; keep criticals
	 * (and errors) fatal, everywhere. */
	g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

	g_test_add_func("/form-autofill/config/defaults", test_defaults);
	g_test_add_func("/form-autofill/config/options-reconfigure", test_options_and_reconfigure);
	g_test_add_func("/form-autofill/config/bad-key-exclude", test_bad_key_and_exclude);
	g_test_add_func("/form-autofill/file/default-path", test_default_file);
	g_test_add_func("/form-autofill/file/missing", test_missing_file);
	g_test_add_func("/form-autofill/file/merge", test_merge);
	g_test_add_func("/form-autofill/file/validation", test_validation);
	g_test_add_func("/form-autofill/file/permissions", test_permissions);
	g_test_add_func("/form-autofill/file/inline-card-refused", test_inline_card_refused);
	g_test_add_func("/form-autofill/page/inject", test_inject);
	g_test_add_func("/form-autofill/page/arm-labels-only", test_arm_labels_only);
	g_test_add_func("/form-autofill/page/bad-messages", test_bad_messages_ignored);
	g_test_add_func("/form-autofill/page/pick-address", test_pick_address);
	g_test_add_func("/form-autofill/page/pick-card", test_pick_card);
	g_test_add_func("/form-autofill/page/excluded-disabled", test_excluded_and_disabled);
	g_test_add_func("/form-autofill/page/key-action", test_key_and_action);
	g_test_add_func("/form-autofill/page/lifetime", test_lifetime);
	g_test_add_func("/form-autofill/js/classifier", test_classifier);
	g_test_add_func("/form-autofill/js/expiry-format", test_expiry_format);

	status = g_test_run();
	g_string_free(captured, TRUE);
	return status;
}
