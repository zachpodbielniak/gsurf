#!/usr/bin/env bash
# test-install-skill.sh - `make install-skill' / `uninstall-skill' in a temporary HOME
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Every destination is under a fresh temporary directory; the real home is
# never written (run_make refuses any path under it). The repository's
# skills/gsurf is only read, and its checksum is compared around the case
# that tries to install over it.
#
# Covers: a copy install with the Claude and Grok links; reinstall replacing
# an edited copy; SKILLS_LINK=1 linking this tree; refusing a foreign
# directory and a foreign link of the same name (left untouched); an agent
# skills directory that aliases the shared one; refusing an AGENTS_HOME whose
# skills/ is this tree's skills/; uninstall removing only this skill; HOME=
# relocating every default.
#
# Usage: tests/test-install-skill.sh   (from anywhere; no build needed)

set -o errexit
set -o nounset
set -o pipefail

unset CDPATH

die () {
	printf 'test-install-skill: %s\n' "$*" >&2
	exit 1
}

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
real_home="$(cd -- "${HOME}" && pwd -P)"
scratch="$(mktemp -d)"
scratch="$(cd -- "${scratch}" && pwd -P)"
[[ -n "${scratch}" && "${scratch}" != / && "${scratch}" != "${real_home}" ]] || die "bad scratch directory"
trap 'rm -rf -- "${scratch}"' EXIT

home="${scratch}/home with spaces"
agents="${home}/.agents"
claude="${home}/.claude"
grok="${home}/.grok"
bundle="${agents}/skills/gsurf"
source_skill="${repo_dir}/skills/gsurf"
mkdir -p -- "${home}"

# run_make <args…>: a fresh make in the repository with the temporary
# destinations; refuses any argument that names the real home.
run_make () {
	local arg=""

	for arg in "$@"
	do
		if [[ "${arg}" == *"${real_home}"* && "${arg}" != *"${scratch}"* ]]
		then
			die "refusing a destination under the real home: ${arg}"
		fi
	done
	env -u AGENTS_HOME -u CLAUDE_DIR -u GROK_DIR -u SKILLS_LINK \
		MAKEFLAGS= MAKELEVEL= MFLAGS= \
		make --no-print-directory -s -C "${repo_dir}" \
			"AGENTS_HOME=${agents}" "CLAUDE_DIR=${claude}" "GROK_DIR=${grok}" "$@"
}

# 1. Copy install: a real directory with every file, two links to it.
run_make install-skill > "${scratch}/install.log"
[[ -d "${bundle}" && ! -L "${bundle}" ]] || die "install did not copy the bundle"
diff -r -- "${source_skill}" "${bundle}" > /dev/null || die "the copy differs from skills/gsurf"
[[ "$(readlink -- "${claude}/skills/gsurf")" == "${bundle}" ]] || die "no Claude link"
[[ "$(readlink -- "${grok}/skills/gsurf")" == "${bundle}" ]] || die "no Grok link"
grep -qx 'name: gsurf' "${claude}/skills/gsurf/SKILL.md" || die "the Claude link does not reach SKILL.md"

# 2. Reinstall replaces an edited copy and never touches the source.
printf 'local edit\n' >> "${bundle}/SKILL.md"
grep -q 'local edit' "${source_skill}/SKILL.md" && die "install edited the repository's skill"
run_make install-skills > /dev/null
cmp -s -- "${source_skill}/SKILL.md" "${bundle}/SKILL.md" || die "reinstall kept the edited copy"

# 3. SKILLS_LINK=1 links this tree instead of copying.
run_make SKILLS_LINK=1 install-skill > /dev/null
[[ -L "${bundle}" && "$(readlink -- "${bundle}")" == "${source_skill}" ]] || die "SKILLS_LINK=1 did not link the tree"
run_make uninstall-skill > /dev/null
[[ ! -e "${bundle}" && ! -L "${bundle}" ]] || die "uninstall left the linked bundle"
[[ -f "${source_skill}/SKILL.md" ]] || die "uninstall removed the repository's skill"
[[ ! -L "${claude}/skills/gsurf" && ! -L "${grok}/skills/gsurf" ]] || die "uninstall left a link"

# 4. A foreign directory named gsurf is refused and left as it was.
mkdir -p -- "${bundle}"
printf 'name: something-else\n' > "${bundle}/SKILL.md"
if run_make install-skill > "${scratch}/foreign.log" 2>&1
then
	die "install replaced a foreign directory"
fi
grep -q 'is not the gsurf skill' "${scratch}/foreign.log" || die "no refusal message for a foreign directory"
grep -qx 'name: something-else' "${bundle}/SKILL.md" || die "the foreign directory was changed"
run_make uninstall-skill > /dev/null
grep -qx 'name: something-else' "${bundle}/SKILL.md" || die "uninstall removed a foreign directory"
rm -rf -- "${bundle}"

# 5. A foreign link at an agent's path is refused and kept by uninstall.
mkdir -p -- "${scratch}/other" "${grok}/skills"
printf 'name: other\n' > "${scratch}/other/SKILL.md"
ln -s -- "${scratch}/other" "${grok}/skills/gsurf"
if run_make install-skill > "${scratch}/foreign-link.log" 2>&1
then
	die "install replaced a foreign link"
fi
[[ "$(readlink -- "${grok}/skills/gsurf")" == "${scratch}/other" ]] || die "the foreign link was changed"
run_make uninstall-skill > /dev/null
[[ "$(readlink -- "${grok}/skills/gsurf")" == "${scratch}/other" ]] || die "uninstall removed a foreign link"
[[ ! -e "${bundle}" ]] || die "uninstall left the copy"
unlink -- "${grok}/skills/gsurf"

# 6. ~/.claude/skills aliasing the shared directory keeps the copy intact.
rm -rf -- "${claude}"
mkdir -p -- "${claude}" "${agents}/skills"
ln -s -- "${agents}/skills" "${claude}/skills"
run_make install-skill > "${scratch}/alias.log"
[[ -d "${bundle}" && ! -L "${bundle}" ]] || die "an aliased skills directory broke the copy"
grep -q 'SKIP' "${scratch}/alias.log" || die "the aliased link was not skipped"
run_make uninstall-skill > /dev/null
[[ ! -e "${bundle}" ]] || die "uninstall left the copy (aliased)"
unlink -- "${claude}/skills"

# 7. An AGENTS_HOME whose skills/ is this tree's skills/ is refused.
before="$(cksum < "${source_skill}/SKILL.md")"
mkdir -p -- "${scratch}/aliased/.agents"
ln -s -- "${repo_dir}/skills" "${scratch}/aliased/.agents/skills"
if env -u AGENTS_HOME -u CLAUDE_DIR -u GROK_DIR MAKEFLAGS= MAKELEVEL= MFLAGS= \
	make --no-print-directory -s -C "${repo_dir}" \
		"AGENTS_HOME=${scratch}/aliased/.agents" "CLAUDE_DIR=${scratch}/aliased/.claude" \
		"GROK_DIR=${scratch}/aliased/.grok" install-skill > "${scratch}/refuse.log" 2>&1
then
	die "install into the tree's own skills/ succeeded"
fi
grep -q 'refusing' "${scratch}/refuse.log" || die "no refusal message for the tree's own skills/"
[[ "$(cksum < "${source_skill}/SKILL.md")" == "${before}" ]] || die "the repository's skill changed"
[[ -f "${source_skill}/SKILL.md" ]] || die "the repository's skill was removed"

# 8. HOME= relocates every default when the specific variables are unset.
env -u AGENTS_HOME -u CLAUDE_DIR -u GROK_DIR -u SKILLS_LINK MAKEFLAGS= MAKELEVEL= MFLAGS= \
	make --no-print-directory -s -C "${repo_dir}" "HOME=${scratch}/preview" install-skill > /dev/null
[[ -d "${scratch}/preview/.agents/skills/gsurf" ]] || die "HOME= did not move AGENTS_HOME"
[[ "$(readlink -- "${scratch}/preview/.claude/skills/gsurf")" == "${scratch}/preview/.agents/skills/gsurf" ]] \
	|| die "HOME= did not move CLAUDE_DIR"
[[ "$(readlink -- "${scratch}/preview/.grok/skills/gsurf")" == "${scratch}/preview/.agents/skills/gsurf" ]] \
	|| die "HOME= did not move GROK_DIR"

printf 'install-skill tests passed\n'
