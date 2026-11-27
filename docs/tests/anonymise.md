# anonymise.py (P0-18.1, WP-42) — test matrix

- Test file / issue: `test/fixtures/anonymise.py` (`--self-test` mode)
- Task: P0-18.1
- Golden files: none

The anonymiser strips the four identifying classes from scene-collection
string values (URLs → `ANON_URL`, absolute paths → `ANON_PATH`, secret-named
setting values → `ANON_SECRET`, window-title-named setting values →
`ANON_WINDOW`), working on the parsed JSON so output stays valid JSON.
`--check` re-applies the transformation to committed corpus files and fails
if anything is still strippable (idempotence makes that a real gate).
CI: the `conformance` job runs `--self-test` + `--check scenes/*.json`.

| Case                  | Input                                                                                                                                                                                                                                                                                                    | Expected                                                                                                                                                                              | Platforms                      | Status |
| --------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------ | ------ |
| seeded secrets absent | a mini collection seeded with a POSIX path (`/home/secretuser/…`), a Windows path (`C:\Users\secretuser\…`), a UNC path (`\\fileserver\…`), an https + rtmp URL (token in query), an RTMP `key`, an `api_token`, a `window` capture line and a bare `window_title`, plus a process-capture `target` line | the anonymised output contains none of the seeded values or fragments (`secretuser`, `sekrettoken123`, `My Secret Meeting`, `fileserver`, …); the four redaction counters are all > 0 | Python 3.13 (CI ubuntu-latest) | pass   |
| structure preserved   | the same seeded document                                                                                                                                                                                                                                                                                 | deep key-equality of input and output (values may change, keys and nesting must not)                                                                                                  | Python 3.13 (CI ubuntu-latest) | pass   |
| hotkey `key` exempt   | a quick-transition `hotkeys` entry `{"control": true, "key": "F9"}` (the obs_hotkey binding shape, where `key` is the key *name*)                                                                                                                                                                        | `"key": "F9"` survives (a hotkey binding is not a secret)                                                                                                                             | Python 3.13 (CI ubuntu-latest) | pass   |
| idempotence           | anonymise(anonymise(x))                                                                                                                                                                                                                                                                                  | byte-identical to anonymise(x)                                                                                                                                                        | Python 3.13 (CI ubuntu-latest) | pass   |
| corpus gate           | all 24 committed `test/fixtures/scenes/*.json`                                                                                                                                                                                                                                                           | `--check` reports 0 strippable values in every file (exit 0)                                                                                                                          | Python 3.13 (CI ubuntu-latest) | pass   |

Notes: known limits (documented in the module docstring) — a single-segment
POSIX value (`/home`) and slashes inside a value (base64, `Mic/Aux` names,
fractions) are intentionally left alone (the path pattern is anchored at a
token start with ≥ 2 segments); relative paths are not redacted (they carry
no absolute location). Both false-positive classes were found by the public
corpus files (a projector `geometry` base64 blob, a `"Mic/Aux"` scene name)
and fixed before commit.
