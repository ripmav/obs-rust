# Architecture Decision Records

ADRs record the decisions that shape the Rust rewrite (`docs/rust-rewrite-plan.md`).
Format, numbering, and status rules: [0000-template.md](0000-template.md).

## How to add an ADR

1. Copy `0000-template.md` to `NNNN-short-slug.md` — next free 4-digit number, never reused.
2. Fill every section. Attach measured data with the reproducible command that produced it.
3. Open a `docs:` PR and add a row to the index below in the same PR.
4. On merge (or a later commit) set `Status: Accepted`. Superseding an ADR: the new ADR
   sets `Supersedes:`, the old one gets `Superseded by:` + status `Superseded` — same PR.

## Index

| # | ADR | Status | Date |
|---|---|---|---|
| 0001 | [Strangler-fig migration strategy](0001-strangler-migration.md) | Accepted (maintainer review pending, P0-01.5) | 2026-11-10 |
| 0005 | [Code policy: unsafe, MSRV, dependencies](0005-code-policy.md) | Accepted | 2026-11-10 |
| 0002 | [C plugin ABI policy](0002-plugin-abi.md) | Accepted | 2026-11-10 |
