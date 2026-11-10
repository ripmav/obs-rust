# 0006. Concurrency model

- Status: Accepted (target model per plan §3.1; current-state table partial — mutex table pending reference checkout, see §Consequences)
- Date: 2026-11-10
- Supersedes: —
- Superseded by: —

## Context

The media pipeline is real-time: a dropped frame or an audio xrun is visible
to the user, so the hot path (audio callback, video tick/render) cannot
allocate, block on a lock, or take a syscall. The plan confirms the target
shape (plan §3.1): dedicated real-time threads for video and audio mixing,
`crossbeam` channels / lock-free ring buffers on the hot path, `tokio` only for
network outputs and services, and **no allocation or locks in the audio
callback**. Phase 2 states the RT rule for every subtask that touches the
audio or video threads: "no allocation, no blocking lock, no syscall on the
hot path; enforce with the harness from P2-12".

The current C layout is the baseline (verified in code,
`docs/rust-rewrite/guides/00-developer-primer.md` §4):

| Thread | Created by | Drives | Notes |
|---|---|---|---|
| Graphics | `obs_graphics_thread_loop` (`libobs/obs-video.c:1094`) | ticks sources, renders, converts, calls `output_frames()`, renders displays, `video_sleep()` to the frame time | owns GPU context |
| video-io | `video_thread` (`media-io/video-io.c:190`) | waits on `update_semaphore`, hands cached frames to `video_input` callbacks (encoders/raw outputs) | graphics thread posts the semaphore |
| audio-io | `audio_thread` (`media-io/audio-io.c:205`) | wakes every `AUDIO_OUTPUT_FRAMES` (1024) samples using `os_sleepto_ns_fast`, calls `input_cb` (= `audio_callback` in `obs-audio.c:548`) to fill 6 mix buffers, clamps to ±1, passes to connected inputs | Windows: MMCSS "Audio" class |
| Encoder threads | `obs-encoder.c` | pull from video/audio-io, call `encode` | one per encoder |
| Output threads | per output type | muxing / network send | e.g. `rtmp-stream.c` |
| Hotkey thread | `obs-hotkey.c` | translate OS events | |
| UI thread | frontend | Qt event loop | most UI-side API calls must be here |

The C core additionally carries **46 `pthread_mutex_t` fields in
`libobs/obs-internal.h`** (1,543 lines) — the lock table below maps each to
its replacement.

## Decision

### Target thread model

Same seven thread kinds as today (the shape works; the discipline changes):

1. **Video (graphics) thread** — real-time. Ticks sources, renders, converts,
   delivers frames. No allocation/lock/syscall on the tick path.
2. **Audio-io thread** — real-time, highest priority (MSCCS "Audio" class on
   Windows as today). Fills preallocated mix buffers; passes them through
   lock-free queues.
3. **video-io / audio-io delivery** — event-driven wakeup (semaphore/eventfd),
   never polling; consumers pull preallocated frames.
4. **Encoder threads** — one per encoder; may allocate; not real-time.
5. **Output threads** — muxing; network send lives in `tokio` workers, fed by
   an SPSC mailbox from the encoder side (see Rule 5).
6. **Hotkey thread** — OS event translation; not real-time.
7. **UI thread** — frontend event loop; UI-side API calls only.

### Lock hierarchy (Rule: downward acquisition only)

```
L0  RT hot path            no locks at all (audio callback, video tick)
L1  per-object locks       source/scene-item/encoder state
L2  collection locks       global source/scene/output lists
L3  configuration/state    obs_data, profile, settings
```

- A thread holding a level-N lock may only acquire level > N locks. No
  upward acquisition, no same-level acquisition across objects.
- The RT threads (1–2) hold **no lock at all**: producers and consumers
  exchange data through SPSC lock-free queues only.
- Enforced by a **debug-only lock-order assert**: debug builds tag every lock
  with its level and record `(thread, held_lock_level)` at acquisition; an
  upward or same-level-across-objects acquisition aborts with the lock chain
  printed. Release builds run lock-free.

### Mutex replacement patterns (applied per entry in the table below)

| C construct | Rust replacement | Level |
|---|---|---|
| `pthread_mutex_t` guarding per-object state | `parking_lot::Mutex`/`RwLock` per object | L1 |
| mutex around a uthash/global list | one collection lock, or sharded lists with per-shard locks | L2 |
| mutex + condition variable on a data path | SPSC `crossbeam` ring (or `OsEvent` equivalent) + event-driven wakeup | L0 bridge |
| lock around `obs_data`/profile reads | `ArcSwap`-style immutable snapshot, write-side L3 lock | L3 |
| manual refcount + mutex (`obs_*_addref/release`) | `Arc` handles (Phase 4, P4 object model) | n/a |

### RT rules (P0-06.4)

**No allocation, no blocking lock, no syscall in the audio callback or video
tick.** Concretely: mix buffers and frame pools are preallocated at connect
(P2-08.5); hot-path data crosses threads only through SPSC queues; timers use
`clock_nanosleep`/`os_sleepto_ns` equivalents on the audio thread only.
**Enforcement:** the P2-12 `assert_no_alloc` harness — "the harness catches a
deliberate alloc and the audio/video ticks pass under it" (P2-12.1); every
Phase 2 subtask touching audio/video threads runs under it.

### tokio boundary

`tokio` exists only in the output/service crates (RTMP/WHIP/WebRTC,
websocket). It never appears in `obs-core`'s dependency tree above the
output layer. Cross-boundary handoff is one-directional: SPSC mailbox →
`tokio` worker; results come back through the same mailbox pattern to
non-RT threads. No `tokio` task may touch a source/scene/encoder directly —
always through the public API on a non-RT thread.

### Mutex table (P0-06.2 — TODO)

> **Blocked on the reference checkout** (feature doc L3: no C source linked on
> this machine). `libobs/obs-internal.h` holds 46 `pthread_mutex_t` fields
> (per `docs/rust-rewrite/guides/phase-0-guide.md`); each must be listed with
> protects / taken by / taken-while-holding, then assigned a level and
> replacement from the table above. Owner: P0-06 owner; do immediately after
> `tools/link-obs-source.sh` links the 33.0.1 checkout. Unknown entries are
> marked `TODO(owner)` per the subtask's done-when, never guessed.

## Alternatives

1. **Keep the C thread layout 1:1, just swap languages.**
   Rejected: (a) the 46-mutex soup survives the port and keeps the xrun risk
   that is the reason for the rewrite; (b) "no allocation in the audio
   callback" is not enforced anywhere in the C code — a 1:1 port preserves
   that absence of discipline.
2. **Full async (`tokio`) core, everything on workers.**
   Rejected: (a) real-time audio cannot ride on a cooperative scheduler —
   preemption latency is exactly the xrun; (b) it rewrites the delivery
   contract with encoder/output plugins that expect a pull-based,
   frame-ordered model.
3. **Mutexes everywhere, no lock-free queues (simpler).**
   Rejected: (a) a mutex on the audio path is a syscall — it fails the RT
   rule by construction; (b) the 8-track mix and 20-item render benchmarks
   (P0-17) show lock contention on the C layout; the rewrite's performance
   gate (no regression beyond 2%) is cheaper to meet lock-free.

## Consequences

- Easier: the hierarchy is mechanically checkable in debug; RT purity is a
  test (P2-12), not a convention; new objects pick their lock level at
  creation.
- Harder: the mutex table (46 entries) must be completed against the C source
  before Phase 4's object model — it is the one part of this ADR not yet
  filled; lock-order asserts add debug-build overhead (acceptable, debug-only).
- Revisit: if the debug lock-order assert fires in CI on a legitimate
  cross-level need, the hierarchy gains the level (ADR amendment) rather than
  the assert being disabled; if `tokio`'s scheduler latency ever leaks into
  a measured output path, the mailbox boundary widens, the rule does not bend.

## Links

- plan §3.1 (`docs/rust-rewrite-plan.md`); Phase 2 RT rule + P2-12
  (`docs/rust-rewrite/phase-2-media-pipeline.md`,
  `docs/rust-rewrite/subtasks/phase-2-subtasks.md` line 5, P2-12.1)
- P0-06.1–P0-06.4 (`docs/rust-rewrite/subtasks/phase-0-subtasks.md`)
- `libobs/obs-video.c` (1200), `obs-audio.c` (721), `media-io/video-io.c`
  (679), `media-io/audio-io.c` (455), `obs-internal.h` (1543) — read in the
  reference checkout; primer §4 as the verified interim table
- Phase 4 object model (P4: `Arc` handles replacing manual refcounts)
