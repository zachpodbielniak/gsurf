/*
 * gsurfctl.c - drive a running gsurf from the shell
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A client of the control socket every gsurf opens (control.enabled,
 * on by default) at $XDG_RUNTIME_DIR/gsurf/<pid>-XXXXXX/control.sock:
 * list the instances, pick one (-i PID|NAME, $GSURF_INSTANCE, the only
 * one running, else the most recently focused), send a command (open a
 * tab, list the tabs or the tab tree, read the page, run an action, drive
 * wmux), print the result as text or JSON (-j), follow the event stream
 * (watch), or send the same command to every instance (-a). Everything
 * the socket offers is reachable with `call METHOD`. See
 * docs/gsurfctl.org and docs/control.org.
 */

#include <gsurf.h>
#include <control/gsurf-control.h>

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* exit statuses */
#define EXIT_OK          (0)
#define EXIT_FAILED      (1)  /* the command failed (the instance said why) */
#define EXIT_USAGE       (2)
#define EXIT_NO_INSTANCE (3)  /* no instance, or it could not be reached */
#define EXIT_KIOSK       (4)  /* refused by kiosk mode */

static gchar   *opt_instance = NULL;
static gchar   *opt_socket = NULL;
static gboolean opt_json = FALSE;
static gboolean opt_pretty = FALSE;
static gboolean opt_all = FALSE;
static gdouble  opt_timeout = 30.0;
static gboolean opt_quiet = FALSE;
static gchar   *opt_completion = NULL;
static gboolean opt_version = FALSE;
static gboolean opt_license = FALSE;

static GOptionEntry entries[] = {
	{ "instance", 'i', 0, G_OPTION_ARG_STRING, &opt_instance,
	  "The instance to talk to: a pid, a control.name or a runtime directory id (default: "
	  "$GSURF_INSTANCE, else the only one running, else the most recently focused)", "ID" },
	{ "socket", 'S', 0, G_OPTION_ARG_FILENAME, &opt_socket,
	  "A control socket's path instead of an instance", "PATH" },
	{ "all", 'a', 0, G_OPTION_ARG_NONE, &opt_all,
	  "Send the command to every running instance", NULL },
	{ "json", 'j', 0, G_OPTION_ARG_NONE, &opt_json,
	  "Print results as JSON (one line; -p to indent)", NULL },
	{ "pretty", 'p', 0, G_OPTION_ARG_NONE, &opt_pretty,
	  "Indent JSON output", NULL },
	{ "timeout", 'T', 0, G_OPTION_ARG_DOUBLE, &opt_timeout,
	  "Seconds to wait for a reply (0: forever; default 30)", "S" },
	{ "quiet", 'q', 0, G_OPTION_ARG_NONE, &opt_quiet,
	  "Print nothing on success", NULL },
	{ "completion", 0, 0, G_OPTION_ARG_STRING, &opt_completion,
	  "Print a shell completion script (bash) and exit", "SHELL" },
	{ "version", 'V', 0, G_OPTION_ARG_NONE, &opt_version,
	  "Print the version and exit", NULL },
	{ "license", 0, 0, G_OPTION_ARG_NONE, &opt_license,
	  "Print the license and exit", NULL },
	{ NULL }
};

static const gchar license_text[] =
	"gsurfctl - drive a running gsurf\n"
	"Copyright (C) 2026 Zach Podbielniak\n"
	"\n"
	"This program is free software: you can redistribute it and/or modify\n"
	"it under the terms of the GNU Affero General Public License as published\n"
	"by the Free Software Foundation, either version 3 of the License, or\n"
	"(at your option) any later version.\n"
	"\n"
	"This program is distributed in the hope that it will be useful,\n"
	"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
	"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
	"GNU Affero General Public License for more details.\n"
	"\n"
	"You should have received a copy of the GNU Affero General Public License\n"
	"along with this program.  If not, see <https://www.gnu.org/licenses/>.\n";

static const gchar help_description[] =
	"Options may come before or after the command; \"--\" ends them. The words\n"
	"after wmux, eval and call are taken literally (put options before those).\n"
	"VIEW is a view id (gsurfctl tabs); without --view a command acts on the\n"
	"active view.\n"
	"\n"
	"Commands (gsurfctl help COMMAND tells one):\n"
	"  list                       the running instances\n"
	"  info                       this instance: pid, version, views, focus\n"
	"  open [URL] [--bg] [--container ID] [--child-of VIEW]\n"
	"  nav URL                    load URL in the view\n"
	"  back, forward, stop, reload [--hard]\n"
	"  tabs                       the views (* active, + on screen)\n"
	"  tab VIEW, close [VIEW]     switch to, close a view\n"
	"  tree                       the tab tree\n"
	"  move VIEW --parent P|--before V|--after V|--index N\n"
	"  collapse VIEW [on|off]     fold a subtree in the tab sidebar\n"
	"  containers, reopen VIEW CONTAINER\n"
	"  title, uri, text           the page's title, URI, rendered text\n"
	"  eval JS                    run JavaScript, print its value\n"
	"  screenshot FILE            write a PNG of the page\n"
	"  actions, action NAME       list, run a key-binding action\n"
	"  mode [normal|insert]       the vim-style input mode\n"
	"  panes [--window N], windows, wmux 'COMMAND',\n"
	"  select-window N, rename-window N [NAME]      wmux panes and windows\n"
	"  config [KEY], modules, module NAME [on|off]\n"
	"  methods                    every method of the control socket\n"
	"  watch [TOPIC...]           print events until interrupted (view, uri,\n"
	"                             title, load, mode, layout, download)\n"
	"  call METHOD [JSON|K=V...]  a raw JSON-RPC call\n"
	"  help [COMMAND|--names]     this list, one command, or the names\n"
	"\n"
	"Exit status: 0 ok, 1 the command failed, 2 usage, 3 no instance (or it\n"
	"could not be reached), 4 refused by kiosk mode.\n"
	"\n"
	"Examples:\n"
	"  gsurfctl list\n"
	"  gsurfctl open https://example.org --bg\n"
	"  gsurfctl open ./notes.html\n"
	"  gsurfctl -j tabs | yq -p json '.[] | .id + \" \" + .uri'\n"
	"  gsurfctl tree\n"
	"  gsurfctl tab 4 && gsurfctl title\n"
	"  gsurfctl move 7 --parent 3\n"
	"  gsurfctl reopen 7 temporary\n"
	"  gsurfctl text --view 7 | less\n"
	"  gsurfctl eval 'document.querySelectorAll(\"a\").length'\n"
	"  gsurfctl screenshot page.png\n"
	"  gsurfctl action zoom-in\n"
	"  gsurfctl mode insert\n"
	"  gsurfctl wmux 'split-window -h https://github.com'\n"
	"  gsurfctl select-window 1 && gsurfctl panes\n"
	"  gsurfctl config modules.wmux.prefix\n"
	"  gsurfctl module wmux off\n"
	"  gsurfctl watch view uri\n"
	"  gsurfctl -i work open https://mail.example.org\n"
	"  gsurfctl -a reload\n"
	"  gsurfctl call views.list\n"
	"  gsurfctl call open uri=https://example.org background=true\n"
	"  source <(gsurfctl --completion bash)\n";

static const gchar bash_completion[] =
	"# bash completion for gsurfctl (source <(gsurfctl --completion bash))\n"
	"_gsurfctl() {\n"
	"\tlocal cur prev words cword cmd i\n"
	"\t_init_completion 2>/dev/null || { cur=\"${COMP_WORDS[COMP_CWORD]}\"; "
	"prev=\"${COMP_WORDS[COMP_CWORD-1]}\"; words=(\"${COMP_WORDS[@]}\"); cword=${COMP_CWORD}; }\n"
	"\tcase \"${prev}\" in\n"
	"\t\t-i|--instance)\n"
	"\t\t\tmapfile -t COMPREPLY < <(compgen -W \"$(gsurfctl list --names 2>/dev/null)\" -- \"${cur}\"); return ;;\n"
	"\t\t-S|--socket)\n"
	"\t\t\tmapfile -t COMPREPLY < <(compgen -f -- \"${cur}\"); return ;;\n"
	"\t\t--completion)\n"
	"\t\t\tmapfile -t COMPREPLY < <(compgen -W \"bash\" -- \"${cur}\"); return ;;\n"
	"\tesac\n"
	"\tif [[ \"${cur}\" == -* ]]; then\n"
	"\t\tmapfile -t COMPREPLY < <(compgen -W \"-i --instance -S --socket -a --all -j --json -p --pretty "
	"-T --timeout -q --quiet --completion -V --version --license -h --help --bg --container --child-of "
	"--hard --view --parent --before --after --index --window --names\" -- \"${cur}\"); return\n"
	"\tfi\n"
	"\tcmd=\"\"\n"
	"\tfor (( i = 1; i < cword; i++ )); do\n"
	"\t\tcase \"${words[i]}\" in\n"
	"\t\t\t-i|--instance|-S|--socket|-T|--timeout|--completion|--container|--child-of|--view|"
	"--parent|--before|--after|--index|--window) (( i++ )) ;;\n"
	"\t\t\t-*) ;;\n"
	"\t\t\t*) cmd=\"${words[i]}\"; break ;;\n"
	"\t\tesac\n"
	"\tdone\n"
	"\tcase \"${cmd}\" in\n"
	"\t\t\"\") mapfile -t COMPREPLY < <(compgen -W \"$(gsurfctl help --names 2>/dev/null)\" -- \"${cur}\") ;;\n"
	"\t\taction) mapfile -t COMPREPLY < <(compgen -W \"$(gsurfctl actions --names 2>/dev/null)\" -- \"${cur}\") ;;\n"
	"\t\tmodule) mapfile -t COMPREPLY < <(compgen -W \"$(gsurfctl modules --names 2>/dev/null) on off\" -- \"${cur}\") ;;\n"
	"\t\tmode) mapfile -t COMPREPLY < <(compgen -W \"normal insert\" -- \"${cur}\") ;;\n"
	"\t\tcollapse) mapfile -t COMPREPLY < <(compgen -W \"on off\" -- \"${cur}\") ;;\n"
	"\t\twatch) mapfile -t COMPREPLY < <(compgen -W \"view uri title load mode layout download\" -- \"${cur}\") ;;\n"
	"\t\thelp) mapfile -t COMPREPLY < <(compgen -W \"$(gsurfctl help --names 2>/dev/null) --names\" -- \"${cur}\") ;;\n"
	"\t\tcall) mapfile -t COMPREPLY < <(compgen -W \"$(gsurfctl methods --names 2>/dev/null)\" -- \"${cur}\") ;;\n"
	"\t\tscreenshot|open) mapfile -t COMPREPLY < <(compgen -f -- \"${cur}\") ;;\n"
	"\t\t*) COMPREPLY=() ;;\n"
	"\tesac\n"
	"}\n"
	"complete -F _gsurfctl gsurfctl\n";

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static gchar *
json_text(JsonNode *node)
{
	g_autoptr(JsonGenerator) generator = json_generator_new();

	json_generator_set_pretty(generator, opt_pretty);
	json_generator_set_root(generator, node);
	return json_generator_to_data(generator, NULL);
}

/* A value on one line for the human form. */
static gchar *
scalar_text(JsonNode *node)
{
	GType type;

	if (node == NULL || JSON_NODE_HOLDS_NULL(node))
		return g_strdup("-");
	if (!JSON_NODE_HOLDS_VALUE(node))
		return json_text(node);
	type = json_node_get_value_type(node);
	if (type == G_TYPE_STRING)
		return g_strdup(json_node_get_string(node));
	if (type == G_TYPE_BOOLEAN)
		return g_strdup(json_node_get_boolean(node) ? "yes" : "no");
	if (type == G_TYPE_INT64)
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
	if (type == G_TYPE_DOUBLE) {
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

		return g_strdup(g_ascii_formatd(buffer, sizeof(buffer), "%.2f",
			json_node_get_double(node)));
	}
	return json_text(node);
}

/* An object of scalars fits one line. */
static gboolean
is_flat(JsonObject *object)
{
	g_autoptr(GList) members = json_object_get_members(object);
	GList *l;

	for (l = members; l != NULL; l = l->next) {
		JsonNode *node = json_object_get_member(object, l->data);

		if (JSON_NODE_HOLDS_OBJECT(node) || JSON_NODE_HOLDS_ARRAY(node))
			return FALSE;
	}
	return TRUE;
}

/* "key: value  key: value" */
static gchar *
flat_line(JsonObject *object)
{
	g_autoptr(GList) members = json_object_get_members(object);
	GString *line = g_string_new(NULL);
	GList *l;

	for (l = members; l != NULL; l = l->next) {
		g_autofree gchar *value = scalar_text(json_object_get_member(object, l->data));

		g_string_append_printf(line, "%s%s: %s", line->len > 0 ? "  " : "",
			(const gchar *)l->data, value);
	}
	return g_string_free(line, FALSE);
}

/* Any JSON as indented "key: value" lines. */
static void
print_human(JsonNode *node, guint indent, const gchar *key)
{
	g_autofree gchar *pad = g_strnfill(indent * 2, ' ');

	if (JSON_NODE_HOLDS_OBJECT(node)) {
		JsonObject *object = json_node_get_object(node);
		g_autoptr(GList) members = NULL;
		GList *l;

		if (key != NULL)
			g_print("%s%s:\n", pad, key);
		members = json_object_get_members(object);
		for (l = members; l != NULL; l = l->next)
			print_human(json_object_get_member(object, l->data),
				key != NULL ? indent + 1 : indent, l->data);
		return;
	}
	if (JSON_NODE_HOLDS_ARRAY(node)) {
		JsonArray *array = json_node_get_array(node);
		g_autofree gchar *inner = g_strnfill((key != NULL ? indent + 1 : indent) * 2, ' ');
		guint i;

		if (key != NULL)
			g_print("%s%s:%s\n", pad, key,
				json_array_get_length(array) == 0 ? " (none)" : "");
		for (i = 0; i < json_array_get_length(array); i++) {
			JsonNode *item = json_array_get_element(array, i);

			if (JSON_NODE_HOLDS_OBJECT(item) && is_flat(json_node_get_object(item))) {
				g_autofree gchar *line = flat_line(json_node_get_object(item));

				g_print("%s- %s\n", inner, line);
			} else if (JSON_NODE_HOLDS_VALUE(item) || JSON_NODE_HOLDS_NULL(item)) {
				g_autofree gchar *value = scalar_text(item);

				g_print("%s- %s\n", inner, value);
			} else {
				g_print("%s-\n", inner);
				print_human(item, (key != NULL ? indent + 1 : indent) + 1, NULL);
			}
		}
		return;
	}
	{
		g_autofree gchar *value = scalar_text(node);

		if (key != NULL)
			g_print("%s%s: %s\n", pad, key, value);
		else
			g_print("%s%s\n", pad, value);
	}
}

static const gchar *
str_member(JsonObject *object, const gchar *name)
{
	JsonNode *node;

	if (object == NULL || !json_object_has_member(object, name))
		return "";
	node = json_object_get_member(object, name);
	if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return "";
	return json_node_get_string(node);
}

static gint64
int_member(JsonObject *object, const gchar *name)
{
	JsonNode *node;

	if (object == NULL || !json_object_has_member(object, name))
		return 0;
	node = json_object_get_member(object, name);
	if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return 0;
	return json_node_get_int(node);
}

static gboolean
bool_member(JsonObject *object, const gchar *name)
{
	JsonNode *node;

	if (object == NULL || !json_object_has_member(object, name))
		return FALSE;
	node = json_object_get_member(object, name);
	return JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN &&
		json_node_get_boolean(node);
}

/* "ID\tTITLE\tURI" for a view object (marked * active, + on screen). */
static void
print_view(JsonObject *view, const gchar *indent)
{
	const gchar *title = str_member(view, "title");

	g_print("%s%s%" G_GINT64_FORMAT "\t%s\t%s\n", indent,
		bool_member(view, "active") ? "*" : bool_member(view, "visible") ? "+" : " ",
		int_member(view, "id"), *title != '\0' ? title : "-", str_member(view, "uri"));
}

static void
print_tree_nodes(JsonArray *nodes, guint depth)
{
	guint i;

	for (i = 0; nodes != NULL && i < json_array_get_length(nodes); i++) {
		JsonObject *node = json_array_get_object_element(nodes, i);
		g_autofree gchar *pad = g_strnfill(depth * 2, ' ');

		print_view(node, pad);
		if (json_object_has_member(node, "children") &&
		    JSON_NODE_HOLDS_ARRAY(json_object_get_member(node, "children")))
			print_tree_nodes(json_object_get_array_member(node, "children"), depth + 1);
	}
}

static void
print_panes(JsonObject *group)
{
	JsonArray *panes = json_object_has_member(group, "panes")
		? json_object_get_array_member(group, "panes") : NULL;
	guint i;

	g_print("window %" G_GINT64_FORMAT ": %s%s\n", int_member(group, "index"),
		str_member(group, "name"), bool_member(group, "current") ? " (current)" : "");
	for (i = 0; panes != NULL && i < json_array_get_length(panes); i++) {
		JsonObject *pane = json_array_get_object_element(panes, i);

		g_print("  %s%" G_GINT64_FORMAT ": view %" G_GINT64_FORMAT "\t%s\t%s%s\n",
			bool_member(pane, "active") ? "*" : " ", int_member(pane, "index"),
			int_member(pane, "view"), str_member(pane, "title"), str_member(pane, "uri"),
			bool_member(pane, "zoomed") ? "\t(zoomed)" : "");
	}
}

/* The human form of @result for command @name. */
static void
print_for(const gchar *name, JsonNode *result)
{
	JsonObject *object = JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
	JsonArray *array = JSON_NODE_HOLDS_ARRAY(result) ? json_node_get_array(result) : NULL;
	guint i;

	/* 1. A view (open, nav, tab, move, ...). */
	if (object != NULL && json_object_has_member(object, "id") &&
	    json_object_has_member(object, "opener_id")) {
		print_view(object, "");
		return;
	}
	if (g_strcmp0(name, "tabs") == 0 && array != NULL) {
		for (i = 0; i < json_array_get_length(array); i++)
			print_view(json_array_get_object_element(array, i), "");
		return;
	}
	if (g_strcmp0(name, "tree") == 0 && object != NULL) {
		print_tree_nodes(json_object_get_array_member(object, "tree"), 0);
		return;
	}
	if ((g_strcmp0(name, "panes") == 0 || g_strcmp0(name, "wmux") == 0 ||
	     g_strcmp0(name, "select-window") == 0 || g_strcmp0(name, "rename-window") == 0) &&
	    object != NULL) {
		print_panes(object);
		return;
	}
	if (g_strcmp0(name, "windows") == 0 && array != NULL) {
		for (i = 0; i < json_array_get_length(array); i++)
			print_panes(json_array_get_object_element(array, i));
		return;
	}
	if (g_strcmp0(name, "containers") == 0 && array != NULL) {
		for (i = 0; i < json_array_get_length(array); i++) {
			JsonObject *c = json_array_get_object_element(array, i);

			g_print("%s\t%s\t%" G_GINT64_FORMAT " views%s\n", str_member(c, "id"),
				str_member(c, "name"), int_member(c, "n_views"),
				bool_member(c, "temporary") ? "\ttemporary" : "");
		}
		return;
	}
	if (g_strcmp0(name, "actions") == 0 && array != NULL) {
		for (i = 0; i < json_array_get_length(array); i++) {
			JsonObject *a = json_array_get_object_element(array, i);

			g_print("%s%s\n", str_member(a, "name"),
				bool_member(a, "allowed") ? "" : "\t(refused in kiosk mode)");
		}
		return;
	}
	if (g_strcmp0(name, "modules") == 0 && array != NULL) {
		for (i = 0; i < json_array_get_length(array); i++) {
			JsonObject *m = json_array_get_object_element(array, i);

			g_print("%-16s %-8s %s\n", str_member(m, "name"),
				bool_member(m, "active") ? "active" : "off", str_member(m, "description"));
		}
		return;
	}
	if (g_strcmp0(name, "methods") == 0 && array != NULL) {
		for (i = 0; i < json_array_get_length(array); i++) {
			JsonObject *m = json_array_get_object_element(array, i);

			g_print("%-22s %s\n", str_member(m, "name"), str_member(m, "params"));
		}
		return;
	}
	if (g_strcmp0(name, "mode") == 0 && object != NULL) {
		g_print("%s\n", str_member(object, "mode"));
		return;
	}
	if (g_strcmp0(name, "screenshot") == 0 && object != NULL) {
		g_print("%s\n", str_member(object, "path"));
		return;
	}
	if (g_strcmp0(name, "action") == 0 || g_strcmp0(name, "close") == 0 ||
	    g_strcmp0(name, "module") == 0) {
		if (object != NULL && json_object_has_member(object, "name") &&
		    json_object_has_member(object, "active"))
			g_print("%s %s\n", str_member(object, "name"),
				bool_member(object, "active") ? "active" : "off");
		else
			g_print("ok\n");
		return;
	}
	if (JSON_NODE_HOLDS_NULL(result)) {
		g_print("ok\n");
		return;
	}
	print_human(result, 0, NULL);
}

/* Prints @result of command @name from @instance. */
static void
print_result(const gchar *name, const gchar *instance, JsonNode *result)
{
	if (opt_quiet)
		return;
	if (opt_json) {
		g_autofree gchar *text = json_text(result);

		if (opt_all) {
			g_autofree gchar *quoted = g_strescape(instance, NULL);

			g_print("{\"instance\":\"%s\",\"result\":%s}\n", quoted, text);
		} else {
			g_print("%s\n", text);
		}
		return;
	}
	if (opt_all)
		g_print("[%s]\n", instance);
	print_for(name, result);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

/* A command's words: positional arguments and --flags. */
typedef struct {
	GPtrArray  *args;    /* gchar* (borrowed) */
	GHashTable *flags;   /* name -> value ("" for a switch); borrowed / static */
} Words;

/* Command flags that take a value. */
static const gchar *const valued_flags[] = {
	"container", "child-of", "view", "parent", "before", "after", "index", "window",
	"zone", "target", "pane", NULL
};

typedef gboolean (*BuildFunc) (Words *w, JsonObject *params, GError **error);

typedef struct {
	const gchar *name;
	const gchar *method;      /* NULL: handled locally / specially */
	const gchar *usage;
	const gchar *summary;
	gint         min_args;
	gint         max_args;    /* -1: any */
	gboolean     literal;     /* every word after the command is an argument */
	BuildFunc    build;
} Command;

static gboolean
usage_error(GError **error, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static gboolean
usage_error(GError **error, const gchar *format, ...)
{
	va_list ap;
	g_autofree gchar *message = NULL;

	va_start(ap, format);
	message = g_strdup_vprintf(format, ap);
	va_end(ap);
	g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, message);
	return FALSE;
}

/* A VIEW word as an integer member. */
static gboolean
set_id(JsonObject *params, const gchar *member, const gchar *text, GError **error)
{
	gchar *end = NULL;
	gint64 value;

	value = g_ascii_strtoll(text, &end, 10);
	if (text == NULL || *text == '\0' || end == NULL || *end != '\0')
		return usage_error(error, "'%s' is not a number", text != NULL ? text : "");
	json_object_set_int_member(params, member, value);
	return TRUE;
}

/* --view VIEW, accepted by every command acting on a view. */
static gboolean
add_view_flag(Words *w, JsonObject *params, GError **error)
{
	const gchar *view = g_hash_table_lookup(w->flags, "view");

	return view == NULL || set_id(params, "view", view, error);
}

static gboolean
b_view(Words *w, JsonObject *params, GError **error)
{
	return add_view_flag(w, params, error);
}

static gboolean
b_reload(Words *w, JsonObject *params, GError **error)
{
	if (g_hash_table_contains(w->flags, "hard"))
		json_object_set_boolean_member(params, "hard", TRUE);
	return add_view_flag(w, params, error);
}

/* A URL word: an existing file becomes a file:// URI (relative to the
 * shell's directory, not gsurf's), anything else goes as typed. */
static gchar *
url_arg(const gchar *word)
{
	if (!g_str_has_prefix(word, "/") && !g_str_has_prefix(word, "./") &&
	    !g_str_has_prefix(word, "../") && !g_str_has_prefix(word, "~/"))
		if (strstr(word, "://") != NULL || !g_file_test(word, G_FILE_TEST_EXISTS))
			return g_strdup(word);
	{
		g_autofree gchar *home = g_str_has_prefix(word, "~/")
			? g_build_filename(g_get_home_dir(), word + 2, NULL) : NULL;
		g_autofree gchar *absolute = g_canonicalize_filename(home != NULL ? home : word,
			NULL);

		return g_filename_to_uri(absolute, NULL, NULL);
	}
}

static gboolean
b_open(Words *w, JsonObject *params, GError **error)
{
	const gchar *container = g_hash_table_lookup(w->flags, "container");
	const gchar *parent = g_hash_table_lookup(w->flags, "child-of");

	if (w->args->len > 0) {
		g_autofree gchar *uri = url_arg(g_ptr_array_index(w->args, 0));

		json_object_set_string_member(params, "uri", uri);
	}
	if (g_hash_table_contains(w->flags, "bg") || g_hash_table_contains(w->flags, "background"))
		json_object_set_boolean_member(params, "background", TRUE);
	if (container != NULL)
		json_object_set_string_member(params, "container", container);
	return parent == NULL || set_id(params, "parent", parent, error);
}

static gboolean
b_nav(Words *w, JsonObject *params, GError **error)
{
	g_autofree gchar *uri = url_arg(g_ptr_array_index(w->args, 0));

	json_object_set_string_member(params, "uri", uri);
	return add_view_flag(w, params, error);
}

/* An optional VIEW argument (else --view). */
static gboolean
b_view_arg(Words *w, JsonObject *params, GError **error)
{
	if (w->args->len > 0)
		return set_id(params, "view", g_ptr_array_index(w->args, 0), error);
	return add_view_flag(w, params, error);
}

static gboolean
b_move(Words *w, JsonObject *params, GError **error)
{
	static const gchar *const places[] = { "parent", "before", "after", "index", NULL };
	guint i;
	guint n = 0;

	if (!set_id(params, "view", g_ptr_array_index(w->args, 0), error))
		return FALSE;
	for (i = 0; places[i] != NULL; i++) {
		const gchar *value = g_hash_table_lookup(w->flags, places[i]);

		if (value == NULL)
			continue;
		n++;
		if (!set_id(params, places[i], value, error))
			return FALSE;
	}
	if (n != 1)
		return usage_error(error, "move needs exactly one of --parent, --before, --after "
			"and --index");
	return TRUE;
}

/* on/off (or true/false, yes/no, 1/0) */
static gboolean
parse_switch(const gchar *word, gboolean *out, GError **error)
{
	static const gchar *const yes[] = { "on", "true", "yes", "1", NULL };
	static const gchar *const no[] = { "off", "false", "no", "0", NULL };

	if (g_strv_contains(yes, word))
		*out = TRUE;
	else if (g_strv_contains(no, word))
		*out = FALSE;
	else
		return usage_error(error, "'%s' is not on or off", word);
	return TRUE;
}

static gboolean
b_collapse(Words *w, JsonObject *params, GError **error)
{
	gboolean on = TRUE;

	if (!set_id(params, "view", g_ptr_array_index(w->args, 0), error))
		return FALSE;
	if (w->args->len > 1 && !parse_switch(g_ptr_array_index(w->args, 1), &on, error))
		return FALSE;
	json_object_set_boolean_member(params, "collapsed", on);
	return TRUE;
}

static gboolean
b_reopen(Words *w, JsonObject *params, GError **error)
{
	if (!set_id(params, "view", g_ptr_array_index(w->args, 0), error))
		return FALSE;
	json_object_set_string_member(params, "container", g_ptr_array_index(w->args, 1));
	return TRUE;
}

/* All the arguments joined by spaces. */
static gchar *
joined(Words *w, guint from)
{
	GString *s = g_string_new(NULL);
	guint i;

	for (i = from; i < w->args->len; i++) {
		if (s->len > 0)
			g_string_append_c(s, ' ');
		g_string_append(s, g_ptr_array_index(w->args, i));
	}
	return g_string_free(s, FALSE);
}

static gboolean
b_eval(Words *w, JsonObject *params, GError **error)
{
	g_autofree gchar *script = joined(w, 0);

	json_object_set_string_member(params, "script", script);
	return add_view_flag(w, params, error);
}

static gboolean
b_screenshot(Words *w, JsonObject *params, GError **error)
{
	g_autofree gchar *path = g_canonicalize_filename(g_ptr_array_index(w->args, 0), NULL);

	json_object_set_string_member(params, "path", path);
	return add_view_flag(w, params, error);
}

static gboolean
b_action(Words *w, JsonObject *params, GError **error)
{
	json_object_set_string_member(params, "name", g_ptr_array_index(w->args, 0));
	return add_view_flag(w, params, error);
}

static gboolean
b_panes(Words *w, JsonObject *params, GError **error)
{
	const gchar *window = g_hash_table_lookup(w->flags, "window");

	if (w->args->len > 0)
		window = g_ptr_array_index(w->args, 0);
	return window == NULL || set_id(params, "window", window, error);
}

static gboolean
b_wmux(Words *w, JsonObject *params, GError **error)
{
	g_autofree gchar *command = joined(w, 0);

	(void)error;
	json_object_set_string_member(params, "command", command);
	return TRUE;
}

static gboolean
b_select_window(Words *w, JsonObject *params, GError **error)
{
	return set_id(params, "window", g_ptr_array_index(w->args, 0), error);
}

static gboolean
b_rename_window(Words *w, JsonObject *params, GError **error)
{
	if (!set_id(params, "window", g_ptr_array_index(w->args, 0), error))
		return FALSE;
	if (w->args->len > 1) {
		g_autofree gchar *name = joined(w, 1);

		json_object_set_string_member(params, "name", name);
	}
	return TRUE;
}

static gboolean
b_config(Words *w, JsonObject *params, GError **error)
{
	(void)error;
	if (w->args->len > 0)
		json_object_set_string_member(params, "key", g_ptr_array_index(w->args, 0));
	return TRUE;
}

static gboolean
b_module(Words *w, JsonObject *params, GError **error)
{
	gboolean on = TRUE;

	json_object_set_string_member(params, "name", g_ptr_array_index(w->args, 0));
	if (w->args->len > 1) {
		if (!parse_switch(g_ptr_array_index(w->args, 1), &on, error))
			return FALSE;
		json_object_set_boolean_member(params, "enabled", on);
	}
	return TRUE;
}

static gboolean
b_mode(Words *w, JsonObject *params, GError **error)
{
	(void)error;
	if (w->args->len > 0)
		json_object_set_string_member(params, "mode", g_ptr_array_index(w->args, 0));
	return add_view_flag(w, params, error);
}

static const Command commands[] = {
	{ "list", NULL, "list [--names]", "the running instances: pid, name, views, active page",
	  0, 0, FALSE, NULL },
	{ "info", "instance.info", "info", "this instance: pid, version, backend, name, kiosk, "
	  "windows, views, the active page, focused_at", 0, 0, FALSE, NULL },
	{ "open", "open", "open [URL] [--bg] [--container ID] [--child-of VIEW]",
	  "open URL (default: the new-view page) in a new tab, focused unless --bg; a file path "
	  "opens as file://; --child-of puts it under VIEW in the tab tree", 0, 1, FALSE, b_open },
	{ "nav", "navigate", "nav URL [--view VIEW]", "load URL in the view", 1, 1, FALSE, b_nav },
	{ "back", "back", "back [--view VIEW]", "go back", 0, 0, FALSE, b_view },
	{ "forward", "forward", "forward [--view VIEW]", "go forward", 0, 0, FALSE, b_view },
	{ "reload", "reload", "reload [--hard] [--view VIEW]", "reload (--hard bypasses the cache)",
	  0, 0, FALSE, b_reload },
	{ "stop", "stop", "stop [--view VIEW]", "stop loading", 0, 0, FALSE, b_view },
	{ "tabs", "views.list", "tabs", "the views: id, title, URI (* active, + on screen)",
	  0, 0, FALSE, NULL },
	{ "tab", "views.activate", "tab VIEW", "switch to VIEW", 1, 1, FALSE, b_view_arg },
	{ "close", "views.close", "close [VIEW]", "close VIEW (default: the active one)",
	  0, 1, FALSE, b_view_arg },
	{ "tree", "tree.get", "tree", "the tab tree (indented)", 0, 0, FALSE, NULL },
	{ "move", "tree.move", "move VIEW --parent P|--before V|--after V|--index N",
	  "move VIEW and its subtree in the tab tree (--parent 0: a root at the end)",
	  1, 1, FALSE, b_move },
	{ "collapse", "tree.collapse", "collapse VIEW [on|off]",
	  "fold (or unfold) VIEW's subtree in the tab sidebar", 1, 2, FALSE, b_collapse },
	{ "containers", "containers.list", "containers", "the containers: id, name, views",
	  0, 0, FALSE, NULL },
	{ "reopen", "containers.reopen", "reopen VIEW CONTAINER",
	  "reopen VIEW in CONTAINER (an id, temporary or default); prints the new view",
	  2, 2, FALSE, b_reopen },
	{ "title", "page.title", "title [--view VIEW]", "the page title", 0, 0, FALSE, b_view },
	{ "uri", "page.uri", "uri [--view VIEW]", "the page URI", 0, 0, FALSE, b_view },
	{ "text", "page.text", "text [--view VIEW]", "the page's rendered text", 0, 0, FALSE,
	  b_view },
	{ "eval", "page.eval", "eval JS", "run JS in the page and print its value",
	  1, -1, TRUE, b_eval },
	{ "screenshot", "page.screenshot", "screenshot FILE [--view VIEW]",
	  "write a PNG of the visible page to FILE", 1, 1, FALSE, b_screenshot },
	{ "actions", "actions.list", "actions [--names]", "every action name", 0, 0, FALSE, NULL },
	{ "action", "action.run", "action NAME [--view VIEW]",
	  "run an action as its key binding would (zoom-in, tab-next, ...)", 1, 1, FALSE,
	  b_action },
	{ "mode", NULL, "mode [normal|insert]", "print or set the input mode (needs modal)",
	  0, 1, FALSE, b_mode },
	{ "panes", "layout.panes", "panes [N] [--window N]",
	  "the panes of the current (or Nth) wmux window", 0, 1, FALSE, b_panes },
	{ "windows", "layout.groups", "windows", "the wmux windows and their panes", 0, 0, FALSE,
	  NULL },
	{ "wmux", "layout.command", "wmux 'COMMAND'",
	  "run a wmux (tmux-style) command, e.g. 'split-window -h https://github.com'",
	  1, -1, TRUE, b_wmux },
	{ "select-window", "layout.select_group", "select-window N", "show wmux window N",
	  1, 1, FALSE, b_select_window },
	{ "rename-window", "layout.rename_group", "rename-window N [NAME]",
	  "rename wmux window N (no NAME: automatic naming)", 1, -1, FALSE, b_rename_window },
	{ "config", "config.get", "config [KEY]",
	  "a config value (browser.homepage, modules.wmux.prefix, ...; no KEY: the core keys)",
	  0, 1, FALSE, b_config },
	{ "modules", "modules.list", "modules [--names]", "the loaded modules", 0, 0, FALSE, NULL },
	{ "module", "modules.toggle", "module NAME [on|off]",
	  "switch a module on or off (no on/off: flip it)", 1, 2, FALSE, b_module },
	{ "methods", "methods.list", "methods [--names]", "every method of the control socket",
	  0, 0, FALSE, NULL },
	{ "watch", NULL, "watch [TOPIC...]",
	  "print events until interrupted (view, uri, title, load, mode, layout, download)",
	  0, -1, FALSE, NULL },
	{ "call", NULL, "call METHOD [JSON|K=V...]",
	  "a raw call; K=V values are JSON when they parse (n=3, b=true), else strings",
	  1, -1, TRUE, NULL },
	{ "help", NULL, "help [COMMAND|--names]", "the commands, one command's usage, or the "
	  "command names", 0, 1, FALSE, NULL },
	{ NULL, NULL, NULL, NULL, 0, 0, FALSE, NULL }
};

static const Command *
find_command(const gchar *name)
{
	guint i;

	for (i = 0; commands[i].name != NULL; i++)
		if (g_strcmp0(commands[i].name, name) == 0)
			return &commands[i];
	return NULL;
}

/* Splits a command's words into arguments and --flags. */
static gboolean
parse_words(const Command *cmd, gint argc, gchar **argv, Words *w, GError **error)
{
	gint i;
	gboolean ended = FALSE;

	w->args = g_ptr_array_new();
	w->flags = g_hash_table_new(g_str_hash, g_str_equal);
	for (i = 0; i < argc; i++) {
		const gchar *word = argv[i];

		/* A literal command (eval, wmux, call) takes its flags only
		 * before its first argument. */
		if (ended || !g_str_has_prefix(word, "--") || (cmd->literal && w->args->len > 0)) {
			g_ptr_array_add(w->args, (gpointer)word);
			continue;
		}
		if (strcmp(word, "--") == 0) {
			ended = TRUE;
			continue;
		}
		{
			const gchar *name = word + 2;
			const gchar *eq = strchr(name, '=');

			if (eq != NULL) {
				gchar *key = g_strndup(name, (gsize)(eq - name));

				/* Interned: the table borrows its keys. */
				g_hash_table_insert(w->flags, (gpointer)g_intern_string(key),
					(gpointer)(eq + 1));
				g_free(key);
			} else if (g_strv_contains(valued_flags, name)) {
				if (i + 1 >= argc)
					return usage_error(error, "%s needs a value", word);
				g_hash_table_insert(w->flags, (gpointer)name, argv[++i]);
			} else {
				g_hash_table_insert(w->flags, (gpointer)name, (gpointer)"");
			}
		}
	}
	if ((gint)w->args->len < cmd->min_args ||
	    (cmd->max_args >= 0 && (gint)w->args->len > cmd->max_args))
		return usage_error(error, "usage: gsurfctl %s", cmd->usage);
	return TRUE;
}

static void
words_clear(Words *w)
{
	g_clear_pointer(&w->args, g_ptr_array_unref);
	g_clear_pointer(&w->flags, g_hash_table_unref);
}

/* The exit status for a failed call. */
static gint
status_for(const GError *error)
{
	if (error->domain != GSURF_CONTROL_ERROR)
		return EXIT_NO_INSTANCE;
	switch (error->code) {
	case GSURF_CONTROL_ERROR_CLOSED:            return EXIT_NO_INSTANCE;
	case GSURF_CONTROL_ERROR_PERMISSION_DENIED: return EXIT_KIOSK;
	default:                                    return EXIT_FAILED;
	}
}

static gint
report(const gchar *instance, const GError *error)
{
	if (opt_json && !opt_quiet) {
		g_autofree gchar *escaped = g_strescape(error->message, NULL);

		g_print("{\"error\":{\"code\":%d,\"message\":\"%s\"}}\n",
			error->domain == GSURF_CONTROL_ERROR ? gsurf_control_error_to_code(error) : -32005,
			escaped);
	}
	g_printerr("gsurfctl: %s%s%s\n", opt_all ? instance : "", opt_all ? ": " : "",
		error->message);
	return status_for(error);
}

/* One K=V value: JSON when it parses (12, true, null, [1], "s"), else the
 * text as a string. */
static JsonNode *
value_from_text(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *node;

	if (json_parser_load_from_data(parser, text, -1, NULL) &&
	    json_parser_get_root(parser) != NULL)
		return json_node_copy(json_parser_get_root(parser));
	node = json_node_new(JSON_NODE_VALUE);
	json_node_set_string(node, text);
	return node;
}

/* `call METHOD [JSON|K=V...]` params. */
static JsonObject *
call_params(Words *w, GError **error)
{
	g_autoptr(JsonObject) params = json_object_new();
	guint i;

	for (i = 1; i < w->args->len; i++) {
		const gchar *arg = g_ptr_array_index(w->args, i);
		const gchar *eq = strchr(arg, '=');

		if (arg[0] == '{') {
			g_autoptr(JsonParser) parser = json_parser_new();
			JsonObject *object;
			g_autoptr(GList) members = NULL;
			GList *l;

			if (!json_parser_load_from_data(parser, arg, -1, NULL) ||
			    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
				usage_error(error, "call: '%s' is not a JSON object", arg);
				return NULL;
			}
			object = json_node_get_object(json_parser_get_root(parser));
			members = json_object_get_members(object);
			for (l = members; l != NULL; l = l->next)
				json_object_set_member(params, l->data,
					json_object_dup_member(object, l->data));
		} else if (eq != NULL && eq != arg) {
			g_autofree gchar *key = g_strndup(arg, (gsize)(eq - arg));

			json_object_set_member(params, key, value_from_text(eq + 1));
		} else {
			usage_error(error, "call: '%s' is neither a JSON object nor KEY=VALUE", arg);
			return NULL;
		}
	}
	return g_steal_pointer(&params);
}

static volatile sig_atomic_t interrupted = 0;

static void
on_interrupt(int signum)
{
	(void)signum;
	interrupted = 1;
}

static gint
run_watch(GsurfControlClient *client, const gchar *instance, Words *w)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) topics = g_ptr_array_new();
	guint i;

	for (i = 0; i < w->args->len; i++)
		g_ptr_array_add(topics, g_ptr_array_index(w->args, i));
	g_ptr_array_add(topics, NULL);
	if (!gsurf_control_client_subscribe(client, (const gchar *const *)topics->pdata, &error))
		return report(instance, error);
	signal(SIGINT, on_interrupt);
	signal(SIGTERM, on_interrupt);
	while (!interrupted) {
		g_autoptr(JsonNode) event = gsurf_control_client_read_event(client, 250, &error);
		JsonObject *object;

		if (event == NULL) {
			if (error != NULL) {
				/* The instance quit: the stream simply ends. */
				if (g_error_matches(error, GSURF_CONTROL_ERROR, GSURF_CONTROL_ERROR_CLOSED))
					return EXIT_OK;
				return report(instance, error);
			}
			continue;
		}
		if (opt_json) {
			g_autofree gchar *text = json_text(event);

			g_print("%s\n", text);
		} else if (JSON_NODE_HOLDS_OBJECT(event)) {
			JsonNode *data;
			g_autofree gchar *line = NULL;

			object = json_node_get_object(event);
			data = json_object_has_member(object, "data")
				? json_object_get_member(object, "data") : NULL;
			line = data != NULL && JSON_NODE_HOLDS_OBJECT(data) &&
				is_flat(json_node_get_object(data))
				? flat_line(json_node_get_object(data))
				: (data != NULL ? json_text(data) : g_strdup(""));
			g_print("[%s] %s %s\n", str_member(object, "topic"),
				str_member(object, "event"), line);
		}
		fflush(stdout);
	}
	return EXIT_OK;
}

/* Prints the "name" member of every element of array @result. */
static void
print_names(JsonNode *result)
{
	JsonArray *array = JSON_NODE_HOLDS_ARRAY(result) ? json_node_get_array(result) : NULL;
	guint i;

	for (i = 0; array != NULL && i < json_array_get_length(array); i++)
		g_print("%s\n", str_member(json_array_get_object_element(array, i), "name"));
}

/* Runs command @cmd (words @w) on the instance at @path. */
static gint
run_on(const gchar *path, const gchar *instance, const Command *cmd, Words *w)
{
	g_autoptr(GsurfControlClient) client = gsurf_control_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) params = NULL;
	g_autoptr(JsonNode) result = NULL;
	const gchar *method = cmd->method;

	if (!gsurf_control_client_connect(client, path, &error)) {
		g_printerr("gsurfctl: %s: cannot connect: %s\n", instance, error->message);
		return EXIT_NO_INSTANCE;
	}
	gsurf_control_client_set_timeout(client,
		opt_timeout > 0.0 ? (guint)(opt_timeout * 1000.0) : 0);

	/* 1. The specials. */
	if (g_strcmp0(cmd->name, "watch") == 0)
		return run_watch(client, instance, w);
	if (g_strcmp0(cmd->name, "call") == 0) {
		method = g_ptr_array_index(w->args, 0);
		params = call_params(w, &error);
		if (params == NULL) {
			g_printerr("gsurfctl: %s\n", error->message);
			return EXIT_USAGE;
		}
	} else if (g_strcmp0(cmd->name, "mode") == 0) {
		method = w->args->len > 0 ? "mode.set" : "mode.get";
	}

	/* 2. The params. */
	if (params == NULL) {
		params = json_object_new();
		if (cmd->build != NULL && !cmd->build(w, params, &error)) {
			g_printerr("gsurfctl: %s\n", error->message);
			return EXIT_USAGE;
		}
	}

	/* 3. The call and its answer. */
	result = gsurf_control_client_call(client, method, params, &error);
	if (result == NULL)
		return report(instance, error);
	if (g_hash_table_contains(w->flags, "names") && !opt_json) {
		print_names(result);
		return EXIT_OK;
	}
	print_result(cmd->name, instance, result);
	return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* Instances                                                           */
/* ------------------------------------------------------------------ */

/* A label for @inst: its name, else its pid. */
static gchar *
label_for(GsurfControlInstance *inst)
{
	if (gsurf_control_instance_get_name(inst) != NULL)
		return g_strdup(gsurf_control_instance_get_name(inst));
	return g_strdup_printf("%" G_GINT64_FORMAT, gsurf_control_instance_get_pid(inst));
}

/* instance.info of @inst, or NULL. */
static JsonNode *
info_of(GsurfControlInstance *inst)
{
	g_autoptr(GsurfControlClient) client = gsurf_control_client_new();

	if (!gsurf_control_client_connect(client, gsurf_control_instance_get_socket_path(inst),
			NULL))
		return NULL;
	gsurf_control_client_set_timeout(client, 3000);
	return gsurf_control_client_call(client, "instance.info", NULL, NULL);
}

static gint
cmd_list(gboolean names_only)
{
	g_autoptr(GPtrArray) list = gsurf_control_list_instances(NULL);
	g_autoptr(JsonBuilder) b = NULL;
	guint i;

	if (names_only) {
		for (i = 0; i < list->len; i++) {
			GsurfControlInstance *inst = g_ptr_array_index(list, i);

			if (gsurf_control_instance_get_name(inst) != NULL)
				g_print("%s\n", gsurf_control_instance_get_name(inst));
			g_print("%" G_GINT64_FORMAT "\n", gsurf_control_instance_get_pid(inst));
		}
		return EXIT_OK;
	}
	if (opt_json)
		b = json_builder_new();
	if (b != NULL)
		json_builder_begin_array(b);
	for (i = 0; i < list->len; i++) {
		GsurfControlInstance *inst = g_ptr_array_index(list, i);
		g_autoptr(JsonNode) info = info_of(inst);
		JsonObject *object = info != NULL && JSON_NODE_HOLDS_OBJECT(info)
			? json_node_get_object(info) : NULL;

		if (b != NULL) {
			json_builder_begin_object(b);
			json_builder_set_member_name(b, "pid");
			json_builder_add_int_value(b, gsurf_control_instance_get_pid(inst));
			json_builder_set_member_name(b, "name");
			if (gsurf_control_instance_get_name(inst) != NULL)
				json_builder_add_string_value(b, gsurf_control_instance_get_name(inst));
			else
				json_builder_add_null_value(b);
			json_builder_set_member_name(b, "id");
			json_builder_add_string_value(b, gsurf_control_instance_get_id(inst));
			json_builder_set_member_name(b, "socket");
			json_builder_add_string_value(b, gsurf_control_instance_get_socket_path(inst));
			json_builder_set_member_name(b, "info");
			if (info != NULL)
				json_builder_add_value(b, json_node_copy(info));
			else
				json_builder_add_null_value(b);
			json_builder_end_object(b);
			continue;
		}
		if (opt_quiet)
			continue;
		g_print("%-8" G_GINT64_FORMAT " %-12s %3" G_GINT64_FORMAT " views  %s\n",
			gsurf_control_instance_get_pid(inst),
			gsurf_control_instance_get_name(inst) != NULL
				? gsurf_control_instance_get_name(inst) : "-",
			int_member(object, "views"), str_member(object, "uri"));
	}
	if (b != NULL) {
		g_autoptr(JsonNode) root = NULL;
		g_autofree gchar *text = NULL;

		json_builder_end_array(b);
		root = json_builder_get_root(b);
		text = json_text(root);
		g_print("%s\n", text);
	}
	if (list->len == 0 && !opt_json && !opt_quiet)
		g_printerr("gsurfctl: no gsurf is running\n");
	return EXIT_OK;
}

/*
 * pick_instance:
 *
 * -S, else -i / $GSURF_INSTANCE, else the only instance, else the most
 * recently focused one (instance.info focused_at). Sets @path and
 * @label; returns an exit status (EXIT_OK when found).
 */
static gint
pick_instance(gchar **path, gchar **label)
{
	const gchar *spec = opt_instance != NULL ? opt_instance : g_getenv("GSURF_INSTANCE");
	g_autoptr(GPtrArray) list = NULL;
	gint64 best_time = G_MININT64;
	gint best = -1;
	guint i;

	if (opt_socket != NULL) {
		*path = g_strdup(opt_socket);
		*label = g_strdup(opt_socket);
		return EXIT_OK;
	}
	if (spec != NULL && *spec != '\0') {
		g_autoptr(GError) error = NULL;
		g_autoptr(GsurfControlInstance) inst = gsurf_control_find_instance(NULL, spec, &error);

		if (inst == NULL) {
			g_printerr("gsurfctl: %s (gsurfctl list shows the running ones)\n",
				error->message);
			return EXIT_NO_INSTANCE;
		}
		*path = g_strdup(gsurf_control_instance_get_socket_path(inst));
		*label = label_for(inst);
		return EXIT_OK;
	}
	list = gsurf_control_list_instances(NULL);
	if (list->len == 0) {
		g_printerr("gsurfctl: no gsurf is running\n");
		return EXIT_NO_INSTANCE;
	}
	if (list->len == 1) {
		best = 0;
	} else {
		for (i = 0; i < list->len; i++) {
			g_autoptr(JsonNode) info = info_of(g_ptr_array_index(list, i));
			gint64 at;

			if (info == NULL || !JSON_NODE_HOLDS_OBJECT(info))
				continue;
			at = int_member(json_node_get_object(info), "focused_at");
			if (best < 0 || at > best_time) {
				best = (gint)i;
				best_time = at;
			}
		}
		if (best < 0)
			best = (gint)list->len - 1;
	}
	*path = g_strdup(gsurf_control_instance_get_socket_path(g_ptr_array_index(list, best)));
	*label = label_for(g_ptr_array_index(list, best));
	return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* Arguments                                                           */
/* ------------------------------------------------------------------ */

/* The global options that take a value as the next word. */
static gboolean
takes_value(const gchar *word)
{
	static const gchar *const valued[] = { "-i", "--instance", "-S", "--socket", "-T",
		"--timeout", "--completion", NULL };

	return g_strv_contains(valued, word);
}

/* Whether @word is one of gsurfctl's own options (not a command flag). */
static gboolean
is_global_option(const gchar *word)
{
	static const gchar *const switches[] = { "-a", "--all", "-j", "--json", "-p", "--pretty",
		"-q", "--quiet", "-V", "--version", "--license", "-h", "--help", "-?",
		"--help-all", NULL };
	static const gchar *const prefixes[] = { "--instance=", "--socket=", "--timeout=",
		"--completion=", NULL };
	guint i;

	if (takes_value(word) || g_strv_contains(switches, word))
		return TRUE;
	for (i = 0; prefixes[i] != NULL; i++)
		if (g_str_has_prefix(word, prefixes[i]))
			return TRUE;
	/* Bundled short switches such as -jp, or -i5 / -T2. */
	if (word[0] == '-' && word[1] != '-' && word[1] != '\0' && !g_ascii_isdigit(word[1])) {
		const gchar *p;

		if (strchr("iST", word[1]) != NULL)
			return TRUE;
		for (p = word + 1; *p != '\0'; p++)
			if (strchr("ajpqVh", *p) == NULL)
				return FALSE;
		return TRUE;
	}
	return FALSE;
}

/*
 * Splits the command line into gsurfctl's options (argv[0] first, for
 * GOption) and the command's words: options anywhere until "--" or,
 * for wmux, eval and call, until the command word.
 */
static void
split_arguments(gint argc, gchar **argv, GPtrArray **out_options, GPtrArray **out_words)
{
	GPtrArray *options = g_ptr_array_new();
	GPtrArray *words = g_ptr_array_new();
	gboolean ended = FALSE;
	gboolean have_command = FALSE;
	gint i;

	g_ptr_array_add(options, argv[0]);
	for (i = 1; i < argc; i++) {
		if (!ended && strcmp(argv[i], "--") == 0) {
			ended = TRUE;
			continue;
		}
		if (!ended && is_global_option(argv[i])) {
			g_ptr_array_add(options, argv[i]);
			if (takes_value(argv[i]) && i + 1 < argc)
				g_ptr_array_add(options, argv[++i]);
			continue;
		}
		if (!have_command) {
			const Command *cmd = find_command(argv[i]);

			have_command = TRUE;
			if (cmd != NULL && cmd->literal)
				ended = TRUE;
		}
		g_ptr_array_add(words, argv[i]);
	}
	g_ptr_array_add(options, NULL);
	g_ptr_array_add(words, NULL);
	*out_options = options;
	*out_words = words;
}

static gint
cmd_help(gint argc, gchar **argv, GOptionContext *context)
{
	const Command *cmd;
	guint i;

	if (argc > 0 && strcmp(argv[0], "--names") == 0) {
		for (i = 0; commands[i].name != NULL; i++)
			g_print("%s\n", commands[i].name);
		return EXIT_OK;
	}
	if (argc > 0) {
		cmd = find_command(argv[0]);
		if (cmd == NULL) {
			g_printerr("gsurfctl: unknown command '%s' (gsurfctl help lists them)\n", argv[0]);
			return EXIT_USAGE;
		}
		g_print("usage: gsurfctl %s\n  %s\n", cmd->usage, cmd->summary);
		if (cmd->method != NULL)
			g_print("  method: %s\n", cmd->method);
		return EXIT_OK;
	}
	{
		g_autofree gchar *help = g_option_context_get_help(context, TRUE, NULL);

		g_print("%s", help);
	}
	return EXIT_OK;
}

int
main(int argc, char **argv)
{
	g_autoptr(GOptionContext) context = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) options = NULL;
	g_autoptr(GPtrArray) words_argv = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *label = NULL;
	const Command *cmd;
	Words w = { NULL, NULL };
	gint status;

	context = g_option_context_new("COMMAND [ARGUMENTS...] - drive a running gsurf");
	g_option_context_set_summary(context,
		"Talks to a running gsurf's control socket\n"
		"($XDG_RUNTIME_DIR/gsurf/<pid>-XXXXXX/control.sock; control.enabled in the config).");
	g_option_context_set_description(context, help_description);
	g_option_context_add_main_entries(context, entries, NULL);

	split_arguments(argc, argv, &options, &words_argv);
	{
		gint n = (gint)options->len - 1;
		gchar **vector = (gchar **)options->pdata;

		if (!g_option_context_parse(context, &n, &vector, &error)) {
			g_printerr("gsurfctl: %s\n", error->message);
			return EXIT_USAGE;
		}
		if (n > 1) {
			g_printerr("gsurfctl: unexpected '%s'\n", vector[1]);
			return EXIT_USAGE;
		}
	}
	argc = (gint)words_argv->len - 1;
	argv = (gchar **)words_argv->pdata;

	/* 1. What needs no instance. */
	if (opt_version) {
		g_print("gsurfctl %s\n", gsurf_get_version_string());
		return EXIT_OK;
	}
	if (opt_license) {
		g_print("%s", license_text);
		return EXIT_OK;
	}
	if (opt_completion != NULL) {
		if (strcmp(opt_completion, "bash") != 0) {
			g_printerr("gsurfctl: --completion: only bash is supported\n");
			return EXIT_USAGE;
		}
		g_print("%s", bash_completion);
		return EXIT_OK;
	}
	if (argc < 1) {
		g_autofree gchar *help = g_option_context_get_help(context, TRUE, NULL);

		g_printerr("%s", help);
		return EXIT_USAGE;
	}
	cmd = find_command(argv[0]);
	if (cmd == NULL) {
		g_printerr("gsurfctl: unknown command '%s' (gsurfctl help lists them)\n", argv[0]);
		return EXIT_USAGE;
	}
	if (g_strcmp0(cmd->name, "help") == 0)
		return cmd_help(argc - 1, argv + 1, context);
	if (!parse_words(cmd, argc - 1, argv + 1, &w, &error)) {
		g_printerr("gsurfctl: %s\n", error->message);
		words_clear(&w);
		return EXIT_USAGE;
	}
	if (g_strcmp0(cmd->name, "list") == 0) {
		status = cmd_list(g_hash_table_contains(w.flags, "names"));
		words_clear(&w);
		return status;
	}

	/* 2. Every instance. */
	if (opt_all) {
		g_autoptr(GPtrArray) list = gsurf_control_list_instances(NULL);
		guint i;

		if (list->len == 0) {
			g_printerr("gsurfctl: no gsurf is running\n");
			words_clear(&w);
			return EXIT_NO_INSTANCE;
		}
		status = EXIT_OK;
		for (i = 0; i < list->len; i++) {
			GsurfControlInstance *inst = g_ptr_array_index(list, i);
			g_autofree gchar *name = label_for(inst);
			gint one = run_on(gsurf_control_instance_get_socket_path(inst), name, cmd, &w);

			if (one != EXIT_OK)
				status = one;
		}
		words_clear(&w);
		return status;
	}

	/* 3. One instance. */
	status = pick_instance(&path, &label);
	if (status == EXIT_OK)
		status = run_on(path, label, cmd, &w);
	words_clear(&w);
	return status;
}
