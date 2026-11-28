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
rm -f libobs/util/srcprof-bridge.c libobs/util/dstr-variadic-bridge.c
# Unstage leftovers from the previous run (git apply refuses a file that is
# in the index).
git reset -q -- libobs 2>/dev/null || true
git apply -v "$patch" >/dev/null

# WP-43 (P1-02.4): the dstr swap also needs the four variadic dstr functions
# (Rust cannot define `va_list` functions) - the C shim compiles into libobs
# only in Rust swap builds (the conformance runner compiles the identical
# copy from crates/obs-ffi/c/). Idempotent: added once, keyed on the dstr.c
# line (bare or already genex-excluded).
python3 - libobs/CMakeLists.txt <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
shim = "    $<$<BOOL:${OBS_ENABLE_RUST}>:util/dstr-variadic-bridge.c>"
if "util/dstr-variadic-bridge.c" not in s:
    bare = "\n    util/dstr.c\n"
    if bare in s:
        s = s.replace(bare, bare + shim + "\n", 1)
    else:
        excl = "    $<$<NOT:$<BOOL:${OBS_ENABLE_RUST}>>:util/dstr.c>\n"
        assert excl in s, "dstr.c line not found"
        s = s.replace(excl, excl + shim + "\n", 1)
    open(p, "w").write(s)
PY

for f in "$@"; do
	fesc="${f//./\\.}"
	sed -i -E "s|^[[:space:]]*$fesc\.c\$|    \$<\$<NOT:\$<BOOL:\${OBS_ENABLE_RUST}>>:$f.c>|" libobs/CMakeLists.txt
done

# The platform cmake files carry the same sources (pipe-posix.c,
# threading-posix.c); the same genex applies (idempotent - only bare
# source lines match).
for f in "$@"; do
	fesc="${f//./\\.}"
	for cm in libobs/cmake/os-linux.cmake libobs/cmake/os-freebsd.cmake libobs/cmake/os-macos.cmake; do
		sed -i -E "s|^[[:space:]]*$fesc\.c\$|    \$<\$<NOT:\$<BOOL:\${OBS_ENABLE_RUST}>>:$f.c>|" "$cm"
	done
done

for f in "$@"; do
	# The file may live in libobs/CMakeLists.txt or a platform cmake
	# (pipe-posix.c, threading-posix.c) - excluded in either.
	grep -qF "NOT:\$<BOOL:\${OBS_ENABLE_RUST}>>:$f.c" libobs/CMakeLists.txt libobs/cmake/os-linux.cmake libobs/cmake/os-freebsd.cmake libobs/cmake/os-macos.cmake || {
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
