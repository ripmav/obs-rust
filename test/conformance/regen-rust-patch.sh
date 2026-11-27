#!/usr/bin/env bash
# Regenerate test/conformance/obs-enable-rust.patch (and the identical
# patches/obs-studio-OBS_ENABLE_RUST.patch) from the obs-studio reference
# checkout: reset libobs to the 33.0.1 pin (fe1ed79), apply the previous
# committed patch, apply the genex exclusions for the WP-43 swap files,
# and emit the format-patch-style file. The patch's message is taken from
# the current committed patch (edit it first to record the WP).
#
# Usage: regen-rust-patch.sh FILE...   (libobs-relative paths without .c,
#                                       e.g. util/bmem)
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
obs="$(cd "$root/../obs-studio" && pwd)"
patch="$root/test/conformance/obs-enable-rust.patch"

cd "$obs"
git checkout -q fe1ed79 -- libobs
rm -f libobs/util/srcprof-bridge.c
git apply -v "$patch" >/dev/null

for f in "$@"; do
	fesc="${f//./\\.}"
	sed -i -E "s|^[[:space:]]*$fesc\.c\$|    \$<\$<NOT:\$<BOOL:\${OBS_ENABLE_RUST}>>:$f.c>|" libobs/CMakeLists.txt
done

for f in "$@"; do
	grep -qF "NOT:\$<BOOL:\${OBS_ENABLE_RUST}>>:$f.c" libobs/CMakeLists.txt || {
		echo "ERROR: $f.c not excluded in CMakeLists.txt" >&2
		grep -nF "$f.c" libobs/CMakeLists.txt >&2
		exit 1
	}
done

git add -A libobs
git reset -q -- libobs/libobs 2>/dev/null || true
git diff --cached fe1ed79 -- libobs > /tmp/wp43-patch-body

# compose: message (everything before the first diff) + body
awk '/^diff --git/{exit} {print}' "$patch" > /tmp/wp43-patch-head
cat /tmp/wp43-patch-head > "$patch"
cat /tmp/wp43-patch-body >> "$patch"
cp "$patch" "$root/patches/obs-studio-OBS_ENABLE_RUST.patch"
echo "regenerated $patch"
