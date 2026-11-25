# Test matrix: `serializer` family (obs-util)

- Test files: `crates/obs-util/src/serializer.rs` (unit tests), `crates/obs-ffi/src/abi_serializer.rs` (C-ABI bridge tests)
- Task: P1-14.2 (spec S-010, R20–R44, Q-8, Q-9)
- Golden files: none (C conformance vectors `test/conformance/scripts/serial/`)

| Case                           | Input                                                                                        | Expected                                                                                                                                                                                                                                                                                      | Platforms             | Status |
| ------------------------------ | -------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------- | ------ |
| R20/R30/R40 init missing       | `FileInput`/`FileOutput`/`BufferedFileOutput` on missing paths/dirs                          | all inits return `None`; nothing created                                                                                                                                                                                                                                                      | Linux, Windows, macOS | pass   |
| R21/R22 file input vector      | 11-byte file `00..0a`; reads, CURRENT/START/END seeks, EOF, empty file                       | reads honour `len`; **END adds the offset to the file size** (Q-8); negative resulting positions fail with the position unchanged; past-end reads 0                                                                                                                                           | Linux, Windows, macOS | pass   |
| Q-8 file vs buffered SEEK_END  | file `seek(-2, END)` on size 11; buffered `seek(1, END)` at pos 8                            | file: `11 - 2 = 9` (ADD to size); buffered: `8 - 1 = 7` (**subtract from the current write position**, not the file size)                                                                                                                                                                     | Linux, Windows, macOS | pass   |
| R30/R31 file output vector     | truncate on open; seek-overwrite; seek to 8 then write; re-open                              | truncate on open (R30); END adds to the **current** file size; holes zero-fill; `get_pos` is the logical position; re-open truncates the previous content                                                                                                                                     | Linux, Windows, macOS | pass   |
| R33/R34 safe temp file         | ext with/without leading `.`; path already ending in `.`; empty ext                          | writes to `<path>.<ext>` (a leading `.` is not duplicated; a trailing-`.` path **gets a second dot** — the name is built from `temp_ext` alone); `free` replaces the target with the temp content; empty ext or missing dir fails with the target untouched; no writes → target becomes empty | Linux, Windows, macOS | pass   |
| R40/R41 buffered write order   | vector `buffered_seek` bs-a sequence; chunked writes (max_bufsize 1); 256 MiB/1 MiB defaults | writes applied in call order over a virtual buffer; holes zero-fill; `free` flushes the whole file (1024-byte start capacity without growth); chunk size only changes the piece count, the file is byte-identical; zero-size write → 0                                                        | Linux, Windows, macOS | pass   |
| R42/Q-9 buffered seek no range | `seek(100, START)` at pos 10; `seek(1000, CURRENT)`; negative START                          | **no range check** (Q-9): far-past seeks succeed; the negative START returns a negative number and the **next write lands after the last flushed chunk** (reference behaviour); a far hole is zero-filled after `free`                                                                        | Linux, Windows, macOS | pass   |
| R43 get_pos logical            | pos after interleaved writes/seeks                                                           | `get_pos` is the logical write position, not the flushed file position                                                                                                                                                                                                                        | Linux, Windows, macOS | pass   |
| R44 virtual capacity growth    | writes beyond the start capacity                                                             | the virtual buffer grows (1 MiB default); `free` returns the total bytes written                                                                                                                                                                                                              | Linux, Windows, macOS | pass   |
| C-ABI shape (bridge)           | `array_output_serializer_init/free`, `buffered_file_serializer_init/free` via `extern "C"`   | the `serializer`/`array_output_data` structs match the C layout; `s_w8` through the bridge appends to the C-side DARRAY in place (the cmocka `test_serializer.c` shape, linked against the staticlib, passes unchanged)                                                                       | Linux, Windows, macOS | pass   |
| buffered init/free round-trip  | buffered serializer init → write → free through the bridge                                   | file content matches the Rust-side vector; no leak (drop-leak counters)                                                                                                                                                                                                                       | Linux, Windows, macOS | pass   |

Notes:

- C-parity source of truth: `libobs/util/{serializer.h,array-serializer.c,file-serializer.c,buffered-file-serializer.c}`.
  The cmocka tests `test/cmocka/test_bitstream.c` and `test/cmocka/test_serializer.c`
  pass unchanged against the Rust staticlib (WP-32).
- The array serializer backend writes into the C-owned DARRAY in place
  (S-005 byte-wise element copy, Q-A2); the file and buffered backends own
  their state in Rust (`Box`) and are synchronous — R44 logs one
  `LOG_DEBUG` line on growth (mirrored). `serializer_get_pos` is a static
  inline in the C header (no export needed).
- The bridge exposes exactly the 33.0.1 header exports: 3 (`array-serializer.h`)
  - 5 (`file-serializer.h`) + 3 (`buffered-file-serializer.h`) = 11,
    which with the 4 bitstream and 4 darray declarations brings the
    cbindgen header diff from 45 to 64 (WP-32).
- Conformance: `serial` area 28/28 in swap mode (Rust staticlib replaces
  `bitstream.c` + the three serializer sources).
