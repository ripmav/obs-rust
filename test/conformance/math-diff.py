#!/usr/bin/env python3
"""P1-15.4 (WP-43): tolerance diff for the math/math2 conformance vectors.

Spec S-008 section 2: the values are printed with `%.9g` and compared with
relative **or** absolute tolerance `1e-6`; `-0` equals `0`. Q-5: the C
reference uses SSE (scalar elsewhere) and the Rust bridge may drift in the
last ulp, so exact-text comparison only holds for the all-C check build —
the swap build diffs through this script instead.

Non-numeric tokens (names, types, the runner's `?` masked-lane marker, hex
bit patterns, digests, `nan`/`inf`) must match exactly. Comment lines are
compared exactly.

Usage: math-diff.py EXPECTED ACTUAL   (exit 0 iff identical within tolerance)
"""

import sys

TOL = 1e-6


def close(a: float, b: float) -> bool:
    if a == b:
        return True
    if a == 0.0 or b == 0.0:
        return abs(a) <= TOL  # covers -0 == 0
    return abs(a - b) <= TOL * max(abs(a), abs(b))


def is_float(tok: str) -> bool:
    # `nan`/`inf` are valid to float() in Python — treat them as exact
    # tokens, not numbers.
    if "nan" in tok.lower() or "inf" in tok.lower():
        return False
    try:
        float(tok)
        return True
    except ValueError:
        return False


def main() -> int:
    with open(sys.argv[1], encoding="utf-8") as f:
        exp = f.read().splitlines()
    with open(sys.argv[2], encoding="utf-8") as f:
        act = f.read().splitlines()
    if len(exp) != len(act):
        print(f"line count: expected {len(exp)}, actual {len(act)}")
        return 1
    bad = 0
    for i, (e, a) in enumerate(zip(exp, act), 1):
        et, at = e.split(), a.split()
        if et == at:
            continue
        if len(et) != len(at) or et[:2] != at[:2]:
            print(f"line {i}:\n- {e}\n+ {a}")
            bad += 1
            continue
        for x, y in zip(et[2:], at[2:]):
            if is_float(x) and is_float(y):
                if not close(float(x), float(y)):
                    print(f"line {i}:\n- {e}\n+ {a}")
                    bad += 1
                    break
            elif x != y:
                print(f"line {i}:\n- {e}\n+ {a}")
                bad += 1
                break
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
