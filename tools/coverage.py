#!/usr/bin/env python3
"""Measure how much of the C/C++ reference is covered by specs and by conformance tests.

Usage (from the repository root):  python3 tools/coverage.py [--src ../obs-studio] [--per-area N]
The OBS Studio checkout given by --src is only read.

Covered by tests = files listed in gen/*.sources or #include'd as .c/.cpp by a gen/ runner.
Covered by spec  = files whose path (or, for sources, base name) appears in a spec.
Counts whole-file lines (comments and blanks included), so treat the result as an estimate.
Excluded: frontend/, deps/, obs-browser, obs-websocket, test/conformance, docs/.
Targets (project decision): >= 85% spec and >= 75% tests, real-C tests only.
"""
import argparse
import collections
import glob
import os
import re
import subprocess

EXCLUDE = r"(deps/|frontend/|plugins/obs-browser|plugins/obs-websocket|test/conformance|docs/)"
SRC = r"\.(c|cpp|h|hpp|m|mm|swift|effect)$"
PLATFORM = (
    r"(^|/)(win-|mac-)|^(libobs-d3d11|libobs-metal|libobs-winrt|shared/obs-d3d8|shared/qt)"
    r"|^plugins/(decklink|aja|nv-filters)|/(windows|mac|apple|win32|macos|cocoa)(/|$)"
    r"|\.mm?$|\.swift$"
)


def area(path):
    p = path.split("/")
    if p[0] in ("plugins", "shared"):
        return "/".join(p[:2])
    if p[0] == "libobs" and len(p) > 2:
        return "libobs/" + p[1]
    return p[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-area", type=int, default=25)
    ap.add_argument("--src", default="../obs-studio")
    args = ap.parse_args()
    n_rows = args.per_area
    src = args.src

    files = subprocess.check_output(["git", "-C", src, "ls-files"], text=True).split("\n")
    lines = {}
    for f in files:
        if re.search(SRC, f) and not re.match(EXCLUDE, f):
            with open(os.path.join(src, f), errors="ignore") as fh:
                lines[f] = sum(1 for _ in fh)
    by_name = collections.defaultdict(list)
    for f in lines:
        by_name[os.path.basename(f)].append(f)

    tested = set()
    for sf in glob.glob("test/conformance/gen/*.sources"):
        for line in open(sf):
            tested.update(t for t in line.split() if t in lines)
    inc = re.compile(r'#\s*include\s+"([^"]+\.(?:c|cpp))"')
    for g in glob.glob("test/conformance/gen/*.[ch]"):
        for m in inc.finditer(open(g, errors="ignore").read()):
            tested.update(by_name.get(os.path.basename(m.group(1)), []))

    spec_text = "".join(open(s, errors="ignore").read() for s in glob.glob("docs/rust-rewrite/specs/S-*.md"))
    specced = {
        f for f in lines
        if f in spec_text
        or (f.endswith((".c", ".cpp", ".m", ".mm", ".effect")) and os.path.basename(f) in spec_text)
    }
    specced |= tested

    total = sum(lines.values())
    plat = {f for f in lines if re.search(PLATFORM, f)}

    def n(s):
        return sum(lines[f] for f in s)

    print(f"non-frontend source lines: {total}")
    print(f"tests: {n(tested)} ({100 * n(tested) / total:.1f}%)  target 75%  need +{max(0, int(.75 * total) - n(tested))}")
    print(f"spec : {n(specced)} ({100 * n(specced) / total:.1f}%)  target 85%  need +{max(0, int(.85 * total) - n(specced))}")
    print(f"platform-only (Windows/macOS/SDK): {n(plat)} ({100 * n(plat) / total:.1f}%); "
          f"tests on those: {n(tested & plat)}")
    rows = collections.defaultdict(lambda: [0, 0, 0, 0])
    for f, c in lines.items():
        r = rows[area(f)]
        r[0] += c
        r[1] += c if f in tested else 0
        r[2] += c if f in specced else 0
        r[3] += c if f in plat else 0
    print(f"\n{'area':30}{'lines':>7} {'spec':>5} {'test':>5} {'plat':>7}")
    for a, (t, e, s, p) in sorted(rows.items(), key=lambda x: -(x[1][0] - x[1][2]))[:n_rows]:
        print(f"{a:30}{t:7} {100 * s // t:4}% {100 * e // t:4}% {p:7}")


if __name__ == "__main__":
    main()
