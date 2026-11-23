# Open decisions — obs-rust (Rust rewrite)

Decisions still open or answered but not yet recorded. Each row names what it blocks and what is
assumed until then. `Status` is `open` until the user answers; as soon as an answer arrives the row
becomes `answered — in progress` with the answer quoted, before any other work. Once the answer is
recorded (ADR, story, tenet or `AGENTS.md`), the row is deleted in that change.

| ID | Status | Decision | Options | Assumed until decided | Blocks |
|---|---|---|---|---|---|
| D6 | answered — in progress: “Option 1” (2026-11-23) | S-009 (vec2/matrix3/plane/bounds/math-extra) is a **Draft** pending a second-author check — when does it happen? | (1) with the E21 batch after M2 (E21S5) — chosen; (2) immediately in M1; (3) left open until Phase 2 | S-009 stays a draft until E21S5; behaviour is already pinned by committed vectors (`27-srgb-256-golden`, `half_exhaustive`) | E21S5 (new) |
