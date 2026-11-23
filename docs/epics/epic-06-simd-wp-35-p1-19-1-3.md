# E6 — SIMD (WP-35, P1-19.1–.3)

**Milestone:** M1 — Phase 1: code & recipe

SIMD (WP-35, P1-19.1–.3)
**Why:** `graphics` math performance parity before Phase 3.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E6S1** Intrinsic inventory (P1-19.1) | list of the intrinsics in use | list merged: `sse-intrin.h` + simde use in `libobs/graphics/*.h` covered | open |
| **E6S2** Implementation (P1-19.2) | `std::arch` SSE2/AVX2 with runtime check + NEON | scalar fallback results equal, on x86_64 and aarch64 | open |
| **E6S3** Benchmark (P1-19.3) | measured parity vs C | "not slower than C" on x86_64 **and** aarch64, using the E11S1 harness/baselines | open |
