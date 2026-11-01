#!/usr/bin/env bash
# Conformance vector tool.
#
#   ./run.sh generate [AREA]   build the C runners from ../../libobs and (re)write vectors/
#   ./run.sh check [AREA]      build the runners and fail if any output differs from vectors/
#
# AREA limits the run to one area (always use it when working on one area so that
# other areas' vectors are not touched).
#
# New areas register themselves with a file gen/<area>.sources (no edit of this script):
#   one path per line, relative to the repository root (the runner source and the libobs
#   files it needs), plus optional lines:  -lfoo / -DFOO (compiler or linker flags),
#   !nostubs (do not link gen/stubs.c / the common sources; use when linking real platform code).
#   '#' starts a comment.
#
# The runners link the *C reference implementation* of the area under test. They
# are what makes vectors/ authoritative. A clean-room implementation never needs
# this script: it implements its own runner for the script language described
# in specs/ and compares against the committed vectors/.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
libobs="$here/../../libobs"
build="${CONFORMANCE_BUILD_DIR:-$(mktemp -d)}"
mode="${1:-check}"
only_area="${2:-}"
root="$(cd "$here/../.." && pwd)"
cc="${CC:-cc}"
cflags=(-std=gnu11 -O0 -g -Wall -Wno-unused-function -I"$libobs" -I"$here/gen")

# Shared sources every runner needs (allocator, logging, threading, text helpers).
common=("$here/gen/stubs.c" "$libobs/util/bmem.c" "$libobs/util/base.c" "$libobs/util/threading-posix.c"
	"$libobs/util/utf8.c" "$libobs/util/dstr.c")
# cf-lexer based declaration parser (decl.c) needs the lexer too.
lexer=("$libobs/util/lexer.c" "$libobs/util/cf-lexer.c" "$libobs/util/cf-parser.c")

declare -A runner_sources=(
	[calldata]="$here/gen/cd_runner.c $libobs/callback/calldata.c"
	[decl]="$here/gen/decl_runner.c $libobs/callback/calldata.c $libobs/callback/decl.c ${lexer[*]}"
	[signal]="$here/gen/signal_runner.c $libobs/callback/calldata.c $libobs/callback/decl.c $libobs/callback/signal.c $libobs/callback/proc.c ${lexer[*]}"
	[darray]="$here/gen/darray_runner.c"
	[deque]="$here/gen/deque_runner.c"
	[bmem]="$here/gen/bmem_runner.c"
	[dstr]="$here/gen/dstr_runner.c"
	[math]="$here/gen/math_runner.c $libobs/graphics/vec3.c $libobs/graphics/vec4.c $libobs/graphics/quat.c $libobs/graphics/matrix4.c $libobs/graphics/matrix3.c $libobs/graphics/axisang.c $libobs/graphics/plane.c $libobs/graphics/math-extra.c"
	[config]="$here/gen/config_runner.c $libobs/util/config-file.c ${lexer[*]}"
)

declare -A extra_flags=()
declare -A no_common=()
for f in "$here"/gen/*.sources; do
	[ -e "$f" ] || continue
	a="$(basename "$f" .sources)"
	srcs=""
	flags=""
	while IFS= read -r line; do
		line="${line%%#*}"
		line="$(echo "$line" | xargs)"
		[ -n "$line" ] || continue
		case "$line" in
		'!nostubs') no_common[$a]=1 ;;
		-*) flags="$flags $line" ;;
		*) srcs="$srcs $root/$line" ;;
		esac
	done <"$f"
	runner_sources[$a]="$srcs"
	extra_flags[$a]="$flags"
done

export CONFORMANCE_TMPDIR="$build"
status=0
for area in "${!runner_sources[@]}"; do
	[ -d "$here/scripts/$area" ] || continue
	[ -z "$only_area" ] || [ "$only_area" = "$area" ] || continue
	commons=("${common[@]}")
	[ -z "${no_common[$area]:-}" ] || commons=()
	# shellcheck disable=SC2086
	"$cc" "${cflags[@]}" ${extra_flags[$area]:-} -o "$build/$area-runner" ${runner_sources[$area]} "${commons[@]}" -lpthread -lm
	mkdir -p "$here/vectors/$area"
	for script in "$here/scripts/$area"/*.script; do
		name="$(basename "$script" .script)"
		out="$here/vectors/$area/$name.expected"
		if [ "$mode" = generate ]; then
			"$build/$area-runner" "$script" >"$out"
		else
			if ! diff -u "$out" <("$build/$area-runner" "$script") >/dev/null; then
				echo "FAIL $area/$name" >&2
				status=1
			else
				echo "ok   $area/$name"
			fi
		fi
	done
done
exit $status
