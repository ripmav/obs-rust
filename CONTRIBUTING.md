# Contributing to the Rust rewrite

Project rules for every task (branch format, test-first, ABI compatibility,
test matrices, commit footer) live in
[`docs/rust-rewrite/README.md`](docs/rust-rewrite/README.md) — "Rules that apply
to every task". This file holds the cross-cutting contributor rules that do not
fit one phase.

## Public header changes

Any change to a public header of the compat-tracked C ABI (the list in
[ADR 0002](docs/adr/0002-plugin-abi.md)) requires an ADR. Breaking changes
additionally require a deprecation path and migration docs (D-05 timeline).
`CODEOWNERS` routes such PRs to the ABI owners automatically.

## Dependency admission

A Rust dependency may be added only when all five hold (ADR 0005, rules 3–4;
enforced by `cargo deny` + review):

1. **stdlib alternative considered** — name it and why it is insufficient.
2. **licence GPLv2+-compatible** — allow-list: MIT, Apache-2.0, BSD-*, ISC,
   Zlib, MPL-2.0 (GPL-2.0-or-later admissible, listed as an exception).
3. **maintained** — latest release < 12 months old.
4. **`cargo audit` clean** — no known advisories at admission.
5. **one-line justification** in the PR description.

Anything outside the allow-list goes through the exceptions process in
[ADR 0005](docs/adr/0005-code-policy.md) (table entry + `deny.toml` exception +
reviewer approval) — it is not a normal dependency addition.
