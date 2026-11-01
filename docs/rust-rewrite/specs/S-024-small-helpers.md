# S-024 Small helpers: option strings, 128-bit math, process arguments, hotkey names, missing files, task queue, profiler, BPM, address ordering, file updater, constants

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1: `shared/opts-parser`, `libobs/util/util_uint64.h`, `util_uint128.h`, `libobs/util/pipe.c` (argument list only), `libobs/obs-hotkey-name-map.c` (+ the key list `obs-hotkeys.h`), `libobs/obs-missing-files.c`, `libobs/util/task.c`, `libobs/util/profiler.c`, `shared/bpm`, `shared/happy-eyeballs` (address ordering only), `shared/file-updater`, `obs-defs.h`, `obs-config.h` and the public enums |
| Target crates | `crates/obs-util-small` (M1-M3, M6, M7, M11), `crates/obs-hotkey-names` (M4), `crates/obs-missing-files` (M5), `crates/obs-bpm` (M8), `crates/obs-netutil` (M9), `crates/obs-file-updater` (M10) -- crate split is a suggestion |
| Depends on | S-001 (allocation, only as implementation detail), S-006 (dstr, only as detail), S-012 (`metadata_obu`, used by M8), S-014 (obs_data: JSON semantic of `obs_data_get_int/get_string`, used by M10) |
| Conformance | `test/conformance/scripts/helpers/` (75 scripts, vectors in `vectors/helpers/`) |

## 1. Scope

Twelve small, mostly pure modules that are good clean-room targets. Each has its own section
(M1-M12), its own rules (`S-024-Rnn`) and its own scripts. All modules are driven by one runner
(`helpers_runner.c`, command prefix selects the module).

| # | Module | In scope | Out of scope / skipped |
|---|---|---|---|
| M1 | `opts-parser` | whole module | |
| M2 | `util_uint64.h` / `util_uint128.h` | whole headers | |
| M3 | `os_process_args_*` (`pipe.c`) | argument list container | the `os_process_pipe_*` process functions (platform code) |
| M4 | `obs-hotkey-name-map.c` | key id <-> name table, both lookups | |
| M5 | `obs-missing-files.c` | list/record API, refcounts, callbacks | `obs_source_replace_missing_file` itself (S-0xx sources), only its observable call is modelled |
| M6 | `util/task.c` | ordering, wait/destroy semantics, `inside` | thread name, failure paths of thread/semaphore creation |
| M7 | `util/profiler.c` | complete (aggregation, snapshot, printing, CSV, name store) driven by a fake clock | the gzip CSV writer (`profiler_snapshot_dump_csv_gz`: same text as the plain CSV, compressed) |
| M8 | `shared/bpm` | counters, timestamps, SEI payloads, per-codec packet injection | `libcaption` is only used as a framing detail (specified here); `obs_output` plumbing |
| M9 | `shared/happy-eyeballs` | address-list ordering, resolver hints, constants | the connection race (sockets, timers, threads): not deterministic, **not specified** |
| M10 | `shared/file-updater` | the full update algorithm, driven by a fake HTTP server and an in-memory file system | threads, real HTTP, TLS options |
| M11 | `obs-defs.h`, `obs-config.h`, public enums | constants table + semantic-version macro | |
| M12 | skipped modules | | `libobs/util/source-profiler.c`: needs `obs_source_t`, `gs_timer_t`, `obs->video` internals and `NUM_TEXTURES`, so it is not linkable without libobs; `shared/file-updater` hash handling: the module has **no hashes**, it compares integer versions |

## 2. Terminology and conventions

RFC 2119 words. 64-bit little-endian target. "Fake clock": all time-dependent modules read time
through one function; the runner makes it a variable. µs = microseconds. `u32`/`u64` arithmetic
wraps modulo 2^32 / 2^64 unless stated. Strings printed by the runner use the common escaping of the
conformance README. Log lines (`log <level> <text>`) follow the README comparison rule (level only)
**except** where a rule says "MUST" (the profiler report, which the runner prints as `text "..."`
data lines, not as log lines, so it is compared byte for byte).

## 3. Interface

```c
/* M1 */ struct obs_option { char *name; char *value; };
         struct obs_options { size_t count; struct obs_option *options;
                              size_t ignored_word_count; char **ignored_words; char **input_words; };
         struct obs_options obs_parse_options(const char *options_string);
         void obs_free_options(struct obs_options options);
/* M2 */ typedef struct util_uint128 { union { uint32_t i32[4]; struct { uint64_t low, high; }; }; } util_uint128_t;
         util_uint128_t util_add128(util_uint128_t a, util_uint128_t b);
         util_uint128_t util_lshift64_internal_32(uint64_t a);   /* a << 32 as 128 bit */
         util_uint128_t util_lshift64_internal_64(uint64_t a);   /* a << 64 as 128 bit */
         util_uint128_t util_mul64_64(uint64_t a, uint64_t b);
         util_uint128_t util_div128_32(util_uint128_t a, uint32_t b);
         uint64_t util_mul_div64(uint64_t num, uint64_t mul, uint64_t div);
/* M3 */ struct os_process_args *os_process_args_create(const char *executable);
         void   os_process_args_add_arg(struct os_process_args *, const char *arg);
         void   os_process_args_add_argf(struct os_process_args *, const char *format, ...);
         char **os_process_args_get_argv(const struct os_process_args *);
         size_t os_process_args_get_argc(struct os_process_args *);
         void   os_process_args_destroy(struct os_process_args *);
/* M4 */ const char *obs_key_to_name(obs_key_t key);   obs_key_t obs_key_from_name(const char *name);
         void obs_hotkey_name_map_free(void);
/* M5 */ obs_missing_files_t *obs_missing_files_create(void);
         obs_missing_file_t  *obs_missing_file_create(const char *path, obs_missing_file_cb cb, int src_type, void *src, void *data);
         void   obs_missing_files_add_file(files, file);      size_t obs_missing_files_count(files);
         obs_missing_file_t *obs_missing_files_get_file(files, int idx);
         void   obs_missing_files_append(dst, src);           void obs_missing_files_destroy(files);
         void   obs_missing_file_issue_callback(file, const char *new_path);
         const char *obs_missing_file_get_path(file);         const char *obs_missing_file_get_source_name(file);
         void   obs_missing_file_release(file);               void obs_missing_file_destroy(file);
         enum obs_missing_file_src { OBS_MISSING_FILE_SOURCE = 0, OBS_MISSING_FILE_SCRIPT = 1 };
         typedef void (*obs_missing_file_cb)(void *src, const char *new_path, void *data);
/* M6 */ os_task_queue_t *os_task_queue_create(void);
         bool os_task_queue_queue_task(tq, void (*task)(void *), void *param);
         void os_task_queue_destroy(tq);   bool os_task_queue_wait(tq);   bool os_task_queue_inside(tq);
/* M7 */ void profile_register_root(const char *name, uint64_t expected_time_between_calls_ns);
         void profile_start(const char *name);  void profile_end(const char *name);
         void profile_reenable_thread(void);    void profiler_start(void);  void profiler_stop(void);
         void profiler_print(snapshot or NULL); void profiler_print_time_between_calls(snapshot or NULL);
         void profiler_free(void);
         profiler_name_store_t *profiler_name_store_create(void);  void profiler_name_store_free(store);
         const char *profile_store_name(store, const char *printf_format, ...);
         profiler_snapshot_t *profile_snapshot_create(void);  void profile_snapshot_free(snap);
         bool profiler_snapshot_dump_csv(snap, const char *filename);
         size_t profiler_snapshot_num_roots(snap);  void profiler_snapshot_enumerate_roots(snap, bool (*f)(ctx, entry), ctx);
         void profiler_snapshot_filter_roots(snap, bool (*f)(data, const char *name, bool *remove), data);
         size_t profiler_snapshot_num_children(entry);  void profiler_snapshot_enumerate_children(entry, f, ctx);
         /* entry accessors: name, times (array of {time_delta, count}), min_time, max_time, overall_count,
            times_between_calls, expected_time_between_calls, min/max_time_between_calls, overall_between_calls_count;
            every accessor returns 0/NULL for a NULL entry, num_roots(NULL)=0, num_children(NULL)=0 */
/* M8 */ void bpm_inject(obs_output_t *output, struct encoder_packet *pkt, struct encoder_packet_time *pkt_time, void *param);
         void bpm_destroy(obs_output_t *output);
/* M9 */ internal: address list builder (see 4.9); constants HAPPY_EYEBALLS_DELAY_MS=200, _MAX_ATTEMPTS=6, _CONNECTION_TIMEOUT_MS=25000
/* M10*/ update_info_t *update_info_create(log_prefix, user_agent, update_url, local_dir, cache_dir, confirm_cb, param);
         update_info_t *update_info_create_single(log_prefix, user_agent, file_url, confirm_cb, param);
         void update_info_destroy(update_info_t *);
         bool (*confirm_file_callback_t)(void *param, struct file_download_data *file);
         struct file_download_data { const char *name; int version; DARRAY(uint8_t) buffer; };
```

Ownership: `obs_parse_options` returns a struct owning all memory (release with `obs_free_options`); the
`value` pointers point *into* `input_words`, `name` is separately allocated. `os_process_args_get_argv` returns
the live internal array (valid until the next add). `profile_store_name` returns memory owned by the store.
Everything else is plain.

## 4. Data formats and algorithms per module

### 4.1 M1 opts-parser

Algorithm `parse(s)`: (1) if `s` is NULL or empty -> result **Z** (all fields zero / NULL). (2) Split `s`
on the single character `' '` (0x20), dropping empty pieces (so runs of spaces and leading/trailing spaces vanish).
If no word remains -> **Z**. (3) For every word `w` in order: it is an *option* iff `w[0] != '='` and `w` contains an
`'='` and the character after the first `'='` exists (value non-empty); then `name` = text before the first `'='`,
`value` = text after it (further `'='` are part of the value). Otherwise `w` is an *ignored word*.
Result: `count`, `options` (in input order, duplicates kept), `ignored_word_count`, `ignored_words`,
`input_words` (all words, NULL-terminated array). In the non-Z result all three pointers are non-NULL even if
`count` or `ignored_word_count` is 0.
There is **no quoting, escaping or tab handling** (Q-1): only 0x20 separates.

### 4.2 M2 128-bit helpers

`value = high * 2^64 + low`; `i32[k]` are the little-endian 32-bit limbs (`i32[0]` is the lowest; on the
64-bit LE target `low = i32[1]<<32 | i32[0]`). `util_add128` = sum mod 2^128 (carry out of bit 127 is dropped).
`lshift64_internal_32(a)` = `a * 2^32` (exact, 96 bits), `_64(a)` = `a * 2^64`. `util_mul64_64(a,b)` = exact
product. `util_div128_32(a,b)` = `floor(a / b)` as 128-bit, `b != 0` is a precondition (the reference raises SIGFPE: D-1).
`util_mul_div64(n, m, d)`: on MSVC x64 it is `floor(n*m/d)` computed with a 128-bit intermediate; **on every other
platform** (the reference for the vectors) it is `(n/d)*m + ((n%d)*m)/d` in wrapping u64 arithmetic. The two are equal
whenever `(n%d)*m < 2^64` and the sum does not overflow. A port MUST compute the exact quotient with a 128-bit
intermediate and return it when it fits in u64 (Q-2/D-2); for a quotient that does not fit, the result is
unspecified (MSVC raises a divide error; the Linux value is meaningless): a port may panic, saturate or wrap
(document which). `d == 0` is a precondition violation (D-1).

### 4.3 M3 process arguments

The container is an ordered list of strings plus an implicit trailing NULL slot: `create(exe)` stores a copy of
`exe` as element 0; `add_arg(s)` appends a copy of `s`; `add_argf(fmt, ...)` formats like `printf` and appends the
result; `get_argc` = number of stored elements (the terminator is not counted); `get_argv` = array of `argc+1`
pointers, element `argc` is NULL. No quoting, splitting or escaping is done anywhere (that is the job of
`os_process_pipe_create2`, platform code). Destroy frees everything; `destroy(NULL)` is a no-op.

### 4.4 M4 hotkey names

`enum obs_key` ids are the 0-based position in the key list; `OBS_KEY_LAST_VALUE = 636` (not a key). The name of
id `i` is the identifier text of that key, e.g. id 0 `OBS_KEY_NONE`, id 1 `OBS_KEY_RETURN`, id 121 `OBS_KEY_A`.
**The vector `vectors/helpers/hotkeys-fallback-table.expected` is the normative table** (lines `ID NAME ID`).
`obs_key_to_name(k)`: `k` out of range (`k >= LAST`, and also any negative value: required behaviour, see D-3) logs an
ERROR and returns `""`; otherwise the name. `obs_key_from_name(n)`: exact, case-sensitive full-string match against
the table; no match, empty string or non-key identifiers (e.g. `OBS_KEY_LAST_VALUE`) -> `OBS_KEY_NONE` (0). The
reference has two lookup paths (a lazily built hash map when the global core exists; a linear search otherwise) with
identical results; a port needs one.

### 4.5 M5 missing files

A *record* has a reference count (starts at 1), a copy of the path, a callback, a source type, an opaque source
pointer, an opaque user-data pointer, and for `OBS_MISSING_FILE_SOURCE` a copy of the source's name taken at creation
(`NULL` for scripts and when the source has no name). A *list* is an ordered vector of record handles.
`add_file` appends the handle **without** taking a reference (ownership moves into the list); `append(dst, src)` adds
each of src's handles to dst in order and increments each record's count (src keeps its entries); list `destroy`
releases every handle once; `release` decrements and destroys at zero; `release(NULL)` is a no-op. `issue_callback(f, p)`:
SOURCE records -> the source layer calls `callback(source_context_data, p, user_data)` (a NULL `p` is passed through);
SCRIPT records -> nothing happens (Q-5).

### 4.6 M6 task queue

One worker thread per queue, FIFO by queueing time. `queue_task` appends `(fn, param)` and wakes the worker.
`wait(tq)`: (1) mark `waiting = true`, `tasks_processed = false`; (2) append an internal *wait marker*; (3) block until the
worker runs it; (4) return `tasks_processed`. The worker sets `tasks_processed = true` whenever it takes a *non-marker*
task off the queue while `waiting` is set; taking the marker clears `waiting`. If the marker reaches the head while other
tasks are still queued (tasks added by running tasks after the marker was queued), the marker is moved to the back and the next task
runs first. `destroy(tq)` appends an internal *stop marker* handled the same way (deferred behind any pending task), joins the
worker and frees the queue; so every task queued before `destroy` runs. Results: `wait` on an idle queue -> `false`; if any
user task ran after the wait started -> `true`. `inside(tq)` is true only on `tq`'s own worker thread.

### 4.7 M7 profiler

**Names** are compared by identity (pointer equality), never by text (Q-7): a port MUST model a name as an
interned handle; `profile_store_name` returns a *new* handle on every call, even for equal text; it never de-duplicates.

**State.** Global `enabled` (default false), per-thread `thread_enabled` (default true), per-thread stack of open calls.
`profiler_start/stop` set `enabled`. `reenable_thread()` sets `thread_enabled = enabled` if it was false.
`lock_root()` := if `!enabled` -> `thread_enabled = false`, return false.

**Recording.** `profile_start(n)`: no-op if `!thread_enabled`; else push call `{name n, start = now}` as a child of the
open call (or as a new root call if none). `profile_end(n)`: `end = now` (read first); no-op if `!thread_enabled`; if no open call:
log ERROR, return; if the open call's name != n: log ERROR; search the *ancestors* (not the open call) for one named `n`;
if none -> return (nothing closed); else repeatedly `profile_end(open.name)` (fresh clock read each) until the call named `n` is
open. Then close the open call with `end_time = end`. If it had a parent: done. If it was a root call: `merge(call)`.
`merge(root call)`: if `lock_root()` fails the call is discarded (and `thread_enabled` is now false, Q-8); else find or create
the root entry by name (creation order is the snapshot order), let `prev` be that root's previous completed call (kept
per root), store this call as the new `prev`, and `merge_call(entry, call, prev)`. `merge_call(e, c, prev)`: for each child call of `c`
in order: `merge_call(child_entry(e, name), child, none)` where `child_entry` finds a child entry of `e` by name or appends a new one
(so children are ordered by first appearance); then if `e.expected != 0` and `prev` exists add a *between* sample
`(c.start - prev.start + 500) / 1000` (µs, u64 arithmetic); then add a *time* sample `(c.end - c.start + 500) / 1000`.
Samples are kept as a multiset `{µs value -> count}`. `register_root(n, ns)`: if `lock_root()` fails nothing happens; else find/create root
`n` and set `expected = (ns + 500) / 1000` (µs). Only roots ever receive between samples.

**Snapshot** (`profile_snapshot_create`): copies the tree under the global lock; per entry: `times` = all `(µs, count)` pairs sorted
by **descending** µs; `overall_count` = sum of counts; `min_time`/`max_time` = smallest/largest µs (an entry without samples:
`min = 2^64-1`, `max = 0`, `count = 0`, Q-9); if `expected != 0` the same for `times_between_calls` (`overall_between_calls_count`,
`min/max_time_between_calls`), else those fields are 0 and the array is empty; `expected_time_between_calls` = `expected`;
`children` = copy of the children in order. `filter_roots(snap, f, data)`: `i = 0; while i < n: remove = false; res = f(data, name_i, &remove);
if remove: delete root i (later roots shift down); if !res: stop; if !remove: i += 1`.

**Report text** (`profiler_print`): blog(INFO) lines, in this order: the header
`== Profiler Results =============================`, then every root (pre-order, `indent = 0`), then the footer of 49 `=`.
A line for entry `e` (`calls = overall_count`, `parent_calls` = parent's `calls`, 0 for roots; `G(x)` = C `%g` of the double `x` followed by
the 3 bytes `C2 A0` (NBSP) and `ms`):
`<indent><name>: ` then (a) if `min == max`: `G(min/1000.)`; (b) else `min=G(min/1000.), median=G(median/1000.), max=G(max/1000.), 99th percentile=G(p99/1000.)`
and, if `expected != 0`, `, <P>% below G(expected/1000.)` where `P` is `%g` of `(1 - A/calls) * 100`, `A` = number of calls whose µs value is `>= expected`.
Then, if `parent_calls != 0 && calls != parent_calls`: `r = calls/parent_calls` (double); if `lround(r*10) != 10`: `, <%g r> calls per parent call`.
Statistics (`times` sorted descending, accumulate counts `accu`): for each entry in order with `old = accu; accu += count`:
`if old < calls*0.01 && accu >= calls*0.01 -> p99 = value; else if old < calls*0.5 && accu >= calls*0.5 -> median = value, stop`.
`p99`/`median` start at 0. **Quirk Q-10:** because of the `else`, an entry that crosses both thresholds sets only `p99`, and
if no entry ever satisfies the second condition `median` stays 0 (e.g. two calls: median is 0). With no samples all stats are 0, P = 0.
**Indent**: level `n >= 1` is the concatenation over `i = 0..n-1` of: if bit `i` of `active`: `" ┣"` (U+2523) when `i = n-1` else `" ┃"` (U+2503);
else `" ┗"` (U+2517) when `i = n-1` else two spaces. Children are printed with `indent+1` after setting `active |= 1 << indent`; for the **last** child
`active &= (1 << indent) - 1` first. (`┣` i.e. "has following sibling" and `┗` for the last child.)
**Between-calls report** (`profiler_print_time_between_calls`): header `== Profiler Time Between Calls ==================`, footer as above, one line
per root with `expected != 0` (roots without it print nothing; children are never printed):
`<name>: min=G(bmin/1000.), median=G(med/1000.), max=G(bmax/1000.), <P>% within ±2% of G(expected/1000.) (<L>% lower, <H>% higher)`
(`±` = U+00B1). With `lo = (u64)(expected*0.98)`, `hi = (u64)(expected*1.02 + 0.5)`, `calls = overall_between_calls_count`,
`min,max` = between min/max, entries sorted descending with `accu`:
`med = 0; P = L = H = 0; if no entries: all zero (done). med = value of the first entry with accu >= calls*0.5 (accumulating counts from the first entry).
found_hi = (max <= hi); found_lo = false. if min >= hi: H = 100, done. if found_hi && min >= lo: P = 100, done.
accu = 0; for each entry (value v, count c): if !found_hi && v <= hi: H = accu/calls*100; accu = 0; found_hi = true.
if !found_lo && v < lo: P = accu/calls*100; accu = 0; found_lo = true. accu += c.
after the loop: if !found_hi: H = 100; else if !found_lo: P = accu/calls*100; else L = accu/calls*100.`

**CSV** (`dump_csv`): header line `id,parent_id,name_id,parent_name_id,name,time_between_calls,time_delta_µs,count` (µ = U+00B5), `\n` endings; then
for each root, depth first: for each `times` pair `<id>,<parent_id>,<name_id>,<parent_name_id>,<name>,0,<µs>,<count>`, then for each `times_between_calls` pair
`...,<name>,<expected>,<µs>,<count>`, then the children (parent = this entry). The four id columns are `%p` of the entry, parent entry, name and parent name
(`(nil)` for none); a port may print any stable unique tokens; the vector replaces them by first-appearance ordinals `#n` and `nil`.

### 4.8 M8 BPM injection

State per `(output, track_idx)` (track < 10): seven `{ref, curr, diff}` u32 counters, four timestamps, rendered flags. `bpm_destroy(output)` forgets all tracks of that output.

`bpm_inject(output, pkt, time)`:
1. `output` or `pkt` NULL -> debug log, return (nothing touched).
2. Only video packets with `keyframe` are processed; anything else returns untouched.
3. `time` NULL -> debug log, return untouched.
4. Run `update_metrics`, then build and append the messages (below). Counters advance even when the packet ends up unchanged.

`update_metrics`: read `session_output = frames output by the output`, `dropped = frames dropped by the output`, `rendered = total frames rendered by core`,
`lagged = lagged frames` and, if the packet's encoder has a video handle, `rend_input = video total frames`, `rend_skipped = video skipped frames`,
`rend_output = encoder encoded frames`; otherwise the three rendition counters are 0 and an ERROR is logged. If `pkt.pts == 0` all seven diffs are 0, else
`diff = curr - ref` (u32, wraps). Then `ref = curr` for all seven. Timestamps (ns of the monotonic clock `os_gettime_ns`): `CTS` always valid; `FER` valid iff
`fer != 0 && fer > cts`; `FERC` valid iff FER valid and `ferc != 0 && ferc > fer`; `PIR` always valid. Each valid timestamp becomes the string
`YYYY-MM-DDTHH:MM:SS.mmmZ` (UTC, `mmm` = nanoseconds/1,000,000 truncated, no rounding) of the wall-clock time `wall(ns)`; the reference computes
`wall(ns) = wall_at_startup + (ns - monotonic_at_startup)` (captured once on first use); the vectors use the identity `wall(ns) = ns` since
1970-01-01T00:00:00Z. A port MUST make this base injectable for tests.

**Payloads** (all integers big-endian; `uuid_ts = 0aecffe7-5272-4e2f-a62f-d19cd61a93b5`, `uuid_sm = ca60e71c-6a8b-4388-a377-151df7bf8ac2`,
`uuid_erm = f1fbc1d5-101e-4fb5-a61e-b8ce3c07b8c0`, written as the 16 bytes in textual order):
- TS: `uuid_ts | (n-1) | { 01 | tag | string 00 }*n` for the valid timestamps in the order CTS (tag 1), FER (2), FERC (3), PIR (4); `n` in 2..4 (the byte holds `n-1` in its low 4 bits, high bits 0).
- SM: `uuid_sm | 00 | 01 | 04 | PIR-string 00 | 03 | 01 u32(rendered.diff) | 02 u32(lagged.diff) | 03 u32(dropped.diff) | 04 u32(session_output.diff)`.
- ERM: `uuid_erm | 00 | 01 | 04 | PIR-string 00 | 02 | 01 u32(rend_input.diff) | 02 u32(rend_skipped.diff) | 03 u32(rend_output.diff)`.

**Logging** (level only is compared): each processed packet emits one DEBUG line summarising the metrics, preceded by an ERROR line `update_metrics(): *video_t==null`
if the encoder has no video handle, and (for `pts == 0`) a DEBUG line `Setting diffs to 0`, in that order; NULL `output`/`pkt` -> one DEBUG line; missing timing -> one DEBUG line;
HEVC without start code -> two DEBUG lines (start code, "BPM injection processing failed"); unknown codec -> one DEBUG line "BPM injection processing failed" (after the summary line).
Audio / non-key packets log nothing.

**Framing** by codec string of the packet's encoder (`h264`, `hevc`, `av1`; others: see Q-11). For each of the three payloads in the order TS, SM, ERM:
- *SEI NAL* `S(p)` = `06 | 05 | size(p) | escape(p) | 80`, with `size(p)` = `ff` repeated `len/255` times then `len%255` as one byte (`len = |p|` before escaping) and
  `escape` = emulation prevention: scanning `p`, insert `03` before every byte `<= 03` that is preceded by two consecutive `00` bytes (the run counter restarts after the
  inserted byte).
- `h264`: append `00 00 00 01 | S(p)`.
- `hevc`: if the packet does not start with `00 00 01` or `00 00 00 01` the packet is returned **unchanged** (debug log; counters already advanced).
  Else let `h0,h1` be the two bytes after that start code; append `00 00 01 | ((40<<1) | (h0 & 1)) | h1 | S(p)[1..]` (the AVC NAL header byte of `S(p)` is dropped).
- `av1`: append `metadata_obu(p, 0x06)` of S-012 (`2a | leb128(|p|+2) | 06 | p | 80`) -- `p` raw (not escaped).
The new packet data is `original bytes | appended` (size grows); the original buffer is released by the callee. `bpm_inject` returns nothing.

### 4.9 M9 address ordering (happy eyeballs)

`build_addr_list(host, port, ctx)`: hints = `{family, SOCK_STREAM, IPPROTO_TCP, AI_ADDRCONFIG}` with family = `AF_INET` if the configured bind
address length equals `sizeof(sockaddr_in)` (16), `AF_INET6` if equal to `sizeof(sockaddr_in6)` (28), else `AF_UNSPEC`; the service string is the decimal port.
Resolver failure -> status -1, error code = `errno`, message = its `strerror`, no list. Else reorder the result list `L` in place (single pass):
`prev = L[0]; cur = L[1]; while cur: if prev.family == cur.family and family in {INET, INET6}: want = the other of the two; find the first node X after cur with family == want;
if none: stop the whole loop; else move X to sit between prev and cur (X.next = cur, prev.next = X); prev = cur; cur = cur.next` (note: after the move `prev = cur` where `cur` is still the node
that was shifted back; the moved node X is skipped over -- see vectors). Nodes of other families (e.g. `AF_UNIX`) never trigger a move and are not moved except when found as `X` (they are not, X is always INET/INET6).
Constants: Resolution delay between attempts 200 ms, max 6 attempts, total connection timeout 25000 ms.

### 4.10 M10 file updater

Inputs: `local` dir (bundled data), `cache` dir, `temp = cache + ("/" unless cache ends with / or \) + ".temp"`, base `update_url`,
callback. `get_path(dir, file)` = `dir` + `"/"` (only if `dir` is non-empty and does not end with `/` or `\`) + `file`; NULL/empty dir -> just `file`.
`package.json` objects: `{"version": int, "url": string, "files": [{"name": string, "version": int}, ...]}`; missing int fields read as 0, missing strings as `""`
(S-014 semantic). The whole run is `update_thread`:

1. Create an HTTP client; read, in this order, `local/package.json` (via `get_path`), `cache/package.json`, `cache/meta.json` (each read is traced as `fs read`; a file
   that is missing or is not valid JSON counts as absent, an invalid one also logs an ERROR). If `cache/meta.json` exists and parses, send header
   `If-None-Match: <etag>` where `<etag>` = its string field `etag`, **or the empty string if that key is missing** (Q-16); always send `User-Agent: <ua>` (header list order:
   If-None-Match first, then User-Agent).
2. `update_local_version`: `lv = local.version`, `cv = cache.version` (0 if missing). If `cv < lv`: for each file in `local.files` in order: look up its name in `cache.files` (first match);
   if not found or `local.version > cache_entry.version` (strict): `copy_local_to_cache(name)`; then `copy_local_to_cache("package.json")`; `cache_package := local_package`; current = `lv`. Else current = `cv`.
   `copy_local_to_cache(f)`: `os_copyfile(local/f, temp/f)`; `os_unlink(cache/f)`; `os_rename(temp/f, cache/f)` (result codes ignored).
3. `update_remote_version(current)`: GET `update_url/package.json`... (`url = get_path(update_url, "package.json")`) with failure-on-error. Transfer failure or HTTP >= 400 -> warn and stop. HTTP 304 -> stop.
   If the body does not exist or its first byte is not `{` -> warn "Remote package does not exist or is not valid json", stop. **Save metadata**: if an `ETag` header was received, write `cache + "meta.json"` (**no separator**, Q-12) with the compact JSON
   `{"etag":"<escaped>"}`. Parse the body as JSON; failure -> warn, stop. `rv = remote.version`; if `rv <= current` stop. Write the body bytes to `temp/package.json`. `remote_url = remote.url` (may be `""`). For each `remote.files` entry: look up the name in the **current cache package**
   (the local one after step 2 if it was installed, else the cached one); if found and `remote.version <= cached.version` skip; else GET `get_path(remote_url, name)`; failures (transfer error or HTTP >= 400) skip only this file; call the callback if present with a buffer that holds the body plus one terminating NUL (`buffer.num = len+1`), the callback may replace the buffer; if it returns false: info log "rejected", skip; else write `buffer[0..num-1]` to `temp/name`, `unlink(cache/name)`, `rename(temp/name, cache/name)`, info log. Finally `replace_file(temp, cache, "package.json")` (unlink old, rename) and info "Successfully updated package (version N)".
4. `os_rmdir(temp)`.
ETag header handling (header callback, only while `remote_url` is unset, i.e. for the package request): a header line starting with `ETag: ` (exact case) and a non-empty remainder is copied, truncated at the first `\r`, then at the first `\n`; later headers overwrite earlier ones (the reference leaks the earlier copy).
Single-file mode (`update_info_create_single`): GET the URL; on failure return; if the body is empty (first byte NUL) return; else call `cb(name = url, version = 0, buffer = body + NUL)`.
Every filesystem call and HTTP request in the vectors appears in a trace (`fs ...`, `http GET ...`): **that trace is normative** (order, operands, result).

### 4.11 M11 constants

`obs-defs.h`: `MAX_CHANNELS 64`, `OBS_ALIGN_CENTER 0 / LEFT 1 / RIGHT 2 / TOP 4 / BOTTOM 8`, `MODULE_SUCCESS 0, MODULE_ERROR -1, MODULE_FAILED_TO_OPEN -2, MODULE_MISSING_EXPORTS -3, MODULE_INCOMPATIBLE_VER -4, MODULE_HARDCODED_SKIP -5`,
`OBS_OUTPUT_SUCCESS 0 ... OBS_OUTPUT_HDR_DISABLED -9`, `OBS_VIDEO_SUCCESS 0 ... OBS_VIDEO_MODULE_NOT_FOUND -5`. `obs-config.h`: major 33, minor 0, patch 1, `LIBOBS_API_VER = 0x21000001 = 553648129`.
`MAKE_SEMANTIC_VERSION(major, minor, patch) = (major << 24) | (minor << 16) | patch` (bitwise OR, **no masking**: Q-13). The full table of 277 names (all enumerators of the public enums plus the defines above) is normative in `vectors/helpers/const-table.expected`.

## 5. Behaviour (numbered rules)

### M1 opts-parser
- **S-024-R01** `parse(NULL)`, `parse("")`, and any string that has no non-space character return the zero result (count 0, all three pointers NULL).
- **S-024-R02** Words are separated only by U+0020; empty words are dropped; the result keeps input order.
- **S-024-R03** A word is an option iff it does not start with `=`, contains `=`, and has at least one character after the first `=`. `name` is the text before the first `=`, `value` everything after it (including further `=`).
- **S-024-R04** All other words are ignored words, in input order; `input_words` lists every word in order.
- **S-024-R05** Duplicate option names are all kept.
- **S-024-R06** No quoting/escaping/tab handling exists (Q-1): quotes, backslashes, tabs, newlines and UTF-8 are plain data.
- **S-024-R07** For a non-zero result `options`, `ignored_words` and `input_words` are non-NULL even when empty (all-ignored input: `count = 0` with non-NULL options).

### M2 128-bit
- **S-024-R08** `util_add128` is addition modulo 2^128, `util_mul64_64` the exact 128-bit product, `lshift` helpers exact shifts, `util_div128_32` truncating division (divisor 0 forbidden: D-1).
- **S-024-R09** `util_mul_div64(n,m,d)` returns `floor(n*m/d)` whenever that value fits in 64 bits and all intermediate products of the reference formula fit; ports MUST return the exact value for every input whose result fits in u64 (Q-2).
- **S-024-R10** Layout: `sizeof(util_uint128_t) == 16`, `i32[k]` are the LE limbs (vector `u128_layout`).

### M3 process arguments
- **S-024-R11** `create(exe)` -> `argc = 1`, `argv = [exe, NULL]`; `add_arg(s)` appends a copy (`argc += 1`); the NULL terminator is always at index `argc`.
- **S-024-R12** `add_argf` appends exactly one argument: the printf expansion of the format and arguments.
- **S-024-R13** No quoting, escaping or splitting of arguments (spaces/quotes are kept literally).
- **S-024-R14** Independent lists do not interact; `destroy(NULL)` is a no-op.
- **S-024-R15** (QUIRK Q-3) NULL `executable` / NULL `arg` are stored as NULL elements and counted in `argc`.
- **S-024-R16** (DEFECT D-4) `add_argf` with a format that expands to the empty string MUST store `""` (the reference stores NULL).

### M4 hotkeys
- **S-024-R17** `obs_key_to_name(i)` for `0 <= i < 636` is the name in the normative table; `obs_key_from_name(name)` returns the id for that exact name (round trip is the identity).
- **S-024-R18** Unknown, wrongly-cased, padded or empty names, and `"OBS_KEY_LAST_VALUE"`, map to 0 (`OBS_KEY_NONE`); no error is logged.
- **S-024-R19** `obs_key_to_name` for an out-of-range id (>= 636 or negative) logs an ERROR and returns `""`.
- **S-024-R20** `obs_key_from_name(NULL)` returns 0 (D-3: the reference crashes on one of its two code paths).
- **S-024-R21** `obs_hotkey_name_map_free` releases the lazily built map; after it the map MUST be rebuilt on demand (the reference leaves a dangling pointer; calling lookups after it is undefined there, so no vector does).

### M5 missing files
- **S-024-R22** `create` -> record with refcount 1, copied path, source name copied only for SOURCE records; `get_path`/`get_source_name` return those.
- **S-024-R23** `list_create` -> empty list (`count 0`); `add_file` appends at the end without changing the refcount; `get_file(i)` returns the i-th appended handle (index must be `< count`).
- **S-024-R24** `append(dst, src)` appends src's records to dst in order and increments each record's refcount; src is unchanged; repeated appends repeat entries.
- **S-024-R25** `list_destroy` releases each entry once; a record is destroyed when its count reaches 0; `release(NULL)` does nothing.
- **S-024-R26** `issue_callback` on a SOURCE record invokes `callback(source context data, new_path, user_data)` exactly once, passing `new_path` unchanged (also NULL or empty).
- **S-024-R27** `issue_callback` on a SCRIPT record does nothing (Q-5).
- **S-024-R28** Preconditions not covered by vectors: `list_destroy(NULL)` and `append(l, l)` (infinite growth) are invalid (D-5).

### M6 task queue
- **S-024-R29** Tasks run on the queue's worker thread, one at a time, in FIFO order of `queue_task` calls (with the exception of rule R31).
- **S-024-R30** `queue_task(NULL, ...)` -> false; `wait(NULL)` -> false; `destroy(NULL)` no-op; `queue_task` on a live queue -> true.
- **S-024-R31** A task queued from within a running task is placed behind any pending *wait* marker, but the marker is deferred behind it, so the new task still runs before `wait()` returns.
- **S-024-R32** `wait(tq)` returns true iff the worker processed at least one user task after the wait started; on an idle queue it returns false; it returns only after all tasks queued before it have run.
- **S-024-R33** `destroy(tq)` runs all tasks already queued, then joins the worker.
- **S-024-R34** `inside(tq)` is true on `tq`'s worker thread only; on the caller's thread, or on another queue's worker, it is false.

### M7 profiler
- **S-024-R35** Sample values are `(ns + 500) / 1000` (integer) and are aggregated per distinct µs value; snapshot `times` are sorted by descending µs.
- **S-024-R36** Entries are matched by name identity (Q-7); roots are listed in creation order; children in order of first appearance under their parent; a child's samples are aggregated over all calls of the root.
- **S-024-R37** `min/max/overall_count` per entry as in 4.7; for an entry without samples `min = u64::MAX`, `max = 0`, `count = 0` (Q-9).
- **S-024-R38** `register_root(n, ns)` creates/updates root `n` with `expected = (ns+500)/1000` µs (the last registration wins); between-call samples exist only for roots with `expected != 0`, only from the second completed call on, and measure start-to-start distance.
- **S-024-R39** While the profiler is stopped (default) `register_root` has no effect; a root call that completes while stopped is discarded and disables the calling thread until `reenable_thread()` is called while running (Q-8). `profile_start` of a disabled thread and its `profile_end` do nothing.
- **S-024-R40** `profile_end` with no open call logs an ERROR and does nothing. A mismatching name logs an ERROR; if an ancestor has that name the open calls are closed innermost first (the inner calls get the clock value at their own closing), otherwise nothing happens.
- **S-024-R41** Snapshots are independent copies; `filter_roots` follows the loop of 4.7; all accessors accept NULL (0/NULL).
- **S-024-R42** `profiler_print` output (MUST, byte exact): header, entry lines per 4.7, footer. Line shape: single value (`min == max`) vs. full statistics; `, P% below E ms` only in the full form and only with `expected != 0`; `, X calls per parent call` when `lround(10 * calls/parent_calls) != 10` and `calls != parent_calls`.
- **S-024-R43** Tree drawing characters per 4.7 (`┣`, `┃`, `┗`, two spaces).
- **S-024-R44** Statistics and the median quirk per 4.7 (Q-10).
- **S-024-R45** `profiler_print_time_between_calls` (MUST, byte exact) per 4.7: only roots with an expectation, window `[expected*0.98, expected*1.02+0.5]` truncated to integers, percent/lower/higher buckets.
- **S-024-R46** `profile_store_name` returns a new name per call.
- **S-024-R47** CSV dump layout per 4.7 (modulo the id tokens).
- **S-024-R48** `profiler_free` disables the profiler and discards all data; it may be called once.
- **S-024-R49** Duration arithmetic is unsigned: a call whose end is earlier than its start produces a huge value (no vector; do not rely on it).

### M8 BPM
- **S-024-R50** Only video keyframe packets with timing are modified; audio, non-key packets, missing timing and NULL arguments leave the packet untouched and do not advance counters.
- **S-024-R51** Counters are reported as `curr - ref` (u32 wrapping), with `ref` starting at 0 per `(output, track)`; `pts == 0` reports all zeros but still stores the references.
- **S-024-R52** Timestamp validity rules of 4.8 (FER > CTS, FERC > FER, PIR/CTS always); ms truncated.
- **S-024-R53** The three payload layouts of 4.8 (count byte = number-1 in low 4 bits; big-endian counters; NUL-terminated strings).
- **S-024-R54** SEI framing: NAL byte 06, payload type 05, 255-extension size (unreachable for BPM payloads, which are < 255 bytes), emulation prevention (visible when counters are 0: `00 00 00 00` becomes `00 00 03 00 00`), trailing 80.
- **S-024-R55** AVC: 4-byte start code before each SEI; HEVC: 3-byte start code + 2-byte header + SEI body; AV1: metadata OBU type 6.
- **S-024-R56** HEVC without an Annex-B start code: packet unchanged, counters advanced.
- **S-024-R57** Unknown codec: packet bytes unchanged in value (Q-11), counters advanced.
- **S-024-R58** Encoder without video handle: rendition counters 0, injection continues.
- **S-024-R59** `bpm_destroy(output)` discards all tracks of the output; per-track and per-output state is independent.

### M9 happy eyeballs
- **S-024-R60** Reordering algorithm of 4.9 (single pass, early exit, other families inert).
- **S-024-R61** Resolver hints: family by bind length (16 -> IPv4, 28 -> IPv6, else unspecified), stream/TCP, port as decimal text.
- **S-024-R62** A resolver error yields status -1 and an empty list. Constants 200 / 6 / 25000.

### M10 file updater
- **S-024-R63** Version decision for local seeding: files are copied iff cache package version < local package version, and per file iff missing in the cache package or strictly newer; then `package.json` is copied.
- **S-024-R64** Copy sequence local -> temp -> (unlink cache) -> rename cache.
- **S-024-R65** Remote step runs only when the remote package version is strictly greater than the current version; files are fetched iff missing or strictly newer than the cache package entry.
- **S-024-R66** Callback: sees name, version and `size = len+1`; may reject (file skipped) or replace the buffer (written bytes = replaced buffer minus terminator); absent callback = accept.
- **S-024-R67** HTTP failure of the package request (>= 400 or transfer error) logs a warning and ends the remote step; 304 ends it silently; a body not starting with `{` is rejected; invalid JSON logs and stops (after the ETag was saved); a failed file download skips only that file.
- **S-024-R68** ETag header (see Q-16 for the request side): exact prefix `ETag: `, cut at CR then LF, non-empty, only for the package request; stored as compact JSON `{"etag":...}` at `cache + "meta.json"` (Q-12); sent as `If-None-Match` next time (when found), before `User-Agent`.
- **S-024-R69** `get_path` rules of 4.10.
- **S-024-R70** `temp` directory is `cache + sep + ".temp"` and is removed at the end (`rmdir`); the package is installed into the cache last.
- **S-024-R71** Missing `url` yields the empty base (Q-14). Single-file mode per 4.10.

### M11 constants
- **S-024-R72** Every constant of the table has the listed value; `LIBOBS_API_VER = (33<<24)|(0<<16)|1`.
- **S-024-R73** `MAKE_SEMANTIC_VERSION` is the unmasked bitwise OR (Q-13); a port provides it as a `const fn` that may mask or reject out-of-range input but MUST equal the C value in range (major < 128, minor < 256, patch < 65536).

## 6. Concurrency

M6 is the only module with a thread of its own (one per queue). Task bodies must not call `wait(tq)` on their own queue (deadlock) nor `destroy`. M7 has a global lock `root_mutex` (state, root list), a per-root lock (merging), and thread-local state;
`profile_start/end` take no lock while a call is open (only the root merge does). M8 has one global lock (track table) and a lock per track. M10 runs entirely on one thread per `update_info`. M1-M5, M9 ordering: single-threaded.

## 7. Reference defects and quirks

| ID | Kind | Input / behaviour | Required behaviour | Script |
|---|---|---|---|---|
| Q-1 | QUIRK | opts: no quoting; `a="b c"` splits into `a="b` (option, value `"b`) and `c"` (ignored) | match | opts-noquoting |
| Q-2 | QUIRK/DEFECT | `util_mul_div64` non-MSVC overflow gives a wrapped value | return the exact value (u128 intermediate) | muldiv64-quirk-overflow (compare field `exact=`) |
| D-1 | DEFECT | division by zero (SIGFPE) in `mul_div64` / `div128_32` | precondition; do not return a value | u128-defect-div0 |
| Q-3 | QUIRK | NULL executable/arg stored as NULL element | SHOULD match | args-quirk-null |
| D-4 | DEFECT | `add_argf` with empty expansion stores NULL | store `""` | args-defect-argf-empty (expected `@null` below `argc` == `""`) |
| D-3 | DEFECT | `obs_key_from_name(NULL)` crashes without core; negative key id is an out-of-bounds read with signed enums | return 0 / `""` + error | hotkeys-defect-null, hotkeys-lookup |
| Q-5 | QUIRK | script records never notified | SHOULD match | missing-files-callback |
| D-5 | DEFECT | list destroy NULL, append to itself | invalid input | -- |
| Q-6 | QUIRK | `wait` returns false when nothing ran after it started (even if tasks ran before) | match | task-wait-idle |
| Q-7 | QUIRK | profiler names compared by pointer | identity semantics | prof-names-identity |
| Q-8 | QUIRK | completing a root call while stopped latches the thread off; `register_root` while stopped is a no-op | match | prof-enable |
| Q-9 | QUIRK | no samples -> `min = u64::MAX` | match | prof-roots, prof-between-edge |
| Q-10 | QUIRK | median stays 0 when the 1 % and 50 % thresholds are crossed by the same entry (`else if`); between-report median uses a different rule | match | prof-print, prof-between-stats |
| Q-11 | QUIRK | bpm unknown codec: packet buffer replaced with an identical copy | bytes identical | bpm-other-codec |
| Q-12 | QUIRK | meta.json written to `cache + "meta.json"` without separator but read from `cache/meta.json` | SHOULD match | fu-quirk-meta-path |
| Q-13 | QUIRK | `MAKE_SEMANTIC_VERSION` unmasked; macro arguments are not parenthesised (`MAKE_SEMANTIC_VERSION(a+1,b,c)` mis-associates in C) | in-range equal | const-semver |
| Q-14 | QUIRK | missing `url` in the remote package -> empty base, request goes to the bare file name | SHOULD match | fu-remote-invalid |
| Q-16 | QUIRK | an existing, parsable `meta.json` without `etag` still produces `If-None-Match: ` (empty value) | SHOULD match | fu-etag-meta-noetag |
| Q-15 | QUIRK | the profiler sorts empty arrays with `qsort(NULL, 0)` | none (UB-harmless) | prof-roots |

## 8. Implementation notes (non-normative)

Rust: model profiler names as `Copy` ids from a `NameStore`; use `BTreeMap<u64,u64>` per entry for samples (hash-table internals are unobservable);
model the BPM wall clock as a trait or injected offset; implement SEI framing with a small `Vec<u8>` writer. Task queue: `std::thread` + `Mutex<VecDeque>` + `Condvar`; keep the wait-marker rule exactly. M10: abstract HTTP and the file system behind traits so the vector trace can be reproduced.
The hotkey table and the constants table are best generated from the vectors by a `build.rs` or a checked-in generated file.

## 9. Conformance coverage

| Rules | Scripts |
|---|---|
| R01-R07 | opts-basic, opts-ignored, opts-empty, opts-noquoting |
| R08-R10 | u128-add, u128-mul64, u128-div32, u128-shift, muldiv64, muldiv64-quirk-overflow, u128-defect-div0 |
| R11-R16 | args-basic, args-argf, args-multiple, args-quirk-null, args-defect-argf-empty |
| R17-R21 | hotkeys-fallback-table, hotkeys-hash-table, hotkeys-lookup, hotkeys-defect-null |
| R22-R28 | missing-files-basic, -append, -append-order, -release, -callback, -quirk-fields |
| R29-R34 | task-order, task-wait-idle, task-requeue, task-destroy, task-inside, task-null |
| R35-R49 | prof-basic, -nested, -print, -print-expected, -between, -between-stats, -between-edge, -enable, -names-identity, -mismatch, -unwind, -burst, -filter, -csv, -roots |
| R50-R59 | bpm-avc-basic, -counters, -timestamps, -skip, -hevc, -av1, -other-codec, -tracks, -novideo |
| R60-R62 | he-order, he-order-edge, he-resolve-args |
| R63-R71 | fu-local-copy, -local-partial, -local-current, -remote-update, -remote-notnewer, -remote-reject, -remote-nocallback, -etag, -etag-forms, -quirk-meta-path, -remote-invalid, -single, -get-path, -etag-meta-noetag |
| R72-R73 | const-table, const-semver |

## 10. Open questions

1. BPM wall-clock base: the reference captures `(wall, monotonic)` at first use; the vectors fix `wall(ns) = ns`. Behaviour for times before the capture instant (negative offset) is untested.
2. `meta.json` escaping of non-ASCII ETags (jansson compact encoder) is untested; ETags are ASCII in practice.
3. `profiler_snapshot_dump_csv_gz` and a profiler used from several threads are not covered by vectors.
4. Behaviour of M5/M3 under allocation failure and of M6 thread-creation failure (returns NULL) is not covered.
