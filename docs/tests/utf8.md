# utf8 extended UTF-8 / wide conversion (S-023 §4.1, §5.1–5.3) — test matrix

WP-17 (P1-08.1), crate `crates/obs-util`, module `utf8`
(`src/utf8.rs`). Spec: `S-023-platform-utf8.md` §4.1 (byte tables), §5.1
decode (R01–R12), §5.2 encode (R13–R17), §5.3 conversions with termination
(R18–R20). "wchar" is the reference's 32-bit `wchar_t`, mirrored as `u32`
(negative values are unreachable through the safe API; the C ABI shim must
check the sign per R15c). The locale `os_mbs_*` conversions (R21/R22) stay
in the C shim.

| Area | Pinned by test | Conformance vector |
| --- | --- | --- |
| **decode valid (R02, R07, R11)** | | |
| ASCII + terminator mode (00 ends input, not converted) + explicit length (embedded 00 is a character) + empty input | `d_ascii_and_terminator` | `01-utf8-decode-valid` |
| spec worked examples: `C3 A9` = U+00E9, `E2 82 AC` = U+20AC, `F0 9F 98 80` = U+1F600, `F4 8F BF BF` = U+10FFFF | `d_worked_examples_from_spec` | `01-utf8-decode-valid` |
| Q-1 lenient forms: `F4 90 80 80` = U+110000, `FD BF BF BF BF BF` = 0x7FFFFFFF, overlong `E0 80 80` = 0 / `F0 80 80 AF` = 0x2F, 5-byte `F8 88 80 80 80` = U+200000 | `d_q1_lenient_forms` | `03-utf8-lenient-forms` |
| roundtrip of 13 boundary values across all six forms | `d_roundtrip_all_forms` | `01-utf8-decode-valid`, `06-utf8-encode` |
| **decode invalid (R03–R07, R09)** | | |
| forbidden leads `C0 C1 F5 FF` -> 0 (strict) | `d_forbidden_leads` | `02-utf8-decode-invalid` |
| no lead bytes (`80..BF`, `FE`) -> 0 (R04) | `d_no_lead_bytes` | `02-utf8-decode-invalid` |
| truncation: explicit-length (R05) and terminator mode (caught by R06) | `d_truncation` | `02-utf8-decode-invalid` |
| bad continuation bytes (R06) | `d_bad_continuations` | `02-utf8-decode-invalid` |
| surrogates `ED A0 80` / `ED AF BF` -> 0 (R07); first error ends the call even after written chars (R09) | `d_surrogates_and_error_returns` | `02-utf8-decode-invalid` |
| **decode flags (R03–R08; Q-2, Q-3)** | | |
| IGNORE: strays skipped, `C0/C1/F5` decoded (Q-2: `61 C0 80` -> 61, 0; `C1 80` -> 0x40; `F5 80×4` -> 0x1000000), FF still skipped (R03->R04), one-byte skip with re-examination (Q-3: `E2 82 22` -> 0x22), surrogate dropped (R07), nothing left -> 0 (R09) | `d_ignore_error` | `04-utf8-decode-flags` |
| BOM: ordinary char without flag, dropped with SKIP_BOM (R08), both flags together | `d_bom_handling` | `04-utf8-decode-flags` |
| **capacity + sizing (R01, R10, R12; Q-4, Q-5)** | | |
| outsize 0 -> 0 (R01), exact fit, too small (R10) | `w_capacity` | `04-utf8-decode-flags` |
| Q-5: a dropped char (surrogate/BOM) still needs a free slot when reached | `q5_dropped_character_still_needs_slot` | `05-utf8-sizing-quirks` |
| R12 sizing: counts surrogates/BOM (Q-4), can succeed where writing fails, larger than the IGNORE writing result, honours the terminator | `s_sizing_mode` | `05-utf8-sizing-quirks` |
| **encode (R13–R17; Q-6)** | | |
| spec worked examples: `41 7F`, U+00E9 -> `C3 A9`, U+20AC -> `E2 82 AC`, U+1F600 -> `F0 9F 98 80`, U+10FFFF -> `F4 8F BF BF` | `e_worked_examples_from_spec` | `06-utf8-encode` |
| Q-6 above 10FFFF: U+110000 -> `F4 90 80 80`, U+200000 -> `F8 88 80 80 80`, 5-byte boundary U+3FFFFFF -> `FB BF×4`, 6-byte start U+40000000 -> `FD 80×5`, max 0x7FFFFFFF -> `FD BF×5` | `e_q6_above_10ffff` | `06-utf8-encode`, `03-utf8-lenient-forms` |
| surrogates -> 0 (R15a); check order: surrogate check before BOM (SKIP_BOM alone does not pass a surrogate); IGNORE drops; SKIP_BOM drops BOM; fully skipped -> 0 (R17) | `e_errors_and_flags` | `07-utf8-encode-flags` |
| R14 length modes: explicit (0 = ordinary char, encoded as `00`) vs terminator; empty -> 0 (R17) | `e_length_modes` | `06-utf8-encode` |
| R16 no partial result (3-byte element in 2 bytes -> 0, exact fit -> 3), R13 outsize 0, R13 sizing (1 + 4 = 5; surrogate -> 0) | `e_capacity_and_sizing` | `06-utf8-encode`, `07-utf8-encode-flags` |
| **conversions with termination (R18–R20; D-1)** | | |
| `os_utf8_to_wcs`: writing + NUL, explicit len, len 0/None = terminator, invalid -> empty string + 0, dst_size 0/1, D-1: text of exactly dst_size chars -> empty string + 0 (no overflow), sizing form (surrogate counted, Q-4) | `o2w_os_utf8_to_wcs` | `08-os-wcs-utf8`, `36-null-arguments` |
| `os_wcs_to_utf8`: writing + NUL, dst_size-1 capacity (needs dst_size or more -> empty + 0), invalid, dst_size 0/1, explicit len (0 = ordinary char), terminator, sizing | `o2u_os_wcs_to_utf8` | `08-os-wcs-utf8`, `36-null-arguments` |
| `_ptr` forms (R20): exactly converted units + terminator, invalid -> empty string (not NULL), empty input, explicit len, full roundtrip across all six forms | `p_ptr_forms` | `08-os-wcs-utf8`, `36-null-arguments` |
| **C-checkout steps (S2, not ticked)** | | |
| C conformance scripts `platform/01..08-utf8*`, cbindgen diff vs `utf8.h`, `OBS_ENABLE_RUST` build with C file excluded | — | `platform/01-utf8-decode-valid` … `08-os-wcs-utf8` |
