//! Platform file helpers (S-023 R61/R62), used by `config_save_safe`
//! (S-007 R05). Per S-007 §8 these belong in the platform crate once
//! it lands (P1-07/P1-14); until then they live here.

use std::fs;
use std::path::Path;

/// S-023-R62 `os_file_exists(path)`: true iff the path can be accessed
/// (`access(path, F_OK)` — symbolic links are followed, so a dangling
/// link does not exist; directories count).
#[must_use]
pub fn os_file_exists(path: &Path) -> bool {
    fs::metadata(path).is_ok()
}

/// S-023-R61 `os_safe_replace(target, from, backup)` (POSIX): if
/// `backup` is given and `target` exists (`os_file_exists`), rename
/// target -> backup (failure -> -1); then rename from -> target
/// (0 or -1). **No rollback**: when the second rename fails, the target
/// has already been moved to the backup (Q-9). An empty backup path is
/// used as a path (the rename fails -> -1 if the target exists).
/// `rename` replaces an existing file target atomically.
pub fn os_safe_replace(target: &Path, from: &Path, backup: Option<&Path>) -> i32 {
    if let Some(backup) = backup
        && os_file_exists(target)
        && fs::rename(target, backup).is_err()
    {
        return -1;
    }
    match fs::rename(from, target) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dir(label: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("obs_rust_os_fs_{label}"));
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn file_exists() {
        let d = dir("exists");
        assert!(!os_file_exists(&d.join("nope")));
        fs::write(d.join("f"), "x").unwrap();
        assert!(os_file_exists(&d.join("f")));
        assert!(os_file_exists(&d), "directories count");
        // a dangling link does not exist (the link is followed)
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(d.join("nope"), d.join("link")).unwrap();
            assert!(!os_file_exists(&d.join("link")));
        }
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn safe_replace_basic() {
        let d = dir("basic");
        let target = d.join("t.ini");
        let from = d.join("t.ini.tmp");
        let backup = d.join("t.ini.bak");
        fs::write(&target, "old").unwrap();
        fs::write(&from, "new").unwrap();
        // target exists -> it is moved to the backup, temp becomes target
        assert_eq!(os_safe_replace(&target, &from, Some(&backup)), 0);
        assert_eq!(fs::read_to_string(&target).unwrap(), "new");
        assert_eq!(fs::read_to_string(&backup).unwrap(), "old");
        assert!(!os_file_exists(&from), "the temp file no longer exists");
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn safe_replace_no_existing_target() {
        let d = dir("new");
        let target = d.join("t.ini");
        let from = d.join("t.ini.tmp");
        let backup = d.join("t.ini.bak");
        fs::write(&from, "new").unwrap();
        // no target -> no backup rename at all
        assert_eq!(os_safe_replace(&target, &from, Some(&backup)), 0);
        assert_eq!(fs::read_to_string(&target).unwrap(), "new");
        assert!(!os_file_exists(&backup));
        // without a backup argument the same holds
        let from2 = d.join("t2.tmp");
        let target2 = d.join("t2");
        fs::write(&from2, "x").unwrap();
        assert_eq!(os_safe_replace(&target2, &from2, None), 0);
        assert_eq!(fs::read_to_string(&target2).unwrap(), "x");
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn safe_replace_q9_no_rollback() {
        // Q-9: when the second rename fails, the target has already
        // been moved to the backup (no rollback). Make the source
        // missing so the second rename must fail.
        let d = dir("q9");
        let target = d.join("t");
        let from = d.join("f");
        let backup = d.join("b");
        fs::write(&target, "old").unwrap();
        assert_eq!(os_safe_replace(&target, &from, Some(&backup)), -1);
        assert_eq!(
            fs::read_to_string(&backup).unwrap(),
            "old",
            "target was already moved to backup"
        );
        assert!(!os_file_exists(&target));
        assert!(!os_file_exists(&from));
        fs::remove_dir_all(&d).unwrap();
    }
}
