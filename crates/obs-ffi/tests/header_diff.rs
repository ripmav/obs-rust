//! P1-29.1 (WP-31): generate the C header of the obs-ffi bridges.
//!
//! cbindgen runs as a dev-dependency (config: `cbindgen.toml` —
//! `language = "C"`, `cpp_compat = true`, no function prefix). The header
//! is written to `$HDR_DIFF_OUT` when `test/conformance/cbindgen-header-diff.py`
//! (P1-29.2) drives this test, or to `target/obs-ffi-generated.h` for
//! inspection when run bare. The diff itself (function declarations vs the
//! C reference headers under `libobs/`) lives in that script; this test
//! only generates.

use std::io::Write;
use std::path::{Path, PathBuf};

#[test]
fn generate_header() {
    let manifest = Path::new(env!("CARGO_MANIFEST_DIR"));
    let out = match std::env::var("HDR_DIFF_OUT") {
        Ok(p) => PathBuf::from(p),
        // cargo's target scratch (not /tmp: reaped on some hosts) — the
        // parent is created because a bare `cargo test` may not have built
        // anything there yet.
        Err(_) => manifest
            .join("../..")
            .join("target")
            .join("obs-ffi-generated.h"),
    };
    if let Some(parent) = out.parent() {
        std::fs::create_dir_all(parent).expect("create output dir");
    }
    let bindings = cbindgen::generate(manifest).expect("cbindgen failed");
    // Unconditional write: `write_to_file` reports false ("unchanged") for
    // an up-to-date file, which is not an error.
    let mut file = std::fs::File::create(&out).expect("create header file");
    bindings.write(&mut file);
    file.flush().expect("write generated header");
    eprintln!("generated header: {}", out.display());
}
