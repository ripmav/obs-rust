//! obs-hotkey-names: the `obs_key` id <-> name table (S-024 M4).
//!
//! The 636 key ids are the 0-based positions in the key list of
//! `obs-hotkeys.h` (`OBS_KEY_NONE = 0`, `OBS_KEY_LAST_VALUE = 636` is not
//! a key). The table below is the normative one — generated from
//! `test/conformance/vectors/helpers/hotkeys-fallback-table.expected`
//! (S-024: "the vector is the normative table"); regenerate with
//! `python3 tools/gen_hotkey_names.py`.
//!
//! The C reference has two lookup paths (a lazily built hash map when the
//! global core exists, a linear search otherwise) with identical results;
//! this port has one (the linear table). There is therefore no lazily
//! built map to free: S-024-R21 (`obs_hotkey_name_map_free`, free +
//! rebuild on demand) has no equivalent here (documented deviation).
//!
//! Logging: S-024-R19 requires an ERROR for an out-of-range id from
//! `obs_key_to_name`; this crate has no logger yet (it arrives with the
//! platform crate, S-023) — the empty-string result is pinned, the FFI
//! layer will wire the log line.

pub(crate) mod table;
pub use table::KEY_NAMES;

/// `OBS_KEY_LAST_VALUE` (S-024 M4): the first id that is not a key.
pub const OBS_KEY_LAST_VALUE: u32 = 636;

/// S-024-R17: the name of key id `key` for `key < 636` (the normative
/// table); out of range (`key >= 636`) returns `""` (the C ERROR log is
/// not wired yet — see the crate docs). C-negative ids are
/// unrepresentable in the safe API (the FFI layer maps them to the same
/// `""` result).
pub fn obs_key_to_name(key: u32) -> &'static str {
    match KEY_NAMES.get(key as usize) {
        Some(name) => name,
        None => "",
    }
}

/// S-024-R17/R18: the id of the exact, case-sensitive full-string key
/// name; no match, empty string or non-key identifiers (e.g.
/// `OBS_KEY_LAST_VALUE`) map to `OBS_KEY_NONE` (0); no error is logged.
/// C-NULL is unrepresentable — the C-NULL case (S-024-R20, required
/// result 0) is the same value the FFI layer passes for an empty string.
pub fn obs_key_from_name(name: &str) -> u32 {
    KEY_NAMES.iter().position(|n| *n == name).unwrap_or(0) as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The normative vector (test/conformance/vectors/helpers/
    /// hotkeys-fallback-table.expected): every line `ID NAME ID` must
    /// match the table (P1-28.1 "Done when: all key names equal").
    #[test]
    #[cfg_attr(miri, ignore)]
    fn table_matches_normative_vector() {
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../test/conformance/vectors/helpers/hotkeys-fallback-table.expected"
        );
        let text = std::fs::read_to_string(path).expect("normative vector");
        let mut checked = 0;
        for line in text.lines() {
            let mut parts = line.split_whitespace();
            let (Some(id), Some(name), Some(id2)) = (parts.next(), parts.next(), parts.next())
            else {
                continue;
            };
            if id == id2 {
                let id: u32 = id.parse().expect("numeric id");
                assert_eq!(obs_key_to_name(id), name, "id {id}");
                assert_eq!(obs_key_from_name(name), id, "name {name}");
                checked += 1;
            }
        }
        assert_eq!(checked, 636, "all entries of the normative table");
    }

    // S-024-R17: round trip is the identity
    #[test]
    fn round_trip_is_identity() {
        for id in 0..OBS_KEY_LAST_VALUE {
            assert_eq!(obs_key_from_name(obs_key_to_name(id)), id);
        }
    }

    // S-024-R17: spot values from the lookup vector
    #[test]
    fn known_names() {
        assert_eq!(obs_key_to_name(0), "OBS_KEY_NONE");
        assert_eq!(obs_key_to_name(1), "OBS_KEY_RETURN");
        assert_eq!(obs_key_to_name(2), "OBS_KEY_ENTER");
        assert_eq!(obs_key_to_name(65), "OBS_KEY_HYPER_L");
        assert_eq!(obs_key_to_name(97), "OBS_KEY_NUMASTERISK");
        assert_eq!(obs_key_to_name(121), "OBS_KEY_A");
        assert_eq!(obs_key_to_name(200), "OBS_KEY_EDIAERESIS");
        assert_eq!(obs_key_to_name(461), "OBS_KEY_MOUSE29");
        assert_eq!(obs_key_to_name(470), "OBS_KEY_0x07");
        assert_eq!(obs_key_to_name(635), "OBS_KEY_VK_OEM_CLEAR");
    }

    // S-024-R19: out of range -> "" (the ERROR log is not wired yet)
    #[test]
    fn to_name_out_of_range() {
        assert_eq!(obs_key_to_name(636), "");
        assert_eq!(obs_key_to_name(100000), "");
        assert_eq!(obs_key_to_name(u32::MAX), "");
    }

    // S-024-R18: unknown, wrongly-cased, padded or empty names, and
    // OBS_KEY_LAST_VALUE, map to 0
    #[test]
    fn from_name_edge_cases() {
        assert_eq!(obs_key_from_name("OBS_KEY_NONE"), 0);
        assert_eq!(obs_key_from_name("OBS_KEY_A"), 121);
        assert_eq!(obs_key_from_name("OBS_KEY_RETURN"), 1);
        assert_eq!(obs_key_from_name("OBS_KEY_MOUSE29"), 461);
        assert_eq!(obs_key_from_name("OBS_KEY_0x07"), 470);
        assert_eq!(obs_key_from_name("obs_key_a"), 0, "case-sensitive");
        assert_eq!(obs_key_from_name("OBS_KEY_A "), 0, "padded");
        assert_eq!(obs_key_from_name("A"), 0, "no prefix");
        assert_eq!(obs_key_from_name(""), 0, "empty");
        assert_eq!(obs_key_from_name("OBS_KEY_LAST_VALUE"), 0, "not a key");
        assert_eq!(obs_key_from_name("OBS_KEY_VK_OEM_CLEARX"), 0);
    }

    // no duplicate names in the table (a duplicate would make
    // obs_key_from_name ambiguous)
    #[test]
    fn no_duplicate_names() {
        let mut sorted = KEY_NAMES.to_vec();
        sorted.sort_unstable();
        sorted.dedup();
        assert_eq!(sorted.len(), KEY_NAMES.len(), "names must be unique");
    }
}
