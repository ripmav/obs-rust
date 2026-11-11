# Test matrix: crc32 / blake2b (obs-util)

- Test files: `crates/obs-util/src/hash.rs` (unit + KAT)
- Task: P1-10.2 / P1-10.3 (spec S-055 U1; blake2 is vector-only, no spec
  section — the subtask names the KAT requirement)
- Golden files: none (KAT vectors embedded; generated with an independent
  implementation: CPython `hashlib.blake2b` / `zlib.crc32`)

| Case | Input | Expected | Platforms | Status |
|---|---|---|---|---|
| crc32_worked_examples_from_spec | `"123456789"`, `"1234"`, `"1"`, continuation | `cbf43926` / `9be3e0a3` / `83dcefb7`; `calc_crc32(9be3e0a3, "56789") == cbf43926` — S-055-R01 worked values | Linux, Windows, macOS | pass |
| crc32_empty_and_seeds | empty slice, seed `0xdeadbeef`, 4096-byte pattern whole vs split-continue | seed returned unchanged (R02); split continuation == whole == `97b84ec2` (CPython zlib) | Linux, Windows, macOS | pass |
| blake2b_known_answers | `""`, `"abc"`, 64/65 zeros, 111618 zeros, 1000-byte pattern | 64-byte digests match CPython `hashlib.blake2b` (incl. the 1- and 2-block boundaries) | Linux, Windows, macOS | pass |

Notes:
- `calc_crc32(crc, buf)` maps 1:1 onto the C
  `uint32_t calc_crc32(uint32_t crc, const void *buf, size_t size)`
  (S-055-R01/R02): zlib-compatible IEEE CRC-32, running finished value
  passed in and returned, table driven via `crc32fast`
  (`Hasher::new_with_initial`).
- `blake2b_512(data)` is the Rust side of the vendored `deps/blake2`;
  the frontend keeps the C copy until Phase 6 (P6-32/P6-23), so no
  `extern "C"` shim is needed in Phase 1.
- Dependencies admitted per ADR 0005 log: `crc32fast` 1.5.x (MIT),
  `blake2` 0.11.x (MIT) — `cargo deny check` green in this WP.
- P1-10.1 base64: **skip** (no base64 in `libobs/`; the only in-tree copy
  is the librtmp vendored `cencode.c`, a Phase 5 concern). Decision noted
  in ADR 0005; the absence re-verification grep is the C-checkout substep.
- C-side remainder: none beyond the P1-10.4 apply recipe (CMake/obs-ffi
  wiring for `crc32`) — blocked on the C checkout.
