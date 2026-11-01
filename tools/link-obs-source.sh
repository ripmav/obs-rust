#!/usr/bin/env bash
# Link the read-only OBS Studio checkout into this repository so that test/conformance/run.sh
# and the paths in gen/*.sources (libobs/..., plugins/..., shared/..., deps/...) resolve.
#
#   tools/link-obs-source.sh [PATH_TO_OBS_STUDIO]     (default: ../obs-studio)
#
# The links are ignored by git (.gitignore) and nothing is ever written through them.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$(cd "${1:-$here/../obs-studio}" && pwd)"
[ -d "$src/libobs" ] || { echo "not an OBS Studio checkout: $src" >&2; exit 1; }
for d in libobs libobs-opengl libobs-d3d11 libobs-metal libobs-winrt plugins shared deps frontend cmake; do
	[ -e "$src/$d" ] || continue
	ln -sfn "$src/$d" "$here/$d"
done
echo "linked $src into $here"
