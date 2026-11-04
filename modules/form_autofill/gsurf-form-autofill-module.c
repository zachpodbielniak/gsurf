/*
 * gsurf-form-autofill-module.c - Fill web forms from YAML profiles
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Identity, address and payment-card profiles come from the module's
 * `profiles:` list in config.yaml and from a separate private file
 * (`file:`, default $XDG_CONFIG_HOME/gsurf/autofill.yaml). The page side
 * (js/form-autofill.js) runs in the isolated script world
 * "gsurf-autofill", so page scripts can neither see it nor call it.
 *
 * Data flow, which is the whole security model:
 *
 *   page loads -> script posts {ready, nonce}  -> host checks enabled +
 *                 exclude rules on the top-level host -> arm(labels only)
 *   user picks a label (dropdown, fill key or `form-autofill` action)
 *              -> script posts {pick, nonce, label, kind}
 *              -> host builds the values of THAT profile, only the card
 *                 part or only the address part, card only on https
 *              -> fill(values) in the isolated world writes them into
 *                 the focused field's form and drops them.
 *
 * Profile values never reach the page's main world except as the values
 * written into its fields, and they are never logged.
 *
 * Hooks: #GsurfScriptInjector (script + message handler per view),
 * #GsurfInputHandler (the fill key), #GsurfActionHandler (`form-autofill`)
 * and #GsurfKeybindProvider (the keybinding overlay).
 */

#define G_LOG_DOMAIN "gsurf-form-autofill"

#include <gsurf/gsurf.h>
#include <gmodule.h>
#include <yaml-glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "gsurf-form-autofill-js.h"

/* The isolated world and the message handler name the script posts to. */
#define AUTOFILL_WORLD   "gsurf-autofill"
#define AUTOFILL_HANDLER "gsurfAutofill"
#define AUTOFILL_DEFAULT_KEY "Ctrl+Alt+f"
/* Marks a view the module already injected into. */
#define AUTOFILL_VIEW_KEY "gsurf-form-autofill-injected"

/* ------------------------------------------------------------------ */
/* Profiles                                                            */
/* ------------------------------------------------------------------ */

/* Scalar fields; `street` and `cc_exp` are stored separately. */
typedef enum {
	FIELD_GIVEN_NAME,
	FIELD_FAMILY_NAME,
	FIELD_EMAIL,
	FIELD_TEL,
	FIELD_ORGANIZATION,
	FIELD_CITY,
	FIELD_REGION,
	FIELD_POSTAL_CODE,
	FIELD_COUNTRY,
	FIELD_CC_NAME,
	FIELD_CC_NUMBER,
	N_TEXT_FIELDS
} AutofillField;

typedef struct {
	gchar  *label;
	gchar  *text[N_TEXT_FIELDS]; /* validated, normalized; NULL = unset */
	GStrv   street;              /* 1-3 lines, or NULL */
	guint   exp_month;           /* 1-12, 0 = unset */
	guint   exp_year;            /* four digits, 0 = unset */
} AutofillProfile;

/* A validator returns the normalized value, or NULL and sets @why to a
 * reason that names no part of the value (reasons are logged). */
typedef gchar *(*FieldCheck)(const gchar *raw, const gchar **why);

static gchar *check_text(const gchar *raw, const gchar **why);
static gchar *check_email(const gchar *raw, const gchar **why);
static gchar *check_tel(const gchar *raw, const gchar **why);
static gchar *check_postal(const gchar *raw, const gchar **why);
static gchar *check_country(const gchar *raw, const gchar **why);
static gchar *check_card_number(const gchar *raw, const gchar **why);

typedef struct {
	const gchar   *key;
	AutofillField  field;
	gboolean       card;
	FieldCheck     check;
} FieldSpec;

/* The YAML keys of a profile with a scalar value. */
static const FieldSpec FIELD_SPECS[] = {
	{ "given_name",   FIELD_GIVEN_NAME,   FALSE, check_text },
	{ "family_name",  FIELD_FAMILY_NAME,  FALSE, check_text },
	{ "email",        FIELD_EMAIL,        FALSE, check_email },
	{ "tel",          FIELD_TEL,          FALSE, check_tel },
	{ "organization", FIELD_ORGANIZATION, FALSE, check_text },
	{ "city",         FIELD_CITY,         FALSE, check_text },
	{ "region",       FIELD_REGION,       FALSE, check_text },
	{ "postal_code",  FIELD_POSTAL_CODE,  FALSE, check_postal },
	{ "country",      FIELD_COUNTRY,      FALSE, check_country },
	{ "cc_name",      FIELD_CC_NAME,      TRUE,  check_text },
	{ "cc_number",    FIELD_CC_NUMBER,    TRUE,  check_card_number },
};

static AutofillProfile *
profile_new(const gchar *label)
{
	AutofillProfile *p = g_new0(AutofillProfile, 1);

	p->label = g_strdup(label);
	return p;
}

static void
profile_free(gpointer data)
{
	AutofillProfile *p = data;
	guint i;

	if (p == NULL)
		return;
	g_free(p->label);
	/* Values are personal data: scrub them before they go back to the
	 * allocator. */
	for (i = 0; i < N_TEXT_FIELDS; i++) {
		if (p->text[i] != NULL) {
			memset(p->text[i], 0, strlen(p->text[i]));
			g_free(p->text[i]);
		}
	}
	if (p->street != NULL) {
		for (i = 0; p->street[i] != NULL; i++)
			memset(p->street[i], 0, strlen(p->street[i]));
		g_strfreev(p->street);
	}
	g_free(p);
}

static gboolean
profile_has_card(const AutofillProfile *p)
{
	return p->text[FIELD_CC_NAME] != NULL || p->text[FIELD_CC_NUMBER] != NULL ||
	       p->exp_month != 0;
}

/* Field names a profile carries, for the test/debug summary. */
static gchar *
profile_describe(const AutofillProfile *p)
{
	GString *s = g_string_new(p->label);
	guint i;

	g_string_append_c(s, ':');
	for (i = 0; i < G_N_ELEMENTS(FIELD_SPECS); i++) {
		if (p->text[FIELD_SPECS[i].field] != NULL)
			g_string_append_printf(s, " %s", FIELD_SPECS[i].key);
	}
	if (p->street != NULL)
		g_string_append_printf(s, " street[%u]", g_strv_length(p->street));
	if (p->exp_month != 0)
		g_string_append(s, " cc_exp");
	return g_string_free(s, FALSE);
}

/* ---- validators ---- */

/* One line of printable text, at most 256 bytes. */
static gchar *
check_text(const gchar *raw, const gchar **why)
{
	g_autofree gchar *s = g_strstrip(g_strdup(raw));
	const gchar *p;

	if (*s == '\0') {
		*why = "is empty";
		return NULL;
	}
	if (strlen(s) > 256 || !g_utf8_validate(s, -1, NULL)) {
		*why = "is too long or not UTF-8";
		return NULL;
	}
	for (p = s; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f) {
			*why = "contains control characters";
			return NULL;
		}
	}
	return g_steal_pointer(&s);
}

static gchar *
check_email(const gchar *raw, const gchar **why)
{
	g_autofree gchar *s = check_text(raw, why);
	const gchar *at;

	if (s == NULL)
		return NULL;
	at = strchr(s, '@');
	if (at == NULL || at == s || strchr(at + 1, '@') != NULL ||
	    strchr(at + 1, '.') == NULL || strchr(s, ' ') != NULL ||
	    at[1] == '.' || g_str_has_suffix(s, ".")) {
		*why = "is not a valid e-mail address";
		return NULL;
	}
	return g_steal_pointer(&s);
}

/* Digits with the usual separators, an optional leading '+', 3-20
 * digits. */
static gchar *
check_tel(const gchar *raw, const gchar **why)
{
	g_autofree gchar *s = check_text(raw, why);
	const gchar *p;
	guint digits = 0;

	if (s == NULL)
		return NULL;
	for (p = s; *p != '\0'; p++) {
		if (g_ascii_isdigit(*p))
			digits++;
		else if (*p == '+' && p == s)
			continue;
		else if (strchr(" -().", *p) == NULL) {
			*why = "is not a telephone number";
			return NULL;
		}
	}
	if (digits < 3 || digits > 20) {
		*why = "is not a telephone number";
		return NULL;
	}
	return g_steal_pointer(&s);
}

static gchar *
check_postal(const gchar *raw, const gchar **why)
{
	g_autofree gchar *s = check_text(raw, why);
	const gchar *p;

	if (s == NULL)
		return NULL;
	if (strlen(s) < 2 || strlen(s) > 12) {
		*why = "is not a postal code";
		return NULL;
	}
	for (p = s; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != ' ' && *p != '-') {
			*why = "is not a postal code";
			return NULL;
		}
	}
	return g_steal_pointer(&s);
}

/* An ISO 3166-1 alpha-2 code, stored uppercase. */
static gchar *
check_country(const gchar *raw, const gchar **why)
{
	g_autofree gchar *s = check_text(raw, why);

	if (s == NULL)
		return NULL;
	if (strlen(s) != 2 || !g_ascii_isalpha(s[0]) || !g_ascii_isalpha(s[1])) {
		*why = "is not a two-letter ISO 3166-1 country code";
		return NULL;
	}
	return g_ascii_strup(s, -1);
}

/* Luhn checksum over a string of digits. */
static gboolean
luhn_valid(const gchar *digits)
{
	gsize n = strlen(digits), i;
	guint sum = 0;

	for (i = 0; i < n; i++) {
		guint d = (guint)(digits[n - 1 - i] - '0');

		if ((i & 1) != 0) {
			d *= 2;
			if (d > 9)
				d -= 9;
		}
		sum += d;
	}
	return sum % 10 == 0;
}

/* 12-19 digits (spaces and dashes are dropped) passing the Luhn check;
 * stored as bare digits. */
static gchar *
check_card_number(const gchar *raw, const gchar **why)
{
	GString *digits = g_string_new(NULL);
	const gchar *p;

	for (p = raw; *p != '\0'; p++) {
		if (g_ascii_isdigit(*p)) {
			g_string_append_c(digits, *p);
		} else if (*p != ' ' && *p != '-') {
			*why = "is not a card number";
			goto fail;
		}
	}
	if (digits->len < 12 || digits->len > 19) {
		*why = "is not a card number (12-19 digits)";
		goto fail;
	}
	if (!luhn_valid(digits->str)) {
		*why = "fails the card number (Luhn) checksum";
		goto fail;
	}
	return g_string_free(digits, FALSE);

fail:
	memset(digits->str, 0, digits->len);
	g_string_free(digits, TRUE);
	return NULL;
}

/* "MM/YY" or "MM/YYYY" (also with '-'). */
static gboolean
parse_expiry(const gchar *raw, guint *month, guint *year)
{
	g_autofree gchar *s = g_strstrip(g_strdup(raw));
	g_auto(GStrv) parts = NULL;
	guint64 m, y;
	gsize ylen;

	parts = g_regex_split_simple("[/-]", s, 0, 0);
	if (g_strv_length(parts) != 2)
		return FALSE;
	ylen = strlen(parts[1]);
	if (strlen(parts[0]) < 1 || strlen(parts[0]) > 2 || (ylen != 2 && ylen != 4))
		return FALSE;
	if (!g_ascii_string_to_unsigned(parts[0], 10, 1, 12, &m, NULL) ||
	    !g_ascii_string_to_unsigned(parts[1], 10, 0, 9999, &y, NULL))
		return FALSE;
	if (ylen == 2)
		y += 2000;
	else if (y < 2000)
		return FALSE;
	*month = (guint)m;
	*year = (guint)y;
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Module                                                              */
/* ------------------------------------------------------------------ */

#define GSURF_TYPE_FORM_AUTOFILL_MODULE (gsurf_form_autofill_module_get_type())
G_DECLARE_FINAL_TYPE(GsurfFormAutofillModule, gsurf_form_autofill_module,
                     GSURF, FORM_AUTOFILL_MODULE, GsurfModule)

struct _GsurfFormAutofillModule
{
	GsurfModule      parent_instance;
	GPtrArray       *profiles;        /* AutofillProfile*, merged */
	gchar           *file;            /* resolved profile file, or NULL */
	gboolean         suggest_on_focus;
	gchar           *key_fill;        /* normalized, or NULL = no key */
	GsurfHostRules  *exclude;
	gboolean         exclude_broken;  /* bad rules: refuse everywhere */
};

enum {
	PROP_0,
	PROP_N_PROFILES,
	PROP_SUMMARY,
	PROP_FILE,
	PROP_SUGGEST_ON_FOCUS,
	PROP_KEY_FILL,
	PROP_N_EXCLUDE,
	PROP_EXCLUDE_BROKEN,
	N_PROPS
};

static GParamSpec *props[N_PROPS];

static void gsurf_form_autofill_injector_init(GsurfScriptInjectorInterface *iface);
static void gsurf_form_autofill_input_init(GsurfInputHandlerInterface *iface);
static void gsurf_form_autofill_action_init(GsurfActionHandlerInterface *iface);
static void gsurf_form_autofill_keybind_init(GsurfKeybindProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GsurfFormAutofillModule, gsurf_form_autofill_module,
	GSURF_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_SCRIPT_INJECTOR, gsurf_form_autofill_injector_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_INPUT_HANDLER, gsurf_form_autofill_input_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_ACTION_HANDLER, gsurf_form_autofill_action_init)
	G_IMPLEMENT_INTERFACE(GSURF_TYPE_KEYBIND_PROVIDER, gsurf_form_autofill_keybind_init))

static AutofillProfile *
find_profile(GPtrArray *profiles, const gchar *label)
{
	guint i;

	for (i = 0; i < profiles->len; i++) {
		AutofillProfile *p = g_ptr_array_index(profiles, i);

		if (g_strcmp0(p->label, label) == 0)
			return p;
	}
	return NULL;
}

/* ---- profile parsing ---- */

/* The scalar text of @node, or NULL when it is not a scalar. */
static const gchar *
scalar_of(YamlNode *node)
{
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

/* `street`: one string or a list of one to three strings. */
static GStrv
parse_street(YamlNode *node, const gchar **why)
{
	g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
	YamlSequence *seq;
	guint i, n;

	if (scalar_of(node) != NULL) {
		gchar *line = check_text(scalar_of(node), why);

		if (line == NULL)
			return NULL;
		g_strv_builder_take(builder, line);
		return g_strv_builder_end(builder);
	}
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE) {
		*why = "must be a string or a list of strings";
		return NULL;
	}
	seq = yaml_node_get_sequence(node);
	n = yaml_sequence_get_length(seq);
	if (n < 1 || n > 3) {
		*why = "must have one to three lines";
		return NULL;
	}
	for (i = 0; i < n; i++) {
		const gchar *raw = scalar_of(yaml_sequence_get_element(seq, i));
		gchar *line;

		if (raw == NULL) {
			*why = "must be a list of strings";
			return NULL;
		}
		line = check_text(raw, why);
		if (line == NULL)
			return NULL;
		g_strv_builder_take(builder, line);
	}
	return g_strv_builder_end(builder);
}

static const FieldSpec *
find_spec(const gchar *key)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(FIELD_SPECS); i++) {
		if (g_strcmp0(FIELD_SPECS[i].key, key) == 0)
			return &FIELD_SPECS[i];
	}
	return NULL;
}

/* Parse one profile mapping. Card keys are dropped (and *saw_card set)
 * unless @allow_card. Every problem is a warning that names the source,
 * the profile and the key — never the value. Returns NULL for a profile
 * without a usable label. */
static AutofillProfile *
parse_profile(YamlNode *node, guint index, const gchar *source,
              gboolean allow_card, gboolean *saw_card)
{
	YamlMapping *m;
	const gchar *label;
	AutofillProfile *p;
	guint i, n;

	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING) {
		g_warning("form_autofill: %s: profile %u is not a mapping; ignoring it",
		          source, index + 1);
		return NULL;
	}
	m = yaml_node_get_mapping(node);
	label = yaml_mapping_has_member(m, "label")
		? scalar_of(yaml_mapping_get_member(m, "label")) : NULL;
	p = profile_new(label);
	if (p->label == NULL || *g_strstrip(p->label) == '\0') {
		g_warning("form_autofill: %s: profile %u has no label; ignoring it",
		          source, index + 1);
		profile_free(p);
		return NULL;
	}

	n = yaml_mapping_get_size(m);
	for (i = 0; i < n; i++) {
		const gchar *key = yaml_mapping_get_key(m, i);
		YamlNode *value = yaml_mapping_get_value(m, i);
		const FieldSpec *spec = find_spec(key);
		const gchar *why = NULL;
		gboolean card;

		if (g_strcmp0(key, "label") == 0)
			continue;
		if (g_strcmp0(key, "cc_csc") == 0) {
			/* Never stored: see docs/form-autofill.org. */
			g_warning("form_autofill: %s: profile '%s': cc_csc is never stored or "
			          "filled (the card security code must not be kept); ignoring it",
			          source, p->label);
			continue;
		}
		card = (spec != NULL && spec->card) || g_strcmp0(key, "cc_exp") == 0;
		if (spec == NULL && !card && g_strcmp0(key, "street") != 0) {
			g_warning("form_autofill: %s: profile '%s': unknown key '%s'; ignoring it",
			          source, p->label, key);
			continue;
		}
		if (card && !allow_card) {
			*saw_card = TRUE;
			continue;
		}

		if (g_strcmp0(key, "street") == 0) {
			GStrv street = parse_street(value, &why);

			if (street == NULL) {
				g_warning("form_autofill: %s: profile '%s': street %s; ignoring it",
				          source, p->label, why);
				continue;
			}
			g_strfreev(p->street);
			p->street = street;
		} else if (g_strcmp0(key, "cc_exp") == 0) {
			const gchar *raw = scalar_of(value);

			if (raw == NULL || !parse_expiry(raw, &p->exp_month, &p->exp_year)) {
				p->exp_month = p->exp_year = 0;
				g_warning("form_autofill: %s: profile '%s': cc_exp is not MM/YY or "
				          "MM/YYYY; ignoring it", source, p->label);
			}
		} else {
			const gchar *raw = scalar_of(value);
			gchar *norm = raw != NULL ? spec->check(raw, &why) : NULL;

			if (raw == NULL)
				why = "must be a string";
			if (norm == NULL) {
				g_warning("form_autofill: %s: profile '%s': %s %s; ignoring it",
				          source, p->label, key, why);
				continue;
			}
			g_free(p->text[spec->field]);
			p->text[spec->field] = norm;
		}
	}
	return p;
}

/* Append the profiles of the sequence @node to @out. A label repeated
 * within one source replaces the earlier profile (with a warning). */
static void
parse_profiles(GPtrArray *out, YamlNode *node, const gchar *source,
               gboolean allow_card, gboolean *saw_card)
{
	YamlSequence *seq;
	guint i, n, j;

	if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
		return;
	if (yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE) {
		g_warning("form_autofill: %s: profiles must be a list; ignoring them", source);
		return;
	}
	seq = yaml_node_get_sequence(node);
	n = yaml_sequence_get_length(seq);
	for (i = 0; i < n; i++) {
		AutofillProfile *p = parse_profile(yaml_sequence_get_element(seq, i), i,
			source, allow_card, saw_card);
		gboolean replaced = FALSE;

		if (p == NULL)
			continue;
		for (j = 0; j < out->len; j++) {
			AutofillProfile *old = g_ptr_array_index(out, j);

			if (g_strcmp0(old->label, p->label) == 0) {
				g_warning("form_autofill: %s: duplicate profile label '%s'; the later "
				          "one replaces the earlier", source, p->label);
				profile_free(old);
				out->pdata[j] = p;
				replaced = TRUE;
				break;
			}
		}
		if (!replaced)
			g_ptr_array_add(out, p);
	}
}

/* Overlay the inline profile @src onto the file profile @dst: every
 * field @src sets replaces @dst's. */
static void
profile_overlay(AutofillProfile *dst, const AutofillProfile *src)
{
	guint i;

	for (i = 0; i < N_TEXT_FIELDS; i++) {
		if (src->text[i] != NULL) {
			g_free(dst->text[i]);
			dst->text[i] = g_strdup(src->text[i]);
		}
	}
	if (src->street != NULL) {
		g_strfreev(dst->street);
		dst->street = g_strdupv(src->street);
	}
	if (src->exp_month != 0) {
		dst->exp_month = src->exp_month;
		dst->exp_year = src->exp_year;
	}
}

/* Load the private profile file into @out. Card data is kept only when
 * the file is private (a regular file owned by us, no group/other
 * access); otherwise the rest of the file is used and the card data is
 * refused with a warning. */
static void
load_file(GsurfFormAutofillModule *self, GPtrArray *out, gboolean explicit_file)
{
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) private_error = NULL;
	g_autofree gchar *source = NULL;
	YamlDocument *doc;
	YamlNode *root;
	YamlMapping *m;
	gboolean is_private, saw_card = FALSE;
	guint i, n;

	if (self->file == NULL)
		return;
	if (!g_file_test(self->file, G_FILE_TEST_EXISTS)) {
		/* The default path is optional; a configured one is not. */
		if (explicit_file)
			g_warning("form_autofill: profile file '%s' does not exist", self->file);
		return;
	}
	source = g_strdup_printf("profile file '%s'", self->file);
	is_private = gsurf_private_file_is_private(self->file, &private_error);

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_file(parser, self->file, &error)) {
		g_warning("form_autofill: %s cannot be read: %s", source, error->message);
		return;
	}
	doc = yaml_parser_get_document(parser, 0);
	root = doc != NULL ? yaml_document_get_root(doc) : NULL;
	if (root == NULL || yaml_node_get_node_type(root) == YAML_NODE_NULL)
		return;
	if (yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_warning("form_autofill: %s must be a mapping with a 'profiles' list", source);
		return;
	}
	m = yaml_node_get_mapping(root);
	n = yaml_mapping_get_size(m);
	for (i = 0; i < n; i++) {
		const gchar *key = yaml_mapping_get_key(m, i);

		if (g_strcmp0(key, "profiles") != 0)
			g_warning("form_autofill: %s: unknown key '%s'; ignoring it", source, key);
	}
	parse_profiles(out, yaml_mapping_has_member(m, "profiles")
		? yaml_mapping_get_member(m, "profiles") : NULL,
		source, is_private, &saw_card);
	if (saw_card)
		g_warning("form_autofill: card data in %s refused: %s",
		          source, private_error != NULL ? private_error->message : "not private");
}

/* ---- configuration ---- */

static void
reset_config(GsurfFormAutofillModule *self)
{
	g_ptr_array_set_size(self->profiles, 0);
	g_clear_pointer(&self->file, g_free);
	self->suggest_on_focus = TRUE;
	g_free(self->key_fill);
	self->key_fill = g_strdup(AUTOFILL_DEFAULT_KEY);
	gsurf_host_rules_clear(self->exclude);
	self->exclude_broken = FALSE;
}

/* The fill key must carry Ctrl, Alt or Super: a bare key could not work
 * anyway, since bare keys reach the page while a field is focused. */
static gchar *
parse_key(const gchar *raw)
{
	g_autofree gchar *norm = NULL;

	if (raw == NULL || *raw == '\0')
		return NULL;
	norm = gsurf_keys_normalize(raw);
	if (norm == NULL || !(g_str_has_prefix(norm, "Ctrl+") ||
	    g_str_has_prefix(norm, "Alt+") || g_str_has_prefix(norm, "Super+") ||
	    strstr(norm, "+Super+") != NULL || strstr(norm, "+Alt+") != NULL)) {
		g_warning("form_autofill: key_fill '%s' needs a Ctrl, Alt or Super modifier; "
		          "the fill key is disabled", raw);
		return NULL;
	}
	return g_steal_pointer(&norm);
}

static const gchar *const CONFIG_KEYS[] = {
	"enabled", "file", "suggest_on_focus", "key_fill", "exclude", "profiles", NULL
};

static void
gsurf_form_autofill_configure(GsurfModule *module, gpointer config_ptr)
{
	GsurfFormAutofillModule *self = GSURF_FORM_AUTOFILL_MODULE(module);
	GsurfConfig *config = config_ptr;
	g_autoptr(GPtrArray) inline_profiles = g_ptr_array_new_with_free_func(profile_free);
	g_autoptr(GError) error = NULL;
	YamlNode *node;
	YamlMapping *m = NULL;
	gboolean explicit_file = FALSE, saw_card = FALSE;
	guint i, n;

	/* Every configure() starts from the defaults, so a second call
	 * (reload) replaces the whole state, including going back to it. */
	reset_config(self);

	node = config != NULL ? gsurf_config_get_module_node(config, "form_autofill") : NULL;
	if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING)
		m = yaml_node_get_mapping(node);

	/* Unknown keys are reported, not fatal. */
	if (m != NULL) {
		n = yaml_mapping_get_size(m);
		for (i = 0; i < n; i++) {
			const gchar *key = yaml_mapping_get_key(m, i);

			if (!g_strv_contains(CONFIG_KEYS, key))
				g_warning("form_autofill: unknown key '%s'; ignoring it", key);
		}
	}

	/* The profile file: configured, or the XDG default. */
	if (m != NULL && yaml_mapping_has_member(m, "file")) {
		const gchar *raw = scalar_of(yaml_mapping_get_member(m, "file"));

		explicit_file = TRUE;
		if (raw != NULL && *raw != '\0')
			self->file = gsurf_expand_home(raw);
	} else {
		self->file = g_build_filename(g_get_user_config_dir(), "gsurf",
			"autofill.yaml", NULL);
	}

	if (m != NULL && yaml_mapping_has_member(m, "suggest_on_focus"))
		self->suggest_on_focus = yaml_mapping_get_boolean_member(m, "suggest_on_focus");

	if (m != NULL && yaml_mapping_has_member(m, "key_fill")) {
		g_free(self->key_fill);
		self->key_fill = parse_key(scalar_of(yaml_mapping_get_member(m, "key_fill")));
	}

	/* Exclude rules fail closed: a bad pattern disables autofill on every
	 * host until fixed, rather than filling where the user said not to. */
	if (m != NULL && yaml_mapping_has_member(m, "exclude") &&
	    !gsurf_host_rules_load(self->exclude, yaml_mapping_get_member(m, "exclude"),
	                           "match", &error)) {
		g_warning("form_autofill: exclude: %s; autofill is disabled on every host "
		          "until this is fixed", error->message);
		gsurf_host_rules_clear(self->exclude);
		self->exclude_broken = TRUE;
	}

	/* Profiles: the file first, then the inline list. Inline card data
	 * is refused: config.yaml is often shared or kept in a dotfiles
	 * repository, so card data lives only in the private file. */
	load_file(self, self->profiles, explicit_file);
	if (m != NULL && yaml_mapping_has_member(m, "profiles"))
		parse_profiles(inline_profiles, yaml_mapping_get_member(m, "profiles"),
			"config.yaml", FALSE, &saw_card);
	if (saw_card)
		g_warning("form_autofill: config.yaml: card data (cc_*) is only read from "
		          "the private profile file (form_autofill.file); ignoring it");

	/* Merge: an inline profile with a file profile's label overlays it
	 * field by field (inline wins); new labels are appended. */
	for (i = 0; i < inline_profiles->len; i++) {
		AutofillProfile *ip = g_ptr_array_index(inline_profiles, i);
		AutofillProfile *fp = find_profile(self->profiles, ip->label);

		if (fp != NULL) {
			profile_overlay(fp, ip);
		} else {
			inline_profiles->pdata[i] = NULL;
			g_ptr_array_add(self->profiles, ip);
		}
	}
}

/* ---- page side ---- */

/* The top-level host of @view if autofill may act there: an http(s)
 * page whose host no exclude rule matches. NULL otherwise. */
static gchar *
allowed_host(GsurfFormAutofillModule *self, GsurfView *view)
{
	const gchar *uri;
	const gchar *scheme;
	g_autofree gchar *host = NULL;

	if (!gsurf_module_is_active(GSURF_MODULE(self)) || view == NULL ||
	    self->exclude_broken)
		return NULL;
	uri = gsurf_view_get_uri(view);
	scheme = uri != NULL ? g_uri_peek_scheme(uri) : NULL;
	if (g_strcmp0(scheme, "http") != 0 && g_strcmp0(scheme, "https") != 0)
		return NULL;
	host = gsurf_host_from_uri(uri);
	if (host == NULL || gsurf_host_rules_matches(self->exclude, host))
		return NULL;
	return g_steal_pointer(&host);
}

static gboolean
view_is_https(GsurfView *view)
{
	const gchar *uri = gsurf_view_get_uri(view);

	return uri != NULL && g_strcmp0(g_uri_peek_scheme(uri), "https") == 0;
}

/* Serialize @builder's root and call `__gsurfAutofill.<fn>(json)` in the
 * isolated world. */
static void
run_in_world(GsurfView *view, const gchar *fn, JsonBuilder *builder,
             GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autoptr(JsonNode) root = json_builder_get_root(builder);
	g_autofree gchar *json = NULL;
	g_autofree gchar *script = NULL;

	json_generator_set_root(gen, root);
	json = json_generator_to_data(gen, NULL);
	script = g_strdup_printf("window.__gsurfAutofill&&window.__gsurfAutofill.%s(%s)",
		fn, json);
	gsurf_view_run_javascript_in_world_async(view, script, AUTOFILL_WORLD, NULL,
		callback, user_data);
	/* The payload may hold profile values: scrub our copies. */
	memset(script, 0, strlen(script));
	memset(json, 0, strlen(json));
}

/* Discard a result; only errors are interesting (and only at debug). */
static void
on_js_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = gsurf_view_run_javascript_in_world_finish(
		GSURF_VIEW(source), result, &error);

	(void)user_data;
	if (error != NULL)
		g_debug("form_autofill: page script: %s", error->message);
}

/* The fill summary holds field types and counts only. */
static void
on_fill_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = gsurf_view_run_javascript_in_world_finish(
		GSURF_VIEW(source), result, &error);
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *obj;

	(void)user_data;
	if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		return;
	obj = json_node_get_object(json_parser_get_root(parser));
	if (json_object_has_member(obj, "filled"))
		g_debug("form_autofill: filled %" G_GINT64_FORMAT " field(s)",
		        json_object_get_int_member(obj, "filled"));
}

static void
send_arm(GsurfFormAutofillModule *self, GsurfView *view, const gchar *nonce,
         const gchar *host)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	guint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "nonce");
	json_builder_add_string_value(b, nonce);
	json_builder_set_member_name(b, "host");
	json_builder_add_string_value(b, host);
	json_builder_set_member_name(b, "suggest");
	json_builder_add_boolean_value(b, self->suggest_on_focus);
	/* Labels and a has-card flag per profile: never any value. */
	json_builder_set_member_name(b, "labels");
	json_builder_begin_array(b);
	for (i = 0; i < self->profiles->len; i++)
		json_builder_add_string_value(b,
			((AutofillProfile *)g_ptr_array_index(self->profiles, i))->label);
	json_builder_end_array(b);
	json_builder_set_member_name(b, "card");
	json_builder_begin_array(b);
	for (i = 0; i < self->profiles->len; i++)
		json_builder_add_boolean_value(b,
			profile_has_card(g_ptr_array_index(self->profiles, i)));
	json_builder_end_array(b);
	json_builder_end_object(b);
	run_in_world(view, "arm", b, on_js_done, NULL);
}

static void
add_text(JsonBuilder *b, const gchar *name, const gchar *value)
{
	if (value == NULL)
		return;
	json_builder_set_member_name(b, name);
	json_builder_add_string_value(b, value);
}

/* Answer a pick: the values of @label for the part @kind names ("card"
 * or "address"), or a refusal the page shows to the user. */
static void
send_fill(GsurfFormAutofillModule *self, GsurfView *view, const gchar *nonce,
          const gchar *host, const gchar *label, const gchar *kind)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	AutofillProfile *p = find_profile(self->profiles, label);
	gboolean card = g_strcmp0(kind, "card") == 0;
	const gchar *refused = NULL;
	guint i;

	if (p == NULL) {
		refused = "unknown";
	} else if (card && !view_is_https(view)) {
		refused = "insecure";
		g_message("form_autofill: refused to fill card details on %s: "
		          "the page is not https", host);
	} else if (card && !profile_has_card(p)) {
		refused = "nocard";
	}

	json_builder_begin_object(b);
	add_text(b, "nonce", nonce);
	add_text(b, "host", host);
	add_text(b, "kind", card ? "card" : "address");
	if (refused != NULL) {
		add_text(b, "refused", refused);
		json_builder_end_object(b);
		run_in_world(view, "fill", b, on_fill_done, NULL);
		return;
	}

	json_builder_set_member_name(b, "values");
	json_builder_begin_object(b);
	if (card) {
		add_text(b, "cc_name", p->text[FIELD_CC_NAME]);
		add_text(b, "cc_number", p->text[FIELD_CC_NUMBER]);
		if (p->exp_month != 0) {
			json_builder_set_member_name(b, "cc_exp_month");
			json_builder_add_int_value(b, p->exp_month);
			json_builder_set_member_name(b, "cc_exp_year");
			json_builder_add_int_value(b, p->exp_year);
		}
	} else {
		for (i = 0; i < G_N_ELEMENTS(FIELD_SPECS); i++) {
			if (!FIELD_SPECS[i].card)
				add_text(b, FIELD_SPECS[i].key, p->text[FIELD_SPECS[i].field]);
		}
		if (p->street != NULL) {
			json_builder_set_member_name(b, "street");
			json_builder_begin_array(b);
			for (i = 0; p->street[i] != NULL; i++)
				json_builder_add_string_value(b, p->street[i]);
			json_builder_end_array(b);
		}
	}
	json_builder_end_object(b);
	json_builder_end_object(b);
	run_in_world(view, "fill", b, on_fill_done, NULL);
}

/* A 32-digit lowercase hex nonce, as the page script makes them. */
static gboolean
valid_nonce(const gchar *nonce)
{
	guint i;

	if (nonce == NULL || strlen(nonce) != 32)
		return FALSE;
	for (i = 0; i < 32; i++) {
		if (!g_ascii_isdigit(nonce[i]) && !(nonce[i] >= 'a' && nonce[i] <= 'f'))
			return FALSE;
	}
	return TRUE;
}

/* script-message::gsurfAutofill — only our isolated-world script can
 * post here (the handler exists in that world alone). */
static void
on_script_message(GsurfView *view, const gchar *name, const gchar *json,
                  gpointer user_data)
{
	GsurfFormAutofillModule *self = user_data;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *host = NULL;
	JsonObject *obj;
	const gchar *type, *nonce;

	(void)name;
	if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		return;
	obj = json_node_get_object(json_parser_get_root(parser));
	type = json_object_get_string_member_with_default(obj, "type", NULL);
	nonce = json_object_get_string_member_with_default(obj, "nonce", NULL);
	if (!valid_nonce(nonce))
		return;

	/* Disabled module or excluded host: no effect at all. */
	host = allowed_host(self, view);
	if (host == NULL)
		return;

	if (g_strcmp0(type, "ready") == 0) {
		send_arm(self, view, nonce, host);
	} else if (g_strcmp0(type, "pick") == 0) {
		const gchar *label = json_object_get_string_member_with_default(obj, "label", NULL);
		const gchar *kind = json_object_get_string_member_with_default(obj, "kind", NULL);

		if (label == NULL || (g_strcmp0(kind, "card") != 0 &&
		    g_strcmp0(kind, "address") != 0))
			return;
		send_fill(self, view, nonce, host, label, kind);
	}
}

static void
gsurf_form_autofill_inject(GsurfScriptInjector *injector, GsurfView *view,
                           const gchar *uri)
{
	GsurfFormAutofillModule *self = GSURF_FORM_AUTOFILL_MODULE(injector);

	(void)uri;
	if (view == NULL || g_object_get_data(G_OBJECT(view), AUTOFILL_VIEW_KEY) != NULL)
		return;
	g_object_set_data(G_OBJECT(view), AUTOFILL_VIEW_KEY, GINT_TO_POINTER(1));

	/* The script runs in every page but stays inert until the host arms
	 * it, so exclude rules (and enabling/disabling) are decided per page
	 * from the live top-level host. */
	gsurf_view_register_script_message(view, AUTOFILL_HANDLER, AUTOFILL_WORLD);
	gsurf_view_add_user_script_in_world(view, gsurf_form_autofill_js, TRUE, NULL,
		AUTOFILL_WORLD);
	g_signal_connect_object(view, "script-message::" AUTOFILL_HANDLER,
		G_CALLBACK(on_script_message), self, 0);
}

static void
gsurf_form_autofill_injector_init(GsurfScriptInjectorInterface *iface)
{
	iface->inject = gsurf_form_autofill_inject;
}

/* ---- the fill key and the form-autofill action ---- */

/* Ask the page to fill the focused field's form: one candidate profile
 * is used at once, several open the picker. FALSE when autofill does not
 * apply to this page (the key then reaches the page untouched). */
static gboolean
request_fill(GsurfFormAutofillModule *self, GsurfView *view)
{
	g_autoptr(JsonBuilder) b = NULL;
	g_autofree gchar *host = allowed_host(self, view);

	if (host == NULL)
		return FALSE;
	b = json_builder_new();
	json_builder_begin_object(b);
	add_text(b, "host", host);
	json_builder_end_object(b);
	run_in_world(view, "trigger", b, on_js_done, NULL);
	return TRUE;
}

static gboolean
gsurf_form_autofill_handle_key(GsurfInputHandler *handler, GsurfView *view,
                               guint keyval, guint keycode, guint state,
                               GsurfModePolicy mode)
{
	GsurfFormAutofillModule *self = GSURF_FORM_AUTOFILL_MODULE(handler);

	(void)keycode;
	(void)mode;
	/* Only our own binding; every other key (typing included) passes. */
	if (self->key_fill == NULL || !gsurf_keys_match(keyval, state, self->key_fill))
		return FALSE;
	return request_fill(self, view);
}

static void
gsurf_form_autofill_input_init(GsurfInputHandlerInterface *iface)
{
	iface->handle_key_event = gsurf_form_autofill_handle_key;
}

static gboolean
gsurf_form_autofill_handle_action(GsurfActionHandler *handler, GsurfView *view,
                                  GsurfAction action)
{
	if (action != GSURF_ACTION_FORM_AUTOFILL)
		return FALSE;
	return request_fill(GSURF_FORM_AUTOFILL_MODULE(handler), view);
}

static void
gsurf_form_autofill_action_init(GsurfActionHandlerInterface *iface)
{
	iface->handle_action = gsurf_form_autofill_handle_action;
}

static void
gsurf_form_autofill_list_keybinds(GsurfKeybindProvider *provider, GPtrArray *entries)
{
	GsurfFormAutofillModule *self = GSURF_FORM_AUTOFILL_MODULE(provider);

	gsurf_keybind_help_append(entries, self->key_fill,
		"Autofill the focused form (picker for several profiles)",
		"form_autofill", "form-autofill");
}

static void
gsurf_form_autofill_keybind_init(GsurfKeybindProviderInterface *iface)
{
	iface->list_keybinds = gsurf_form_autofill_list_keybinds;
}

/* ---- GObject ---- */

static const gchar *
gsurf_form_autofill_get_name(GsurfModule *module)
{
	(void)module;
	return "form_autofill";
}

static const gchar *
gsurf_form_autofill_get_description(GsurfModule *module)
{
	(void)module;
	return "Fill forms from identity/address/card profiles (isolated world)";
}

static gboolean
gsurf_form_autofill_activate(GsurfModule *module)
{
	(void)module;
	return TRUE;
}

static void
gsurf_form_autofill_get_property(GObject *object, guint prop_id, GValue *value,
                                 GParamSpec *pspec)
{
	GsurfFormAutofillModule *self = GSURF_FORM_AUTOFILL_MODULE(object);

	switch (prop_id) {
	case PROP_N_PROFILES:
		g_value_set_uint(value, self->profiles->len);
		break;
	case PROP_SUMMARY: {
		g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
		guint i;

		for (i = 0; i < self->profiles->len; i++)
			g_strv_builder_take(builder,
				profile_describe(g_ptr_array_index(self->profiles, i)));
		g_value_take_boxed(value, g_strv_builder_end(builder));
		break;
	}
	case PROP_FILE:
		g_value_set_string(value, self->file);
		break;
	case PROP_SUGGEST_ON_FOCUS:
		g_value_set_boolean(value, self->suggest_on_focus);
		break;
	case PROP_KEY_FILL:
		g_value_set_string(value, self->key_fill);
		break;
	case PROP_N_EXCLUDE:
		g_value_set_uint(value, gsurf_host_rules_get_n_rules(self->exclude));
		break;
	case PROP_EXCLUDE_BROKEN:
		g_value_set_boolean(value, self->exclude_broken);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
gsurf_form_autofill_module_finalize(GObject *object)
{
	GsurfFormAutofillModule *self = GSURF_FORM_AUTOFILL_MODULE(object);

	g_clear_pointer(&self->profiles, g_ptr_array_unref);
	g_clear_pointer(&self->file, g_free);
	g_clear_pointer(&self->key_fill, g_free);
	g_clear_object(&self->exclude);

	G_OBJECT_CLASS(gsurf_form_autofill_module_parent_class)->finalize(object);
}

static void
gsurf_form_autofill_module_class_init(GsurfFormAutofillModuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GsurfModuleClass *module_class = GSURF_MODULE_CLASS(klass);
	const GParamFlags ro = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

	object_class->finalize = gsurf_form_autofill_module_finalize;
	object_class->get_property = gsurf_form_autofill_get_property;
	module_class->activate = gsurf_form_autofill_activate;
	module_class->get_name = gsurf_form_autofill_get_name;
	module_class->get_description = gsurf_form_autofill_get_description;
	module_class->configure = gsurf_form_autofill_configure;

	/* Read-only introspection for tests and debugging: labels and field
	 * names, never values. */
	props[PROP_N_PROFILES] = g_param_spec_uint("n-profiles", NULL, NULL,
		0, G_MAXUINT, 0, ro);
	props[PROP_SUMMARY] = g_param_spec_boxed("summary", NULL, NULL, G_TYPE_STRV, ro);
	props[PROP_FILE] = g_param_spec_string("file", NULL, NULL, NULL, ro);
	props[PROP_SUGGEST_ON_FOCUS] = g_param_spec_boolean("suggest-on-focus", NULL, NULL,
		TRUE, ro);
	props[PROP_KEY_FILL] = g_param_spec_string("key-fill", NULL, NULL, NULL, ro);
	props[PROP_N_EXCLUDE] = g_param_spec_uint("n-exclude", NULL, NULL,
		0, G_MAXUINT, 0, ro);
	props[PROP_EXCLUDE_BROKEN] = g_param_spec_boolean("exclude-broken", NULL, NULL,
		FALSE, ro);
	g_object_class_install_properties(object_class, N_PROPS, props);
}

static void
gsurf_form_autofill_module_init(GsurfFormAutofillModule *self)
{
	self->profiles = g_ptr_array_new_with_free_func(profile_free);
	self->exclude = gsurf_host_rules_new();
	reset_config(self);
}

G_MODULE_EXPORT GType
gsurf_module_register(void)
{
	return GSURF_TYPE_FORM_AUTOFILL_MODULE;
}
