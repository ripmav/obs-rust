# 0001. Strangler-fig migration strategy

- Status: Accepted (strategy confirmed with the project owner 2026-11-06; two-maintainer review pending, P0-01.5)
- Date: 2026-11-10
- Supersedes: —
- Superseded by: —

## Context

OBS Studio at the libobs 33.0.1 reference checkout: about 494k tracked source lines,
15,819 commits, 3 operating systems, 41 in-tree plugin modules.[^1]

| Area | Path | ~Lines | Language |
|---|---|---|---|
| Core engine (`libobs`) | `libobs/` | 94k | C (+ObjC for macOS) |
| Graphics backends | `libobs-opengl`, `libobs-d3d11`, `libobs-metal`, `libobs-winrt` | 17k | C / C++ / Swift |
| Frontend (Qt6 GUI) | `frontend/` | 109k | C++ (+ `.ui`) |
| Plugins (41 modules) | `plugins/` | 172k | C / C++ / ObjC |
| Shared helpers | `shared/` | 22k | C / C++ |
| Vendored deps | `deps/` (blake2, glad, json11, libcaption, libdshowcapture, ...) | 70k | C / C++ |

The constraint that shapes every option: **the plugin ABI is C**. `libobs` exports
~680 `EXPORT` symbols in `obs.h` alone,[^2] plugins are shared libraries registering
`struct obs_source_info`-style vtables through `obs_module_load`
(`libobs/obs-module.h`, 201 lines), 41 in-tree modules plus hundreds of third-party
plugins (obs-websocket, obs-browser, capture tooling) depend on it, and the plugin
ecosystem is OBS's biggest asset. Rust also does not remove the native dependencies
(FFmpeg, CEF, Qt, WASAPI/DirectShow/PipeWire/V4L2/AVFoundation, hardware-encoder SDKs),
so a large share of the code stays FFI glue regardless of language.

[^1]: Measured with `tools/task-coverage.py` against the libobs 33.0.1 tracked set
  (`.c .cpp .cc .h .hpp .m .mm .swift .ui .effect`), as recorded in
  `docs/rust-rewrite-plan.md` §1 and `docs/rust-rewrite/subtasks/phase-0-subtasks.md`
  (facts verified in the read-only checkout). Per-area re-derivation, run in the
  reference checkout (linked via `tools/link-obs-source.sh`):

      git ls-files libobs/ | grep -E '\.(c|cpp|cc|h|hpp|m|mm|swift|ui|effect)$' | xargs cat | wc -l
      # same for frontend/ plugins/ shared/ deps/; commits: git rev-list --count HEAD

[^2]: `grep -c EXPORT libobs/obs.h` in the reference checkout.

## Decision

**Strangler-fig migration: Rust and C coexist in one build for years.**

- A Cargo workspace lives at the repo root next to the CMake build; CMake invokes
  Cargo via Corrosion during the transition (`OBS_ENABLE_RUST`, default OFF until
  Phase 4).
- **Every phase ships a working OBS.** No phase is allowed to leave the tree
  unbuildable or unshippable; a phase's exit criteria (table below) are the gate.
- The C ABI (`obs.h`, `obs-module.h`, `obs-source.h`, ...) stays a **stable
  compatibility layer generated from Rust** (`#[no_mangle] extern "C"`, cbindgen)
  until a native Rust plugin API is proven (policy: ADR 0002).
- "Everything in Rust" is the **end state, reached incrementally**: leaf libraries
  first, core second, plugins third, GUI last (plan §4).

### What "ships" means

A phase ships when its nightly build passes the smoke test on all 3 OSes
(Linux, Windows, macOS) — defined concretely as the Phase 0 headless smoke test
(P0-16: scene with colour + text source, 5 s record, `ffprobe` validation) plus the
manual 60-second record of a scene with browser source, mic, and game capture
(plan §5, "no regression" gate).

### Exit criteria per phase (verbatim from the phase files)

| Phase | Exit criteria |
|---|---|
| 0 — Foundation | "ADRs 1-6 merged, CI green on 3 OSes with Rust jobs, golden files generated from C, ABI plugin test passing, benchmark baseline stored." |
| 1 — Leaf libraries | "all `libobs/util`, math, `callback` and `obs-data` code is served by Rust crates on all OSes; goldens, ABI test and benchmarks pass." |
| 2 — Media pipeline | "video-io, audio-io, mixer, converters and parsers served by Rust; goldens and fuzzers green; soak test shows no regression." |
| 3 — Graphics | "all built-in and plugin effects compile, `wgpu` backend renders deterministic goldens, capture sources can share textures on the three OSes, legacy backend retained as a fallback." |
| 4 — Core object model | "C `frontend` and all C plugins run on the Rust core; symbol, layout and behaviour tests pass; `obs-cli` records a scene." |
| 5 — Plugins | "every plugin is either ported or explicitly kept as a thin FFI shim with an owner; the old C plugin directories are removed from the build." |
| 6 — Frontend | "Rust application reaches feature parity per `docs/rust-rewrite/parity-checklist.md` (generated from the C++ UI by walking menus and settings), passes the UI test suite, ships as beta on 3 OSes." |
| 7 — Retire C/C++ | "no C/C++ of our own remains except documented FFI shims; `cargo xtask package` produces all release artifacts; SDK 1.0 published; security review closed." |

## Alternatives

1. **Big-bang rewrite.** One new codebase, one cutover.
   Rejected:
   - (a) The ~680-symbol C plugin ABI (41 in-tree plugins, hundreds of third-party
     plugins) breaks for the entire ecosystem at a single instant, with no
     intermediate point where compatibility can be verified — the ABI conformance
     test (P0-15) only makes sense against a coexisting C reference.
   - (b) Nothing ships for years: feature work freezes for the whole effort while
     the existing C OBS keeps taking upstream fixes that must be re-merged at
     cutover.
   - (c) The regression surface is one un-bisectable multi-year diff; a behaviour
     change introduced in month 8 is practically unfindable.
2. **New repository.** Rewrite in a fresh repo, merge when done.
   Rejected:
   - (a) The strangler needs Rust and C in **one build** — CMake invokes Cargo via
     Corrosion and links Rust static libs into `libobs`; a separate repository
     cannot produce a single linked binary.
   - (b) Dual maintenance of the C tree (upstream fixes, security patches) across
     two remotes for the life of the project, plus loss of the 15,819-commit
     history as context for maintainers mid-migration.
3. **Status quo (stay C/C++).** No rewrite.
   Rejected:
   - (a) The motivation — memory safety in the highest-risk parsing and network
     surfaces (RTMP, NAL/AVC/HEVC/AV1, effect parsing, FFmpeg muxing) — goes
     unaddressed; these are exactly the classes of bugs C cannot stop.
   - (b) The C ABI surface keeps growing as plugins are added, so the eventual
     cost of any later rewrite only increases.
4. **Rust only for new plugins, C core stays.**
   Rejected:
   - (a) `libobs` is the product: 94k lines of core (sources, scenes, mixers,
     hotkeys, data) would remain in memory-unsafe C forever, so the safety win
     applies only to the smallest slice.
   - (b) The two cores share the media pipeline and graphics; a C core means every
     Rust plugin pays an FFI crossing on the real-time hot path, and the
     `gs_*`/effect-language surface (highest parser risk) is never touched.

## Consequences

- Easier: each phase is independently valuable (memory-safe parsing/muxing lands in
  Phases 1–2, before any core risk); regressions are bisectable per phase; the
  plugin ecosystem is never cut off (ADR 0002).
- Harder: years of dual maintenance (CMake + Cargo, two toolchains, CI for both);
  the C ABI compatibility layer is active maintenance cost until Phase 7;
  contributors must understand both languages through most of the project.
- New risk: transition bugs at the C/Rust boundary (FFI, Corrosion link order, CRT
  selection) — mitigated by the Phase 0 safety net (goldens, ABI test, smoke test,
  benchmarks) running in CI for the whole migration.
- Revisit: this ADR reopens if (a) a phase's exit criteria stall for two
  consecutive release cycles, (b) the Phase 0 prototypes show `wgpu` cannot import
  platform shared textures (reopens ADR 0003's fallback path instead), or
  (c) the C ABI maintenance cost per release exceeds the per-phase porting cost —
  at which point the native Rust plugin API (ADR 0002) accelerates rather than the
  C core retreating.

## Links

- Plan: `docs/rust-rewrite-plan.md` (strategy: §2, roadmap: §4, rules: §5)
- Phase files: `docs/rust-rewrite/phase-0-foundation.md` … `phase-7-retire-c.md`
  (exit criteria quoted above; sources of the table)
- Decisions: D-01…D-39 in `docs/rust-rewrite/DECISIONS-NEEDED.md` (all confirmed 2026-11-05/06)
- ADR 0002 (C plugin ABI policy), ADR 0003 (graphics backend), ADR 0006 (concurrency)
- Tasks: P0-01.1–P0-01.5 (`docs/rust-rewrite/subtasks/phase-0-subtasks.md`)
