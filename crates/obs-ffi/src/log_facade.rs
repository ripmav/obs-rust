//! P1-13.3 (WP-37): the `log`-crate facade — Rust `log` records are
//! forwarded to `blog` (S-001 §4.2) through [`crate::blog`], so they flow
//! through the installed C handler exactly like C `blog` calls.
//!
//! Level mapping: `log::Trace` has no C counterpart and is demoted to
//! `LOG_DEBUG` (S-001 Q-3: the default handler accepts only the four
//! `blog` level constants and silently drops anything else).

use std::os::raw::c_int;

use crate::{LOG_DEBUG, LOG_ERROR, LOG_INFO, LOG_WARNING};

/// The `blog` level for a `log` level (S-001 §4.2, R20).
pub fn obs_level_for(level: log::Level) -> c_int {
    match level {
        log::Level::Error => LOG_ERROR,
        log::Level::Warn => LOG_WARNING,
        log::Level::Info => LOG_INFO,
        log::Level::Debug | log::Level::Trace => LOG_DEBUG,
    }
}

/// A `log::Log` implementation that forwards records to `blog` without
/// filtering (like the C `blog`, which does no level filtering; the C
/// handler decides what to print — Q-3).
pub struct ObsLogLogger;

impl log::Log for ObsLogLogger {
    fn log(&self, record: &log::Record) {
        crate::blog(obs_level_for(record.level()), &format!("{}", record.args()));
    }
    fn enabled(&self, _metadata: &log::Metadata) -> bool {
        true
    }
    fn flush(&self) {}
}

/// Install [`ObsLogLogger`] as the process `log` logger (max level
/// `Trace`: no filtering, matching the C `blog`).
pub fn init() -> Result<(), log::SetLoggerError> {
    log::set_logger(Box::leak(Box::new(ObsLogLogger)))
        .map(|()| log::set_max_level(log::LevelFilter::Trace))
}

#[cfg(test)]
mod tests {
    use super::*;

    // P1-13.3 Done-when: the level mapping table, tested.
    #[test]
    fn level_mapping_table() {
        assert_eq!(obs_level_for(log::Level::Error), LOG_ERROR);
        assert_eq!(obs_level_for(log::Level::Warn), LOG_WARNING);
        assert_eq!(obs_level_for(log::Level::Info), LOG_INFO);
        assert_eq!(obs_level_for(log::Level::Debug), LOG_DEBUG);
        assert_eq!(
            obs_level_for(log::Level::Trace),
            LOG_DEBUG,
            "no C counterpart: demoted to DEBUG"
        );
    }
}
