# Open decisions — obs-rust (Rust rewrite)

Decisions still open or answered but not yet recorded. Each row names what it blocks and what is
assumed until then. `Status` is `open` until the user answers; as soon as an answer arrives the row
becomes `answered — in progress` with the answer quoted, before any other work. Once the answer is
recorded (ADR, story, tenet or `AGENTS.md`), the row is deleted in that change.

| ID | Status | Decision | Options | Assumed until decided | Blocks |
|---|---|---|---|---|---|
| D4 | answered — in progress: “3. ABER mix aus öffentlichem Material und Syntetisch” (2026-11-23) | Where do the ≥20 user config/script collections for P0-18 come from (consenting users, anonymised)? | (3, amended) mix of licence-compliant public material (OBS issue-tracker configs, gists, …) + synthetic fixtures — chosen; (1) ripmav's own network — dropped for now; (2) synthetic-only — dropped | E12S1 builds on synthetic fixtures; the corpus target is ≥20 collections from public + synthetic material | E12S2, E13S1 (corpus-dependent ticks) |
| D5 | open | When are the missing specs S-064…S-067 written (P1-34.4 is parked on S-065)? | (1) after M2 — recommended, nothing in M1–M3 needs them; (2) in parallel during M1 | parked; P1-34.4 stays unticked | none in M1–M3 (Phase 2 prep) |
| D6 | open | S-009 (vec2/matrix3/plane/bounds/math-extra) is a **Draft** pending a second-author check — when does it happen? | before the first Phase 2 math consumer lands | the draft is binding for math2 work; behaviour is pinned by committed vectors (`27-srgb-256-golden`, `half_exhaustive`) regardless | none in M1–M3 (Phase 2 prep) |
