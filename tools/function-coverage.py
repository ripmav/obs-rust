#!/usr/bin/env python3
"""Function-level coverage of the C reference by plan tasks, specs and conformance tests.

tools/task-coverage.py proves every *file* is mapped to a task. This tool goes one
level down: it inventories every *function* in the C reference (ctags for C/C++,
line-based extractors for Objective-C/Swift) and checks which ones are addressed by:

  T1  function name appears in the plan (phase/subtask/guide/plan docs)
  T2  function's file is mapped to a task in the coverage matrix
  S1  function name appears in a spec (specs/S-*.md)
  S2  function's file is cited in a spec
  X1  function name appears in the conformance corpus (scripts/vectors/gen)
  X2  function's file is referenced in the conformance corpus

Headline numbers use "non-trivial" functions (body >= 5 lines); one-line getters
and setters are reported separately and excluded, per the coverage target.

Generated output (never hand-edit):
  docs/rust-rewrite/coverage/function-coverage.md   report
  docs/rust-rewrite/coverage/function-inventory.csv per-function ground truth

Usage:  python3 tools/function-coverage.py [--src ../obs-studio] [--ctags PATH] [--write]
"""

import argparse
import csv
import glob
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CTAGS = os.environ.get("CTAGS", "/tmp/opencode/ctags/x/usr/bin/ctags-universal")

SRC_EXT = {".c", ".h", ".hpp", ".hxx", ".hh", ".cc", ".cpp", ".cxx", ".m", ".mm", ".swift"}

# third-party code kept/retired as a block (P7-04, D-19, SDK headers); reported
# separately because porting it function-by-function is out of scope by design
VENDORED_PREFIXES = (
    "deps/librtmp/",
    "deps/w32-pthreads/",
    "deps/json11/",
    "deps/blake2/",
    "deps/libcaption/",
    "deps/glad/",
    "plugins/obs-outputs/librtmp/",  # in-tree librtmp copy (P5-17.4)
    "plugins/decklink/decklink-sdk/",
    "plugins/decklink/decklink-sdk-2/",
    "plugins/decklink/linux/decklink-sdk/",
    "plugins/decklink/mac/decklink-sdk/",
    "plugins/decklink/win/decklink-sdk/",
    "plugins/aja/aja-sdk/",
)

OBJC_KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "new",
                 "delete", "throw", "do", "else", "and", "or", "not", "asm"}


# ---------------------------------------------------------------- inventory

def list_files(src):
    """Superproject + initialized submodules, recursively (same rule as task-coverage.py)."""
    files = subprocess.check_output(["git", "-C", src, "ls-files"], text=True).split("\n")
    files = [f for f in files if f]

    def walk(base, prefix):
        nonlocal files
        try:
            cfg = subprocess.check_output(
                ["git", "-C", base, "config", "-f", ".gitmodules", "--get-regexp", "path"], text=True)
        except subprocess.CalledProcessError:
            return
        for line in cfg.splitlines():
            sub_dir = os.path.join(base, line.split()[-1])
            sub_prefix = prefix + line.split()[-1] + "/"
            try:
                sub_files = subprocess.check_output(["git", "-C", sub_dir, "ls-files"], text=True)
            except subprocess.CalledProcessError:
                pass
            else:
                files += [f"{sub_prefix}{p}" for p in sub_files.split("\n") if p]
            walk(sub_dir, sub_prefix)

    walk(src, "")
    out = []
    for f in files:
        base = f[:-3] if f.endswith(".in") else f
        ext = os.path.splitext(base)[1]
        if ext in SRC_EXT:
            out.append(f)
    return sorted(set(out))


def strip_noise(text):
    """Blank out comments and string/char literal contents, keep newlines and line numbers."""
    out = []
    i, n = 0, len(text)
    state = "code"  # code | block | line | str | chr
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "str"
                out.append(" ")
                i += 1
                continue
            if c == "'":
                state = "chr"
                out.append(" ")
                i += 1
                continue
            out.append(c)
        elif state == "block":
            if c == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
        elif state == "line":
            if c == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
        elif state in ("str", "chr"):
            if c == "\\":
                if nxt == "\n":
                    # C line splice: keep the newline so line numbers stay aligned
                    out.append(" \n")
                else:
                    out.append("  ")
                i += 2
                continue
            if (state == "str" and c == '"') or (state == "chr" and c == "'"):
                state = "code"
            out.append("\n" if c == "\n" else " ")
        i += 1
    return "".join(out)


def resolve_preprocessor(lines):
    """Collapse #if/#ifdef/#ifndef blocks to a single branch (the first kept one).

    Raw source contains all branches, so a `#ifdef X ... #else ... #endif` pair
    with braces in both branches is unbalanced as written; keeping one branch per
    condition gives a brace-consistent view. Every input line yields exactly one
    output line, so line numbers are preserved.
    """
    out = []
    stack = []  # [skipping_this_branch, parent_was_skipping, a_branch_was_taken]
    for l in lines:
        s = l.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)$", s)
        if m:
            kind, rest = m.group(1), m.group(2).strip()
            if kind in ("if", "ifdef", "ifndef"):
                parent_skipping = bool(stack) and stack[-1][0]
                disabled = kind == "if" and rest == "0"
                skipping = disabled or parent_skipping
                stack.append([skipping, parent_skipping, not skipping])
                out.append("" if skipping else l)
                continue
            if not stack:
                out.append(l)
                continue
            top = stack[-1]
            if kind == "elif":
                skipping = top[1] or top[2]
                top[2] = top[2] or not skipping
            elif kind == "else":
                skipping = top[1] or top[2]
                top[2] = True
            else:  # endif
                stack.pop()
                out.append(l)
                continue
            top[0] = skipping
            out.append("" if skipping else l)
            continue
        skipping = bool(stack) and stack[-1][0]
        out.append("" if skipping else l)
    return out


def body_end_line(cleaned_lines, start_line, max_scan=20000):
    """Brace-match the function body starting at 1-based start_line. Returns (end_line or None)."""
    # find the first '{' on/after the signature line (signatures can span many lines)
    brace = None
    for li in range(start_line - 1, min(len(cleaned_lines), start_line - 1 + 25)):
        line = cleaned_lines[li]
        if "{" in line:
            brace = (li, line.index("{"))
            break
    if brace is None:
        return None
    depth = 0
    li, ci = brace
    started = False
    scanned = 0
    while li < len(cleaned_lines) and scanned < max_scan * 4:
        for c in cleaned_lines[li][ci:]:
            if c == "{":
                depth += 1
                started = True
            elif c == "}":
                depth -= 1
                if started and depth == 0:
                    return li + 1
        li += 1
        ci = 0
        scanned += 1
    return None


def ex_to_python(pattern):
    """Convert a ctags ex pattern to a Python regex.

    ctags leaves Python metacharacters (( ) . * ? + | { } [ ]) unescaped in its
    patterns, where they are literal — escape them; keep anchors and any
    existing backslash escapes intact.
    """
    m = re.match(r"^/(.*)/$", pattern or "", re.S)
    if not m:
        return None
    s = m.group(1)
    if s.startswith("^"):
        s = s[1:]
    if s.endswith("$"):
        s = s[:-1]
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            out.append(c)
            out.append(s[i + 1])
            i += 2
        elif c in r"().*?+|{}[]":
            out.append("\\" + c)
            i += 1
        else:
            out.append(c)
            i += 1
    return "^" + "".join(out) + "$"


def tag_line(name, pattern, src_lines):
    """Resolve a ctags tag to its 1-based definition line via its pattern."""
    rx = None
    try:
        rx = re.compile(ex_to_python(pattern))
    except (re.error, TypeError):
        pass
    if rx:
        nm = name.split("::")[-1]
        for i, l in enumerate(src_lines, 1):
            if rx.match(l) and nm in l:
                return i
    return None


def name_scan_line(name, src_lines):
    """Fallback: first line that looks like a definition of `name`.

    Skips preprocessor lines (the name's first textual occurrence is often an
    #include or a #define slot), call targets (->, ., &), and requires a
    signature '(' to be present.
    """
    nm = name.split("::")[-1]
    rx = re.compile(rf"\b{re.escape(nm)}\b")
    bad = re.compile(rf"(?:->|\.)\s*{re.escape(nm)}\b|&{re.escape(nm)}\b")
    for i, l in enumerate(src_lines, 1):
        if l.lstrip().startswith("#") or not rx.search(l) or bad.search(l):
            continue
        if re.search(rf"\b{re.escape(nm)}\s*\(", l) or re.search(rf"\b{re.escape(nm)}\b[ \t]*\n", l + "\n"):
            return i
    return None


def ctags_functions(path):
    """(name, start_line) definitions from ctags; line-verified against the source."""
    r = subprocess.run([CTAGS, "--output-format=json", path],
                       capture_output=True, text=True)
    tags = []
    for line in r.stdout.split("\n"):
        if not line.strip():
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        if d.get("_type") == "tag" and d.get("kind") in ("function", "subprogram"):
            tags.append((d["name"], d.get("pattern", "")))
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            src_lines = fh.read().split("\n")
    except OSError:
        return []
    out, seen = [], set()
    for name, pattern in tags:
        nm = name.split("::")[-1]
        if not re.match(r"^[A-Za-z_~][A-Za-z0-9_]*$", nm):
            continue
        line = tag_line(name, pattern, src_lines) or name_scan_line(name, src_lines)
        if line and (name, line) not in seen:
            # verify the signature looks like a definition within 3 lines
            window = " ".join(src_lines[line - 1:line + 3])
            if "(" in window:
                seen.add((name, line))
                out.append((name, line))
    return out


def objc_functions(path):
    """Objective-C method definitions: '- (ret)sel:(type)part {'. .m/.mm only."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        cleaned = strip_noise(fh.read()).split("\n")
    in_impl = False
    depth = 0
    out = []
    for i, line in enumerate(cleaned, 1):
        if re.match(r"^\s*@(implementation|interface)\b", line):
            in_impl = bool(re.match(r"^\s*@implementation\b", line))
            depth = 0
            continue
        if re.match(r"^\s*@end\b", line):
            in_impl = False
            continue
        if not in_impl:
            continue
        m = re.match(r"^[ \t]*[-+]\s*\([^)]*\)\s*([A-Za-z_][A-Za-z0-9_]*)", line)
        if not m:
            continue
        # definition, not declaration: a '{' must follow within a few lines
        for j in range(i - 1, min(len(cleaned), i + 3)):
            if "{" in cleaned[j]:
                out.append((m.group(1), i))
                break
    return out


SWIFT_FUNC = re.compile(
    r"^[ \t]*(?:[\w]+\s+)*(?:public|private|internal|fileprivate|open|static|class|final|"
    r"override|mutating|convenience|lazy|weak|unowned|async|throws|rethrows)?\s*"
    r"func\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def swift_functions(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        cleaned = strip_noise(fh.read()).split("\n")
    out = []
    for i, line in enumerate(cleaned, 1):
        m = SWIFT_FUNC.match(line)
        if not m:
            continue
        for j in range(i - 1, min(len(cleaned), i + 30)):
            if "{" in cleaned[j]:
                out.append((m.group(1), i))
                break
    return out


def area_of(f):
    if f.startswith("plugins/obs-websocket/") or f.startswith("plugins/obs-browser/"):
        return f.split("/", 2)[1]
    if f.startswith("deps/libdshowcapture/"):
        return "libdshowcapture (submodule)"
    top = f.split("/")[0]
    if top == "plugins" and len(f.split("/")) > 1:
        return f.split("/")[1]
    return top


def build_inventory(src):
    files = list_files(src)
    inventory = {}  # (relpath, name, line) -> dict
    for f in files:
        path = os.path.join(src, f)
        ext = os.path.splitext(f[:-3] if f.endswith(".in") else f)[1]
        funcs = []
        if ext == ".swift":
            funcs = swift_functions(path)
        else:
            funcs = ctags_functions(path)
            if ext in (".m", ".mm"):
                funcs += objc_functions(path)
        # de-dup
        seen = set()
        for name, line in funcs:
            key = (f, name, line)
            if key in seen or line <= 0:
                continue
            seen.add(key)
            inventory[key] = {"file": f, "name": name, "line": line,
                              "area": area_of(f),
                              "vendored": 1 if f.startswith(VENDORED_PREFIXES) else 0,
                              "lang": "swift" if ext == ".swift" else "objc" if ext in (".m", ".mm") else "c/c++",
                              "loc": None}
    # measure bodies
    by_file = {}
    for it in inventory.values():
        by_file.setdefault(it["file"], []).append(it)
    for f, items in by_file.items():
        with open(os.path.join(src, f), encoding="utf-8", errors="replace") as fh:
            cleaned = resolve_preprocessor(strip_noise(fh.read()).split("\n"))
        for it in sorted(items, key=lambda x: x["line"]):
            it["loc"] = body_end_line(cleaned, it["line"])
    return files, sorted(inventory.values(), key=lambda x: (x["file"], x["line"]))


# ---------------------------------------------------------------- matching

def corpus_words(paths, backticked_only=False):
    """Set of identifiers that occur in the corpus.

    backticked_only: plan/spec prose is mixed English+code, and code identifiers
    are backticked by house style — so only take identifiers from inside `spans`
    (avoids counting the English word `render` as coverage of a render() function).
    The conformance corpus is code (scripts/gen runners), so plain word scan.
    """
    words = set()
    for p in paths:
        try:
            text = open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        if backticked_only:
            for span in re.findall(r"`([^`]+)`", text):
                for line in span.split("\n"):
                    line = line.strip()
                    if not line:
                        continue
                    if " " not in line:
                        # a single token: accept only whole-token symbol shapes
                        # (no filename extensions, no path/glob fragments)
                        if re.match(
                                r"^[~A-Za-z_][A-Za-z0-9_]*(?:::[~A-Za-z_][A-Za-z0-9_]*)*"
                                r"(\*+)?(\([^)]*\))?$", line) \
                                and not re.search(r"\.(c|h|hpp|hxx|cc|cpp|cxx|m|mm|swift|ui|effect|json|py|md|in)$", line):
                            words.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]+", line))
                    elif re.search(r"[\(\){};=&*><\-#]", line):
                        # code-looking line (call, declaration, assignment): take its identifiers
                        words.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]+", line))
                    # else: prose inside backticks ("Init steps in order") — no identifiers
        else:
            words.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]{2,}", text))
    return words


def corpus_files(paths):
    """Set of C file paths (relative) and basenames cited in the corpus."""
    paths_cited = set()
    base_cited = set()
    # '+' for ObjC category files (MTLTexture+Extensions.swift)
    pat = re.compile(r"[\w.+/~-]+\.(?:c|h|hpp|hxx|cc|cpp|cxx|m|mm|swift|effect|ui)(?:\.in)?(?![\w.])")
    # brace-form citations: plugins/x/{a,b}.c -> plugins/x/a.c + plugins/x/b.c
    # (round-7 verifier found S-049 cites `plugins/linux-jack/{jack-input,jack-wrapper}.c`)
    brace = re.compile(
        r"([A-Za-z0-9_/.~-]*)(\{[A-Za-z0-9_,\-]+\})\.(c|h|hpp|hxx|cc|cpp|cxx|m|mm|swift|effect|ui)(\.in)?(?![\w.])")
    for p in paths:
        try:
            text = open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        text = brace.sub(
            lambda m: " ".join(
                f"{m.group(1)}{part}.{m.group(3)}{m.group(4) or ''}"
                for part in m.group(2).strip("{}").split(",")),
            text)
        for m in pat.findall(text):
            paths_cited.add(m)
            base_cited.add(os.path.basename(m))
    return paths_cited, base_cited


def build_corpora(repo):
    plan = (glob.glob(os.path.join(repo, "docs/rust-rewrite/phase-*.md"))
            + glob.glob(os.path.join(repo, "docs/rust-rewrite/subtasks/*.md"))
            + glob.glob(os.path.join(repo, "docs/rust-rewrite/guides/phase-*guide.md"))
            + [os.path.join(repo, "docs/rust-rewrite-plan.md")])
    spec = glob.glob(os.path.join(repo, "docs/rust-rewrite/specs/S-*.md"))
    spec.append(os.path.join(repo, "docs/rust-rewrite/specs/README.md"))
    conf = [os.path.join(repo, "test/conformance/README.md")] + glob.glob(
        os.path.join(repo, "test/conformance", "**", "*"), recursive=True)
    # X1 = a test NAMES the function: scripts (drivers) and gen/ runners (code that
    # calls it) count; vectors/ is expected-output DATA (the C program printed the
    # name as a string) — a mention there is not a citation.
    conf = [p for p in conf if os.path.isfile(p) and "/vectors/" not in p]
    matrix = os.path.join(repo, "docs/rust-rewrite/coverage/coverage-matrix.md")
    return {
        "task_words": corpus_words(plan, backticked_only=True),
        "task_paths": corpus_files(plan),
        "spec_words": corpus_words(spec, backticked_only=True),
        "spec_paths": corpus_files(spec),
        "test_words": corpus_words(conf),
        "test_paths": corpus_files(conf),
        "matrix": matrix,
    }


def matrix_files(repo):
    """Files that have a row in the coverage matrix (task assigned by construction)."""
    cited, _ = corpus_files([os.path.join(repo, "docs/rust-rewrite/coverage/coverage-matrix.md")])
    return cited


def classify(inv, corp, repo):
    t_paths, t_bases = corp["task_paths"]
    s_paths, s_bases = corp["spec_paths"]
    x_paths, x_bases = corp["test_paths"]
    m_files = matrix_files(repo)
    # normalise matrix paths to repo-relative obs-studio paths
    m_norm = set()
    for p in m_files:
        m_norm.add(p)
        m_norm.add(os.path.basename(p))

    for it in inv:
        f, name = it["file"], it["name"]
        base = os.path.basename(f)
        it["T1"] = 1 if name in corp["task_words"] else 0
        it["F1"] = 1 if (f in t_paths or base in t_bases) else 0
        it["T2"] = 1 if (f in m_norm or base in m_norm) else 0
        it["S1"] = 1 if name in corp["spec_words"] else 0
        it["S2"] = 1 if (f in s_paths or base in s_paths) else 0
        it["X1"] = 1 if name in corp["test_words"] else 0
        it["X2"] = 1 if (f in x_paths or base in x_paths) else 0
        it["any1"] = 1 if (it["T1"] or it["S1"] or it["X1"]) else 0
        it["any2"] = 1 if (it["T2"] or it["S2"] or it["X2"]) else 0
        it["all2"] = 1 if (it["T2"] and it["S2"] and it["X2"]) else 0


# ---------------------------------------------------------------- report

def pct(n, d):
    return f"{100.0 * n / d:.1f}%" if d else "-"


def stats(items):
    n = len(items)
    g = lambda k: sum(i[k] for i in items)
    return {"n": n, "T1": g("T1"), "F1": g("F1"), "T2": g("T2"), "S1": g("S1"), "S2": g("S2"),
            "X1": g("X1"), "X2": g("X2"), "any1": g("any1"), "any2": g("any2"), "all2": g("all2")}


def report_table(rows, label, items):
    s = stats(items)
    return (f"| {label} | {s['n']} | {s['T1']} ({pct(s['T1'], s['n'])}) | {s['F1']} ({pct(s['F1'], s['n'])}) "
            f"| {s['T2']} ({pct(s['T2'], s['n'])}) | {s['S1']} ({pct(s['S1'], s['n'])}) | {s['S2']} ({pct(s['S2'], s['n'])}) "
            f"| {s['X1']} ({pct(s['X1'], s['n'])}) | {s['X2']} ({pct(s['X2'], s['n'])}) "
            f"| {s['any1']} ({pct(s['any1'], s['n'])}) | {s['any2']} ({pct(s['any2'], s['n'])}) "
            f"| {s['all2']} ({pct(s['all2'], s['n'])}) |")


def main():
    global CTAGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="../obs-studio")
    ap.add_argument("--ctags", default=CTAGS)
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()
    CTAGS = args.ctags
    src = args.src
    repo = REPO

    rev = subprocess.check_output(["git", "-C", src, "describe", "--always"], text=True).strip()
    files, inv = build_inventory(src)
    corp = build_corpora(repo)
    classify(inv, corp, repo)

    def body_len(i):
        return None if i["loc"] is None else i["loc"] - i["line"] + 1
    # non-trivial: body (sig..closing brace) spans >= 5 lines; unmeasured counts as non-trivial
    nt = [i for i in inv if body_len(i) is None or body_len(i) >= 5]
    raw = inv
    vend = [i for i in nt if i["vendored"]]
    core = [i for i in nt if not i["vendored"]]

    # per-area table
    areas = {}
    for i in core:
        areas.setdefault(i["area"], []).append(i)

    md = []
    md.append("# Function-level coverage of the C reference")
    md.append("")
    md.append("Generated by `python3 tools/function-coverage.py --write` (ctags 6.2.1 + ObjC/Swift line extractors). "
              f"Reference: obs-studio @ {rev}. Never hand-edit; regenerate after plan or reference changes.")
    md.append("")
    md.append("## Method")
    md.append("")
    md.append("Every function definition in the tracked source files (incl. initialized submodules) is inventoried; "
              "a function is **non-trivial** when its body spans >= 5 lines (one-line getters/setters and similar "
              "helpers are counted separately and excluded from the headline numbers — they do not need individual coverage).")
    md.append("")
    md.append("Measurement limits (verified in the round-5 audit, 2026-11): macro-generated functions are invisible "
              "to ctags (e.g. `DEFINE_VOID_DATA_CALLBACK` in `obs-scripting-lua-source.c` generates 4 callbacks that "
              "never appear here — the porting subtasks list them explicitly); body measurement resolves `#if/#else` "
              "to one branch and can fail on exotic constructs (a handful of rows stay unmeasured and count as "
              "non-trivial); C++ method extraction was verified on samples, not exhaustively; name matching is by "
              "identifier, so two different symbols with the same name can collide.")
    md.append("")
    md.append("| Flag | Meaning |")
    md.append("|---|---|")
    md.append("| T1 | function name appears in the plan (phase/subtask/guide/plan docs) |")
    md.append("| F1 | function's file is explicitly named in the plan text (stronger than T2's matrix rule, which may be a `*` glob) |")
    md.append("| T2 | function's file has a row in `coverage-matrix.md` (task assigned) |")
    md.append("| S1 | function name appears in a spec (`specs/S-*.md`) |")
    md.append("| S2 | function's file is cited in a spec |")
    md.append("| X1 | function name appears in the conformance corpus (`test/conformance/`) |")
    md.append("| X2 | function's file is referenced in the conformance corpus |")
    md.append("| any1 | T1 or S1 or X1 — the function is named by at least one artefact |")
    md.append("| any2 | T2 or S2 or X2 — the function's file is covered by at least one artefact |")
    md.append("| all2 | T2 and S2 and X2 — task, spec and test all reach the file |")
    md.append("")
    md.append(f"Inventory: {len(files)} source files, {len(raw)} functions total, {len(nt)} non-trivial "
              f"({len(vend)} in vendored third-party code, {len(core)} in first-party code).")
    md.append("")
    md.append("## Headline (non-trivial, first-party code)")
    md.append("")
    md.append("| set | n | T1 named | F1 file named in plan | T2 file->task | S1 named | S2 file in spec | X1 named | X2 file in tests | any1 named | any2 file | all2 triple |")
    md.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    md.append(report_table(md, "all non-trivial", nt))
    md.append(report_table(md, "first-party", core))
    md.append(report_table(md, "vendored third-party", vend))
    md.append(report_table(md, "trivial (helpers, excluded)", [i for i in raw if i not in nt]))
    md.append("")
    md.append("## Per area (non-trivial, first-party)")
    md.append("")
    md.append("| area | n | T1 named | F1 file named in plan | T2 file->task | S1 named | S2 file in spec | X1 named | X2 file in tests | any1 named | any2 file | all2 triple |")
    md.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for a in sorted(areas, key=lambda a: -len(areas[a])):
        md.append(report_table(md, a, areas[a]))
    md.append("")
    # gap list: non-trivial first-party functions no artefact names
    gaps = [i for i in core if not i["any1"]]
    gaps.sort(key=lambda i: -(body_len(i) or 0))
    md.append(f"## Gap list: {len(gaps)} non-trivial first-party functions named by no task, spec or test")
    md.append("")
    md.append("Top 60 by body size — candidates for a follow-up round (name them in a subtask step, a spec rule, or a vector):")
    md.append("")
    md.append("| function | file | lines | file->task | in spec | in tests |")
    md.append("|---|---|---|---|---|---|")
    for i in gaps[:60]:
        md.append(f"| `{i['name']}` | `{i['file']}` | {body_len(i)} | {i['T2']} | {i['S2']} | {i['X2']} |")
    md.append("")
    md.append(f"Per-area gap totals (non-trivial first-party, `any1` = 0): "
              + ", ".join(f"{a}: {sum(1 for i in v if not i['any1'])}"
                          for a, v in sorted(areas.items(), key=lambda kv: -sum(1 for i in kv[1] if not i['any1']))
                          if sum(1 for i in v if not i['any1']))
              + ".")
    md.append("")

    s = stats(core)
    union = sum(1 for i in core if i["F1"] or i["any1"])
    pinned = sum(1 for i in core if i["S2"] or i["X2"])
    # round-7 headline: covered by (sub)tasks, specs or tests — a task owns the
    # file (T2), the plan names it (F1), or any artefact names the function
    # (any1) or cites the file (any2)
    covered = sum(1 for i in core if i["T2"] or i["F1"] or i["any1"] or i["any2"])
    md.append("## Coverage verdict (non-trivial first-party functions, helpers excluded)")
    md.append("")
    md.append(f"- Task coverage (file -> task, T2): **{pct(s['T2'], s['n'])}** — the plan owns every file.")
    md.append(f"- File explicitly named in the plan (F1): **{pct(s['F1'], s['n'])}**.")
    md.append(f"- Function named by a task, spec or test (any1): **{pct(s['any1'], s['n'])}**.")
    md.append(f"- Behaviour pinned by a spec or a test (S2 or X2): **{pct(pinned, s['n'])}**.")
    md.append(f"- Covered by (sub)tasks, specs or tests (T2 or F1 or any1 or any2 — round-7 headline): **{pct(covered, s['n'])}**.")
    md.append(f"- Combined practical coverage (F1 or any1 — a developer can locate how the function is treated): **{pct(union, s['n'])}**.")
    md.append(f"- Full triple (task + spec + test all reach the file, all2): **{pct(s['all2'], s['n'])}**.")
    md.append("")

    summary = (f"functions: {len(raw)} total, {len(nt)} non-trivial ({len(core)} first-party); "
               f"first-party: file->task {pct(s['T2'], s['n'])}, file named in plan {pct(s['F1'], s['n'])}, "
               f"named-by-artefact (any1) {pct(s['any1'], s['n'])}, F1|any1 {pct(union, s['n'])}, "
               f"triple {pct(s['all2'], s['n'])}")
    print(summary)

    if args.write:
        out_md = os.path.join(repo, "docs/rust-rewrite/coverage/function-coverage.md")
        with open(out_md, "w") as fh:
            fh.write("\n".join(md) + "\n")
        out_csv = os.path.join(repo, "docs/rust-rewrite/coverage/function-inventory.csv")
        with open(out_csv, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["file", "area", "name", "line", "body_lines", "lang", "vendored",
                        "T1", "F1", "T2", "S1", "S2", "X1", "X2", "any1", "any2", "all2"])
            for i in inv:
                w.writerow([i["file"], i["area"], i["name"], i["line"], body_len(i) or "", i["lang"],
                            i["vendored"], i["T1"], i["F1"], i["T2"], i["S1"], i["S2"], i["X1"], i["X2"],
                            i["any1"], i["any2"], i["all2"]])
        print(f"wrote {out_md}")
        print(f"wrote {out_csv}")


if __name__ == "__main__":
    sys.exit(main())
