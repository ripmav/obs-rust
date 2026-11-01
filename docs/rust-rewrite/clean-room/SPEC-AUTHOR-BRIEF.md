# Brief for spec authors (humans or AI agents)

You are a **spec author** in the clean-room process ([PROCESS.md](PROCESS.md)): you may read
the C reference; your job is to produce a specification, an executable conformance suite and
vectors precise enough for someone who never reads the C to implement the component in Rust.

Study these finished examples first (they define the quality bar and style):

- Specs: `docs/rust-rewrite/specs/S-002-calldata.md` (binary format), `S-004-signals-and-procs.md`
  (stateful behaviour, quirks), `S-007-config-file.md` (parsing + typed conversion),
  `S-008-vec-quat-matrix4.md` (numerics). Template: `clean-room/SPEC-TEMPLATE.md`.
- Harness: `test/conformance/README.md`, `gen/script.h`, `gen/harness.h`, and runners such as
  `gen/cd_runner.c`, `gen/dstr_runner.c`, `gen/decl_runner.c` (fork-per-case crash capture),
  `gen/math_runner.c`.

## Deliverables (your area only)

1. `docs/rust-rewrite/specs/S-0NN-<name>.md` following the template: interface/layouts, data
   formats with byte-level worked examples, numbered rules (`S-0NN-R01 …`), algorithms in
   **your own pseudocode** (never paste C), concurrency, reference defects/quirks
   (`Q-n` QUIRK, `D-n` DEFECT with required behaviour), implementation notes, a coverage table
   mapping every rule to scripts, open questions.
2. `test/conformance/gen/<area>_runner.c` — a script-driven C runner linking the real C
   implementation (uses `harness.h` / `script.h`).
3. `test/conformance/gen/<area>.sources` — registration (see header of `run.sh`; paths relative to
   the repository root, `-l` flags, `!nostubs` if you link real platform code instead of
   `gen/stubs.c`). **Do not edit `run.sh`, `gen/stubs.c`, `gen/harness.h`, `gen/script.h`,
   `specs/README.md`** or any other area's files. If you need extra stub functions put them in
   `gen/<area>_stubs.c` and list it in your `.sources`.
4. `test/conformance/scripts/<area>/*.script` — hand-written scripts (original), grouped by topic,
   each with a header comment explaining what it demonstrates, and
   `test/conformance/vectors/<area>/*.expected` generated with `./run.sh generate <area>`.
5. Extend `test/conformance/README.md` **only by appending** a section `## <area> runner (spec S-0NN)`
   documenting your script language (append at the end of the file).

## Method

1. Read the reference thoroughly (all exported functions *and* header-inline code that plugins
   compile in). Write down every observable behaviour: edge cases, NULL handling, allocation/
   capacity/ownership, error returns, logging (level; text only if it matters), ordering,
   rounding, endianness, thread-safety.
2. Probe, don't assume: whenever you are not sure what the reference does, write a script and run
   it. Prefer many small scripts per topic over a few big ones. Cover: normal use, every error path,
   boundaries, empty/NULL inputs, documented preconditions' edges (but stay *inside* preconditions).
3. Classify every oddity: CORE (must match), QUIRK (reference behaviour that looks accidental but
   callers/files may depend on: SHOULD match), DEFECT (reference crashes/UB/ABI-visible bug:
   state the required behaviour). Put quirk demonstrations in separate, clearly named scripts.
4. Keep vectors **deterministic and independent of the environment**: no pointers, no addresses,
   no time, no thread scheduling, no uninitialised memory (print only defined bytes; mask others),
   no locale-dependent output, no file system paths. Floating point: print with `%.9g`
   (`%.17g` for doubles) and state the comparison tolerance in the spec.
5. Run the runner under `-fsanitize=address,undefined` (compile it by hand into the scratchpad)
   to find script-induced UB or leaks that signal a script outside the preconditions; fix the
   script. Record reference crashes with a fork-per-case runner (see `decl_runner.c`) as
   `crash` results.
6. Verify stability: `./run.sh generate <area>` then `./run.sh check <area>` at least 3 times, all
   `ok`.
7. **Sufficiency check (required):** in your scratchpad, implement the component in Rust using
   *only your spec and the conformance README* (no looking at the C while implementing; fix the
   spec if you must look something up), write a runner for the script language, and run all your
   vectors. Report pass counts. Every mismatch is a spec bug: fix the spec (and add a script if the
   gap was a missing test) and re-run. Do not commit this throw-away crate.

## Rules of the environment

- Use the session scratchpad directory for scratch work and builds, **not `/tmp`**; do not use
  `rm -rf`. Runners that need files use `$CONFORMANCE_TMPDIR` (set by `run.sh`).
- Do not run `git commit`, `git add`, `git checkout`, or anything that changes git state.
- Work only inside your own files listed above. Other authors work in parallel in the same tree.
- Platform: Linux x86-64, 64-bit little-endian; note Windows/macOS differences you can see in the
  source but do not need to test them.
- Build tools available: gcc, clang, cargo/rustc, python3; libraries: jansson, uthash.

## Final report (your last message)

Return: spec file, rule count, number of scripts/vectors, quirks and defects found (one line
each), sufficiency-check result (`N/N`), spec bugs the check exposed, open questions, and anything
you could not specify and why.

## Repository layout (new repository)

Work in this repository (`obs-studio-rust`) only. The OBS Studio checkout (`../obs-studio`) is
**read-only reference**: never create, edit or delete files there, and run builds with
`CONFORMANCE_BUILD_DIR` set to your scratchpad. Run `tools/link-obs-source.sh` once so that
`libobs/`, `plugins/`, `shared/` and `deps/` resolve to the reference (the links are git-ignored).
All paths in this brief ("repository root", `gen/<area>.sources`) refer to this repository.
Coverage of the reference is measured with `python3 tools/coverage.py`.
