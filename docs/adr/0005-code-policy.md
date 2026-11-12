# 0005. Code policy: unsafe, MSRV, dependencies

- Status: Accepted (decisions confirmed 2026-11-05/06: D-07, D-24; rules from `docs/rust-rewrite-plan.md` §3.7 and P0-05 subtasks)
- Date: 2026-11-10
- Supersedes: —
- Superseded by: —

## Context

The rewrite produces a Rust workspace next to a C build that will coexist for
years (ADR 0001). Four things need to be pinned down before any crate lands,
because every later crate inherits them: where `unsafe` may live (memory safety
is the point of the rewrite), how old the toolchain is allowed to get, which
third-party crates are admissible (the project is GPL-2.0-or-later, D-07), and
how native/vendor dependencies are pinned and updated. The lint enforcement for
the first three lands in `[workspace.lints]` with the workspace skeleton (P0-07.1);
the `deny.toml` allow-list lands in P0-10.

## Decision

### Rule 1 — `unsafe` boundaries (P0-05.1)

`unsafe` is allowed **only** in:

- `*-sys` crates (FFI bindings to native libraries),
- `obs-ffi` (the C ABI surface),
- platform capture/audio crates (WASAPI, DirectShow, PipeWire, V4L2, CoreAudio, AVFoundation, hardware-encoder SDKs).

Every `unsafe` block requires a `// SAFETY:` comment stating the invariant.
Crate-level `#![deny(unsafe_op_in_unsafe_fn)]` applies everywhere. Pure crates
(`obs-util`, `obs-math`, `obs-data`, `obs-signal`) run under Miri in CI
(P0-09.3); a pure crate containing any `unsafe` fails review by definition.
Lint config is committed in P0-07.1.

### Rule 2 — MSRV (P0-05.2, D-24)

**Track latest stable.** MSRV = the current stable toolchain at release time;
bump within one release cycle. Enforced by `rust-toolchain.toml` +
`rust-version` in the workspace manifest (P0-07.2) and an MSRV CI job (lands in
P0-09; no `.github/` in this checkout yet). Rationale: a 3–4 year project (ADR
0001) should not pin a 2026 toolchain; "latest stable" keeps `cargo deny`/audit
and stdlib fixes flowing, at the cost of periodic CI churn.

### Rule 3 — License allow-list (D-07)

The project is **GPL-2.0-or-later**. Dependency allow-list:
**MIT, Apache-2.0, BSD (all variants), ISC, Zlib, MPL-2.0**. Anything else is
rejected by `cargo deny` (P0-10.1) unless it passes the exception process below.
GPL-2.0-or-later code is admissible (same license family) but is listed in the
`deny.toml` exceptions section so it is visible.

### Rule 4 — stdlib first (plan §3)

Before admitting any dependency: the standard-library / platform alternative is
considered and the rejection recorded in the PR description. The dependency
admission checklist below is the gate (P0-05.3, `CONTRIBUTING.md`).

### Rule 5 — `-sys` and vendored code (P0-05.4)

- All native/vendor dependencies (FFmpeg, x264, CEF, srt/rist, libvlc, RNNoise,
  vendor SDKs per D-04/D-09/D-10/D-11) are consumed through `-sys` crates or
  hand-written FFI inside platform crates — never re-implemented.
- **Pins:** native dependency versions are pinned in the CMake dependency
  presets (`cmake/`) and in `deps/` for vendored sources; `-sys` crate versions
  are pinned exactly in `[workspace.dependencies]`; `Cargo.lock` is committed
  (binary project — `guides/phase-0-guide.md`).
- **No network in `build.rs`:** builds never download anything; artifacts come
  from the CMake presets / system packages / pinned vendored trees.
- **Pin owners:** FFmpeg — P2/P5-15 media-pipeline and `obs-ffmpeg-mux` owners;
  x264 — P5-07 (`obs-x264`) owner; CEF — P5-45 (`obs-browser`) owner. A pin bump
  is a `build:` PR with a one-line note added to this section.

### Dependency admission checklist (P0-05.3 — mirrored in `CONTRIBUTING.md`)

A dependency may be added only when all five hold:

1. stdlib alternative considered (Rule 4),
2. licence GPLv2+-compatible (Rule 3),
3. maintained (release < 12 months),
4. `cargo audit` clean,
5. one-line justification in the PR.

### Exceptions process (P0-10.3)

A dependency outside the allow-list requires, in one PR: (1) an entry in the
table below with crate, licence, justification, owner, and review-by date;
(2) a matching `[exceptions]` entry in `deny.toml`; (3) reviewer approval.
Exceptions expire at the review-by date and must be renewed or the dependency
removed.

| Crate | Licence | Justification | Owner | Review by |
|---|---|---|---|---|
| _(none yet — illustrative row removed on first real exception)_ | | | | |

Example of a correctly recorded exception (illustrative format):
`libfoo` — AGPL-3.0 — "no GPL-compatible implementation of X exists; network
surface isolated behind trait Y" — owner P5-20 — review 2027-11-01.

## Alternatives

1. **Freeze MSRV at 2026 stable** (e.g. one-year-old toolchain).
   Rejected: (a) a 3–4 year project on a frozen toolchain accumulates
   `cargo audit` debt and stdlib fixes that must be backported forever;
   (b) CI matrix cost (testing two toolchains) for a benefit — old-cargo
   compatibility — that no downstream user of a *release* binary needs.
2. **Permissive (MIT/Apache) project licence.**
   Rejected: (a) OBS is GPLv2+ and linking its code/headers would force the
   constraint anyway (D-07 confirmed 2026-11-05); (b) the ecosystem plugins
   and the C tree are already GPL.
3. **Ban all `unsafe` (safe Rust only, no FFI crates).**
   Rejected: (a) the entire point is to wrap FFmpeg/CEF/platform APIs that are
   C — an `unsafe`-free rewrite is impossible by construction; (b) banning it
   wholesale would push FFI into macro-generated code that hides the same
   unsafety with worse tooling. The boundary policy above is the standard
   mitigation (SAFETY comments, Miri on pure crates, `-sys` crate isolation).

### Admitted dependencies (log)

| Dependency | Licence | Admitted | Reason (Rule 4 checklist) |
|---|---|---|---|
| `crc32fast` 1.5.x | MIT | 2026-11-11 (P1-10, S-055 U1) | `calc_crc32` is the whole of `libobs/util/crc32.c`; the stdlib has no CRC-32; the crate is the table-driven reference implementation with the running-state API the C signature needs (S-055-R01/R02) |
| `blake2` 0.11.x (feature `blake2b`) | MIT | 2026-11-11 (P1-10) | Rust side of the vendored `deps/blake2` (blake2b-512); the frontend keeps the C copy until Phase 6 (P6-32/P6-23); no reimplementation of a published hash standard |
| `parking_lot` 0.13.x | MIT OR Apache-2.0 | 2026-11-11 (P1-22, S-004) | `ReentrantMutex` is a direct requirement of S-004 (per-signal and global-callback recursive locks; procs R60); std has no reentrant mutex; both licences on the D-07 allow-list |
| `libc` 0.2.x | MIT OR Apache-2.0 | 2026-11-12 (P1-11, S-023 R107 + S-055 U2) | `dlopen/dlsym/dlclose` and `pipe/fork/exec/waitpid` are the platform FFI of S-023/S-055; the stdlib has no dlopen or low-level child-pipe API; the `extern "C"` surface stays in `obs-ffi` (Rule 1) — only these two platform-helper modules in `obs-util` touch libc, each block with a `// SAFETY:` comment |

P1-10.1 **base64: skip** — there is no base64 in `libobs/`; the only in-tree
copy is the librtmp vendored `cencode.c` (a Phase 5 concern, P5-17.4). No Rust
base64 dependency is admitted for Phase 1; the absence re-verification by grep
is the C-checkout substep.

## Consequences

- Easier: reviewers have a mechanical checklist for deps and unsafe; Miri gives
  pure crates a real safety net; pin owners make vendor updates a named
  responsibility.
- Harder: "latest stable" MSRV means periodic churn when the toolchain bumps;
  the exceptions table is permanent maintenance.
- Revisit: if two or more exceptions are live simultaneously, reopen Rule 3
  (the allow-list is too tight for the dependency set); if Miri finds a
  systemic class of bugs in a "pure" crate, add that crate to the `-sys`
  boundary rules instead of exempting it.

## Links

- D-07, D-24 (`docs/rust-rewrite/DECISIONS-NEEDED.md`); plan §3 item 7
  (`docs/rust-rewrite-plan.md`); P0-05.1–P0-05.4, P0-10.1/.3
  (`docs/rust-rewrite/subtasks/phase-0-subtasks.md`); `CONTRIBUTING.md`
  (checklist); P0-07.1 (lint config), P0-09.3 (Miri/audit jobs).
