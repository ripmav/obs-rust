# Spec-coverage backlog (round 6, 2026-11)

Goal: **every behaviour in the C reference is pinned by a spec** — extend an existing spec or write a new one. Measured from `function-inventory.csv`: first-party, non-trivial functions whose file no spec cites (`S2 = 0`), 33.0.1@65c320b46: **14 files / 64 functions**.

New spec IDs continue the series (S-061…S-066 = the DECISIONS-NEEDED backlog written by round-5 subtasks, S-067 = preview interaction from P6-07.9). Regenerate this file with `python3 tools/spec-backlog.py --write`; it fails if a file has no action.

## Actions

| Action | Kind | Task | Title | files | fns | lines |
|---|---|---|---|---|---|---|

## Documented exclusions

- `test/` — C-reference test harness (test-input plugin, cmocka unit tests, osx/win test hosts) — no port target; exercised by the conformance suite instead. (64 fns / 1153 lines)

## Files per action

