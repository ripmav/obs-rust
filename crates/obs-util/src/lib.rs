//! obs-util: bmem/dstr/darray/circlebuf/threading/platform helpers (Phase 1).
//!
//! Phase 0 skeleton (P0-07.3). Real content and tests arrive with the phase
//! named above; the skeleton only proves the workspace builds.

pub mod bmem;
pub mod config;
pub mod darray;
pub mod deque;
pub mod dstr;
pub mod hash;
pub mod os_fs;
pub mod os_str;

#[cfg(test)]
mod tests {
    /// Skeleton smoke test (P0-07.3); replaced by real tests in the port.
    #[test]
    fn skeleton_smoke() {}
}
