#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Zach Podbielniak
#
# fetch-lists.sh - download uBlock Origin / EasyList filter lists for the
# gsurf adblock module, including every list they pull in with
# `!#include`, and print the matching `filter_lists:` config block.
#
# The module parses these text lists natively; no conversion step.

set -o errexit
set -o nounset
set -o pipefail

UBO_BASE='https://ublockorigin.github.io/uAssets/filters'
DEFAULT_LISTS=(
	"easylist=https://easylist.to/easylist/easylist.txt"
	"easyprivacy=https://easylist.to/easylist/easyprivacy.txt"
	"ublock-filters=${UBO_BASE}/filters.txt"
	"ublock-privacy=${UBO_BASE}/privacy.txt"
	"ublock-badware=${UBO_BASE}/badware.txt"
	"ublock-quick-fixes=${UBO_BASE}/quick-fixes.txt"
	"ublock-unbreak=${UBO_BASE}/unbreak.txt"
)

usage () {
	cat <<'EOF'
Usage: fetch-lists.sh [-h] [--license] [-d DIR] [NAME=URL ...]

Download filter lists (and their !#include files) into DIR and print the
filter_lists: block for the adblock section of gsurf's config.

Options:
  -d DIR      output directory (default: ~/.config/gsurf/adblock)
  -h, --help  show this help
  --license   show the license

Lists named ublock-* come from uBlock Origin's own repository and are
marked trusted (they may use trusted-* scriptlets).

Examples:
  fetch-lists.sh
  fetch-lists.sh -d /tmp/lists
  fetch-lists.sh easylist=https://easylist.to/easylist/easylist.txt
  make adblock-lists ADBLOCK_DIR=~/lists
EOF
}

# Download one list, then every list it !#includes (resolved next to it).
fetch_list () {
	if [[ $# -ne 3 ]]
	then
		echo 'fetch_list() requires 3 positional arguments'
		echo 'fetch_list <dir> <file> <url>'
		exit 1
	fi

	local dir="${1}"
	local file="${2}"
	local url="${3}"
	local base="${url%/*}"
	local tmp
	local include

	tmp="$(mktemp "${dir}/.${file}.XXXXXX")"
	if ! curl -fsSL --max-time 120 -o "${tmp}" "${url}"
	then
		echo "  ${file}: download failed (${url}); keeping any previous copy" >&2
		rm -f "${tmp}"
		return 0
	fi
	mv -f "${tmp}" "${dir}/${file}"
	echo "  ${file}" >&2

	# Includes are plain names in the same remote directory.
	while read -r include
	do
		[[ "${include}" == */* || -z "${include}" ]] && continue
		[[ -n "${SEEN[${include}]:-}" ]] && continue
		SEEN["${include}"]=1
		fetch_list "${dir}" "${include}" "${base}/${include}"
	done < <(sed -n 's/^!#include[[:space:]]\{1,\}\([^[:space:]]*\).*/\1/p' "${dir}/${file}")
}

main () {
	local dir="${HOME}/.config/gsurf/adblock"
	local -a lists=()
	local spec
	local name

	while [[ $# -gt 0 ]]
	do
		case "${1}" in
			-h|--help)
				usage
				exit 0
				;;
			--license)
				echo 'AGPL-3.0-or-later <https://www.gnu.org/licenses/agpl-3.0.html>'
				exit 0
				;;
			-d)
				[[ $# -ge 2 ]] || { usage >&2; exit 1; }
				dir="${2}"
				shift
				;;
			*=*)
				lists+=("${1}")
				;;
			*)
				usage >&2
				exit 1
				;;
		esac
		shift
	done
	[[ ${#lists[@]} -gt 0 ]] || lists=("${DEFAULT_LISTS[@]}")

	command -v curl >/dev/null 2>&1 || { echo 'fetch-lists.sh: curl is required' >&2; exit 1; }
	mkdir -p "${dir}"
	declare -gA SEEN=()

	echo "Downloading into ${dir}:" >&2
	for spec in "${lists[@]}"
	do
		name="${spec%%=*}.txt"
		SEEN["${name}"]=1
		fetch_list "${dir}" "${name}" "${spec#*=}"
	done

	echo
	echo 'Add under modules.adblock in your config:'
	echo '    filter_lists:'
	for spec in "${lists[@]}"
	do
		name="${spec%%=*}"
		if [[ "${name}" == ublock-* ]]
		then
			echo "      - { path: \"${dir}/${name}.txt\", trusted: true }"
		else
			echo "      - \"${dir}/${name}.txt\""
		fi
	done
}

main "$@"
