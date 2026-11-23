# Open decisions — obs-rust (Rust rewrite)

Decisions still open or answered but not yet recorded. Each row names what it blocks and what is
assumed until then. `Status` is `open` until the user answers; as soon as an answer arrives the row
becomes `answered — in progress` with the answer quoted, before any other work. Once the answer is
recorded (ADR, story, tenet or `AGENTS.md`), the row is deleted in that change.

| ID | Status | Decision | Options | Assumed until decided | Blocks |
|---|---|---|---|---|---|
| D5 | answered — in progress: “Mach wie empfohlen” (2026-11-23) | When are the missing specs S-064…S-067 written (P1-34.4 is parked on S-065)? | (1) after M2, tracked as a new epic in M2 — chosen; (2) in parallel during M1; (3) parked indefinitely | specs get written after the phase-1 review; tracked, not parked | E21 (new), P1-34.4 |
| D6 | open | S-009 (vec2/matrix3/plane/bounds/math-extra) is a **Draft** pending a second-author check — when does it happen? | before the first Phase 2 math consumer lands | the draft is binding for math2 work; behaviour is pinned by committed vectors (`27-srgb-256-golden`, `half_exhaustive`) regardless | none in M1–M3 (Phase 2 prep) |
