# Open decisions — obs-rust (Rust rewrite)

Decisions still open or answered but not yet recorded. Each row names what it blocks and what is
assumed until then. `Status` is `open` until the user answers; as soon as an answer arrives the row
becomes `answered — in progress` with the answer quoted, before any other work. Once the answer is
recorded (ADR, story, tenet or `AGENTS.md`), the row is deleted in that change.

| ID | Status | Decision | Options | Assumed until decided | Blocks |
|---|---|---|---|---|---|
| D6 | open | S-009 (vec2/matrix3/plane/bounds/math-extra) is a **Draft** pending a second-author check — when does it happen? | before the first Phase 2 math consumer lands | the draft is binding for math2 work; behaviour is pinned by committed vectors (`27-srgb-256-golden`, `half_exhaustive`) regardless | none in M1–M3 (Phase 2 prep) |
