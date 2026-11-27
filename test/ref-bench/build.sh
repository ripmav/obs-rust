#!/usr/bin/env bash
# Build the P0-17 C baseline runner (ref-bench.c).
#
# Links the pinned libobs C sources (same files test/ref-dump uses for the
# math/pixel side) so the numbers stay authoritative for the 33.0.1
# reference. -O3 (this is a PERFORMANCE baseline, unlike ref-dump's -O0
# correctness dumps). Standalone by design: the in-tree CMake half is the
# documented deviation (it would need a full OBS configure — GL/glib/ffmpeg
# — that CI does not verify; same precedent as test/ref-dump and
# test/abi-host). Overrides via env (same contract as test/ref-dump/build.sh
# and test/abi-host/build.sh):
#   LIBOBS   libobs checkout          (default: local obs-studio-3301)
#   OUT      output binary            (default: test/ref-bench/ref-bench)
#   CC       C compiler               (default: cc — cross builds pass x86_64-linux-gnu-gcc)
#   CONFORMANCE_EXTRA_CFLAGS  extra CFLAGS (same contract as test/conformance/run.sh:
#                    CI passes the simde include for the x86_64 build; the
#                    aarch64 build uses the system simde)
set -eu

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

LIBOBS="${LIBOBS:-/home/ripmav/projects/obs-studio-3301/libobs}"
OUT="${OUT:-test/ref-bench/ref-bench}"
cc="${CC:-cc}"

cflags=()
cflags+=(${CONFORMANCE_EXTRA_CFLAGS:-})

# No `-I$LIBOBS/util` — every source includes the util headers via the
# `util/...` prefix (resolved through -I$LIBOBS), so the libobs uthash shim
# can never shadow the real one (same rule as test/ref-dump/build.sh).
"$cc" -std=gnu11 -O3 -Wall -Wno-unused-function -Wno-deprecated-declarations \
	-I"$LIBOBS" \
	"${cflags[@]}" \
	"$here/ref-bench.c" \
	"$LIBOBS"/graphics/{vec2,vec3,vec4,matrix3,matrix4,quat,axisang,math-extra}.c \
	"$LIBOBS"/media-io/format-conversion.c \
	-lm -o "$OUT"
