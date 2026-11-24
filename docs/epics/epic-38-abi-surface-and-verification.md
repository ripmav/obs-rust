# E38 — ABI surface and verification

**Milestone:** M6 — Phase 4: core object model (section F)
**Subtasks:** [phase-4-subtasks.md](../rust-rewrite/subtasks/phase-4-subtasks.md) § F

**Why:** the complete `obs.h` surface + vtable adapters — unmodified C plugins must load (the phase exit gate).

| Story                                            | I want                                                               | Acceptance                                             | Status |
| ------------------------------------------------ | -------------------------------------------------------------------- | ------------------------------------------------------ | ------ |
| **E38S1** P4-34 `obs-ffi` header completeness    | Port `obs-ffi` header completeness to Rust (`obs.h`, `obs-source.h`) | file committed                                         | open   |
| **E38S2** P4-35 ABI struct layout checks         | Port ABI struct layout checks to Rust                                | spec S-035, S-042; asserts compile                     | open   |
| **E38S3** P4-36 Third-party plugin matrix        | Port Third-party plugin matrix to Rust                               | list committed in docs/tests/third-party-plugins.md    | open   |
| **E38S4** P4-37 In-tree plugin compatibility run | Port In-tree plugin compatibility run to Rust                        | both submodules' sources are present in the build tree | open   |
| **E38S5** P4-38 Thread-safety test               | Port Thread-safety test to Rust                                      | smoke test clean                                       | open   |
| **E38S6** P4-39 Memory test                      | Port Memory test to Rust                                             | smoke test leak-free                                   | open   |
| **E38S7** P4-40 Retire libobs C                  | Port Retire libobs C to Rust                                         | the default build links the Rust core                  | open   |
| **E38S8** P4-41 Headless `obs-cli`               | Port Headless `obs-cli` to Rust                                      | --help lists all flags                                 | open   |
| **E38S9** P4-42 Phase 4 review                   | Port Phase 4 review to Rust                                          | matrix committed in docs/tests/                        | open   |
