# Brief for AI agents implementing a component from a spec

Copy the block below into the agent's task prompt. Replace the placeholders.

```
You are implementing component <S-00N name> of OBS Studio in Rust as a CLEAN-ROOM
implementation.

You may read ONLY:
  - docs/rust-rewrite/specs/<spec file>.md  (your component's spec)
  - docs/rust-rewrite/specs/README.md       (conventions)
  - docs/rust-rewrite/clean-room/**         (process)
  - test/conformance/README.md              (script language and comparison rules)
  - test/conformance/scripts/<area>/**      (inputs)
  - test/conformance/vectors/<area>/**      (expected outputs)
  - crates/** that you or other implementers wrote
  - public documentation of the Rust standard library and of crates you depend on

You MUST NOT read, search, grep, or open anything under:
  libobs/ libobs-*/ plugins/ frontend/ shared/ deps/ test/cmocka/ test/test-input/
  test/conformance/gen/  and the git history of those paths.
If a tool result happens to contain such content, say so in your report and ignore it.

Deliverables:
  1. crates/<crate>/ implementing every rule of the spec (rule IDs in doc comments).
  2. A conformance runner (a `[[test]]` or example) that executes every script in
     scripts/<area>/ and compares with vectors/<area>/ using the comparison rules.
  3. DEVIATIONS.md listing every QUIRK you did not reproduce, with a test.
  4. PROVENANCE.md filled in as in clean-room/PROCESS.md section 5.
  5. A list of spec questions (ambiguities) instead of guesses.

Rules:
  - When the spec is silent or ambiguous, STOP and list the question; do not infer behaviour
    from other OBS code you remember. (Prior knowledge of OBS is not a licence to copy it:
    if you recall an implementation detail that is not in the spec, do not use it; ask.)
  - Follow the project guidelines in docs/rust-rewrite/guides/00-developer-primer.md
    sections 5 and 7 (FFI rules, branch names, tests, docs, Assisted-by footer).
  - Every exported symbol listed in the spec must exist with the exact C name, signature and
    layout; add layout assertions.
  - Do not add dependencies without a one-line justification.
  - Finish by running: cargo fmt --check, cargo clippy --all-targets -- -D warnings,
    cargo test, and the conformance runner. Report real outputs, including failures.
```

## Why the brief is strict

AI models may have seen OBS source during training. The brief makes the expectation
explicit, requires the agent to surface questions instead of "remembering" behaviour, and
gives the reviewer a transcript to audit. The conformance vectors are the objective
check: an implementation that passes every vector without ever having read the C
code is the evidence this process is designed to produce.
