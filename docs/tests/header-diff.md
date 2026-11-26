# header_diff (P1-29.1/.2, WP-31) — test matrix

- Test file / issue: `crates/obs-ffi/tests/header_diff.rs`
- Task: P1-29
- Golden files: none

The test generates the obs-ffi C header with cbindgen (dev-dependency,
config `crates/obs-ffi/cbindgen.toml` — `language = "C"`,
`cpp_compat = true`, no function prefix, exclusion list for the 12
non-bridge declarations). The declaration diff lives in
`test/conformance/cbindgen-header-diff.py` (the script drives this test
for the generation half; see its module docstring for scope,
normalisation and the out-of-scope list).

| Case              | Input                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | Expected                                                                                                                                                                                                                                                                                                                                                                                                                                              | Platforms                         | Status |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------- | ------ |
| `generate_header` | the `#[no_mangle] extern "C"` bridges (incl. the WP-37 fifteen `source_profiler_*` exports) + the `cbindgen.toml` config                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           | cbindgen writes the header to `$HDR_DIFF_OUT` (or `target/obs-ffi-generated.h`); the header declares the bridges and the `#[repr(C)]` types they use                                                                                                                                                                                                                                                                                                  | Linux, Windows, macOS             | pass   |
| header diff       | generated header vs the C reference declarations: `util/text-lookup.h` (4 EXPORT), `util/threading.h` (14), `util/lexer.h` (10 EXPORT), `util/cf-lexer.h` (9 EXPORT), `util/cf-parser.h` (2 EXPORT), `util/platform-nix.c` (6 extern dbus\_/portal), and the WP-32 additions `util/bitstream.h` (4 EXPORT), `util/array-serializer.h` (3), `util/file-serializer.h` (5), `util/buffered-file-serializer.h` (3), `util/darray.h` (4 static inlines — the P1-03.2 helpers), the WP-34 `shared/` helper headers (bpm 2, opts-parser 2, file-updater 3) and the WP-37 additions `util/source-profiler.h` (4 EXPORT) + `obs-internal.h` (11 extern `source_profiler_*`) | the mismatch report is empty (86/86 signatures match after whitespace/parameter-name/default-argument normalisation + the opaque-typedef/enum/`size_t` alias table, incl. the `darray` → `darray_c` mirror alias and the WP-37 `obs_source_t`/`gs_timer_t`/`obs_video_info` → `void`, `profiler_result_t` → `struct profiler_result` aliases); a deliberate type drift, a removed or added bridge reds the report (negative self-checks done locally) | linux-x86_64 (CI conformance job) | pass   |

Notes: the diff is linux-only by design — the CI step runs in the ubuntu
conformance job with the pinned obs-studio 33.0.1 checkout, and the
reference declarations are the unix ones (`platform-nix.c` has no header
in the 33.0.1 tree; the diff scope is its `GIO_FOUND` extern block).
The Windows/macOS runs of `generate_header` generate only (no libobs
link, no diff).

WP-32 additions (45 → 64): the P1-14.2 bridges bring the bitstream and
serializer header declarations (12), and the P1-03.2 helpers are diffed
against the four `static inline` darray helpers of `util/darray.h`
(their C reference form — in the OBS_ENABLE_RUST build the C header
calls the exported Rust functions of the same name). The `struct darray`
reference is aliased to the bridge's `darray_c` mirror (the cf-lexer
bridge owns the crate-level `darray` name). `bitstream_reader_read_bit`
and the `obs_ffi_deque_new/free` test-only symbols stay excluded from
the generated header (they are not 33.0.1 public declarations).

WP-34 additions (64 → 71): the shared-helper bridges (bpm 2, opts-parser
2, file-updater 3) diff against the plain declarations of the `shared/`
headers (no EXPORT macro — the 33.0.1 `shared/` headers are plain).

WP-37 additions (71 → 86): the source profiler brings the four EXPORT
declarations of `util/source-profiler.h` and the eleven `source_profiler_*`
externs of `obs-internal.h` (the `EXTERN_NAMES` filter selects the
profiler externs from the file's large extern set). The C opaque handles
alias to `void` (`obs_source_t`, `gs_timer_t`, `obs_video_info`) and
`profiler_result_t` to the bridge's `struct profiler_result` mirror. The
C service symbols the bridge calls (`gs_timer_*`, `obs_queue_task`, the
source accessors, the `srcprof` context shim) and the test stand-ins
`gs_enter_context`/`gs_leave_context` stay excluded from the generated
header (the cf-lexer stand-in precedent).
