#!/usr/bin/env bash
# Conformance vector tool.
#
#   ./run.sh generate [AREA]   build the C runners from ../../libobs and (re)write vectors/
#   ./run.sh check [AREA]      build the runners and fail if any output differs from vectors/
#   ./run.sh swap [AREA]       P1-08.5 recipe: first `cargo build -p obs-ffi --release --features c-swap`,
#                              then build each runner like check EXCEPT the C sources the Rust
#                              staticlib replaces are dropped and ../../target/release/libobs_ffi.a
#                              is appended before -lpthread -lm. Part A of P1-08.5 covers the
#                              base lexer (lexer.c), text-lookup (text-lookup.c) and the
#                              base-logging shim (base.c); part B adds the C-family lexer
#                              (cf-lexer.c) and parser (cf-parser.c); P1-05.4 adds the threading
#                              primitives (threading-posix.c); P1-07.3 adds the Flatpak
#                              sleep-inhibition files (platform-nix-dbus.c,
#                              platform-nix-portal.c — utilrest area) — the full swap set of
#                              the OBS_ENABLE_RUST CMake patch. Areas without any swapped
#                              file build normally.
#
# AREA limits the run to one area (always use it when working on one area so that
# other areas' vectors are not touched).
#
# New areas register themselves with a file gen/<area>.sources (no edit of this script):
#   one path per line, relative to the repository root (the runner source and the libobs
#   files it needs), plus optional lines:  -lfoo / -DFOO (compiler or linker flags),
#   !nostubs (do not link gen/stubs.c / the common sources; use when linking real platform code),
#   !rustlib (link the Rust staticlib in check mode too — the runner drives Rust-owned
#   state through test-only symbols; P1-04.2 deque layout sharing).
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
# Extra includes for system dependencies that OBS 33.0.1 expects installed
# (e.g. simde, `CONFORMANCE_EXTRA_CFLAGS="-I/path/to/simde-repo"`).
# shellcheck disable=SC2206
cflags+=(${CONFORMANCE_EXTRA_CFLAGS:-})

# Shared sources every runner needs (allocator, logging, threading, text helpers).
# rustlib_stubs.c: weak abort stubs for the C services the obs-ffi
# staticlib references (see the file header for the weak-symbol rationale).
common=("$here/gen/stubs.c" "$here/gen/rustlib_stubs.c" "$libobs/util/bmem.c" "$libobs/util/base.c" "$libobs/util/threading-posix.c"
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
	# threading-posix.c comes from the common sources.
	[threading]="$here/gen/threading_runner.c"
)
# P1-04.2 (WP-32): the deque runner drives a Rust-owned deque through the
# test-only obs_ffi_deque_new/free symbols — link the staticlib in check
# mode too (the !rustlib form of the .sources marker). Every runner that
# links the staticlib also picks up gen/rustlib_stubs.c (weak abort stubs
# for the C services the other bridges reference — P1-33, WP-34) from the
# common sources.
declare -A rustlib_area=()
rustlib_area[deque]=1

declare -A extra_flags=()
declare -A extra_libs=()
declare -A no_common=()
for f in "$here"/gen/*.sources; do
	[ -e "$f" ] || continue
	a="$(basename "$f" .sources)"
	srcs=""
	flags=""
	libs=""
	while IFS= read -r line; do
		line="${line%%#*}"
		line="$(echo "$line" | xargs)"
		[ -n "$line" ] || continue
		case "$line" in
		'!nostubs') no_common[$a]=1 ;;
		# P1-04.2 (WP-32): link the Rust staticlib in check mode too —
		# the area's runner drives Rust-owned state through test-only
		# symbols (the deque area: obs_ffi_deque_new/free).
		'!rustlib') rustlib_area[$a]=1 ;;
		# -l flags are emitted after the objects: linkers that default to
		# --as-needed (e.g. Ubuntu) drop a shared lib listed before the
		# objects that need its symbols.
		-l*) libs="$libs $line" ;;
		-*) flags="$flags $line" ;;
		*) srcs="$srcs $root/$line" ;;
		esac
	done <"$f"
	runner_sources[$a]="$srcs"
	extra_flags[$a]="$flags"
	extra_libs[$a]="$libs"
done

export CONFORMANCE_TMPDIR="$build"

# P1-08.5 swap mode: the Rust staticlib that replaces the swapped C sources
# (--features c-swap compiles the abi_* bridges into the staticlib; the
# OBS_ENABLE_RUST=ON build passes the same feature).
rustlib="$root/target/release/libobs_ffi.a"
need_rustlib=0
if [ "$mode" = swap ]; then
	need_rustlib=1
elif [ -n "$only_area" ]; then
	if [ -n "${rustlib_area[$only_area]:-}" ]; then
		need_rustlib=1
	fi
else
	for a in "${!rustlib_area[@]}"; do
		need_rustlib=1
		break
	done
fi
if [ "$need_rustlib" = 1 ]; then
	( cd "$root" && cargo build -p obs-ffi --release --features c-swap )
fi

# Removes the swapped C sources from the array named by $1; sets SWAPPED=1
# if anything was removed (the full swap set of the OBS_ENABLE_RUST CMake
# patch: lexer.c, cf-lexer.c, cf-parser.c, text-lookup.c, base.c,
# threading-posix.c, platform-nix-dbus.c, platform-nix-portal.c, and —
# P1-14.2 (WP-32) — bitstream.c, array-serializer.c, file-serializer.c,
# buffered-file-serializer.c; plus — P1-33 (WP-34) — the ported shared/ leaf
# modules: opts-parser.c is a runner source here, bpm.c / happy-eyeballs.c /
# file-updater.c are #included by the runner behind the per-module
# -DOBS_CONFORMANCE_RUST_SWAP_{BPM,HE,FU} macros, which swap mode defines
# below; and — P1-12.3 (WP-37) — source-profiler.c (S-055 U3)).
apply_swap() {
	local -n _srcs=$1
	local f n=${#_srcs[@]} out=()
	for f in "${_srcs[@]}"; do
		case "$f" in
		*"libobs/util/lexer.c" | *"libobs/util/cf-lexer.c" | *"libobs/util/cf-parser.c" | *"libobs/util/text-lookup.c" | *"libobs/util/base.c" | *"libobs/util/threading-posix.c" | *"libobs/util/platform-nix-dbus.c" | *"libobs/util/platform-nix-portal.c" | *"libobs/util/bitstream.c" | *"libobs/util/array-serializer.c" | *"libobs/util/file-serializer.c" | *"libobs/util/buffered-file-serializer.c" | *"libobs/util/source-profiler.c" | *"shared/opts-parser/opts-parser.c" | *"shared/bpm/bpm.c" | *"shared/happy-eyeballs/happy-eyeballs.c" | *"shared/file-updater/file-updater/file-updater.c") SWAPPED=1 ;;
		*) out+=("$f") ;;
		esac
	done
	if [ "${#out[@]}" -lt "$n" ]; then
		_srcs=("${out[@]}")
	fi
	return 0
}

status=0
for area in "${!runner_sources[@]}"; do
	[ -d "$here/scripts/$area" ] || continue
	[ -z "$only_area" ] || [ "$only_area" = "$area" ] || continue
	commons=("${common[@]}")
	[ -z "${no_common[$area]:-}" ] || commons=()
	if [ "$mode" = swap ]; then
		# swap mode: drop the swapped C sources; the staticlib provides the
		# replaced symbols (appended below before -lpthread -lm, like a -l flag).
		srcs=(${runner_sources[$area]})
		SWAPPED=0
		apply_swap srcs
		apply_swap commons
		rustlib_link=""
		if [ "$SWAPPED" = 1 ] || [ -n "${rustlib_area[$area]:-}" ]; then
			rustlib_link="$rustlib"
			# P1-12.3 (WP-37): the profiler bridge's graphics-context shim
			# (compiles against the pinned libobs headers; the staticlib's
			# profiler bridge references its two functions).
			[ "$SWAPPED" = 1 ] && srcs+=("$root/crates/obs-ffi/c/srcprof-bridge.c")
			# !nostubs areas (no common sources) link the staticlib too and
			# need its C-service stubs (the strong providers in their stub
			# files win where present; see gen/rustlib_stubs.c).
			[ "${#commons[@]}" -gt 0 ] || srcs+=("$here/gen/rustlib_stubs.c")
		fi
		# P1-33 (WP-34): the shared/ modules the helpers runner #includes
		# (bpm, happy-eyeballs, file-updater) are gated on their per-module
		# swap macros; opts-parser.c is a runner source removed above.
		# shellcheck disable=SC2086
		"$cc" "${cflags[@]}" -DOBS_CONFORMANCE_RUST_SWAP_BPM -DOBS_CONFORMANCE_RUST_SWAP_HE -DOBS_CONFORMANCE_RUST_SWAP_FU ${extra_flags[$area]:-} -o "$build/$area-runner" "${srcs[@]}" "${commons[@]}" $rustlib_link ${extra_libs[$area]:-} -lpthread -lm
	else
		rustlib_link=""
		if [ -n "${rustlib_area[$area]:-}" ]; then
			rustlib_link="$rustlib"
			# The staticlib provides the blog family (base-logging.c shim) and
			# the posix threading bridge — drop those C sources (like swap
			# mode does) to avoid duplicate definitions. base.c (33.0.1)
			# provides nothing beyond the blog family.
			out=()
			for f in "${commons[@]}"; do
				case "$f" in
				*"libobs/util/base.c" | *"libobs/util/threading-posix.c") ;;
				*) out+=("$f") ;;
				esac
			done
			commons=("${out[@]}")
		fi
		# shellcheck disable=SC2086
		"$cc" "${cflags[@]}" ${extra_flags[$area]:-} -o "$build/$area-runner" ${runner_sources[$area]} "${commons[@]}" $rustlib_link ${extra_libs[$area]:-} -lpthread -lm
	fi
	mkdir -p "$here/vectors/$area"
	for script in "$here/scripts/$area"/*.script; do
		name="$(basename "$script" .script)"
		out="$here/vectors/$area/$name.expected"
		if [ "$mode" = generate ]; then
			"$build/$area-runner" "$script" >"$out"
		else
			# Allowlists (see the two files): scripts the C reference cannot run
			# reliably are still built and run (a crash still fails) but their diff
			# against the spec-ideal vector is skipped.
			skip=""
			if grep -qxF "$area/$name" "$here/known-c-bugs.txt" 2>/dev/null; then
				skip="known C reference bug"
			elif [ -f "$here/known-racy.txt" ] && grep -qxF "$area/$name" "$here/known-racy.txt"; then
				skip="racy C reference runner"
			fi
			if [ -n "$skip" ]; then
				"$build/$area-runner" "$script" >/dev/null
				echo "skip $area/$name ($skip)"
			else
				actual="$build/$name.actual"
				"$build/$area-runner" "$script" >"$actual"
				if ! diff -u "$out" "$actual" >/dev/null; then
					echo "FAIL $area/$name" >&2
					diff -u "$out" "$actual" 2>&1 | head -40 >&2
					status=1
				else
					echo "ok   $area/$name"
				fi
			fi
		fi
	done
done
exit $status
