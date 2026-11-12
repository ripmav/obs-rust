//! obs-data: `obs_data_t` settings objects and their JSON form (S-014).
//!
//! The JSON reader/writer is a hand-rolled port of jansson 2.15.1's
//! behaviour (S-014 §6); the data model, reference counting and the
//! typed API follow S-014 §4–§5. `obs_properties` (S-015) is a later
//! work package on top of this crate.
//!
//! The safe Rust API is the contract: C-`NULL` cases are unrepresentable
//! and noted per function; `get_json*` returns owned strings (QUIRK Q4
//! permits this, `get_last_json` is kept); names and string values are
//! byte-exact internally (they may be invalid UTF-8 once set through the
//! FFI) while the public API takes and returns `&str`/`String`.
//!
//! Log calls specified by S-014 (`LOG_ERROR`/`LOG_WARNING`) are not
//! emitted yet: this crate has no logger (it arrives with the platform
//! crate, S-023); the FFI layer will wire them.

pub mod json;
pub mod model;

#[cfg(test)]
mod tests {
    /// Crate smoke test; the real vectors live in the modules.
    #[test]
    fn crate_smoke() {}
}
