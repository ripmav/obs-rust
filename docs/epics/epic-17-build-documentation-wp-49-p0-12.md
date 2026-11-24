# E17 — Build documentation (WP-49, P0-12)

**Milestone:** M3 — Phase 0 close-out & future ADRs

Build documentation (WP-49, P0-12)
**Why:** a newcomer must be able to build from a clean machine using only the docs.

| Story                                  | I want                                                     | Acceptance                                      | Status |
| -------------------------------------- | ---------------------------------------------------------- | ----------------------------------------------- | ------ |
| **E17S1** Linux doc (P0-12.1)          | `docs/rust-rewrite/BUILDING.md` with the full package list | a clean container builds following only the doc | open   |
| **E17S2** Windows/macOS doc (P0-12.2)  | the same for the other two OSes                            | tested on fresh VMs                             | open   |
| **E17S3** Reproducible shell (P0-12.3) | `.devcontainer/` or `flake.nix`                            | `cargo test` runs inside it; `INSTALL` updated  | open   |
