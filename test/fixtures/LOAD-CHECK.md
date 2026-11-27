# Scene corpus — load check (P0-18.3)

Every file in `scenes/` is parsed with the Rust `obs-data` crate's
`data_create_from_json_file` (the same `obs_data_create_from_json_file`
entry point the 33.0.1 frontend uses; `obs-ref-dump` was not usable —
it runs a fixed fixture set, not arbitrary files). The permanent check is
`crates/obs-data/tests/corpus_load_check.rs` (runs in CI on every push,
also asserts the corpus holds ≥ 20 files).

Result: **24 / 24 files parsed without errors.**

Regenerate this log: `cargo test -p obs-data --test corpus_load_check -- --nocapture`.

- Generator: `test/fixtures/generate_corpus.py` (synthetic files)
- Anonymiser: `test/fixtures/anonymise.py` (every file, synthetic and
  public, passed through it before committing; `--check` is a CI gate)

| File                                   | Bytes | Parsed | Collection name        | `sources` array |
| -------------------------------------- | ----- | ------ | ---------------------- | --------------- |
| public-crowd-cast.json                 | 1445  | OK     | crowd-cast Capture     | present         |
| public-flow-toggl-demo.json            | 8245  | OK     | Flow Reports           | present         |
| public-goobs-docker.json               | 9353  | OK     | test                   | present         |
| public-irl-homekit-blank.json          | 799   | OK     | IRL Homekit            | present         |
| public-opencomputer-legacy.json        | 4377  | OK     | Untitled               | present         |
| public-superbacked-screen-capture.json | 5331  | OK     | Screen capture         | present         |
| syn-audio-devices.json                 | 13188 | OK     | syn-audio-devices      | present         |
| syn-audio-filters.json                 | 16351 | OK     | syn-audio-filters      | present         |
| syn-browser-basic.json                 | 5025  | OK     | syn-browser-basic      | present         |
| syn-browser-multiscene.json            | 16563 | OK     | syn-browser-multiscene | present         |
| syn-canvases.json                      | 9785  | OK     | syn-canvases           | present         |
| syn-capture-devices.json               | 23118 | OK     | syn-capture-devices    | present         |
| syn-game-capture.json                  | 10945 | OK     | syn-game-capture       | present         |
| syn-groups-items.json                  | 40873 | OK     | syn-groups-items       | present         |
| syn-image-text-color.json              | 14237 | OK     | syn-image-text-color   | present         |
| syn-item-transforms.json               | 23105 | OK     | syn-item-transforms    | present         |
| syn-media-playback.json                | 11308 | OK     | syn-media-playback     | present         |
| syn-projectors-vcam.json               | 5427  | OK     | syn-projectors-vcam    | present         |
| syn-text-gdiplus-vlc.json              | 8861  | OK     | syn-text-gdiplus-vlc   | present         |
| syn-transitions-all.json               | 8487  | OK     | syn-transitions-all    | present         |
| syn-unicode.json                       | 13401 | OK     | Szène – 日本語 – 🎥    | present         |
| syn-v1-absolute.json                   | 6984  | OK     | syn-v1-absolute        | present         |
| syn-video-filters.json                 | 21891 | OK     | syn-video-filters      | present         |
| syn-vst-filters.json                   | 11540 | OK     | syn-vst-filters        | present         |
