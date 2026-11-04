/*
 * test-host-rules.c - Per-host regex rules and the private-file helpers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gsurf.h>
#include <yaml-glib.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "test-util.h"

/* Parse a YAML snippet and return its root node (owned by the parser). */
static YamlNode *
parse_root(YamlParser *parser, const gchar *yaml)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(yaml_parser_load_from_data(parser, yaml, -1, &error));
	g_assert_no_error(error);
	return yaml_parser_get_root(parser);
}

/* First match wins, matching is case-insensitive and not implicitly
 * anchored, and NULL/empty hosts never match. */
static void
test_match_order(void)
{
	g_autoptr(GsurfHostRules) rules = gsurf_host_rules_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(gsurf_host_rules_add(rules, "(^|\\.)youtube\\.com$", NULL, &error));
	g_assert_true(gsurf_host_rules_add(rules, "tube", NULL, &error));
	g_assert_no_error(error);

	g_assert_cmpuint(gsurf_host_rules_get_n_rules(rules), ==, 2);
	g_assert_cmpint(gsurf_host_rules_find(rules, "www.YouTube.com"), ==, 0);
	g_assert_cmpint(gsurf_host_rules_find(rules, "youtube.com"), ==, 0);
	g_assert_cmpint(gsurf_host_rules_find(rules, "notyoutube.com"), ==, 1);
	g_assert_cmpint(gsurf_host_rules_find(rules, "example.org"), ==, -1);
	g_assert_cmpint(gsurf_host_rules_find(rules, NULL), ==, -1);
	g_assert_cmpint(gsurf_host_rules_find(rules, ""), ==, -1);
	g_assert_false(gsurf_host_rules_matches(rules, "example.org"));
	g_assert_cmpstr(gsurf_host_rules_get_pattern(rules, 1), ==, "tube");
	g_assert_null(gsurf_host_rules_get_pattern(rules, 2));

	gsurf_host_rules_clear(rules);
	g_assert_cmpuint(gsurf_host_rules_get_n_rules(rules), ==, 0);
	g_assert_false(gsurf_host_rules_matches(rules, "youtube.com"));
}

/* Bad patterns are rejected with a pattern error and add nothing. */
static void
test_invalid_pattern(void)
{
	g_autoptr(GsurfHostRules) rules = gsurf_host_rules_new();
	g_autoptr(GError) error = NULL;

	g_assert_false(gsurf_host_rules_add(rules, "(unclosed", NULL, &error));
	g_assert_error(error, GSURF_HOST_RULES_ERROR, GSURF_HOST_RULES_ERROR_PATTERN);
	g_clear_error(&error);
	g_assert_false(gsurf_host_rules_add(rules, "", NULL, &error));
	g_assert_error(error, GSURF_HOST_RULES_ERROR, GSURF_HOST_RULES_ERROR_PATTERN);
	g_clear_error(&error);
	g_assert_false(gsurf_host_rules_add(rules, NULL, NULL, &error));
	g_assert_error(error, GSURF_HOST_RULES_ERROR, GSURF_HOST_RULES_ERROR_PATTERN);
	g_assert_cmpuint(gsurf_host_rules_get_n_rules(rules), ==, 0);
}

/* YAML lists: scalar shorthand, mapping rules with data, lookup and
 * lookup_all in rule order, and each kind of shape error. */
static void
test_load_yaml(void)
{
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(GsurfHostRules) rules = gsurf_host_rules_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) all = NULL;
	YamlMapping *data;
	YamlNode *root;

	root = parse_root(parser,
		"- 'localhost'\n"
		"- match: '(^|\\.)github\\.com$'\n"
		"  mode: native\n"
		"- match: 'git'\n"
		"  brightness: 90\n");
	g_assert_true(gsurf_host_rules_load(rules, root, "match", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(gsurf_host_rules_get_n_rules(rules), ==, 3);

	g_assert_null(gsurf_host_rules_lookup(rules, "localhost"));
	g_assert_true(gsurf_host_rules_matches(rules, "localhost"));
	data = gsurf_host_rules_lookup(rules, "gist.github.com");
	g_assert_nonnull(data);
	g_assert_cmpstr(yaml_mapping_get_string_member(data, "mode"), ==, "native");

	all = gsurf_host_rules_lookup_all(rules, "gist.github.com");
	g_assert_cmpuint(all->len, ==, 2);
	g_assert_cmpint(yaml_mapping_get_int_member(g_ptr_array_index(all, 1), "brightness"), ==, 90);
	g_clear_pointer(&all, g_ptr_array_unref);
	all = gsurf_host_rules_lookup_all(rules, NULL);
	g_assert_cmpuint(all->len, ==, 0);

	/* NULL and a YAML null add nothing. */
	g_assert_true(gsurf_host_rules_load(rules, NULL, "match", &error));
	g_assert_cmpuint(gsurf_host_rules_get_n_rules(rules), ==, 3);
}

static void
test_load_errors(void)
{
	static const gchar *const bad[] = {
		"match: 'not-a-list'\n",
		"- [nested]\n",
		"- mode: native\n",
		"- match: '(bad'\n",
	};
	static const gint codes[] = {
		GSURF_HOST_RULES_ERROR_SHAPE,
		GSURF_HOST_RULES_ERROR_SHAPE,
		GSURF_HOST_RULES_ERROR_PATTERN,
		GSURF_HOST_RULES_ERROR_PATTERN,
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autoptr(YamlParser) parser = yaml_parser_new();
		g_autoptr(GsurfHostRules) rules = gsurf_host_rules_new();
		g_autoptr(GError) error = NULL;
		YamlNode *root = parse_root(parser, bad[i]);

		g_assert_false(gsurf_host_rules_load(rules, root, "match", &error));
		g_assert_error(error, GSURF_HOST_RULES_ERROR, codes[i]);
	}
}

/* JSON for in-page evaluation keeps rule order and data types. */
static void
test_to_json(void)
{
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(GsurfHostRules) rules = gsurf_host_rules_new();
	g_autoptr(JsonParser) json = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	JsonArray *array;
	JsonObject *first;
	JsonObject *data;

	g_assert_true(gsurf_host_rules_load(rules, parse_root(parser,
		"- match: 'a\\.example$'\n"
		"  brightness: 90\n"
		"  off: true\n"
		"- 'b\\.example$'\n"), "match", &error));
	text = gsurf_host_rules_to_json(rules);
	g_assert_true(json_parser_load_from_data(json, text, -1, &error));
	g_assert_no_error(error);

	array = json_node_get_array(json_parser_get_root(json));
	g_assert_cmpuint(json_array_get_length(array), ==, 2);
	first = json_array_get_object_element(array, 0);
	g_assert_cmpstr(json_object_get_string_member(first, "pattern"), ==, "a\\.example$");
	data = json_object_get_object_member(first, "data");
	g_assert_cmpint(json_object_get_int_member(data, "brightness"), ==, 90);
	g_assert_true(json_object_get_boolean_member(data, "off"));
	g_assert_true(json_object_get_null_member(json_array_get_object_element(array, 1), "data"));
}

/* GVariant for web-process extensions: scalars keep their type, scalar
 * lists become "as", nested mappings travel as JSON text. */
static void
test_to_variant(void)
{
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(GsurfHostRules) rules = gsurf_host_rules_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) variant = NULL;
	g_autoptr(GVariant) rule = NULL;
	g_autoptr(GVariant) data = NULL;
	g_autoptr(GVariant) skip = NULL;
	g_autofree const gchar **skips = NULL;
	const gchar *pattern = NULL;
	const gchar *nested = NULL;
	gint64 level = 0;
	gboolean flag = FALSE;
	gdouble ratio = 0.0;

	g_assert_true(gsurf_host_rules_load(rules, parse_root(parser,
		"- match: '^bank\\.example$'\n"
		"  skip: [referer, https_only]\n"
		"  level: 3\n"
		"  flag: true\n"
		"  ratio: 0.5\n"
		"  nested: {a: 1}\n"), "match", &error));
	g_assert_no_error(error);

	variant = g_variant_ref_sink(gsurf_host_rules_to_variant(rules));
	g_assert_cmpstr(g_variant_get_type_string(variant), ==, "a(sa{sv})");
	g_assert_cmpuint(g_variant_n_children(variant), ==, 1);
	rule = g_variant_get_child_value(variant, 0);
	g_variant_get(rule, "(&s@a{sv})", &pattern, &data);
	g_assert_cmpstr(pattern, ==, "^bank\\.example$");

	skip = g_variant_lookup_value(data, "skip", G_VARIANT_TYPE("as"));
	g_assert_nonnull(skip);
	skips = g_variant_get_strv(skip, NULL);
	g_assert_cmpstr(skips[0], ==, "referer");
	g_assert_cmpstr(skips[1], ==, "https_only");
	g_assert_true(g_variant_lookup(data, "level", "x", &level));
	g_assert_cmpint(level, ==, 3);
	g_assert_true(g_variant_lookup(data, "flag", "b", &flag));
	g_assert_true(flag);
	g_assert_true(g_variant_lookup(data, "ratio", "d", &ratio));
	g_assert_cmpfloat(ratio, ==, 0.5);
	g_assert_true(g_variant_lookup(data, "nested", "&s", &nested));
	g_assert_nonnull(strstr(nested, "\"a\""));
	/* The match pattern itself is part of the data mapping too. */
	g_assert_true(g_variant_lookup(data, "match", "&s", &pattern));
}

static void
test_host_from_uri(void)
{
	static const struct { const gchar *uri; const gchar *host; } cases[] = {
		{ "https://WWW.Example.com/path?q", "www.example.com" },
		{ "http://example.com.:8080/", "example.com" },
		{ "https://user:pw@sub.example.org/", "sub.example.org" },
		{ "http://[::1]:8000/", "::1" },
		{ "http://127.0.0.1/", "127.0.0.1" },
		{ "about:blank", NULL },
		{ "data:text/plain,hi", NULL },
		{ "file:///etc/hosts", NULL },
		{ "not a uri", NULL },
		{ "", NULL },
		{ NULL, NULL },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *host = gsurf_host_from_uri(cases[i].uri);

		g_assert_cmpstr(host, ==, cases[i].host);
	}
}

/* Private files: 0700 parents, 0600 atomic writes, and the permission
 * check used before trusting sensitive files. */
static void
test_private_file(void)
{
	g_autofree gchar *root = g_dir_make_tmp("gsurf-private-XXXXXX", NULL);
	g_autofree gchar *dir = g_build_filename(root, "a", "b", NULL);
	g_autofree gchar *file = g_build_filename(dir, "data.yaml", NULL);
	g_autofree gchar *contents = NULL;
	g_autofree gchar *home = NULL;
	g_autoptr(GError) error = NULL;
	GStatBuf st;

	g_assert_nonnull(root);
	g_assert_true(gsurf_private_write(file, "secret: 1\n", -1, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_stat(dir, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);
	g_assert_cmpint(g_stat(file, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_true(g_file_get_contents(file, &contents, NULL, NULL));
	g_assert_cmpstr(contents, ==, "secret: 1\n");
	g_assert_true(gsurf_private_file_is_private(file, &error));

	/* Loose permissions, a missing file and a directory are refused. */
	g_assert_cmpint(g_chmod(file, 0644), ==, 0);
	g_assert_false(gsurf_private_file_is_private(file, &error));
	g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_PERM);
	g_clear_error(&error);

	/* Rewriting a loose file makes it private again (GLib would keep the
	 * old mode of a replaced file). */
	g_assert_true(gsurf_private_write(file, "secret: 2\n", -1, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_stat(file, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_cmpint(g_chmod(file, 0644), ==, 0);
	g_assert_false(gsurf_private_file_is_private(dir, &error));
	g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
	g_clear_error(&error);
	g_assert_cmpint(g_unlink(file), ==, 0);
	g_assert_false(gsurf_private_file_is_private(file, &error));
	g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
	g_clear_error(&error);

	/* A regular file in the way of a directory is an error. */
	g_assert_true(g_file_set_contents(file, "x", -1, NULL));
	{
		g_autofree gchar *under = g_build_filename(file, "child", NULL);

		g_assert_false(gsurf_private_mkdir(under, &error));
		g_assert_nonnull(error);
	}

	home = gsurf_expand_home("~/x/y");
	g_assert_true(g_str_has_prefix(home, g_get_home_dir()));
	g_assert_true(g_str_has_suffix(home, "x/y"));
	g_clear_pointer(&home, g_free);
	home = gsurf_expand_home("/abs");
	g_assert_cmpstr(home, ==, "/abs");
	g_assert_null(gsurf_expand_home(NULL));

	g_assert_true(gsurf_test_rmtree(root));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gsurf/host-rules/match-order", test_match_order);
	g_test_add_func("/gsurf/host-rules/invalid-pattern", test_invalid_pattern);
	g_test_add_func("/gsurf/host-rules/load-yaml", test_load_yaml);
	g_test_add_func("/gsurf/host-rules/load-errors", test_load_errors);
	g_test_add_func("/gsurf/host-rules/to-json", test_to_json);
	g_test_add_func("/gsurf/host-rules/to-variant", test_to_variant);
	g_test_add_func("/gsurf/host-rules/host-from-uri", test_host_from_uri);
	g_test_add_func("/gsurf/host-rules/private-file", test_private_file);
	return g_test_run();
}
