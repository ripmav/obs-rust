# Tenets — obs-rust (Rust rewrite)

Design principles that override convenience. When a change seems to contradict a
tenet, stop and ask.

______________________________________________________________________

## T1 — C and Rust coexist; every phase ships a working OBS.

Strangler-fig migration, not big-bang: a C source is only deleted after its Rust replacement
is proven (vectors, goldens, ABI, benchmarks) and the CMake swap (D3 patch) excludes it.
A change that breaks the all-C build (`OBS_ENABLE_RUST=OFF`) breaks T1.
[ADR 0001](adr/0001-strangler-migration.md).

## T2 — The C plugin ABI is a hard contract until Phase 7.

Unmodified C plugins (41 in-tree, hundreds third-party) must load and run through
[ADR 0002](adr/0002-plugin-abi.md)'s compat path. ABI changes go through the recorded
deprecation path only (D-02/D-05); a change that alters an exported symbol, struct layout,
or vtable without an ADR breaks T2.

## T3 — Specs are the source of truth, C is the reference.

Ports implement the clean-room spec (S-xxx), not "read the C" — the C code supplies
conformance vectors, not requirements. When spec and C disagree, the spec wins and the
difference is recorded (known-C-bug list, quirks). A change that copies a C defect to make
a vector pass breaks T3.

## T4 — Parity is proven, not asserted.

"Done" means covered by a conformance vector, golden, swap run or ABI test — never by code
alone. Subtask boxes tick only for what actually passed (the 2026-11-21 audit's lesson).
A tick without evidence breaks T4.

## T5 — `unsafe` lives in named places and every block says why.

`unsafe` is confined to the [ADR 0005](adr/0005-code-policy.md) exception sets (C-ABI layout
modules, platform-tier `libc` modules, FFI surface); every `unsafe` block carries a
`// SAFETY:` comment (lint-enforced via `undocumented_unsafe_blocks`, deny in CI);
`unsafe_op_in_unsafe_fn` is denied workspace-wide.

## T6 — The real-time hot path stays allocation- and lock-free.

No allocation, no blocking lock, no syscall on the audio/video hot path (plan §3.1,
enforced by the P2-12 harness once it lands). A change that adds a `Vec` allocation,
a `Mutex` or a `println!` into the audio callback breaks T6.

## T7 — Concurrency follows the dedicated-RT-thread model.

Dedicated video/audio mixing threads as today; `crossbeam` channels / lock-free ring
buffers on the hot path; `tokio` only for network outputs and services.
[ADR 0006](adr/0006-concurrency.md).

## T8 — wgpu is the single graphics backend.

OpenGL stays C as the non-wgpu fallback (D-13); D3D11/Metal are ported to Rust
(S-078/S-079) and retired in Phase 7 — not retained in parallel. A second new backend
breaks T8.

## T9 — Vendor SDKs and native libraries are permanent FFI inputs.

FFmpeg, CEF, NVENC/AMF/QSV/VideoToolbox, libfdk, libvlc, DeckLink/AJA/VST: the glue
around them is Rust (hand-written FFI where no crate exists); the libraries themselves are
never "rewritten" and never counted as rewrite progress (D-09/D-10/D-11, D-15…D-17).

## T10 — Licence policy is GPL-2.0-or-later plus an allow-list.

Enforced by `cargo-deny` in CI (P0-10). A new dependency that is not GPL-2.0-or-later
compatible breaks T10 unless it is explicitly allow-listed in the deny config with a reason.

## T11 — Parity evidence is pinned to obs-studio 33.0.1.

Conformance vectors, goldens and swap runs compare against the 33.0.1 tree (drift evidence:
`modservice/24-register-other-kinds`). Regenerating vectors against another tree requires an
ODD entry and a note in the affected area's exclusion list.
