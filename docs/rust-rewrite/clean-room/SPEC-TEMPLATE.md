# S-NNN <Component name>

| | |
|---|---|
| Status | Draft / Ready |
| Spec version | commit of last change |
| Reference | C implementation, libobs 33.0.1 (names of files only for the spec author; implementers do not read them) |
| Target crate | `crates/<name>` |
| Depends on | S-xxx |
| Conformance | `test/conformance/scripts/<area>/` |

## 1. Scope
What this component is for, what is in and out of scope, who calls it (plugins? libobs? frontend?).

## 2. Terminology and conventions
Words are used as in RFC 2119 (MUST, SHOULD, MAY). `W` = width of `size_t` in bytes
(8 for the committed vectors). All multi-byte values use native byte order unless stated.

## 3. Interface
Exact C names, signatures, struct layouts (field order, types, offsets for W = 8), constants,
enums (with numeric values). Everything a re-implementation must export. For each function:
signature, preconditions, effect, return value, ownership of every pointer, thread-safety.

## 4. Data formats
Byte-level layouts, with a worked example whose bytes appear in a vector.

## 5. Behaviour (numbered rules)
`S-NNN-R01 …` Each rule is testable. Group by function or by topic. State error behaviour,
logging (level, and text if the text is MUST), and process-terminating paths.

## 6. Algorithms
Original pseudocode for anything with a precise observable result: growth policies,
ordering, search order, tie-breaks, rounding, iteration under mutation.

## 7. Concurrency
Locks, which operations may run concurrently, re-entrancy, callback rules.

## 8. Reference defects and quirks
List each as `Q-n` (QUIRK) or `D-n` (DEFECT) with: input, reference behaviour, required
behaviour of a conforming implementation, script that demonstrates it.

## 9. Implementation notes (non-normative)
Suggestions for Rust (types, crates, pitfalls), memory/FFI notes.

## 10. Conformance coverage
Table: rule ID → scripts. Every rule must appear.

## 11. Open questions
Anything the author could not determine; implementers must not guess these.
