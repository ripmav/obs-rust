# Test matrix: `bitstream` (obs-util)

- Test files: `crates/obs-util/src/bitstream.rs` (unit tests), `crates/obs-ffi/src/abi_bitstream.rs` (C-ABI bridge tests)
- Task: P1-14.2 (spec S-010, R50–R54, Q-3, Q-4)
- Golden files: none (C conformance vectors `test/conformance/scripts/serial/`)

| Case                        | Input                                                           | Expected                                                                                                                                                       | Platforms             | Status |
| --------------------------- | --------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------- | ------ |
| R50 init state              | `init(data, 5)`                                                 | `pos = 0`, `sub_pos = 0`, `buf`/`len` stored; reading at `len` yields 0                                                                                        | Linux, Windows, macOS | pass   |
| R51 MSB-first + end         | read 8/1/3/4 bits of `34 ff e1 23 91 45` (len 5)                | `0x34, 1, 7, 0xF` — MSB-first; `len` boundary stops further reads (Q-4 end case)                                                                               | Linux, Windows, macOS | pass   |
| R52 n=0/negative/wide (Q-4) | `read_bits(0)`, `read_bits(-1)`, `read_bits(n > 8)`             | 0 / 0 / all `n` bits consumed but only the **last 8** retained (Q-4)                                                                                           | Linux, Windows, macOS | pass   |
| R53 r8/r16 big-endian       | `r8`, `r16` mid-stream; past the end                            | 8/16-bit big-endian reads; past the end bytes read as 0 (a partial `r16` keeps the already-read leading bytes, e.g. `0x3400`; cmocka `test_bitstream.c` shape) | Linux, Windows, macOS | pass   |
| Q-3 8-bit pos wrap          | reader over `len > 256` and `len = 256`                         | `pos` is 8 bits: reading wraps to byte 0 after byte 255 and **never reaches the end** for `len ≥ 256`; `len = 255` ends correctly (reproduced, `wrapping_add`) | Linux, Windows, macOS | pass   |
| R54 len 0 / clamped         | `len = 0`; reads past `len`                                     | all reads return 0; no access past `len`                                                                                                                       | Linux, Windows, macOS | pass   |
| C-ABI shape (bridge)        | `bitstream_reader_init`/`read_bits`/`r8`/`r16` via `extern "C"` | matches the C `struct bitstream_reader` field layout and the cmocka `test_bitstream.c` expectations (linked against the staticlib, passes unchanged)           | Linux, Windows, macOS | pass   |
| Q-3 through C ABI           | `len > 256` reader through the bridge                           | same wrap behaviour as the Rust unit test                                                                                                                      | Linux, Windows, macOS | pass   |

Notes:

- C-parity source of truth: `libobs/util/bitstream.c` (51 lines); the
  cmocka test `test/cmocka/test_bitstream.c` passes unchanged against the
  Rust staticlib (WP-32).
- `bitstream_reader_read_bit` is exported for the C inline helpers but
  excluded from the cbindgen header (test/linkable symbol, not part of the
  33.0.1 public header).
- Conformance: `serial` area 28/28 in swap mode (Rust staticlib replaces
  `bitstream.c` + the serializer sources).
