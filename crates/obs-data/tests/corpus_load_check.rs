//! P0-18.3 (WP-42): load check for the scene-collection corpus.
//!
//! Every file in `test/fixtures/scenes/*.json` (OBS 33.0.1 scene-collection
//! format; public + synthetic per D4) must parse without errors through the
//! obs-data JSON engine (`data_create_from_json_file` — the same entry point
//! the 33.0.1 frontend uses, `obs_data_create_from_json_file_safe`).
//!
//! The committed evidence log is `test/fixtures/LOAD-CHECK.md`; regenerate
//! it from this test's stdout (`cargo test -p obs-data --test
//! corpus_load_check -- --nocapture`).

use std::path::Path;

use obs_data::api::{data_get_array, data_get_string};
use obs_data::io::data_create_from_json_file;

#[test]
fn corpus_files_parse() {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../test/fixtures/scenes");
    let mut files: Vec<_> = std::fs::read_dir(&dir)
        .expect("corpus dir test/fixtures/scenes")
        .map(|e| e.expect("dir entry").path())
        .filter(|p| p.extension().is_some_and(|e| e == "json"))
        .collect();
    files.sort();
    assert!(
        files.len() >= 20,
        "corpus must hold >= 20 scene files (P0-18.2), found {}",
        files.len()
    );

    for path in &files {
        let name = path
            .file_name()
            .expect("file name")
            .to_string_lossy()
            .to_string();
        let size = std::fs::metadata(path).expect("stat").len();
        let data = data_create_from_json_file(path.to_str().expect("utf8 path"))
            .unwrap_or_else(|| panic!("{name}: parse failed (P0-18.3)"));
        // structural probes: the collection identity and the source list
        let title = data_get_string(&data, "name");
        let sources = data_get_array(&data, "sources")
            .map(|_| "present")
            .unwrap_or("absent");
        println!("OK {name} {size} bytes name={title:?} sources={sources}");
    }
    println!(
        "corpus load check: {} files parsed without errors",
        files.len()
    );
}
