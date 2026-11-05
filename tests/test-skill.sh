#!/usr/bin/env bash
# test-skill.sh - structure checks for the /gsurf agent skill (skills/gsurf)
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The checks of agent-skills' scripts/validate-skills.sh, without its rule
# that a skill directory starts with `skill-' (this skill is deliberately
# named `gsurf'):
#
#   1. SKILL.md has YAML frontmatter on line one with a non-empty string
#      `name' equal to the directory name and a non-empty `description'.
#   2. The body has a `# ' title and the sections the house layout uses
#      (When to Use, Prerequisites, Instructions, Output Format, Examples,
#      Constraints) outside fenced code.
#   3. Only references/ and templates/ (and the other directories the
#      validator allows) exist; references are kebab-case .md, templates
#      are NAME.EXT.template.
#   4. Sizes: SKILL.md <= 500 lines, references <= 200, templates <= 100,
#      except the exceptions documented in docs/skill.org.
#   5. Every relative Markdown link outside fenced code resolves.
#   6. Shell templates are ShellCheck-clean (when shellcheck is installed).
#
# When the agent-skills repository is checked out next to this one (or
# AGENT_SKILLS_DIR points at it), its real validator also runs, and its only
# allowed error is the directory-name prefix rule.
#
# Usage: tests/test-skill.sh [SKILL_DIR]   (from anywhere; no build needed)

set -o errexit
set -o nounset
set -o pipefail
shopt -s nullglob globstar

repo_dir="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
skill_dir="${1:-${repo_dir}/skills/gsurf}"
skill_dir="${skill_dir%/}"
failures=0

# Templates allowed past the 100-line recommendation; keep in step with the
# "Size exceptions" table of docs/skill.org.
declare -A size_exceptions=(
	["templates/module.c.template"]=110
)

fail () {
	printf 'not ok - %s\n' "$*"
	failures=$((failures + 1))
}

# pass <before> <message>: report success only if no failure was added since
# the caller recorded <before>.
pass () {
	if [[ $# -ne 2 ]]
	then
		echo 'pass() requires 2 positional arguments: <before> <message>' >&2
		exit 2
	fi
	local before="${1}"
	local message="${2}"

	[[ ${failures} -eq ${before} ]] && printf 'ok - %s\n' "${message}"
	return 0
}

# Print the lines of a Markdown file that are outside fenced code blocks.
outside_fences () {
	if [[ $# -ne 1 ]]
	then
		echo 'outside_fences() requires 1 positional argument: <file>' >&2
		exit 2
	fi
	local file="${1}"

	awk '
		/^ *(```+|~~~+)/ {
			match($0, /^ *(```+|~~~+)/)
			marker = substr($0, RSTART, RLENGTH)
			gsub(/ /, "", marker)
			if (!fence) { fence = substr(marker, 1, 1); width = length(marker) }
			else if (substr(marker, 1, 1) == fence && length(marker) >= width && $0 ~ /^ *(```+|~~~+) *$/) fence = ""
			next
		}
		!fence { print }
	' "${file}"
}

check_frontmatter () {
	local entry="${skill_dir}/SKILL.md"
	local metadata=""
	local name=""
	local description=""

	if [[ ! -f "${entry}" ]]
	then
		fail "${entry} is missing"
		return
	fi
	if ! metadata="$(awk 'NR == 1 { if ($0 != "---") exit 1; next } /^---$/ { closed = 1; exit } { print } END { if (!closed) exit 1 }' "${entry}")"
	then
		fail "SKILL.md: frontmatter delimiters must open on line 1 and close"
		return
	fi
	if ! command -v yq > /dev/null
	then
		fail "yq (Mike Farah v4) is required to parse the frontmatter"
		return
	fi
	if ! printf '%s\n' "${metadata}" | yq -e 'tag == "!!map" and (.name | tag == "!!str") and (.description | tag == "!!str") and (.name | test("^[a-z0-9]+(-[a-z0-9]+)*$")) and (.description | test("[^[:space:]]"))' > /dev/null 2>&1
	then
		fail "SKILL.md: frontmatter needs string name (kebab-case) and description"
		return
	fi
	name="$(printf '%s\n' "${metadata}" | yq -r '.name')"
	description="$(printf '%s\n' "${metadata}" | yq -r '.description')"
	if [[ "${name}" != "${skill_dir##*/}" ]]
	then
		fail "SKILL.md: name '${name}' does not match the directory '${skill_dir##*/}'"
	else
		pass "${failures}" "frontmatter: name '${name}', description ${#description} characters"
	fi
	grep -qx "name: ${name}" "${entry}" || fail "SKILL.md: no literal 'name: ${name}' line (make install-skill checks it)"
}

check_sections () {
	local before="${failures}"
	local entry="${skill_dir}/SKILL.md"
	local headings=""
	local section=""

	headings="$(outside_fences "${entry}" | grep -E '^#{1,2} ' || true)"
	[[ "${headings}" == '# Skill: '* ]] || fail "SKILL.md: the first heading must be '# Skill: <name>'"
	for section in 'When to Use' Prerequisites Instructions 'Output Format' Examples Constraints
	do
		if ! grep -qx "## ${section}" <<< "${headings}"
		then
			fail "SKILL.md: missing '## ${section}' outside fenced code"
		fi
	done
	pass "${before}" "SKILL.md sections"
}

check_layout () {
	local before="${failures}"
	local resource=""
	local file=""
	local rel=""
	local lines=0
	local limit=0

	for resource in "${skill_dir}"/*/
	do
		resource="${resource%/}"
		case "${resource##*/}" in
			references|scripts|templates|assets|agents) ;;
			*) fail "${resource}: unsupported resource directory" ;;
		esac
	done
	for file in "${skill_dir}"/**/*
	do
		[[ -f "${file}" ]] || continue
		rel="${file#"${skill_dir}"/}"
		limit=0
		case "${rel}" in
			SKILL.md) limit=500 ;;
			references/*)
				limit=200
				[[ "${rel##*/}" =~ ^[a-z0-9]+(-[a-z0-9]+)*\.md$ ]] || fail "${rel}: references must be kebab-case .md"
				;;
			templates/*)
				limit=100
				[[ "${rel##*/}" =~ ^[a-z0-9]+([.-][a-z0-9]+)+\.template$ ]] || fail "${rel}: templates must be NAME.EXT.template"
				;;
		esac
		[[ -n "${size_exceptions[${rel}]:-}" ]] && limit="${size_exceptions[${rel}]}"
		lines="$(wc -l < "${file}")"
		if [[ ${limit} -gt 0 && ${lines} -gt ${limit} ]]
		then
			fail "${rel}: ${lines} lines (limit ${limit})"
		fi
	done
	pass "${before}" "layout, names and sizes"
}

check_links () {
	local before="${failures}"
	local file=""
	local target=""

	for file in "${skill_dir}"/**/*.md
	do
		while IFS= read -r target
		do
			case "${target}" in
				*://*|mailto:*|\#*|/*) continue ;;
			esac
			target="${target%%#*}"
			[[ -z "${target}" || -e "${file%/*}/${target}" ]] || fail "${file#"${skill_dir}"/}: broken link ${target}"
		done < <(outside_fences "${file}" | grep -oE '\]\([^)]+\)' | sed -E 's/^\]\((.*)\)$/\1/')
	done
	pass "${before}" "relative links"
}

check_shell_templates () {
	local before="${failures}"
	local file=""

	if ! command -v shellcheck > /dev/null
	then
		printf 'ok - shellcheck not installed; shell templates not linted # SKIP\n'
		return
	fi
	for file in "${skill_dir}"/templates/*.sh.template
	do
		shellcheck -s bash "${file}" || fail "${file##*/}: ShellCheck"
	done
	pass "${before}" "shell templates are ShellCheck-clean"
}

# The real validator, when agent-skills is checked out: exactly one error,
# the `skill-' directory-name prefix.
check_real_validator () {
	local common=""
	local validator=""
	local output=""
	local errors=""

	common="$(git -C "${repo_dir}" rev-parse --path-format=absolute --git-common-dir 2> /dev/null || true)"
	for validator in "${AGENT_SKILLS_DIR:-/nonexistent}/scripts/validate-skills.sh" \
		"${repo_dir}/../agent-skills/scripts/validate-skills.sh" \
		"${common%/.git}/../agent-skills/scripts/validate-skills.sh"
	do
		[[ -f "${validator}" ]] && break
		validator=""
	done
	if [[ -z "${validator}" ]]
	then
		printf 'ok - agent-skills not checked out; real validator not run # SKIP\n'
		return
	fi
	output="$(bash "${validator}" "${skill_dir}" 2>&1 || true)"
	errors="$(grep '^ERROR:' <<< "${output}" || true)"
	if [[ "$(grep -c . <<< "${errors}")" -eq 1 && "${errors}" == *'invalid skill directory name' ]]
	then
		pass "${failures}" "agent-skills validator: only the skill- prefix error"
	else
		printf '%s\n' "${output}"
		fail "agent-skills validator reported other errors"
	fi
}

check_frontmatter
check_sections
check_layout
check_links
check_shell_templates
check_real_validator

if [[ ${failures} -gt 0 ]]
then
	printf '%s skill check(s) failed\n' "${failures}"
	exit 1
fi
printf 'skill checks passed\n'
