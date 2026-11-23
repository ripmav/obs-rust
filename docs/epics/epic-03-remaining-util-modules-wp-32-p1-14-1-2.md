# E3 — Remaining `util` modules (WP-32, P1-14.1/.2/.4 + P1-03.2 + P1-04.2 + P1-02.4)

**Milestone:** M1 — Phase 1: code & recipe

Remaining `util` modules (WP-32, P1-14.1/.2/.4 + P1-03.2 + P1-04.2 + P1-02.4)
**Why:** closes the porting gaps the audit found inside "done" areas (darray/deque C helpers) and finishes the serializer family.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E3S1** Serializers & bitstream (P1-14.2) | port `bitstream`, `array-serializer`, `file-serializer`, `buffered-file-serializer` per S-010 | [test_bitstream.c](https://github.com/ripmav/obs-rust/blob/main/test/cmocka/test_bitstream.c) + [test_serializer.c](https://github.com/ripmav/obs-rust/blob/main/test/cmocka/test_serializer.c) pass unchanged against the Rust build; S-010 Q-3 (8-bit `pos` wrap) and Q-8 (three SEEK_END conventions) have dedicated tests | open |
| **E3S2** darray C helpers (P1-03.2) | the functions behind `da_push_back`/`da_insert`/`da_erase`/`da_move_item` | [test_darray.c](https://github.com/ripmav/obs-rust/blob/main/test/cmocka/test_darray.c) (31 cases) passes unchanged; S-005 Q-A2: `da_find` `memcmp`s the whole element incl. padding — mirrored byte-wise with a test | open |
| **E3S3** Deque layout sharing (P1-04.2) | C inline code driving a Rust-owned `struct deque` | a C test builds a Rust-owned deque, drives it with the C inline functions, and gets the same dump as the [deque/](https://github.com/ripmav/obs-rust/tree/main/test/conformance/scripts/deque) vectors | open |
| **E3S4** `util` inventory & curl note (P1-14.1, P1-14.4) | keep/port/drop decision for every remaining `libobs/util` file; curl stays FFI until Phase 6 | inventory table merged (every P1-14 file has a decision); curl FFI note recorded in [ADR 0005](../adr/0005-code-policy.md) | open |
| **E3S5** dstr fuzz target (P1-02.4, code half) | `cargo-fuzz` target applying random op sequences to Rust and C builds | fuzz target runs clean; recipe half (CMake exclusion) is ticked by E13S1 | open |
