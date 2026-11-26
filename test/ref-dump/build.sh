#!/usr/bin/env bash
# Build obs-ref-dump (WP-39): the Rust rewrite's reference dumper.
#
# Links the pinned libobs C sources (same files test/conformance uses) so the
# dumps stay authoritative for the 33.0.1 reference. Overrides via env:
#   LIBOBS   libobs checkout          (default: local obs-studio-3301)
#   JANSSON  jansson prefix w/ lib/   (default: obs-conform cache)
#   OUT      output binary            (default: test/ref-dump/obs-ref-dump)
set -eu

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

LIBOBS="${LIBOBS:-/home/ripmav/projects/obs-studio-3301/libobs}"
JANSSON="${JANSSON:-/home/ripmav/.cache/obs-conform/prefix}"
OUT="${OUT:-test/ref-dump/obs-ref-dump}"

# $JANSSON/usr/include (real uthash.h) must come before $LIBOBS/util: the libobs
# shim does `#include <uthash.h>`, and a later -I$LIBOBS/util would make that
# re-resolve to the shim itself (include cycle, silently dropped).
cc -std=gnu11 -O0 -g -Wall -Wno-unused-function -Wno-deprecated-declarations \
	-I"$LIBOBS" -I"$JANSSON/usr/include" -I"$LIBOBS/util" -I"$JANSSON/include" \
	"$here/ref-dump.c" \
	"$LIBOBS"/graphics/{vec2,vec3,vec4,matrix3,matrix4,quat,axisang,plane,bounds,math-extra}.c \
	"$LIBOBS"/obs-data.c "$LIBOBS"/callback/calldata.c \
	"$LIBOBS"/media-io/{format-conversion,video-frame,video-matrices,video-fourcc}.c \
	"$LIBOBS"/util/{bmem,base,threading-posix,utf8,dstr}.c \
	"$here/../conformance/gen/stubs.c" \
	"$here/refdump_stubs.c" \
	-L"$JANSSON/lib" -ljansson -lm -o "$OUT"
