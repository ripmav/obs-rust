#!/bin/bash
# P1-08.5 recipe step 3: the cbindgen header of the obs-ffi bridge must
# declare the same functions as the C headers it replaces (drift detector
# for future WPs). Run from anywhere:  bash test/conformance/cbindgen-check.sh
#
# If cbindgen is not installed the check is SKIPPED (exit 0) — the conformance
# swap builds (run.sh swap) remain the binding ABI proof.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
libobs="$root/libobs"

if ! command -v cbindgen >/dev/null 2>&1; then
    echo "skip: cbindgen not on PATH (install: cargo install cbindgen)"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
if ! (cd "$root" && cbindgen --crate obs-ffi --lang C) >"$tmp/gen.h" 2>"$tmp/gen.err"; then
    echo "FAIL: cbindgen errored:"
    cat "$tmp/gen.err"
    exit 1
fi

# Function names the C headers export (the ABI the bridge must provide).
grep -hE "^EXPORT " "$libobs"/util/lexer.h "$libobs"/util/cf-lexer.h \
    "$libobs"/util/cf-parser.h "$libobs"/util/text-lookup.h \
    | sed -nE 's/^EXPORT [a-zA-Z_]+ ?\**([a-zA-Z_][a-zA-Z0-9_]*)\(.*/\1/p' \
    | sort -u >"$tmp/c_fns.txt"

# Function names the bridge actually declares: cbindgen keeps the C signature
# quoted in the rustdoc comment right above each declaration, so the same
# pattern applies to the generated header.
grep -E '^ \* `EXPORT ' "$tmp/gen.h" \
    | sed -nE 's/^ \* `EXPORT [a-zA-Z_]+ ?\**([a-zA-Z_][a-zA-Z0-9_]*)\(.*/\1/p' \
    | sort -u >"$tmp/gen_fns.txt"

status=0
missing=$(comm -23 "$tmp/c_fns.txt" "$tmp/gen_fns.txt")
if [ -n "$missing" ]; then
    echo "FAIL: exported by the C headers but missing from the bridge:"
    echo "$missing"
    status=1
fi
extra=$(comm -13 "$tmp/c_fns.txt" "$tmp/gen_fns.txt")
if [ -n "$extra" ]; then
    echo "FAIL: declared by the bridge but not exported by the C headers:"
    echo "$extra"
    status=1
fi

# Every declared name must also exist as a real (non-comment) declaration.
while IFS= read -r fn; do
    if ! grep -qE "^([A-Za-z_][A-Za-z0-9_ *]*[ *])?${fn}\(" "$tmp/gen.h"; then
        echo "FAIL: $fn has a quoted C signature but no declaration in the generated header"
        status=1
    fi
done <"$tmp/gen_fns.txt"

if [ "$status" = 0 ]; then
    echo "cbindgen-check: OK — $(wc -l < "$tmp/c_fns.txt")/$(wc -l < "$tmp/c_fns.txt") C-exported functions bridged"
fi
exit "$status"
