# E21 — Phase 2+ spec prep (D5, decided 2026-11-23)

**Milestone:** M2 — Phase 1: integration & review


**Why:** S-064…S-067 are unwritten; the phases that would otherwise "read the C" (P4-32.2, P2-15.3/.4, P5-30) need them. S-009 is still a Draft (D6, decided 2026-11-23: checked in this batch). Written after E14 so the phase gate is not delayed by spec writing.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E21S1** Write S-064 (Wayland hotkey platform) | spec for `obs-nix-wayland.c` (wl_keyboard + xkbcommon) per D-30 | S-064 committed; P4-32.2 cites it instead of "read the C" | open |
| **E21S2** Write S-065 (audio-monitoring win32/osx/null) | spec per D-31 (S-055 covers PulseAudio only), scheduled as P2-15.6 | S-065 committed; P1-34.4 becomes tickable; P2-15.3/.4 cite it | open |
| **E21S3** Write S-066 (win-wasapi) + P5-30.9 dedupe | spec per D-32 (S-051 covers window matching/hooks/vqueue only); the duplicate P5-30.9 entry folded into the single "Write S-066" subtask | S-066 committed; P5-30.3–.5 cite it alongside S-051 §5.G; no duplicate entry remains | open |
| **E21S4** Write S-067 (frontend preview interaction) | the spec S-069 references for preview interaction | S-067 committed; S-069's cross-reference resolves | open |
| **E21S5** S-009 second-author check (Draft → Final) | the math2 spec (vec2/matrix3/plane/bounds/math-extra/half/sRGB) checked per the spec process | second author re-derives the rules; corrections merged; S-009 marked final, P1-15…P1-18 cite the final version (behaviour already pinned by the `27-srgb-256-golden` / `half_exhaustive` vectors) | open |

---
