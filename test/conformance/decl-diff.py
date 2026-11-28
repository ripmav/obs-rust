#!/usr/bin/env python3
"""WP-43: block diff for the decl conformance vectors.

Implements the conformance rule for `parse crash` (test/conformance/README.md):
a `decl` block whose expected result is `parse crash` means the C reference
crashed on that input. A conforming implementation must print `parse fail`
instead, and its `log` lines for that block are ignored (the reference died
before it could log). Every other block is compared exactly.

Usage: decl-diff.py EXPECTED ACTUAL   (exit 0 iff the swap output conforms)
"""

import sys


def blocks(lines: list[str]) -> list[list[str]]:
    """Group the output into per-case blocks (each starts with `decl `)."""
    out: list[list[str]] = []
    cur: list[str] | None = None
    for ln in lines:
        if ln.startswith("decl "):
            if cur is not None:
                out.append(cur)
            cur = [ln]
        elif cur is not None:
            cur.append(ln)
        else:
            out.append([ln])
    if cur is not None:
        out.append(cur)
    return out


def main() -> int:
    with open(sys.argv[1], encoding="utf-8") as f:
        exp = blocks(f.read().splitlines())
    with open(sys.argv[2], encoding="utf-8") as f:
        act = blocks(f.read().splitlines())
    if len(exp) != len(act):
        print(f"block count: expected {len(exp)}, actual {len(act)}")
        return 1
    for i, (e, a) in enumerate(zip(exp, act), 1):
        if e[0] != a[0]:
            print(f"block {i} header:\n- {e[0]}\n+ {a[0]}")
            return 1
        if "parse crash" in e:
            # The reference crashed: expect `parse fail`, ignore the
            # conforming side's log lines for this input.
            norm_e = [("parse fail" if l == "parse crash" else l) for l in e]
            norm_a = [l for l in a if not l.startswith("log ")]
            if norm_e != norm_a:
                print(f"block {i} (crash case):\n- {' | '.join(norm_e)}\n+ {' | '.join(norm_a)}")
                return 1
        elif e != a:
            for le, la in zip(e, a):
                if le != la:
                    print(f"block {i}:\n- {le}\n+ {la}")
                    break
            if len(e) != len(a):
                print(f"block {i}: {len(e)} lines vs {len(a)}")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
