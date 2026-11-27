#!/usr/bin/env bash
# WP-40 ABI conformance harness (P0-15 + P1-27.5).
#
# Builds the mini-libobs host (abi-host) + the trace plugin
# (libtest-input.so from trace-plugin.c) against the pinned 33.0.1 libobs C
# sources. One script, two modes — the two builds of the
# OBS_ENABLE_RUST conformance pair:
#
#   LIBOBS=... OUT=... bash build.sh c     # all-C build (OBS_ENABLE_RUST "OFF")
#   LIBOBS=... OUT=... bash build.sh rust  # Rust-staticlib swap build
#                                          # (OBS_ENABLE_RUST "ON"): the
#                                          # 18-file swap set of the OBS_ENABLE_RUST
#                                          # CMake patch — of those, exactly the
#                                          # 12 util files this mini build compiles
#                                          # are dropped and
#                                          # target/release/libobs_ffi.a is linked
#
# The 12 dropped files (kept in sync with the apply_swap pattern of
# test/conformance/run.sh; the other 6 of the 18 — platform-nix-dbus/portal
# and shared/{opts-parser,bpm,happy-eyeballs,file-updater} — are not
# compiled by the mini build):
#   util/{lexer,cf-lexer,cf-parser,text-lookup,base,threading-posix,
#         bitstream,array-serializer,file-serializer,
#         buffered-file-serializer,source-profiler,bmem}.c
# obs-data.c / obs-properties.c / graphics stay C in BOTH builds (they are
# not in the swap set).
#
# Environment (mirrors test/conformance/run.sh + test/ref-dump/build.sh):
#   LIBOBS   libobs checkout (default: local obs-studio-3301)
#   JANSSON  jansson prefix with lib/ (default: obs-conform cache)
#   OUT      output directory (default: <script dir>/out/<mode>);
#            produces $OUT/abi-host, $OUT/libtest-input.so, $OUT/data/libobs/
#   CC       C compiler (default: cc)
#   CONFORMANCE_EXTRA_CFLAGS  extra CFLAGS (same contract as run.sh)
#
# Rust mode additionally needs (from the repo root):
#   cargo build -p obs-ffi --release --features c-swap
# and links <repo root>/target/release/libobs_ffi.a plus the weak abort
# stubs test/conformance/gen/rustlib_stubs.c and -lcurl (the staticlib's
# happy-eyeballs bridge references curl).
set -eu

mode="${1:-}"
case "$mode" in
c | rust) ;;
*)
	echo "usage: $(basename "$0") c|rust" >&2
	exit 2
	;;
esac

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"

LIBOBS="${LIBOBS:-/home/ripmav/projects/obs-studio-3301/libobs}"
JANSSON="${JANSSON:-/home/ripmav/.cache/obs-conform/prefix}"
OUT="${OUT:-$here/out/$mode}"
cc="${CC:-cc}"
DEPS="$(cd "$LIBOBS/.." && pwd)/deps"

[ -d "$LIBOBS" ] || { echo "LIBOBS not found: $LIBOBS" >&2; exit 1; }
[ -d "$JANSSON/lib" ] || { echo "JANSSON prefix without lib/: $JANSSON" >&2; exit 1; }
if [ "$mode" = rust ]; then
	[ -f "$root/target/release/libobs_ffi.a" ] || {
		echo "target/release/libobs_ffi.a missing — run: cargo build -p obs-ffi --release --features c-swap" >&2
		exit 1
	}
fi

mkdir -p "$OUT/data/libobs" "$OUT/plugins"

# --- obsconfig.h (CMake stand-in; see cfg/obsconfig.h.in) -------------
sed -e "s|@OBS_DATA_PATH@|$OUT/data|" \
	-e "s|@OBS_PLUGIN_PATH@|$OUT/plugins|" \
	-e "s|@OBS_PLUGIN_DESTINATION@|$OUT/plugins|" \
	-e "s|@OBS_INSTALL_PREFIX@|/|" \
	"$here/cfg/obsconfig.h.in" > "$OUT/obsconfig.h"

# --- libobs data files (the .effect files find_libobs_data_file reads) --
cp "$LIBOBS"/data/*.effect "$OUT/data/libobs/"

# --- common compile flags ----------------------------------------------
cflags=(-std=gnu11 -O0 -g -Wall -include stdint.h -D_GNU_SOURCE -Wno-unused-function -Wno-deprecated-declarations -Wno-unused-variable)
cflags+=(-I"$OUT" -I"$LIBOBS" -I"$JANSSON/usr/include" -I"$JANSSON/include" -I"$DEPS/libcaption" -I"$DEPS/libcaption/caption")
# shellcheck disable=SC2206
cflags+=(${CONFORMANCE_EXTRA_CFLAGS:-})

# --- libobs sources (the mini build set; identical in both modes except
# --- the swapped util files below) -------------------------------------
core=(
	"$LIBOBS"/obs.c "$LIBOBS"/obs-module.c "$LIBOBS"/obs-source.c "$LIBOBS"/obs-source-transition.c
	"$LIBOBS"/obs-encoder.c "$LIBOBS"/obs-service.c "$LIBOBS"/obs-output.c "$LIBOBS"/obs-output-delay.c
	"$LIBOBS"/obs-properties.c "$LIBOBS"/obs-data.c "$LIBOBS"/obs-video.c "$LIBOBS"/obs-audio.c
	"$LIBOBS"/obs-nix.c "$LIBOBS"/obs-nix-platform.c "$LIBOBS"/obs-display.c "$LIBOBS"/obs-scene.c
	"$LIBOBS"/obs-canvas.c "$LIBOBS"/obs-view.c "$LIBOBS"/obs-video-gpu-encode.c "$LIBOBS"/obs-audio-controls.c
	"$LIBOBS"/obs-source-deinterlace.c "$LIBOBS"/obs-hotkey.c "$LIBOBS"/obs-hotkey-name-map.c
	"$LIBOBS"/obs-missing-files.c "$LIBOBS"/obs-core-modules-loader-nix.c
	"$LIBOBS"/callback/calldata.c "$LIBOBS"/callback/decl.c "$LIBOBS"/callback/signal.c "$LIBOBS"/callback/proc.c
	"$LIBOBS"/graphics/matrix3.c "$LIBOBS"/graphics/matrix4.c "$LIBOBS"/graphics/vec2.c "$LIBOBS"/graphics/vec3.c
	"$LIBOBS"/graphics/vec4.c "$LIBOBS"/graphics/quat.c "$LIBOBS"/graphics/axisang.c "$LIBOBS"/graphics/plane.c
	"$LIBOBS"/graphics/bounds.c "$LIBOBS"/graphics/math-extra.c
	"$LIBOBS"/media-io/video-frame.c "$LIBOBS"/media-io/format-conversion.c "$LIBOBS"/media-io/video-matrices.c
	"$LIBOBS"/media-io/video-fourcc.c "$LIBOBS"/media-io/audio-io.c "$LIBOBS"/media-io/video-io.c
	"$LIBOBS"/audio-monitoring/null/null-audio-monitoring.c
)
# CMake-listed caption sources ONLY: dvtcc.c is corrupt upstream (broken
# operators in the 33.0.1 tag) and is excluded from the CMake build too.
caption=(
	"$DEPS/libcaption/src"/caption.c "$DEPS/libcaption/src"/cea708.c "$DEPS/libcaption/src"/eia608.c
	"$DEPS/libcaption/src"/eia608_charmap.c "$DEPS/libcaption/src"/eia608_from_utf8.c
	"$DEPS/libcaption/src"/mpeg.c "$DEPS/libcaption/src"/scc.c "$DEPS/libcaption/src"/srt.c
	"$DEPS/libcaption/src"/utf8.c "$DEPS/libcaption/src"/xds.c "$DEPS/libcaption/src"/vtt.c
)
util=(
	"$LIBOBS"/util/utf8.c "$LIBOBS"/util/dstr.c "$LIBOBS"/util/config-file.c
	"$LIBOBS"/util/crc32.c "$LIBOBS"/util/task.c "$LIBOBS"/util/profiler.c "$LIBOBS"/util/pipe.c
	"$LIBOBS"/util/pipe-posix.c "$LIBOBS"/util/platform.c "$LIBOBS"/util/platform-nix.c
)

# The 12 swapped util files (OBS_ENABLE_RUST swap set ∩ mini build).
swap=(
	"$LIBOBS"/util/lexer.c "$LIBOBS"/util/cf-lexer.c "$LIBOBS"/util/cf-parser.c
	"$LIBOBS"/util/text-lookup.c "$LIBOBS"/util/base.c "$LIBOBS"/util/threading-posix.c
	"$LIBOBS"/util/bitstream.c "$LIBOBS"/util/array-serializer.c "$LIBOBS"/util/file-serializer.c
	"$LIBOBS"/util/buffered-file-serializer.c "$LIBOBS"/util/source-profiler.c
	"$LIBOBS"/util/bmem.c
)

rustlink=()
ruststubs=()
if [ "$mode" = rust ]; then
	# drop exactly the swapped files (not appended below); link the Rust
	# staticlib AFTER the C objects (left-to-right static archive scan)
	# plus the weak abort stubs and -lcurl (the staticlib's
	# happy-eyeballs bridge references curl).
	ruststubs=("$here/../conformance/gen/rustlib_stubs.c")
	rustlink=("$root/target/release/libobs_ffi.a" -lcurl)
	srcs=("${util[@]}" "${core[@]}" "${caption[@]}" "${ruststubs[@]}")
else
	srcs=("${util[@]}" "${swap[@]}" "${core[@]}" "${caption[@]}")
fi

# --- plugin (always C; dlopen'd at runtime, resolved against the host's
# --- -rdynamic export table) --------------------------------------------
"$cc" "${cflags[@]}" -fPIC -shared \
	"$here/trace-plugin.c" \
	-o "$OUT/libtest-input.so"

# --- host ----------------------------------------------------------------
"$cc" "${cflags[@]}" -rdynamic \
	"$here/abi-host.c" "$here/glstubs.c" "$here/nixstubs.c" \
	"${srcs[@]}" \
	${rustlink[@]+"${rustlink[@]}"} \
	-L"$JANSSON/lib" -ljansson -lpthread -lm -ldl -lz -luuid \
	-o "$OUT/abi-host"

echo "built: $OUT/abi-host + $OUT/libtest-input.so (mode: $mode)"
