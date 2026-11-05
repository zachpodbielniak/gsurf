#!/usr/bin/env bash
# test-build-config.sh - regression tests for the LRG_BACKEND build plumbing
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Exercises the Makefile without compiling anything, against a STUB libregnum
# (a directory with a fake graylib whose Makefile only records how it was
# called), in a throwaway BUILDDIR, so it never touches the real build tree,
# the bundled deps/libregnum, or an embedder's copy:
#
#   1. LRG_BACKEND=1 with no graylib at LIBREGNUM_DIR is a hard error naming
#      the path (it used to build a GTK-only libgsurf without a word).
#   2. The LRG objects are ordered after the graylib build, because graylib.h
#      includes graylib's generated grl-version.h (a fresh recursive clone
#      failed with "grl-version.h: No such file or directory").
#   3. The graylib sub-make runs ONCE for both archives (grouped target), with
#      graylib's BUILDDIR pinned and BUILD_GIR=0, even when gsurf itself is
#      given BUILDDIR= / BUILD_GIR= on the command line.
#   4. Toggling LRG_BACKEND rebuilds objects without `make clean' (the
#      $(OBJDIR)/.build-flags stamp), and an unchanged configuration does not.
#
# Usage: tests/test-build-config.sh [make-program] [builddir]
#   Run from the gsurf source root (`make test' does this).

set -o errexit
set -o nounset
set -o pipefail

# fail - print a TAP "not ok" with a reason and remember the failure
fail () {
	if [[ $# -ne 2 ]]
	then
		echo 'fail() requires 2 positional arguments'
		echo 'fail <name> <reason>'
		exit 2
	fi

	local name="${1}"
	local reason="${2}"

	echo "not ok - ${name}: ${reason}"
	failures=$((failures + 1))
}

# pass - print a TAP "ok"
pass () {
	if [[ $# -ne 1 ]]
	then
		echo 'pass() requires 1 positional argument'
		echo 'pass <name>'
		exit 2
	fi

	echo "ok - ${1}"
}

# run_make - run the gsurf Makefile hermetically (no parent MAKEFLAGS, so a
# surrounding `make -j LRG_BACKEND=1 test' cannot leak its variables in)
run_make () {
	env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL -u MAKEOVERRIDES \
		"${make_prog}" --no-print-directory "$@"
}

# make_stub - create a stub libregnum at <dir> whose graylib Makefile appends
# its command-line variables to <dir>/calls instead of building anything
make_stub () {
	if [[ $# -ne 1 ]]
	then
		echo 'make_stub() requires 1 positional argument'
		echo 'make_stub <dir>'
		exit 2
	fi

	local dir="${1}"

	mkdir -p "${dir}/deps/graylib/src" "${dir}/src"
	printf '/* stub */\n' > "${dir}/deps/graylib/src/graylib.h"
	# The stub records at PARSE time ($(file ...)), because gsurf runs it under
	# -n and a recipe would only be printed.  $(...) is make syntax for the
	# stub, so it must NOT expand here.
	# shellcheck disable=SC2016
	printf '$(file >>%s/calls,$(MAKEOVERRIDES))\nall:\n\t@:\n' "${dir}" \
		> "${dir}/deps/graylib/Makefile"
	: > "${dir}/calls"
}

make_prog="${1:-make}"
failures=0
# A fixed scratch tree under build/ (gitignored, removed by `make clean-all'):
# every check below resets what it relies on, so nothing has to be deleted.
build_dir="${2:-build}"
[[ "${build_dir}" == /* ]] || build_dir="${PWD}/${build_dir}"
work="${build_dir}/build-config-test"
mkdir -p "${work}"

if [[ ! -f Makefile || ! -d src/backend/lrg ]]
then
	echo 'test-build-config.sh must run from the gsurf source root'
	exit 2
fi

echo '# build-config: LRG_BACKEND plumbing'

# --- 1. missing graylib is an error -------------------------------------------
name='missing graylib is a hard error'
mkdir -p "${work}/empty"
if out="$(run_make -n LRG_BACKEND=1 LIBREGNUM_DIR="${work}/empty" \
	BUILDDIR="${work}/b1" lib 2>&1)"
then
	fail "${name}" 'make succeeded without graylib'
elif [[ "${out}" != *"graylib was not found at ${work}/empty/deps/graylib/src/graylib.h"* ]]
then
	fail "${name}" "unexpected message: ${out}"
else
	pass "${name}"
fi

# --- 2./3. graylib is built first, once, with pinned BUILDDIR -----------------
stub="${work}/stub"
make_stub "${stub}"
view_o="${work}/b2/release/obj/backend/lrg/gsurf-lrg-view.o"

name='LRG objects are ordered after the graylib build'
# -n prints recipes without running them, except $(MAKE) lines, which run (the
# stub just logs).  The stub creates no archive, so the graylib recipe must
# appear before the LRG object's compile line.
if ! out="$(run_make -n LRG_BACKEND=1 LIBREGNUM_DIR="${stub}" \
	BUILDDIR="${work}/b2" BUILD_GIR=1 "${view_o}" 2>&1)"
then
	fail "${name}" "make -n failed: ${out}"
else
	gray_line="$(grep -n -- "-C ${stub}/deps/graylib" <<< "${out}" | head -n 1 | cut -d: -f1 || true)"
	cc_line="$(grep -n -- "-o ${view_o}" <<< "${out}" | head -n 1 | cut -d: -f1 || true)"
	if [[ -z "${gray_line}" || -z "${cc_line}" ]]
	then
		fail "${name}" "graylib build (${gray_line:-none}) or compile (${cc_line:-none}) missing"
	elif (( gray_line >= cc_line ))
	then
		fail "${name}" "graylib built at line ${gray_line}, after the compile at ${cc_line}"
	else
		pass "${name}"
	fi
fi

name='graylib sub-make runs once, BUILDDIR pinned, GIR off'
: > "${stub}/calls"
run_make -n LRG_BACKEND=1 LIBREGNUM_DIR="${stub}" BUILDDIR="${work}/b2" \
	BUILD_GIR=1 "${stub}/deps/graylib/build/lib/libgraylib.a" \
	"${stub}/deps/graylib/deps/raylib/src/libraylib.a" > /dev/null 2>&1 || true
calls="$(wc -l < "${stub}/calls")"
if [[ "${calls}" -ne 1 ]]
then
	fail "${name}" "graylib sub-make ran ${calls} times"
elif ! grep -q -- "BUILDDIR=${stub}/deps/graylib/build" "${stub}/calls"
then
	fail "${name}" "BUILDDIR not pinned: $(cat "${stub}/calls")"
elif ! grep -q -- 'BUILD_GIR=0' "${stub}/calls"
then
	fail "${name}" "BUILD_GIR not forced off: $(cat "${stub}/calls")"
else
	pass "${name}"
fi

# --- 4. the configuration stamp ------------------------------------------------
# Make one object look freshly built for LRG_BACKEND=0, then ask make (-q,
# nothing runs) whether it is up to date under each configuration.
name='LRG_BACKEND toggle rebuilds without make clean'
b3="${work}/b3"
backend_o="${b3}/release/obj/core/gsurf-backend.o"
run_make -n BUILDDIR="${b3}" "${backend_o}" > /dev/null 2>&1 || true
stamp="${b3}/release/obj/.build-flags"
if [[ ! -f "${stamp}" ]]
then
	fail "${name}" 'no .build-flags stamp was written'
else
	mkdir -p "$(dirname "${backend_o}")" "${b3}/include/gsurf"
	touch "${backend_o}"
	if ! run_make -q BUILDDIR="${b3}" "${backend_o}" > /dev/null 2>&1
	then
		fail "${name}" 'object not up to date under an unchanged configuration'
	elif run_make -q LRG_BACKEND=1 LIBREGNUM_DIR="${stub}" BUILDDIR="${b3}" \
		"${backend_o}" > /dev/null 2>&1
	then
		fail "${name}" 'object still up to date after LRG_BACKEND=0 -> 1'
	elif ! grep -q 'LRG_BACKEND=1' "${stamp}"
	then
		fail "${name}" 'stamp does not record LRG_BACKEND=1'
	else
		pass "${name}"
	fi
fi

if [[ "${failures}" -gt 0 ]]
then
	echo "# ${failures} build-config test(s) failed"
	exit 1
fi
echo '# all build-config tests passed'
