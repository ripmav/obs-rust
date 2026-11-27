#!/usr/bin/env python3
"""P1-29.2 (WP-31): diff the cbindgen-generated obs-ffi header against the
C reference headers.

Generates the header (cbindgen as a dev-dependency of obs-ffi, driven via
`cargo test -p obs-ffi --test header_diff`, config
`crates/obs-ffi/cbindgen.toml`), then compares every function declaration
in the generated header against the matching declaration in the C
reference headers under `libobs/` (the pinned obs-studio 33.0.1 checkout)
and reports the mismatches. Exit 0 iff the report is empty.

Usage:
  cbindgen-header-diff.py                 generate + compare (needs the libobs link)
  cbindgen-header-diff.py GEN.h LIBOBS    compare an already-generated header

The diff compares FUNCTION DECLARATIONS ONLY. Intentionally out of scope:
- struct/typedef/enum layouts and the DARRAY field views — the `#[repr(C)]`
  mirrors are pinned by the layout tests and the conformance swap builds;
- macros (`EXPORT`, `OBS_DEPRECATED`, ...) and default arguments —
  normalised away (neither occurs in the reference declarations below);
- the `static inline` helpers of the C headers (not linkable in C, no
  bridge) — except the two `pthread_mutex_init_{value,recursive}` helpers,
  whose linkable form the bridge provides (the `bstrdup` precedent);
- the C symbols the bridges call and the `cargo test` link stand-ins
  (excluded from the generated header by `crates/obs-ffi/cbindgen.toml`);
- the opaque handle typedefs, which the bridges type as `void *` and C
  never dereferences: `lookup_t`, `os_event_t`, `os_sem_t`,
  `struct dbus_sleep_info`, `struct portal_inhibit_info` (aliased below).

Reference declarations in scope (the exact set the bridge must provide,
172 in total):
  util/bmem.h                   6  EXPORT (WP-43, P1-01.3)
  util/text-lookup.h             4  EXPORT
  util/threading.h              14 EXPORT + the two pthread_mutex_init_* static inlines
  util/lexer.h                  10 EXPORT
  util/cf-lexer.h                9 EXPORT
  util/cf-parser.h               2 EXPORT
  util/platform-nix.c            6 extern dbus_/portal_inhibit* declarations
  util/bitstream.h               4  EXPORT (bitstream_reader_read_bit has no
                                  declaration in any C header — excluded)
  util/array-serializer.h        3  EXPORT
  util/file-serializer.h         5  EXPORT
  util/buffered-file-serializer.h 3 EXPORT
  util/darray.h                  4  static inlines (the P1-03.2 helpers)
  shared/bpm/bpm.h                2  plain (WP-34, P1-33.1)
  shared/opts-parser/opts-parser.h 2 plain (WP-34, P1-33.5)
  shared/file-updater/file-updater/file-updater.h 3 plain (WP-34, P1-33.2)
  util/source-profiler.h          4  EXPORT (WP-37, P1-12.3)
  obs-internal.h                 11 extern source_profiler_* declarations
                                 (WP-37, P1-12.3 — the internal API)
  graphics/vec3.h                 8  EXPORT (WP-43, P1-15.4)
  graphics/vec4.h                 2  EXPORT (WP-43, P1-15.4)
  graphics/quat.h                11  EXPORT (WP-43, P1-15.4)
  graphics/axisang.h              1  EXPORT (WP-43, P1-16.4)
  graphics/matrix3.h             11  EXPORT (WP-43, P1-16.5)
  graphics/matrix4.h             17  EXPORT (WP-43, P1-16.5 —
                                 matrix4_mul_4x3_only is internal to the
                                 .c, declared in no header, excluded from
                                 the generated header in cbindgen.toml)
  graphics/plane.h                7  EXPORT (WP-43, P1-17.4)
  graphics/bounds.h              16  EXPORT (WP-43, P1-18.4)
  graphics/math-extra.h           7  EXPORT (WP-43, P1-18.4)
  (graphics/vec2.h is out of scope: all its helpers are static inline)
  (shared/happy-eyeballs: build_addr_list is file-local static in the .c —
  no reference declaration, the conformance runner declares it in swap
  mode; the bitstream_reader_read_bit precedent, excluded from the
  generated header. All paths are relative to the obs-studio checkout
  root — libobs/ is a symlink into it, shared/ is its sibling.)

The PR #73 platform exports have no header in the 33.0.1 tree — their
reference declarations are the `extern` block of the `GIO_FOUND` section
in `util/platform-nix.c` (there is no `platform-nix.h`), so that file is
part of the diff scope.

Normalisation before comparing: whitespace, parameter names, default
arguments, the declaration prefixes (`EXPORT` / `static inline` /
`OBS_DEPRECATED`), and the deliberate Rust-vs-C naming choices: the opaque
typedefs above, the C enums used as plain ints (`os_event_type`,
`ignore_whitespace`), and `size_t` (cbindgen prints Rust `usize` as
`uintptr_t`).

Linux-only by design: the CI step runs in the ubuntu conformance job with
the pinned checkout; the reference declarations are the unix ones.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
LIBOBS = ROOT / "libobs"

# C names in the reference declarations -> the form the generated header
# uses (see the module docstring for why).
ALIASES = {
    "lookup_t": "void",
    "os_event_t": "void",
    "os_sem_t": "void",
    "dbus_sleep_info": "void",
    "portal_inhibit_info": "void",
    # WP-37 (P1-12.3): the opaque source / graphics-timer handles and the
    # C profiler_result_t (the bridge's mirror is `profiler_result`).
    "obs_source_t": "void",
    "gs_timer_t": "void",
    "obs_video_info": "void",
    "profiler_result_t": "structprofiler_result",
    # WP-34 (P1-33): the opaque handle typedefs of the shared/ modules —
    # the bridges type them as void* / the repr(C) mirrors.
    "obs_output_t": "void",
    "update_info_t": "structupdate_info",
    # the confirm callback typedef renders as a bare C function pointer in
    # the generated header (cbindgen's form of Option<extern "C" fn>);
    # parameter names are dropped by norm_param on both sides.
    "confirm_file_callback_t": "bool*",
    # abi_file_updater.rs Rust callback alias (same shape as the C
    # confirm_file_callback_t typedef).
    "ConfirmCallback": "bool*",
    "size_t": "uintptr_t",
    # The C `struct darray` (darray.h) — the bridge's mirror is named
    # `darray_c` (the cf-lexer bridge owns the crate-level `darray` name
    # for its generic DARRAY view); the alias includes the `struct` prefix
    # so both sides normalise identically.
    "darray": "structdarray_c",
    # WP-43 (P1-15.4/P1-16.5/P1-17.4/P1-18.4): the math struct tags — the
    # C headers use lowercase `struct vec3` & co, cbindgen renders the
    # Rust mirrors as `struct Vec3` & co; the alias normalises both sides.
    "vec2": "structVec2",
    "vec3": "structVec3",
    "vec4": "structVec4",
    "quat": "structQuat",
    "axisang": "structAxisAng",
    "matrix3": "structMatrix3",
    "matrix4": "structMatrix4",
    "plane": "structPlane",
    "bounds": "structBounds",
}
# enum parameter types that are plain `int`s in C (the bridges take c_int).
ENUM_AS_INT = {"os_event_type", "ignore_whitespace"}

# (reference file, extraction patterns) — the diff scope. Paths are
# relative to the obs-studio checkout root (the libobs/ symlink's parent);
# the shared/ entries (WP-34) are siblings of libobs/.
REFS = [
    # WP-43 (P1-01.3): the bmem allocator (the six bmem.c exports; from
    # here on every swap build links the staticlib, whose bmem exports
    # are the process allocator).
    ("libobs/util/bmem.h", ("export",)),
    ("libobs/util/text-lookup.h", ("export",)),
    ("libobs/util/threading.h", ("export", "pthread-inline")),
    ("libobs/util/lexer.h", ("export",)),
    ("libobs/util/cf-lexer.h", ("export",)),
    ("libobs/util/cf-parser.h", ("export",)),
    ("libobs/util/platform-nix.c", ("extern",)),
    ("libobs/util/bitstream.h", ("export",)),
    ("libobs/util/array-serializer.h", ("export",)),
    ("libobs/util/file-serializer.h", ("export",)),
    ("libobs/util/buffered-file-serializer.h", ("export",)),
    ("libobs/util/darray.h", ("darray-inline",)),
    # WP-34 (P1-33): the shared/ leaf modules — plain declarations (no
    # EXPORT); every function declaration of these headers is in scope
    # (bpm.h: 2, opts-parser.h: 2, file-updater.h: 3).
    ("shared/bpm/bpm.h", ("plain",)),
    ("shared/opts-parser/opts-parser.h", ("plain",)),
    ("shared/file-updater/file-updater/file-updater.h", ("plain",)),
    # WP-37 (P1-12.3): the public profiler API (EXPORT) and the internal
    # source_profiler_* externs (obs-internal.h; the name filter below).
    ("libobs/util/source-profiler.h", ("export",)),
    ("libobs/obs-internal.h", ("extern",)),
    # WP-43 (P1-15.4/P1-16.5/P1-17.4/P1-18.4): the nine exported graphics
    # math headers — the 80 EXPORT declarations the math bridge must
    # provide (matrix4.h has 17: `matrix4_mul_4x3_only` is internal to
    # matrix4.c, declared in no header, excluded from the generated
    # header in cbindgen.toml). vec2.h is out of scope (all its helpers
    # are static inline).
    ("libobs/graphics/vec3.h", ("export",)),
    ("libobs/graphics/vec4.h", ("export",)),
    ("libobs/graphics/quat.h", ("export",)),
    ("libobs/graphics/axisang.h", ("export",)),
    ("libobs/graphics/matrix3.h", ("export",)),
    ("libobs/graphics/matrix4.h", ("export",)),
    ("libobs/graphics/plane.h", ("export",)),
    ("libobs/graphics/bounds.h", ("export",)),
    ("libobs/graphics/math-extra.h", ("export",)),
]
# The only `extern` declarations in scope: the six dbus_/portal_inhibit*
# symbols the platform bridge provides and the eleven source_profiler_*
# symbols the profiler bridge provides (WP-37).
EXTERN_NAMES = {
    "dbus_sleep_info_create",
    "dbus_inhibit_sleep",
    "dbus_sleep_info_destroy",
    "portal_inhibit_info_create",
    "portal_inhibit",
    "portal_inhibit_info_destroy",
    "source_profiler_frame_begin",
    "source_profiler_frame_collect",
    "source_profiler_render_begin",
    "source_profiler_render_end",
    "source_profiler_reset_video",
    "source_profiler_async_frame_received",
    "source_profiler_source_tick_start",
    "source_profiler_source_tick_end",
    "source_profiler_source_render_begin",
    "source_profiler_source_render_end",
    "source_profiler_remove_source",
}
# The only `static inline` declarations of darray.h in scope: the four
# P1-03.2 C helpers whose linkable form the bridge provides (the rest of
# the header's inlines stay static until the P1-03.3 recipe).
DARRAY_NAMES = {"darray_push_back", "darray_insert", "darray_erase", "darray_move_item"}

TOK = re.compile(r"[A-Za-z_][A-Za-z0-9_]*|\*+")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")

# Function declarations of the generated header: `RET NAME(params);`
# (optionally `extern`-prefixed — cbindgen prints the non-bridge items that
# way; the cbindgen.toml exclusion list keeps them out). Separators between
# declarations are `;`/`{`/`}` (the extern "C" wrapper braces); the
# lookbehind keeps consecutive matches from fighting over them.
GEN_FN = re.compile(
    r"(?:^|(?<=[;{}]))\s*[{};]*\s*(?:extern\s+)?"
    r"([A-Za-z_][A-Za-z0-9_]*(?:\s+[A-Za-z_][A-Za-z0-9_]*|\s*\**)*?)"
    r"\s*([a-z_][a-z0-9_]*)\s*\(([^;()]*)\)\s*;"
)


def die(msg):
    print(f"cbindgen-header-diff: {msg}", file=sys.stderr)
    sys.exit(1)


def strip_comments(text):
    """Drop // and /* */ comments (string/char literals preserved)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            end = min(j + 1, n)
            out.append(text[i:end])
            i = end
        else:
            out.append(c)
            i += 1
    return "".join(out)


def flatten(text):
    """Comments, preprocessor lines and the `extern "C"` wrapper out, whitespace collapsed (multi-line declarations -> one)."""
    text = strip_comments(text)
    text = re.sub(r"^[ \t]*#.*$", " ", text, flags=re.M)
    text = re.sub(r"extern\s*\"C\"\s*\{", " ", text)
    return re.sub(r"\s+", " ", text)


def norm_type(toks):
    """Canonical form of a token list (types, no names, no spaces)."""
    out = []
    i = 0
    while i < len(toks):
        t = toks[i]
        if t in ("struct", "enum") and i + 1 < len(toks):
            name = toks[i + 1]
            if t == "enum" and name in ENUM_AS_INT:
                out.append("int")
            elif t == "struct" and name in ALIASES:
                out.append(ALIASES[name])
            else:
                out.append(t + name)
            i += 2
            continue
        out.append(ALIASES.get(t, t))
        i += 1
    return "".join(out)


def split_params(params):
    """Top-level comma split (no paren-nested parameters in scope)."""
    parts, cur = [], []
    for ch in params:
        if ch == ",":
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return [p for p in parts if p.strip()]


def norm_param(param):
    """Canonical form of one parameter (name and default argument dropped)."""
    # Default arguments do not occur in the reference declarations in
    # scope; strip them if a future one ever does.
    param = re.sub(r"\s*=\s*[^,]*\Z", "", param.strip())
    toks = TOK.findall(param)
    if not toks:
        return ""
    # The last `*`-token separates type from the parameter name
    # (`*` and `**` are single tokens: the TOK regex groups consecutive stars).
    star_i = max((i for i, t in enumerate(toks) if t.startswith("*")), default=-1)
    if star_i >= 0:
        rest = toks[star_i + 1 :]
        if len(rest) == 1 and IDENT.match(rest[0]):
            toks = toks[: star_i + 1]
    elif toks[0] in ("struct", "enum") and len(toks) > 2:
        # `struct X name` / `enum X name` without a pointer: X is the tag.
        toks = toks[:2]
    elif len(toks) >= 2 and all(IDENT.match(t) for t in toks):
        # `size_t n`, `unsigned long ms`, `char in`: last word is the name.
        toks = toks[:-1]
    # A leading `const` carries no ABI difference in this scope: by-value
    # const is meaningless in C, and pointee const (`const void *`) is a
    # promise about the callee, not part of the signature cbindgen prints.
    # Both sides are normalised, so the comparison stays symmetric.
    if toks and toks[0] == "const":
        toks = toks[1:]
    return norm_type(toks)


def signature(ret, name, params):
    """Canonical `name(ret,p1,p2,...)` of a declaration."""
    rets = norm_type(TOK.findall(ret))
    plist = [norm_param(p) for p in split_params(params)]
    return f"{name}({rets},{','.join(plist)})"


def take_decl(body):
    """`RET NAME(params` -> (signature, name), or None if it does not parse."""
    i = body.rfind("(")
    if i < 0:
        return None
    # Tokenise, don't split on whitespace: C glues the star to the name
    # (`lookup_t *text_lookup_create`).
    head = TOK.findall(body[:i])
    if len(head) < 2 or not IDENT.match(head[-1]):
        return None
    name = head[-1]
    ret = " ".join(head[:-1])
    params = body[i + 1 :]
    return signature(ret, name, params), name


def extract_c(text, patterns, where):
    """Function declarations of one C reference file, per its patterns."""
    text = flatten(text)
    decls = {}
    for p in patterns:
        for m in RX[p].finditer(text):
            # The plain pattern captures ret/name/params separately;
            # reassemble the `RET NAME(params` shape take_decl expects.
            body = (
                f"{m.group(1)} {m.group(2)}({m.group(3)}" if p == "plain" else m.group(1)
            )
            got = take_decl(body)
            if got is None:
                continue
            sig, name = got
            if p == "pthread-inline" and not name.startswith("pthread_mutex_init_"):
                continue
            if p == "darray-inline" and name not in DARRAY_NAMES:
                continue
            if p == "extern" and name not in EXTERN_NAMES:
                continue
            if name in decls:
                die(f"duplicate C reference for {name} in {where}")
            decls[name] = sig
    return decls


# Declaration shapes: EXPORT fns (with an optional OBS_DEPRECATED suffix),
# the two linkable pthread_mutex_init_* static inlines, the extern
# dbus_/portal declarations of platform-nix.c, and plain declarations of
# the shared/ module headers (WP-34; the ret/name/params shape of GEN_FN,
# whose identifier-before-parenthesis requirement keeps typedefs and macro
# parameters out). The body capture ends at the first `)` — no parameter
# in scope carries nested parens.
RX = {
    "export": re.compile(r"\bEXPORT\b(.*?)\)\s*(?:OBS_DEPRECATED\s*)?[;{]"),
    "pthread-inline": re.compile(r"\bstatic\s+inline\b(.*?)\)\s*(?:;|\{)"),
    "darray-inline": re.compile(r"\bstatic\s+inline\b(.*?)\)\s*(?:;|\{)"),
    "extern": re.compile(r"\bextern\b(.*?)\)\s*;"),
    "plain": re.compile(
        r"(?:^|(?<=[;{}]))\s*[{};]*\s*"
        r"([A-Za-z_][A-Za-z0-9_]*(?:\s+[A-Za-z_][A-Za-z0-9_]*|\s*\**)*?)"
        r"\s*([a-z_][a-z0-9_]*)\s*\(([^;()]*)\)\s*;"
    ),
}


def parse_generated(text):
    """Function declarations of the generated header."""
    decls = {}
    for m in GEN_FN.finditer(flatten(text)):
        name = m.group(2)
        sig = signature(m.group(1), name, m.group(3))
        if name in decls:
            die(f"duplicate generated declaration for {name}")
        decls[name] = sig
    return decls


def generate_header():
    """Generate the header via the cbindgen dev-dependency of obs-ffi.

    Output goes to cargo's target scratch (gitignored, and not subject to
    the /tmp reaping seen on some hosts).
    """
    out = ROOT / "target" / "obs-ffi-header-diff" / "obs-ffi.h"
    env = dict(os.environ, HDR_DIFF_OUT=str(out))
    try:
        subprocess.run(
            ["cargo", "test", "-p", "obs-ffi", "--test", "header_diff"],
            cwd=ROOT,
            env=env,
            check=True,
        )
    except subprocess.CalledProcessError as e:
        die(f"header generation failed (cargo test, exit {e.returncode})")
    if not out.exists():
        die(f"the test did not write {out}")
    return out


def main():
    args = sys.argv[1:]
    if len(args) == 0:
        if not (LIBOBS / "util").is_dir():
            die(f"{LIBOBS} is not a libobs checkout (tools/link-obs-source.sh)")
        gen_path = generate_header()
        libobs = LIBOBS
    elif len(args) == 2:
        gen_path, libobs = Path(args[0]), Path(args[1])
        if not (libobs / "util").is_dir():
            die(f"{libobs} is not a libobs checkout")
    else:
        die("usage: cbindgen-header-diff.py [GEN.h LIBOBS]")

    # The shared/ reference headers are siblings of libobs/ in the
    # checkout (the libobs/ entry is a symlink into it).
    root = libobs.resolve().parent

    gen_decls = parse_generated(gen_path.read_text())
    c_decls = {}
    for rel, patterns in REFS:
        text = (root / rel).read_text()
        c_decls.update(extract_c(text, patterns, rel))

    mismatches = []
    for name in sorted(set(gen_decls) | set(c_decls)):
        g, c = gen_decls.get(name), c_decls.get(name)
        if g is None:
            mismatches.append(f"missing from generated header: {c}")
        elif c is None:
            mismatches.append(f"no C reference in scope: {g}")
        elif g != c:
            mismatches.append(f"signature mismatch {name}:\n  C  : {c}\n  gen: {g}")

    if mismatches:
        print(f"header diff: {len(mismatches)} mismatch(es):")
        for m in mismatches:
            print(f" - {m}")
        sys.exit(1)
    print(
        f"header diff: OK — {len(c_decls)}/{len(c_decls)} C reference "
        "declarations match the generated header"
    )


if __name__ == "__main__":
    main()
