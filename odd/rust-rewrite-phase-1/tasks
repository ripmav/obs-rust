# Feature: Rust Rewrite — Phase 1 (Leaf Libraries)

Stack: branches off the Phase 0 head `docs/rewrite-coverage-2026-11` (PR #10); rebase onto `main` once #10 merges. Each WP = own branch + PR; small work packages = commits.

## Specs

- S1 (user, 2026-11-11, verbatim, German): “Arbeite autonom die Roadmap ab. Jeder Milestone wird ein Stack, wenn möglich mit main als base, und jedes WP wird ein PR im Stack. Jedes WP soll in möglichst kleine Arbeitspakete aufgeteilt werden welche dann, abgearbeitet, als commit im jeweiligen Branch/PR commited und gepushed werden.”
- S2 (state): no C checkout on this machine (`../obs-studio` absent). Per-WP C-side substeps — cbindgen header diff vs committed `libobs/*.h`, `OBS_ENABLE_RUST` build switch, fuzz/golden comparison *against C*, P1-29/30/31 — are recorded on each WP and tick ONLY after `tools/link-obs-source.sh` + CMake env. Spec-driven implementation and spec-vector tests proceed in this repo (strangler pattern: Rust crate + safe API + tests now; C swap later).
- S3 (phase pattern, verbatim from `docs/rust-rewrite/phase-1-leaf-libraries.md`): “(1) Rust crate with a safe API, (2) `extern "C"` shim with the exact C signature, (3) `cbindgen` check that the generated header is compatible with the existing one, (4) goldens from P0-14 pass, (5) C file kept but excluded from the build when `OBS_ENABLE_RUST=ON`.”
- S4 (WP list, verbatim short titles from the phase file): P1-01 bmem (S-001); P1-02 dstr (S-006); P1-03 darray (S-005); P1-04 deque (S-005); P1-05 threading; P1-06 time/sleep; P1-07 filesystem; P1-08 text/lexers (S-010/S-011); P1-09 config-file (S-007); P1-10 base64/crc32/blake2; P1-11 process/module; P1-12 profiler (S-024 M7); P1-13 logging; P1-14 misc util; P1-15 vec2/3/4 (S-008); P1-16 matrix3/4 (S-008); P1-17 quat/axisang/plane/bounds (S-008/S-009); P1-18 math-extra/half/srgb (S-009); P1-19 SIMD; P1-20 calldata (S-002); P1-21 decl/proc (S-003); P1-22 signal (S-004); P1-23 typed Rust signals; P1-24 JSON engine (S-014); P1-25 obs_data_t core (S-014); P1-26 autoselect/ownership (S-014); P1-27 obs_properties (S-015); P1-28 hotkey data layer (S-034); P1-33 shared/ helpers (S-024 M8/M10/M9/M1); P1-29 header diff check (C); P1-30 default switch (C); P1-31 vendored deps scan (C); P1-32 phase review.
- S5 (dependency policy): ADR 0005 — stdlib first; new deps need allow-list license + recorded exception. Currently planned additions: `crc32fast` (MIT, P1-10), `blake2` (MIT, P1-10), `half` (MIT/Apache-2.0, P1-18). Each gets a cargo-deny green check in its WP.

## WPs

- WP-1 = P1-01 bmem — spec S-001 — branch `p1/bmem` — C-blocked: alloc-counter golden vs C, cbindgen diff, OBS_ENABLE_RUST.
- WP-2 = P1-04 deque — spec S-005 — branch `p1/deque` — C-blocked: wraparound property vs C, layout check vs C header. Done spec-side 2026-11-11 (see L5).
- WP-3 = P1-03 darray — spec S-005 — branch `p1/darray` — C-blocked: layout vs C header, OBS_ENABLE_RUST. Done spec-side 2026-11-11 (see L6).
- WP-4 = P1-10 crc32/blake2 — known-answer tests — branch `p1/crc-hash` — C-blocked: none beyond usage scan (deps are frontend-only per phase file). Done spec-side 2026-11-11 (see L7).
- WP-5 = P1-15/P1-16 vec + matrix4 — spec S-008 — branch `p1/vec-matrix` — C-blocked: 10k goldens vs C, size/align vs C headers, benchmark.
- WP-6+ = dstr (S-006), calldata (S-002), decl (S-003), signal (S-004), math-rest (S-009), config-file (S-007), obs-data (S-014) — branches `p1/<name>` as capacity allows. WP-6 = P1-02 dstr done spec-side 2026-11-11 (see L9). WP-7 = P1-06 calldata done spec-side 2026-11-11 (see L10). WP-8 = P1-07 decl done spec-side 2026-11-11 (see L11). WP-9 = P1-22 signal (S-004) done spec-side 2026-11-11 (see L13). WP-10 = P1-15/16/17/18 math-rest (S-009) done spec-side 2026-11-11 (see L14). WP-11 = P1-09 config-file (S-007) done spec-side 2026-11-11 (see L15). WP-12 = P1-24/P1-25 obs-data (S-014) done spec-side 2026-11-12 (see L16). WP-13 = P1-27 obs-properties (S-015) done spec-side 2026-11-12 (see L17). WP-14 = P1-28.1 hotkey key table (S-024 M4) done spec-side 2026-11-12 (see L18). WP-15 = P1-11 process + module loading (S-055 U2, S-024 M3, S-023 R107/R82) done spec-side 2026-11-12 (see L19). WP-16 = P1-14.3 task queue + 128-bit helpers (S-024 M2/M6) done spec-side 2026-11-12 (see L20). Next: remaining spec-side Phase 1 WPs as capacity allows (S-034 registry itself is Phase 4).
- C-tail WPs = P1-29/30/31/32 — blocked on C checkout + two-week nightly soak.

**L11 (WP-8, 2026-11-11) — P1-07 decl (S-003) — branch `p1/decl` (off `p1/calldata`), PR #19.**
`crates/obs-signal/src/decl.rs`: `CallParamType` (repr u32 = C enum int,
5W-struct), `DeclParam` 16B, `DeclInfo` 5W, `parse_decl` (safe API) +
`parse_decl_string` (C-facade, R01 borrow, R10 reset, R11 warning),
`decl_info_free`/`decl_param_free`. Handwritten `&[u8]` scanner
(bytes ≥ 0x80 = unknown tokens, no UTF-8), grammar per spec §4,
parameter-list loop per §5.1. R01–R12 + all quirks: Q-1 (leading WS
fails — first token never WS-skipped), Q-2 (trailing junk ignored),
Q-3 (silent stop), Q-4 (trailing comma), Q-5 (empty string: `false`
+ no log), D-1 (reference crash on non-NAME function name → fail +
log, never crash; the crash class is documented in the module).
Bugs found in own draft (fixed before commit): (1) the first
`list`-loop version could only `break` (clippy `never_loop`) and the
Q-3 arm re-entered the list loop — restructured to entry match +
after-param loop; (2) the after-param loop ran for the **empty**
list and reported "Unexpected EOF" on `void f()` — the loop now
lives inside the NAME arm; (3) `fail()` test helper had a type error
(`unwrap_err` on `Decl`); (4) layout expectation wrong: C
`decl_param` is 16 bytes (enum @8, flags @12, no padding), not 3W.
Evidence: obs-signal lib 28/28 (12 decl tests), Miri 28/28 (6.5s),
workspace 21 binaries green, clippy 0, deny ok. C-side: 9 conformance
scripts + cbindgen diff blocked on C checkout.

**L12 (Incident, 2026-11-11) — uncommitted working-tree leftovers broke the committed tree.**
While committing WP-8, two working-tree changes turned out to be
uncommitted fixes from earlier WPs: (1) `deque.rs` — the removal of the
parallel `bnum_allocs()` counter test (WP-3 fix, helper
`__test_serialize_counter` was removed from bmem in 2ca4270, so the
committed lib test target **did not compile** on any branch from
`p1/darray` through `p1/decl`, i.e. PRs #14–#19 on fresh checkouts);
(2) `Cargo.lock` — the obs-signal→obs-util dependency from WP-7.
Fix: `a7d13f5` on `p1/darray` (PR #14); cherry-picked onto
`p1/crc-hash` (1848746, #15), `p1/vec-matrix` (e76f940, #16),
`p1/dstr` (40b1a5c, #17), `p1/calldata` (e32ccff, #18), `p1/decl`
(4eccf41, #19); lock committed on `p1/calldata` (694d045, #18).
No force-push (safety guard blocked it): each fix landed as a
fast-forward cherry-pick; all PR diffs stay clean (fix present in
both base and head cancels in the tree diff). Lesson recorded:
**verify `git status` is clean before a WP commit is declared done**
— gates run on the working tree and can mask uncommitted fixes.

**L13 (WP-9, 2026-11-11) — P1-22 signal + procs (S-004) — branch `p1/signal` (off `p1/decl`), PR #20.**
`crates/obs-signal/src/signal.rs` + `src/proc.rs`: the full S-004 surface —
handler create/destroy with refs (R02, R22, R30), add/add_array (R10/R11 +
header inline), connect/disconnect/connect_ref (R20–R22, R30), emit
(R40–R45: index loop re-reading the length, skip-marked, erase-after,
global pass under the recursive global lock, keep_ref subtraction R41.6),
`signal_handler_remove_current` (Q-2), globals (R23–R26, Q-5), procs
(R60–R63). Locks: handler mutex, per-signal recursive mutex, recursive
global mutex (parking_lot). `SignalHandler` = opaque `Box<Inner>`; both
lists are `UnsafeCell<Vec<Box<…>>>` (entry allocations stable across
pushes → Q-1 mid-emit appends reachable; raw `*mut Signal` valid while the
handler lock is released, S-004 §7). Three safe Rust deviations documented:
deferred free of a dead handler until the in-flight emit unwinds;
remove-current outside a callback is a no-op; connect_ref on a missing
signal does not leak a ref. **Miri findings (both fixed):** (1)
`addr_of!` + `cast_mut` + deref is only SharedReadOnly under Stacked
Borrows — lists moved to `UnsafeCell`; (2) a live `&mut` held across a
callback is invalidated by the re-entrant `remove_current`'s own `&mut`
— every list access now derives a short-lived reference per helper call
(`cb_*`/`g_*`), none across a callback. **Miri limitation (documented):**
Miri gives every function-item coercion a different address (verified:
same item → 0x212b7 vs 0x212c2), so the 8 vectors whose semantics need
C's fn-pointer-identity dedupe are `#[cfg(not(miri))]` (they run on real
hardware; C conformance covers them). Tests: dedicated sequential binary
`tests/signal_proc.rs` (23 vectors in one test, own process). Evidence:
obs-signal lib 28/28 + signal_proc 1/1 (real hw, all 23 vectors), **Miri
28/28 lib + signal_proc green** (15 vectors), workspace 166 passed / 0
failed, clippy 0, deny ok. New dep per ADR 0005 log: `parking_lot` 0.13.x
(MIT OR Apache-2.0) — reentrant mutexes, allow-list D-07. Matrix:
`docs/tests/signal.md` (23 rows). NOT ticked: P1-22.3 loom check,
P1-22.4 recipe (C checkout), P1-23 typed API (own substep). Review status:
see PR body.

**L22 (2026-11-12) — re-stack of all Phase 1 branches onto main.** PR #10 merged (main = `ccca683` merge of `a32c7ef` + rewritten docs `accfe83`); the 17 working branches still rested on the docs tip, so GitHub PRs against main would have shown `renovate.json` as deleted. Rebased in stack order with `--committer-date-is-author-date --onto` (ci/rust-phase-0 and p1/bmem onto main, each chained branch onto its predecessor's new tip); verified per branch: merge-base == main tip, WP commit count preserved, `renovate.json` present in tip tree, only the two corrected trailer variants remain. Force-pushed 17/17; created 17 replacement PRs (#29-#45) with stack bases (#29 on main, each other PR on its predecessor's branch); the original PRs (#11-#28) remain closed - GitHub refuses re-open once the head branch no longer contains the pinned head commit - their bodies are refreshed and cross-linked to the replacements. PR-body SHA quotes refreshed to the post-rebase SHAs. New tips: ci/rust-phase-0 `e8f9eb0`, p1/bmem `085b300`, p1/deque `914a711`, p1/darray `53170fc`, p1/crc-hash `bf418bc`, p1/vec-matrix `06a2fd2`, p1/dstr `66c7fd1`, p1/calldata `6dca2b8`, p1/decl `3063b8c`, p1/signal `3091776`, p1/math-rest `050c0dd`, p1/config-file `a31d82e`, p1/obs-data `c182be6`, p1/properties `bb05033`, p1/hotkey-names `3021f04`, p1/process `258cbb9`, p1/uint128-task `f5f6693`.
**L21 (2026-11-12) — Assisted-by trailer rewrite (user directive).** All 123 AI-assisted commits across the 18 working branches carried the wrong trailer: 85 Pi commits used the raw model id without effort (`Assisted-by: qwen3.8-27b via pi`) and the 38 OpenCode commits from the spec/docs work (11/01-06, checkout `~/github/obs-rust`) lacked the effort part (`Assisted-by: Qwen 3.8 27B via OpenCode`). Established convention (fixed the same way in streamcrew): `Assisted-by: <model name> (<effort>) via <harness>`. Rewritten (message-only, verified: trees, author, committer and both dates identical per branch; 18/18 branches): Pi → `Assisted-by: Qwen 3.8 27B (off) via Pi` (both Pi sessions ran `thinkingLevel: off`), OpenCode → `Assisted-by: Qwen 3.8 27B (default) via OpenCode` (model variant `default`, no effort override). All 123 SHAs changed — every SHA quoted in this file, the WP logs above, PR bodies and the review records now points at the pre-rewrite commit; content is unchanged. Parked review lineages stay content-valid (trees unchanged); their quoted bases are old SHAs. From now on: always the format above.
Re-root correction (same day): the initial `filter-branch` pass also rewrote the two trailer-free root commits (`172e54a`/`0b970de` "Adds plan and tests for rustup OBS"), which detached the rewritten histories from `main` (PR #10 had merged at `06:31Z`, before the force-push, so `main` still carried the pre-rewrite copies). All 18 branches were re-anchored onto the original `0b970de` (commit-tree rebuild, trees/messages/dates identical; per-branch tree sequences verified against the pre-rewrite history), and `main` was re-merged with identical author/committer/date and message (new merge commit `ccca683`, tree byte-identical to the original merge tree `8479eb4`). Final branch tips: docs/rewrite-coverage-2026-11 `accfe83`, ci/rust-phase-0 `56e2f53`, p1/bmem `dc1de9e`, p1/deque `80b3716`, p1/darray `24d17d0`, p1/crc-hash `a3c78a5`, p1/vec-matrix `742f324`, p1/dstr `2d951cb`, p1/calldata `2f84e9d`, p1/decl `1e938d9`, p1/signal `9bb8696`, p1/math-rest `696a697`, p1/config-file `6856c10`, p1/obs-data `dde47be`, p1/properties `57c31ef`, p1/hotkey-names `b758d0b`, p1/process `f6ee482`.

**L20 (WP-16, 2026-11-12) — P1-14.3 task queue + 128-bit helpers (S-024 M2/M6) — branch `p1/uint128-task` (off `p1/process`), PR #28.**
One commit: `acb30eb` new crate `crates/obs-util-small` (the S-024
crate split is a suggestion; M3 and the S-023/S-055 platform helpers
already landed in `obs-util` per the P1-11 subtasks) — module
`uint128`: `UtilUInt128` (16 bytes, LE i32 limbs, R10) +
`util_add128` (mod 2^128), `util_lshift64_internal_32/_64`,
`util_mul64_64`, `util_div128_32` (divisor 0 = D-1 precondition:
panic, no value) and `util_mul_div64` — exact `floor(n*m/d)` via a
u128 intermediate (R09/Q-2: exact wins over the reference's wrapping
formula); an exact quotient that does not fit in u64 saturates to
`u64::MAX` (documented choice). Module `task_queue`: `os_task_queue`
— one worker thread per queue, FIFO (R29); `wait` with the R31 marker
deferral (marker at the head with a task queued behind it is moved to
the back — a task queued from a running task still runs before
`wait()` returns) and R32 semantics (true iff a non-marker task is
taken off the queue while a wait marker is in the queue — a task
already in flight does not count); `destroy` defers its stop marker
behind every pending task and joins (R33); `inside` true only on the
queue's own worker (R34). C-NULL arguments unrepresentable (R30 maps
them in the FFI layer). Implementation note: the vectors'
`task_wait_release` = wait while the gate is released from another
thread — the tests reproduce that exactly (a gate task that is
already in flight when the wait starts does not count as processed;
verified flaky without the entered-synchronisation, pinned with it).
No new dependencies (std only). Evidence: `cargo test
-p obs-util-small` 15/15 (vectors u128_layout/u128-add/u128-mul64/
u128-div32/u128-shift/muldiv64/muldiv64-quirk-overflow/
u128-defect-div0 + task-order/task-requeue/task-wait-idle/
task-inside/task-destroy/task-null mirrored; stable ×20), **Miri
15/15** (real threads under Miri), workspace 378/0, clippy 0, fmt ok,
`cargo deny check` ok (no deps). Matrix: `docs/tests/util-small.md`
(21 rows). Tick policy: P1-14.3 ticked (Rust artifacts + tests);
**not** ticked: P1-14.1 (inventory, separate substep), P1-14.4
(curl) and the apply-recipe steps (C checkout). Review status: see
PR body.

**L19 (WP-15, 2026-11-12) — P1-11 process + module loading (S-055 U2, S-024 M3, S-023 R107/R82) — branch `p1/process` (off `p1/hotkey-names`), PR #27.**
Four commits: `3660bc2` `obs_util::process_args` — the S-024 M3
argument container (`create`/`add_arg`/`add_argf` with the pinned
printf subset `%d` `%i` `%s` `%x` `%X`, decimal width, zero pad, `%%`;
unsupported specifiers pass through verbatim) + `argc`/`argv`
(NULL-terminated view, count without it). R16 defect fixed: a format
expanding to the empty string stores `""` (C stored NULL). Q-3 (NULL
executable/arg stored as NULL elements) is unrepresentable in the safe
API — the FFI layer maps C-NULL to `""` (documented). `97f469b`
`obs_util::dl` — S-023 R107 `os_dlopen`/`os_dlsym`/`os_dlclose`
(`.so` append rule, `RTLD_NOW`); the R107 error log is not wired yet
(no logger in the crate) — the used file name is pinned through the
failed `dlerror` text instead. `3609c47` `obs_util::pipe` — S-055 U2
`os_process_pipe_create` (`/bin/sh -c cmd`) / `create2` (`argv[0]`
direct, no shell, no PATH lookup) / `read` + `read_err` (`fread`
semantics) / `write` / `destroy` (exit status as
`(int)(signed char)status`: 7→7, 200→-56, 255→-1, 128→-128; D-1
signalled child → the pid). Mode by first char of `type` (R04);
missing executable → `None` with no leaked descriptor (R06, pre-`access`
check); missing command → handle + 127 on destroy. `os_proc` —
S-023-R82 `os_get_executable_path_ptr` (`/proc/self/exe`). Two
documented deviations: Q-1 write buffering — the port writes directly
(delivery at least as early, same exit statuses), and the spawn
mechanism is fork + dup2 + execv instead of posix_spawn file actions
(this glibc 2.41's `posix_spawn_file_actions_adddup2` argument order
made file actions unusable — verified with a C probe; observable
behaviour R03-R09 identical). New dependency per ADR 0005 log:
`libc` 0.2.x (MIT OR Apache-2.0, allow-list D-07) — the platform FFI
of S-023/S-055; the `extern "C"` surface stays in `obs-ffi` (Rule 1).
All FFI tests `#[cfg_attr(miri, ignore)]`. Evidence:
`cargo test -p obs-util --lib` 133/133 (stable ×5, vectors
pipe_read/pipe_write/pipe_null_and_args + args-* mirrored + P1-11.3
edge cases), **Miri 111 + 22 ignored**, workspace 363/0, clippy 0,
fmt ok, `cargo deny check` ok. Matrix: `docs/tests/process.md`
(27 rows). Tick policy: P1-11.1, P1-11.2, P1-11.3 ticked (Rust
artifacts + tests); **not** ticked: P1-11.4 apply recipe (C checkout:
`extern "C"` shims, cbindgen diff, `OBS_ENABLE_RUST`), the 3-OS
roundtrip of the P1-11 "Done when" (Linux only here), R107 error-log
text (needs the logger). Review status: see PR body.

**L18 (WP-14, 2026-11-12) — P1-28.1 hotkey key table (S-024 M4) — branch `p1/hotkey-names` (off `p1/properties`), PR #26.**
One commit: `ed2912e` new crate `crates/obs-hotkey-names` — the
normative 636-entry key-name table (generated from the vector
`vectors/helpers/hotkeys-fallback-table.expected`, regenerated by
`tools/gen_hotkey_names.py`; pins the verbatim names
`OBS_KEY_0x07` (470) and `OBS_KEY_EDIAERESIS` (200); no
duplicates), `OBS_KEY_LAST_VALUE = 636`, `obs_key_to_name`
(>= 636 → `""` — the R19 ERROR log not wired yet, no logger in the
crate) and `obs_key_from_name` (exact case-sensitive match;
unknown/padded/empty/`OBS_KEY_LAST_VALUE` → 0, no error, R18).
Findings: (1) the C reference's two lookup paths (lazy hash map with
core, linear search without) have identical results — the port has
one, so R21 (`obs_hotkey_name_map_free`) has no equivalent
(documented deviation; S-024: "a port needs one"); (2) the whole
hotkey registry of S-034 is Phase 4 (P4-28) — P1-28.2 is vectors
only, P1-28.3/.4 stay C checkout (S2). Tick policy: P1-27→P1-28.1
ticked (table + both lookups complete, all key names equal to the
normative vector); **not** ticked: P1-28.2 (combo JSON — Phase 4
implementation), P1-28.3 (golden vs C), P1-28.4 (apply recipe).
Evidence: `cargo test -p obs-hotkey-names` 6/6 (the
table-vs-vector test is `#[cfg_attr(miri, ignore)]` under Miri
isolation), **Miri 5 + 1 ignored**, workspace 350/0, clippy 0, fmt
ok, deny ok (no dependencies). Matrix:
`docs/tests/hotkey-names.md`. Review status: see PR body.

**L17 (WP-13, 2026-11-12) — P1-27 obs-properties (S-015) — branch `p1/properties` (off `p1/obs-data`), PR #25.**
Three commits: `0422899` enums + model (ABI enums with `Unknown`
catch-all for out-of-range values; the property tree as
`Rc<RefCell<…>>` with `Weak` parent/owner links — no reference
cycles; container/property handles as `Rc`-cloning wrappers so
handles stay valid across removals (D4); R05/R06 param + destroy
order, R07 first/next, R08 get/parent, R09 duplicate rejection
(direct children only, Q-1), R10/R11 group add with self/
descendant/already-grouped rejection and the silent-NULL cycle
case (Q-2), `group_content`, `SettingsHandle` as the opaque
obs_data identity token via `From<&ObsData>` — the crate never
reads the data), `1ca6d3f` simple kinds + list + frame rate +
callbacks (R12–R26: bool/int/float/text/path/button/button2 with
description/long (empty == NULL, R15), visible/enabled (R17),
int/float limits + suffix, text defaults, path filters stored for
FILE/FILE_SAVE only (Q-1), url type (R18); R27–R33 list:
editable-combo refusal (R09 first), silent no-op returning 0 on
wrong format (Q-3), `idx > count` insert ignored (D-5), item
access per format, disable, clear; R36–R40 frame rate: options +
ranges, no validation (zero denominators stored), `(usize)-1` on
wrong kind; R16–R19 callbacks: the single priv slot shared by
button2 and modified2 (Q-4), v1 wins over v2 (Q-5), `modified` /
`button_clicked` get the TOP-most container, `apply_settings`
gets the applied container, reverse of the depth-first pre-gather
(Q-8), a property removed during the run is skipped (D-6),
callbacks taken out before the call so they may mutate (S-015
§9)). Findings: (1) the 25 conformance scripts are mirrored in
the test modules (v01–v25 names); the runner itself needs the C
checkout — P1-27.5/.6 stay unticked (S2); (2) the two logged
messages of R09/R27 are not emitted yet (no logger in the crate —
platform crate, S-023); the rejections are pinned; (3) Miri leak
check: test tokens moved from `Box::leak` to string-literal
addresses (binary, never freed — Miri-clean, pointer equality in
assertions); (4) the callback tests share recording statics and
run under a test lock (parallel test functions corrupted each
other). Tick policy: P1-27.1/.2/.3/.4 ticked; **not** ticked:
P1-27.5 (ABI test, extends the P0-15 plugin), P1-27.6 (apply
recipe, CMake + obs-ffi shim) — C checkout (S2). Evidence:
`cargo test -p obs-properties --lib` 28/28, **Miri 28/28**,
clippy 0, fmt ok, deny ok (depends on `obs-data` only, no new
external deps). Matrix: `docs/tests/obs-properties.md`. Review
status: see PR body.

**L16 (WP-12, 2026-11-12) — P1-24/P1-25 obs-data (S-014) — branch `p1/obs-data` (off `p1/config-file`), PR #24.**
Four commits: `a85a3a0` §6 JSON engine (jansson 2.15.1 reader +
writer, hand-rolled; `serde_json` rejected — it accepts duplicate
keys, `1e400`, unbounded depth and `\u0000`), `53f844e` model
(`ObsData`/`ObsDataArray`/`ObsDataItem` on `Rc<RefCell<…>>`:
clone=addref/drop=release, three value layers, byte-exact names and
strings, present-NULL obj/arr values R26, D1 refcount fix, D3
type-change releases mismatching layers, D4 handles survive growth
by construction, §6.1 size model with Q1 item-moves-to-end, item
handles R40–R43 incl. the detached walk, arrays R50–R53, `apply`
R32 + `get_defaults` R33), `8ffdbe5` typed API (3-layer
set/get ×7 kinds, zero values R20, R22/D5 number conversion,
has/unset R24/R30, erase/clear R31, vec/quat R60/R61 — the C vec3
has no `w` —, frames-per-second R65/R66 with clamp-to-`[0,
4294967295]` stored even on a false result, item forms R13),
`99199cf` JSON entry points + file helpers (get_json family R80–R83
with Q4 `last_json`, `create_from_json` with Q2/Q3, files R70–R72
on S-023 R57/R58/R60 + WP-11 `os_safe_replace`, Q6 backup
consumed even for a merely-missing primary). Findings: (1) the
runner's C reference (`obsdata_runner.c` + `libobs/obs-data.c` +
system jansson) needs the C checkout — P1-24.2/.3, P1-25.5/.6 stay
unticked (S2); (2) `obs-math` `Vec3` has a `w` lane the C `vec3`
does not — `get_vec3` never touches `out.w` (pinned); (3) a number
shell writes as DOUBLE `0.0` (R82/§6.2), a shell item in
`get_defaults` creates no member (R33); (4) invalid UTF-8 through
the safe API renders lossily (a `String` cannot carry raw bytes —
documented deviation, the FFI layer handles bytes); the C log lines
(LOG_ERROR on parse failure, LOG_WARNING on backup use) are not
wired yet (no logger in the crate). Tick policy: P1-24.1 (decision +
differences note), P1-25.1/.2/.3/.4 ticked (Rust artifacts complete,
tests from spec + existing behaviour); **not** ticked: P1-24.2
formatting diff vs C, P1-24.3 corpus round trip, P1-25.5 reference
dump on the corpus, P1-25.6 apply recipe (CMake exclusion,
obs-ffi shim, header diff) — all C checkout (S2). Evidence:
`cargo test -p obs-data --lib` 60/60, **Miri 57 + 3 fs ignored**,
workspace 22 targets ok, clippy 0, fmt ok, deny ok (reuses
`obs-util`, no new deps). Matrix: `docs/tests/obs-data.md`
(66 rows). Review status: see PR body.

**L15 (WP-11, 2026-11-11) — P1-09 config-file (S-007) — branch `p1/config-file` (off `p1/math-rest`).**
Four commits: `bc9f94b` platform helpers (S-023 R40/R41: `os_dtostr` D-2-fixed,
`os_strtod` — Rust has no `%g` and no hex-float `FromStr`, both implemented
from scratch and verified against a glibc `printf`/`strtod` probe),
`f2b726a` store core (model, INI parser per §4.1, open family,
R06/R10/R15), `14354ba` typed API (R07–R13: setters/getters,
`strtoll`/`strtoull` conversion semantics, `%g` six significant digits for
default doubles (Q-3)), `f257e32` save/`save_safe` (writer per §4.2,
Q-2 empty save, `os_safe_replace` S-023 R61 + Q-9 no-rollback,
`os_file_exists` R62). Findings: (1) **open question 2 decided**: the port
takes the D-2-fixed branch for `os_dtostr` (per P1-09.3 "the port MUST make
text and return value consistent"); the corrupted reference texts are pinned
as documented divergences (`1e21` → `1e21`, the C build writes `1e211`);
(2) **Q-1 lookup semantics**: "the last inserted entry" is scanned across
duplicate section records — a key defined in an earlier same-named section
stays findable (caught by `open_defaults_accumulates`); (3) **Miri:
`f64::powi` is inexact** (same as the WP-10 `f32` finding) — the hex-float
conversion uses exact bit-built powers of two; (4) invalid UTF-8 → U+FFFD
(spec uncharacterised; a Rust `String` cannot carry raw bytes — documented
deviation), NUL bytes pass through; (5) file-system tests are
`#[cfg_attr(miri, ignore)]` under Miri isolation, each with a unique temp
dir (a shared dir raced with `remove_dir_all` in parallel runs). Tick
policy: P1-09.2 (default-vs-user precedence tested) and P1-09.3 (power-loss
window: a kill after the temp write leaves the old target intact — the swap
happens only in the atomic rename) ticked; **not** ticked: P1-09.1
("10 real files parse" — C checkout), P1-09.4 (byte-identical corpus —
C checkout), P1-09.5 (apply recipe — C checkout). Evidence:
`cargo test -p obs-util --lib` 120/120, **Miri obs-util lib green**
(106 passed, 13 fs tests ignored), fmt ok, clippy 0, deny ok (no new
deps), workspace 257/0. Matrix: `docs/tests/config-file.md`. Review
status: see PR body.

**L14 (WP-10, 2026-11-11) — P1-15/16/17/18 math-rest (S-009) — branch `p1/math-rest` (off `p1/signal`).**
`obs-math` gains `plane`, `bounds`, `math_extra` (extended), `half`,
`srgb`; `vec2`/`vec3` get their S-009 surfaces; **`Matrix3` rebuilt to
the C 3×4 layout** (four `Vec3` rows `x,y,z,t`, 64 bytes, align 16 —
supersedes the WP-5 provisional 3×`Vec4`/48-byte reading; no external
consumers existed, only `matrix4_from_matrix3`, updated per R32: rows
keep their `w` lanes, `t.w = 1`). Commits: `c52b981` core (matrix3
R30–R41, vec2 R10–R15, vec3 R20–R25, plane struct R50, rand_float R96,
math-defs R01/R02), `3a862ef` plane R51–R58, `ea08acc` bounds R70–R81,
`a9b4665` math-extra R90–R95 + half R110–R113 + srgb R120–R125. Findings:
(1) **WP-5 Q-1 divergence fixed** — `vec2_norm` now leaves the
destination unwritten for zero/NaN length (S-009 Q-1; `vec3_norm` still
writes zeros); (2) vec2 `min/max` use explicit C-conditional comparisons
(not `f32::min`/`max`) — NaN returns the second operand (R12); (3)
**Miri `f32::powi` is not exact** (2⁻²⁵ → `0x32FFFFFC`) — the half
tie-boundary vectors use `f32::from_bits` inputs; (4) Q-10 verified on
representable values: `65504.0` (`0x477FE000`) stays `0x7BFF`, the
smallest f32 above it (`65504.00390625`) already overflows to inf;
(5) `calc_torque` Q-9 (`d == v1` corruption) is unreachable from Rust
(caller aliasing violation) — documented deviation, the port reads
`v1` fully before the store; (6) `gs_float_to_u8` deduped into
`srgb.rs` (single definition, C `srgb.h` `static inline`) — vec4.rs
uses it, clears the ambiguous glob re-export; (7) `bounds_min_dist`
pinned to the spec formula (`p.dist` cancels: `dot4(c,dir) − half`).
Miri-clean throughout (no UB found this WP; the half integer algorithm
is bit-exact on both). Tick policy: P1-15.1/.2, P1-16.1/.2/.3,
P1-17.3, P1-18.1/.2/.3 ticked (Rust artifacts + spec-list complete);
**not** ticked: P1-15.3 10k goldens, P1-16.4 benchmark, P1-16.5/P1-17.4
recipes (C checkout, S2), P1-17.1 full quat set + P1-17.2
`axisang_from_quat` (S-008 §5, not delivered). Evidence:
`cargo test -p obs-math --lib` 90/90, **Miri 90/90**, fmt ok, clippy 0,
workspace 216/0, deny ok (no new deps). Matrix: `docs/tests/math-rest.md`
(55 rows); `docs/tests/vec-matrix.md` updated (matrix3 layout + Q-1 +
w-lane note). Review status: see PR body.

## Log

L1 (2026-11-11): user request verbatim — see S1.
L2 (2026-11-11): routing — no C checkout, so WPs execute spec-driven in this repo; C substeps tracked per WP (S2). Stack base = Phase 0 head until PR #10 merges (S1 “wenn möglich mit main als base”). Review at bounded stop per Phase 0 doc L16–L19; PRs carry review-status section.
L3 (2026-11-11): WP-1 (P1-01 bmem) implemented spec-driven on branch `p1/bmem` (stacked on `ci/rust-phase-0`, i.e. PR #10 → #11 → this WP). Commits: `7ea5715` (allocator core: bmalloc/brealloc/bfree, 32-align size-header scheme per S-001 §7, counter, crash mechanism R24/R25) + `b52707d` (dup helpers R11–R13, BMem safe handle, fatal-path subprocess tests). Evidence: `cargo test -p obs-util` 18/18 (14 unit + 4 subprocess fatal), workspace 33 passed / 0 failed, `cargo fmt --check` clean, `cargo clippy --workspace --all-targets` 0 warnings. `docs/tests/bmem.md` matrix (16 cases). NOT ticked in `phase-1-subtasks.md`: the "Done when" requires the allocation-counter golden *vs C* + `extern "C"` shim + cbindgen diff + `OBS_ENABLE_RUST` — all C-checkout steps (S2).
L4 (2026-11-11, delivery strategy / slice boundary): the branch carries the pre-existing docs history + Phase 0 unit vs `origin/main` (≈ 35k lines, 60 commits at WP-1 start). That history was delivered as its own units: Phase 0 unit = PR #10, CI WP = PR #11. **Delivery units from now on are per WP = per PR** (user directive S1); each WP PR is its own review/delivery slice (WP-1 ≈ 500 authored lines in `crates/obs-util/` + matrix + this doc). Native review per WP is subject to the Phase 0 bounded-stop disposition (Phase 0 doc L16–L19).
L10 (2026-11-11): WP-7 (P1-06 calldata) implemented spec-driven on branch `p1/calldata` (stacked on `p1/dstr`) in `obs-signal` (module `calldata`, new `obs-util` dep). `#[repr(C)] Calldata` (4W) + the three exported functions (`calldata_get_data`/`set_data`/`get_string`) + the header-inline surface (init/init_fixed/clear/free with the Q-2 stale-fields quirk, typed accessors, set_string with NULL ≡ size-0 payload), R11 first-write allocation `max(128, 3W + name_len + size)`, R12 in-place replace with tail shift (both directions), R13 append, R14 strict capacity (Q-4) with the exact R14 log text and abandon-before-change, R15, R16 fixed-first-write-via-append, R21 clear/reuse, R22 (Drop frees + resets), R25 `cd_copy_data` deep copy. 15 unit tests mirroring the 12 conformance vectors + layout + R22/R25. **Root-cause bug found in self-review**: the search walk double-counted W and read payload bytes as `data_size` — corrupting every multi-record calldata; fixed to the spec algorithm (S-002 §6) in all three walkers. Evidence: `cargo test -p obs-signal --lib` 16/16, **Miri 16/16**, workspace green (21 test binaries), clippy 0, deny ok. Matrix: `docs/tests/calldata.md` (14 rows). NOT ticked: P1-06 C conformance (12 scripts, cbindgen diff) — S2. Review status: see PR body.
L9 (2026-11-11): WP-6 (P1-02 dstr) implemented spec-driven on branch `p1/dstr` (stacked on `p1/vec-matrix`) in `obs-util` (module `dstr`). `Dstr` (`{array, len, capacity}`, 3×usize, S-001 allocator, Drop) + `StrRef` (borrowed `{array, len}` per S-011), full S-006 surface: allocation (R01–R05 incl. the exact capacity growth vector 02 and the R02 reserve-compares-len quirk), copy/cat/insert/remove (R10–R16 incl. `count == len` → free), search/compare (R20, R30–R34, `astrstri`), replace (R21 vector 05 + Q-6 empty-find no-op), depad (R22), left/mid/right (R23), case (R24 ASCII), wide conversions (R25 + Q-2 cap 0), safe_printf (R63 + Q-4 sequential), strlist_split (R40 vector 09, single S-001 block), cat_quoted/strref (R66). 79 unit tests (obs-util lib now 79/79). Five Rust-vs-C divergences documented in code + matrix: (1) `dstr_move` source reset is field-wise — `*src = Dstr::init()` runs the old value's Drop first = double free (caught by glibc tcache); (2) self-aliasing `dstr_left(d, d, pos)` is unrepresentable with Rust references (Miri Stacked Borrows) → `dstr_truncate`; (3) Q-5 find-on-NULL returns None (C crashes); (4) Q-6 empty-find no-op (C loops forever); (5) ncopy reads exactly n bytes (Q-1). Evidence: `cargo test -p obs-util --lib` 79/79, **Miri 79/79 (107s)**, workspace green (21 test binaries), clippy 0, deny ok (no new deps). Matrix: `docs/tests/dstr.md` (24 rows). NOT ticked: P1-02.1/.2/.3 C conformance (10 scripts, cbindgen diff) — S2. Review status: see PR body.
L8 (2026-11-11): WP-5 (P1-15/P1-16 vec2/vec3/vec4 + matrix3/matrix4) implemented spec-driven on branch `p1/vec-matrix` (stacked on `p1/crc-hash`) in `obs-math`. 7 new modules: `Vec2` (R37), `Vec3` (R01–R03, R05–R07, R38 incl. the Q-1 `w`-lane semantics: `dot`/`mul`/`mulf` live-lane, all other ops zero it), `Vec4` (R04, R05, R07, R08 incl. the R08 worked value `(1,0.5,0,1) → 0xff0080ff` and the sRGB curve), minimal `Quat`/`AxisAng` kernel (R10 identity/dot, R14 from_axisang, R44 basics) for the matrix helpers, `Matrix3` (R41) and `Matrix4` (R30–R36 + the R41 inlines: identity/transpose/mul (alias-safe)/mul_4x3_only/determinant/inv (0.0005 threshold, vector 04)/from_quat/axisang/matrix3, translate/rotate/scale + all _i variants, vector 05 `ti = 20 60 120 1`). 40 unit tests, all at the spec tolerance (rel or abs 1e-6). Three findings: (1) **Miri caught a Q-5 case** — `FRAC_1_SQRT_2` is an approximate unit quaternion, so `from_quat` rows carry ~1e-7; an `assert_eq` passed on real hardware (FMA contraction) and failed under Miri; rotation-derived rows now compare at tolerance (pinned in the matrix); (2) **matrix3 `w`-lane convention**: S-008 fixes the layout but not how `matrix3_mul` composes the row `w` lanes — implemented with the implicit `(0,0,0,1)` fourth row (the only mathematically consistent reading, makes `translate3f` composition work) and documented as to-be-confirmed against the C source; (3) several exported functions have no pinned rule in S-008 (`vec3_rotate`, `vec3_transform3x4`, `vec3_mirror(v)`, `vec3_plane_dist`, `vec3_rand`, `matrix3_mirror(v)`) — deferred to P1-17/P1-18 or the C substep, listed in the matrix. Evidence: `cargo test -p obs-math` 40/40, **Miri 40/40**, workspace green, clippy 0 (refactored det3/adjugate to shut the lints). Matrix: `docs/tests/vec-matrix.md` (30 rows). NOT ticked in `phase-1-subtasks.md`: P1-15/P1-16 C conformance (scripts/math, cbindgen diff) — S2. Review status: see PR body.
L7 (2026-11-11): WP-4 (P1-10 crc32/blake2) implemented spec-driven on branch `p1/crc-hash` (stacked on `p1/darray`). `obs_util::hash`: `calc_crc32(crc, buf)` = the C `calc_crc32` (S-055-R01/R02: zlib-compatible IEEE CRC-32, running finished value in/out, any seed, empty returns the seed) via `crc32fast` `Hasher::new_with_initial`; `blake2b_512(data)` = the Rust side of the vendored `deps/blake2` via the `blake2` crate. KAT vectors generated with an independent implementation (CPython `hashlib.blake2b`/`zlib.crc32`): the S-055 worked examples all confirmed (`cbf43926`, `9be3e0a3`, continuation, `83dcefb7`), blake2b at the 1-/2-block boundaries (64/65/111618 zeros) plus `""`, `"abc"`, 1000-byte pattern. Dependencies admitted per ADR 0005 log (new section): `crc32fast` 1.5.x (MIT), `blake2` 0.11.x (MIT) — both on the D-07 allow-list, `cargo deny check` green. P1-10.1 base64: skip decision recorded in ADR 0005 (no base64 in libobs; the librtmp cencode copy is Phase 5, P5-17.4). Evidence: `cargo test -p obs-util` 51/51 unit + 5 integration, Miri 51/51 lib, workspace green, clippy 0, deny ok. Matrix: `docs/tests/crc-hash.md` (3 cases). NOT ticked in `phase-1-subtasks.md`: P1-10.1 grep re-verification, P1-10.2/.4 conformance vectors + CMake/obs-ffi wiring (S2, C checkout). Review status: see PR body.
L6 (2026-11-11): WP-3 (P1-03 darray) implemented spec-driven on branch `p1/darray` (stacked on `p1/deque`). `#[repr(C)] DArray<T>` with the C `DARRAY(T)` field order `{array, num, capacity}` (elements), S-001 allocator, R-A01…R-A14 incl. quirks Q-A1 (split copies everything — reproduced, incl. the single-element-loss defect), Q-A2 (whole-element byte compare), Q-A3 (insert_array preconditions debug-asserted). 18 unit tests + 2000-op deterministic property test vs a `Vec` model. Two real bugs found and fixed: (1) `move_from` double free — `*src = Self::new()` dropped the old value whose `Drop` freed the already-moved pointer (same class as the deque `free()` bug; fixed by clearing the fields before the overwrite); (2) **test-infra race**: the process-wide `bnum_allocs()` snapshots ran in parallel with the container tests and flaked (observed: 3–4 of 8 runs) — all counter assertions moved to a dedicated sequential binary `tests/leak_counters.rs` (one test function; also covers the bmem/darray/deque drop-leaks). This retroactively fixes a latent flake in the bmem WP. Finding: Rust padding bytes are unspecified and compiler-normalised on copies — the observable Q-A2 quirk (memcmp over padding) cannot be pinned in Rust; the C conformance substep covers it. `darray_swap` C signature (two elements vs two arrays) is not distinguishable from the spec; the port implements element swap per the R-A13 "a == b → nothing" reading — confirm against the C checkout (no vector covers R-A13). Evidence: `cargo test -p obs-util` 48/48 unit ×12 runs, Miri 48/48 lib + 1/1 leak_counters, workspace green, clippy 0, deny ok. Matrices: `docs/tests/darray.md` (new), `bmem.md`/`deque.md` updated for the counter binary. NOT ticked in `phase-1-subtasks.md`: P1-03 "Done when" requires the C comparison / layout / OBS_ENABLE_RUST (S2). Review status: see PR body.
L5 (2026-11-11): WP-2 (P1-04 deque) implemented spec-driven on branch `p1/deque` (stacked on `p1/bmem`). Faithful port of S-005 algorithms 6.1–6.3 with all rules R-D01…R-D13 and quirks Q-D1/Q-D2; `#[repr(C)] Deque` (5×usize, C field order) on the S-001 allocator. 18 unit tests incl. a 2000-op deterministic property test vs a `VecDeque` model and a Q-D1 upsize stall regression (found during implementation: `end_pos == capacity` made the zero-fill loop spin). Evidence: `cargo test -p obs-util` 32/32, workspace green, clippy 0, fmt clean, **Miri green (32/32)** — Miri caught a real double free in the first `free()` version (`*self = Self::default()` inside `Drop` re-dropped the un-nulled pointer); fixed by clearing fields before the free. `docs/tests/deque.md` matrix (18 cases). CI fix in the same WP: Miri job scoped to `--lib` (subprocess re-exec tests are incompatible with Miri isolation). NOT ticked in `phase-1-subtasks.md`: P1-04.1–3 "Done when" requires the C comparison / C-driven layout test / `OBS_ENABLE_RUST` recipe — all C-checkout steps (S2). Review status: see PR body.
L23 (2026-11-12): CI x86_64 portability + nextest 0.9.151 config migration — fixing the first CI runs of replacement PRs #29–#45. Three failure classes surfaced because CI runners are x86_64 while local dev is aarch64, and nextest moved its config location: (1) **all test jobs**: `profile ci not found` — the nextest 0.9.151 CI installs no longer reads the root-level `cargo-nextest.toml`; (2) **lint on p1/signal**: E0277 — `CString::as_ptr()` is `*const c_char` (u8 on aarch64, i8 on x86_64), so the declaration-pointer array only compiled locally; (3) **Miri on p1/bmem + p1/process**: `readlink` unsupported under Miri isolation (subprocess re-exec test `bmem_fatal`) and E0308 in `os_proc.rs` — `libc::readlink` expects `*mut c_char` (i8 on x86_64). Fixes, one commit each, stacked and propagated by rebase: **W1** on `ci/rust-phase-0` — pin nextest 0.9.151 in the workflow, `cargo-nextest.toml` → `.config/nextest.toml` (`nextest-version = "0.9.151"`, `[profile.ci]` fail-fast=false, `[profile.ci.junit]` path="junit.xml"), Miri step gains `--lib` (re-exec tests stay in the normal OS test matrix), junit artifact uploaded from the nextest store dir `target/nextest/ci/junit.xml` (the config path is store-relative; the first W1 revision pointed the upload at the repo root and would have failed the upload step — verified locally before pushing). **W2** on `p1/signal` — normalize the declaration pointer array to `*const u8` (byte data; `as_ptr()` cast). **W3** on `p1/process` — `readlink` buffer `.cast()` to the platform `c_char` type. Pre-push verification on the final tip: `cargo clippy --workspace --all-targets --target x86_64-unknown-linux-gnu -- -D warnings` clean; `cargo nextest run --workspace --profile ci` (0.9.151) **378/378**, junit.xml produced. Propagation: 16 WP branches rebased in stack order. Two botched re-run scripts (wrong upstream selection — a branch's own tip passed as rebase upstream) transiently lost the WP commits from the branch refs; fully recovered from the reflog, re-run with correct upstreams (predecessor's old tip), and verified per branch: merge-base = main, 0 merges, tree diff vs the pre-fix tip = exactly the intended W1/W2/W3 files. Final tips: ci/rust-phase-0 715f0e4, p1/bmem 949df6e, p1/deque dca0663, p1/darray 9193f10, p1/crc-hash e4befba, p1/vec-matrix da3af32, p1/dstr 02f0a1e, p1/calldata e651065, p1/decl f4fc48f, p1/signal d87dedf, p1/math-rest fb115cc, p1/config-file 9af29fc, p1/obs-data 40729d8, p1/properties 8d42083, p1/hotkey-names 7ea199d, p1/process 17ea664, p1/uint128-task = this commit. Force-push 17/17; CI re-runs on #29–#45.
L24 (2026-11-12): Native review disposition for candidate sha256:03869975… (full Phase-1 workspace vs main, 84 files / 27,218 lines, provider risk level high): inspect offered review.start (lineage review-03921c9c149b8ae1, consent=relay); START with lens selection risk/resilience/reliability → host-resolved consent returned **consent-declined-this-candidate**; no lineage created, no mutations, candidate explicitly left unreviewed. Same disposition pattern as the earlier WP-16 candidate (sha256:a6c2094…).
