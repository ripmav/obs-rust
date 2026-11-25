# 0002. C plugin ABI policy

- Status: Accepted (D-02, D-05, D-06 confirmed 2026-11-05; policy text per P0-02). Typed Rust signal API per Amendment 1 (2026-11-25, P1-23).
- Date: 2026-11-10
- Supersedes: —
- Superseded by: —

## Context

The plugin ABI is the migration's hardest constraint (ADR 0001): 41 in-tree
modules and hundreds of third-party plugins are shared libraries exporting
`obs_module_load`/`obs_module_unload` via `OBS_DECLARE_MODULE`
(`libobs/obs-module.h`, 201 lines) and registering vtables
(`struct obs_source_info`, `obs_encoder_info`, `obs_output_info`,
`obs_service_info`) through `obs_register_source` and friends.

Two mechanics make the ABI tolerant of growth and fragile to change:

- **Struct size tolerance.** `obs_register_source` is a macro over
  `obs_register_source_s(info, sizeof(struct obs_source_info))`
  (`libobs/obs-source.h:568`, defined `libobs/obs-module.c:1035`): libobs
  zero-initialises the internal struct and `memcpy`s exactly the caller's
  `size` bytes. A caller struct **larger** than libobs's is rejected with a
  warning — so **appending fields at the end keeps old plugins working**.
  The ABI conformance test (P0-15) includes a plugin compiled against a
  smaller struct to pin this.
- **Versioning.** `LIBOBS_API_MAJOR_VER/MINOR/PATCH` in `libobs/obs-config.h`
  (52 lines). Modules built against older minor versions work as long as the
  layout they copied is untouched.

Breaking the ABI breaks the ecosystem — OBS's biggest asset. The confirmed
answer is therefore best-effort, not frozen (D-02):

> **Confirmed (user, 2026-11-05): Best-effort.** `obs-ffi` still serves the C ABI (`obs.h` & friends) from Phase 1 onward and compatibility is actively maintained, but it is not a frozen hard requirement: breakage is allowed only with a deprecation path and migration docs (the D-05 timeline governs removal).

## Decision

### Policy

1. `obs-ffi` serves the C ABI from Phase 1 onward; compatibility is
   **actively maintained**, not fossilised.
1. **Breakage is allowed only with a deprecation path**: announcement +
   migration docs at the time of the breaking change, removal governed by
   D-05 ("announce with SDK 1.0, remove after 3 Rust releases"; P7-10.3 names
   the exact release in this ADR at that time).
1. Every change to a compat-tracked header requires an ADR
   (`CONTRIBUTING.md`, "Public header changes"); `CODEOWNERS` routes such PRs.
1. The compatibility contract is proven mechanically: the ABI conformance
   plugin (P0-15) + golden traces run in CI from Phase 0; symbol/layout
   tests from Phase 4.

### Compat-tracked header list (libobs 33.0.1 snapshot)

The compat-tracked ABI is **the install set of `libobs/CMakeLists.txt`**
(396 lines). Snapshot measured in the read-only reference checkout (per
`docs/rust-rewrite/subtasks/phase-0-subtasks.md` P0-02.1, line counts in
parentheses), grouped by directory. **Public** = installed and consumed by
plugins/frontend; **internal** = not part of the plugin contract
(classification provisional — see verification note).

**`libobs/` top level (public):** `obs-module.h` (201), `obs-source.h` (572),
`obs-output.h` (96), `obs-encoder.h` (365), `obs-service.h` (115),
`obs-data.h` (314), `obs-properties.h` (364), `obs-hotkey.h` (272),
`obs-hotkeys.h` (653), `obs-interaction.h` (56), `obs-audio-controls.h` (250),
`obs-nal.h` (37), `obs-avc.h` (55), `obs-hevc.h` (81), `obs-av1.h` (47)

**`libobs/graphics/` (21):** public — `graphics.h` (1025), `vec2.h` (148),
`vec3.h` (224), `vec4.h` (241), `matrix3.h` (98), `matrix4.h` (102), `quat.h`
(170), `axisang.h` (65), `plane.h` (85), `bounds.h` (108), `half.h` (100),
`image-file.h` (58), `srgb.h` (177); internal — `graphics-internal.h` (335),
`device-exports.h` (177), `effect-parser.h` (290), `shader-parser.h` (273),
`math-defs.h` (45), `math-extra.h` (61), `input.h` (34)

**`libobs/graphics/libnsgif/` (internal):** `lzw.h` (137), `nsgif.h` (527)

**`libobs/media-io/` (10):** public — `video-io.h` (338), `audio-io.h` (228),
`video-frame.h` (64), `format-conversion.h` (50), `audio-resampler.h` (44),
`audio-math.h` (43), `frame-rate.h` (29); internal — `media-io-defs.h` (20),
`media-remux.h` (37), `video-scaler.h` (43)

**`libobs/util/` (30):** public — `platform.h` (224), `bmem.h` (94),
`dstr.h` (320), `darray.h` (606), `deque.h` (319), `threading.h` (103),
`config-file.h` (103), `serializer.h` (158), `file-serializer.h` (34),
`buffered-file-serializer.h` (32), `array-serializer.h` (37), `bitstream.h`
(28), `crc32.h` (29), `utf8.h` (35), `text-lookup.h` (45), `pipe.h` (51),
`profiler.h` (97), `task.h` (22); internal — `base.h` (97), `c99defs.h` (79),
`cf-lexer.h` (199), `cf-parser.h` (281), `lexer.h` (273), `sse-intrin.h` (41),
`source-profiler.h` (66), `threading-posix.h` (77), `threading-windows.h`
(142), `uthash.h` (34, vendored), `util_uint64.h` (34), `util_uint128.h` (108)

**`libobs/callback/` (public):** `calldata.h` (195), `decl.h` (61), `proc.h`
(52), `signal.h` (73)

Re-derivation script (run in the reference checkout linked via
`tools/link-obs-source.sh`):

```bash
git ls-files 'libobs/*.h' 'libobs/graphics/*.h' 'libobs/media-io/*.h' \
  'libobs/util/*.h' 'libobs/callback/*.h' | xargs wc -l | sort -k2
# then diff against the install(FILES ...) blocks in libobs/CMakeLists.txt
```

**Verification note (open):** the subtask snapshot above is the measured
source set, but (a) it does not list `obs.h` or `obs-config.h` — the script
re-run must confirm whether the CMake install set includes them, and
(b) the public/internal classification is provisional pending that re-run.
Both close when the reference checkout is linked (feature doc L3).

### Versioning rules (`LIBOBS_API_VER`)

- **MINOR bump** — additive, non-breaking: appended struct field, new
  function, new enum constant with a fresh value.
- **MAJOR bump** — breaking: struct layout change (insertion/removal/
  reordering of fields), removed symbol, changed callback signature,
  changed enum constant value.
- Every MAJOR bump ships the D-05 deprecation path (announcement, migration
  docs, scheduled removal release).

Worked examples (classified):

| Change                                        | Breaking? | Bump  | Why                                                                                                                                    |
| --------------------------------------------- | --------- | ----- | -------------------------------------------------------------------------------------------------------------------------------------- |
| Add field at end of `struct obs_source_info`  | No        | MINOR | loader `memcpy`s only the caller's `sizeof` — old (smaller) plugin structs keep working; this is the mechanic P0-15 pins               |
| Remove `obs_source_get_active`                | Yes       | MAJOR | removed symbol; any plugin calling it fails to load — deprecation path mandatory                                                       |
| Change value of an `OBS_SOURCE_TYPE` constant | Yes       | MAJOR | observable value change; plugins comparing the numeric value misbehave even though the header regenerates — deprecation path mandatory |

### Coexistence with the native Rust plugin API (D-06)

Confirmed: **native `cdylib` + `#[obs_module]` proc-macro**; WASM stays
research (P7-12). How a module declares its API:

- **C module** (existing): exports `obs_module_load`/`obs_module_unload`
  (`OBS_DECLARE_MODULE`).
- **Rust module** (new): `cdylib` exporting the proc-macro-generated
  `obs_rust_module_init`/`obs_rust_module_unload`; registers through the typed
  Rust API of `obs-plugin-api` (no vtable, no `memcpy` size dance).

Loader order (implemented in Phase 4's module loader, `libloading`; the three
platform C loaders merge into one): for each module path the loader probes the
C entry symbols first (compat path), then the Rust entry symbols. For in-tree
modules with the same id, the native Rust registration wins; the C compat
registration is the fallback until the Rust module is marked stable.

Sequence — C module:

```
host               loader (obs-module)                 C module .so/.dll
 |--open_module-->| os_dlopen(path) ------------------>|
 |                | dlsym "obs_module_load" <----------|
 |--init_module->| call obs_module_load() ------------>|
 |                |                                     | registers
 |                |<-- obs_register_source/encoder/... --|  vtables; typed
 |                | store vtable, issue typed id <-------| IDs to C ABI
```

Sequence — Rust module:

```
host               loader (obs-module)                 Rust cdylib
 |--open_module-->| dlopen(path); probe entries -------|
 |                | dlsym "obs_rust_module_init" <-----|
 |--init_module->| call init(ObsModuleCtx) ----------->|
 |                |                                     | #[obs_module]
 |                |<-- typed registrations ------------| (no vtable copy)
```

Both diagrams refine when the Phase 4 module-loader task lands; they fix the
contract (two entry families, one loader, probe order), not the internals.

Deprecation timeline placeholder: C ABI compatibility is announced as
deprecated at native SDK 1.0 (P7-11) and removed **three Rust releases later**
(P7-10.3 names the release; D-05).

### Amendment 1 (2026-11-25) — typed Rust signal API: `Signal<A>` + `Connection`

**Trigger:** P1-23 (WP-33) delivers the typed Rust API over the S-004 signal
handler in `crates/obs-signal` (`typed` module) — the first slice of the native
Rust API announced in §"Coexistence with the native Rust plugin API (D-06)".
No C-ABI-visible change: the amendment fixes the Rust-side contract only.

**Decision:**

1. **`Signal<'h, A>`** — zero-allocation handle: a borrow of `SignalHandler`
   plus the argument type `A: SignalArgs` (name, S-003 declaration and the
   calldata layout live in `A`). `Copy`.
1. **`Connection<'h, A>`** — the RAII guard for `Signal::connect( f: impl Fn(A) + Send + Sync + 'static)`. **Drop is disconnect** (no manual
   disconnect API): the drop calls `signal_handler_disconnect` and *then* frees
   the closure — so no C-side connection leaks (T2) and no closure box leaks.
   Dropping a guard from inside a callback for **another** connection (or from
   another thread) is well-defined (S-004-R30/R45: marked during the emit,
   erased at its end, skipped by any nested re-emit). **A guard must not be
   dropped from inside its own connected callback** — the drop would free the
   closure object while it is being called (undefined behavior; the trampoline
   borrows the closure for the call — unlike C, where the callback code is
   separate from the `data` payload; caught by the CI Miri job in WP-33). To
   disconnect oneself: `signal_handler_remove_current()` (S-004-R32) or defer
   the drop. A guard may outlive the handler's destruction (`refs` →
   0): its disconnect is a documented no-op on a dead handler.
1. **Typed arguments use the S-002 byte layout verbatim** (`A::write` /
   `A::read` over the calldata stack): the stack bytes are identical to what a
   C plugin packs; a missing or mis-sized parameter takes the C default
   (int `0`, float `0.0`, bool `false`, ptr `NULL`, string `NULL`) — no
   fallible read, matching the `calldata_int`-family contract.
1. **`Signal` and `Connection` are always `Send + Sync`** (the handles hold
   no `A` data; `A`'s thread-safety requirements fall on the closure bound and
   on each `emit`'s `&A` argument). This required `unsafe impl Sync for SignalHandler` (all shared state mutex-guarded per S-004 §7; SAFETY comment
   at the impl). `SignalHandler` is **not** `Send` — its `data` payloads are
   opaque C memory (raw pointers are not `Send` on the current toolchain) —
   so the `Box<SignalHandler>` stays on its creating thread and plugin code
   shares `Signal`/`Connection` handles instead.
1. **Allocation behaviour (T6):** `connect` costs two small heap allocations
   (the closure box plus the outer identity box that keeps the C `data` token
   a thin pointer — freed with the guard, never on the emit path); `emit` is
   one S-001-allocator round trip (≥128 B, S-002-R11), freed at emit end —
   the same profile as a C caller's `calldata_create()`/`calldata_destroy()`;
   `emit_on` reuses a caller-owned calldata (zero allocations after the first
   write — the real-time path); each callback invocation reconstructs `A`
   (a `string` parameter allocates a `String` per callback, where C plugins
   borrow a NUL-terminated pointer).
1. **Panic policy:** a panic in a connected closure **aborts** the process —
   the trampoline is `extern "C"`, and unwinding through it would corrupt the
   emit state (same class as the C reference, which cannot unwind at all;
   S-001 "no unwinding into C").

**Caller contract (inherited from the C reference, S-004):** do not destroy a
handler (`refs` → 0) while a `connect` of it is in flight from another thread.
Everything else the guards make safe: a handler may be destroyed while guards
are alive, as long as no emit of that handler is running at that moment.

**Rejected alternatives:**

- `Signal<A>` owning the handler (`Arc`): rejected — (a) it would hide the
  destroy ordering behind a refcount and diverge from the C
  `signal_t { handler, name }` borrow shape; (b) `SignalHandler` is not `Send`
  on the current toolchain, so the `Arc` would need an `unsafe impl Send`
  over opaque C `data` payloads with no evidence to support it.
- manual `Connection::disconnect()` in addition to `Drop`: rejected as
  redundant — drop is the only required path, and a reconnect is a fresh
  guard (the C identity is (trampoline, box pointer), so no accidental
  dedupe).
- fallible `A::read` (`Result<Self, _>`): rejected — (a) the C contract is
  silent defaults (S-002), and a `Result` in the hot callback path adds
  branching with no C counterpart; (b) a missing parameter is a *value*
  condition in calldata (type is only the byte count, Q-1), not an error.

**Consequence added:** the `typed` module's module docs (drop-ordering proof,
allocation behaviour, panic policy) are the standing reference for P1-23 and
for later typed APIs built on the handler (procs, P1-21's `Proc` handle).

## Alternatives

1. **Frozen hard ABI** ("never break, ever").
   Rejected: (a) a frozen C ABI stops the core's object model from evolving
   (Phase 4's `Arc` handles + typed IDs) — every ergonomic fix in the C
   surface becomes a permanent tax; (b) it contradicts the confirmed
   best-effort answer (D-02): the ecosystem is protected by the deprecation
   path, not by immutability.
1. **Drop C ABI after one release.**
   Rejected: (a) obs-websocket/obs-browser/capture plugins are the reason
   people use OBS; cutting them after one release breaks the product for the
   power-user base; (b) the D-05 timeline (3 releases) is the confirmed floor
   and already generous to the rewrite schedule.
1. **ABI via `abi_stable`/WASM only, no C compat.**
   Rejected: (a) D-06 confirmed cdylib + proc-macro for the native path with
   WASM explicitly kept as research (P7-12) — adopting it as the *only* path
   would strand every existing C plugin; (b) `abi_stable` adds a runtime
   dependency and codegen surface the C plugins don't need.

## Consequences

- Easier: the rewrite can change internals freely as long as the install-set
  surface holds; new plugins get a typed Rust path from Phase 5 on.
- Harder: every compat-tracked header change is ADR-gated (review overhead);
  the provisional public/internal split must be re-verified (verification
  note) before Phase 4 generates the header set from Rust.
- Revisit: if third-party plugin breakage reports exceed one per release
  during Phase 4–5, the deprecation timeline tightens (shorter announcement
  windows) rather than the policy loosening; if the native API proves out
  early, P7-11/10.3 dates move earlier.

## Links

- D-02, D-05, D-06 (`docs/rust-rewrite/DECISIONS-NEEDED.md`);
  `docs/rust-rewrite-plan.md` §3.4
- P0-02.1–P0-02.4 (`docs/rust-rewrite/subtasks/phase-0-subtasks.md`);
  P0-15 (ABI conformance plugin); P4-34…P4-37 (obs-ffi surface); P7-10/P7-11
  (deprecation/removal)
- `libobs/obs-module.h` (201), `libobs/obs-module.c` (1274: `obs_open_module`/
  `obs_init_module`), `libobs/obs-source.h:568`, `libobs/obs-config.h` (52)
- `CONTRIBUTING.md` ("Public header changes"), `CODEOWNERS`
