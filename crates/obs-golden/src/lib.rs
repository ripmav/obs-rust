//! obs-golden: golden-file assertions for the Phase 0 safety net (P0-13).
//!
//! - [`assert_golden_str`] / [`assert_golden_bytes`] compare a value against
//!   `<golden-dir>/<name>.golden`.
//! - `UPDATE_GOLDEN=1` regenerates goldens and prints a summary; it is
//!   refused (panic) when `CI` is set, so CI can never rewrite goldens.
//! - Mismatches report the first differing line (str) or byte (bytes) with
//!   its offset and a truncated diff.
//!
//! The golden directory defaults to `tests/golden` relative to the current
//! working directory (cargo test sets it to the crate root); set `GOLDEN_DIR`
//! to override (used by the tests in this crate).
//!
//! Matrix: `docs/tests/obs-golden.md`.

use std::env;
use std::fs;
use std::path::PathBuf;

/// Maximum lines of diff shown in a mismatch report.
const DIFF_MAX_LINES: usize = 40;

/// Directory containing golden files (see crate docs).
pub fn golden_dir() -> PathBuf {
    env::var("GOLDEN_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("tests/golden"))
}

fn golden_path(name: &str) -> PathBuf {
    golden_dir().join(format!("{name}.golden"))
}

fn update_requested() -> bool {
    if env::var_os("UPDATE_GOLDEN").is_none() {
        return false;
    }
    if env::var_os("CI").is_some() {
        panic!("UPDATE_GOLDEN=1 is forbidden in CI (P0-13.2)");
    }
    true
}

fn read_golden(path: &std::path::Path) -> Option<Vec<u8>> {
    match fs::read(path) {
        Ok(existing) => Some(existing),
        Err(_) if update_requested() => None, // written by the caller
        Err(_) => panic!(
            "no golden file at {} — run with UPDATE_GOLDEN=1 to create it (P0-13.1)",
            path.display()
        ),
    }
}

fn write_golden(path: &std::path::Path, value: &[u8]) {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).unwrap_or_else(|e| panic!("creating {:?}: {e}", parent));
    }
    fs::write(path, value).unwrap_or_else(|e| panic!("writing {:?}: {e}", path));
    println!(
        "UPDATE_GOLDEN: wrote {} ({} bytes)",
        path.display(),
        value.len()
    );
}

/// Assert that `value` matches the golden file for `name` (text mode).
///
/// On mismatch the panic message names the first differing line and byte
/// offset and shows a truncated diff. With `UPDATE_GOLDEN=1` (and `CI` unset)
/// the file is rewritten instead.
pub fn assert_golden_str(name: &str, value: &str) {
    let path = golden_path(name);
    if let Some(existing) = read_golden(&path) {
        if existing != value.as_bytes() {
            let expected = String::from_utf8_lossy(&existing);
            panic!(
                "golden mismatch for {name} ({}):\n{}",
                path.display(),
                str_diff(&expected, value)
            );
        }
    } else {
        write_golden(&path, value.as_bytes());
    }
}

/// Assert that `value` matches the golden file for `name` (binary mode).
///
/// On mismatch the panic message names the first differing byte offset and
/// shows 16 bytes of context per side in hex.
pub fn assert_golden_bytes(name: &str, value: &[u8]) {
    let path = golden_path(name);
    if let Some(existing) = read_golden(&path) {
        if existing != value {
            let offset = existing
                .iter()
                .zip(value.iter())
                .position(|(a, b)| a != b)
                .unwrap_or_else(|| existing.len().min(value.len()));
            panic!(
                "golden mismatch for {name} ({}): first difference at byte {offset}\nexpected: {}\nactual:   {}",
                path.display(),
                hex_context(&existing, offset),
                hex_context(value, offset),
            );
        }
    } else {
        write_golden(&path, value);
    }
}

/// Truncated, unified-style diff of two strings around the first difference.
fn str_diff(expected: &str, actual: &str) -> String {
    let elines: Vec<&str> = expected.lines().collect();
    let alines: Vec<&str> = actual.lines().collect();
    let first = elines.iter().zip(alines.iter()).position(|(e, a)| e != a);

    let mut out = String::from("--- expected\n+++ actual\n");
    match first {
        Some(i) => {
            let line_start = expected.lines().take(i).map(|l| l.len() + 1).sum::<usize>();
            let byte_offset = elines[i]
                .as_bytes()
                .iter()
                .zip(alines[i].as_bytes())
                .position(|(x, y)| x != y)
                .unwrap_or_else(|| elines[i].len().min(alines[i].len()));
            out.push_str(&format!(
                "@@ around line {} (first difference at byte offset {}) @@\n",
                i + 1,
                line_start + byte_offset
            ));
            let from = i.saturating_sub(1);
            let mut shown = 0;
            for (j, line) in elines.iter().enumerate() {
                if j < from || shown >= DIFF_MAX_LINES {
                    continue;
                }
                match alines.get(j) {
                    Some(a) if a == line => out.push_str(&format!("  {line}\n")),
                    Some(a) => out.push_str(&format!("-{line}\n+{a}\n")),
                    None => out.push_str(&format!("-{line}\n")),
                }
                shown += 1;
            }
            for line in alines.iter().skip(elines.len()).take(DIFF_MAX_LINES) {
                out.push_str(&format!("+{line}\n"));
            }
        }
        None => {
            out.push_str(&format!(
                "  (all shared lines equal; line count differs: expected {} lines, actual {} lines)\n",
                elines.len(),
                alines.len()
            ));
            if elines.len() > alines.len() {
                for line in elines.iter().skip(alines.len()).take(DIFF_MAX_LINES) {
                    out.push_str(&format!("-{line}\n"));
                }
            } else {
                for line in alines.iter().skip(elines.len()).take(DIFF_MAX_LINES) {
                    out.push_str(&format!("+{line}\n"));
                }
            }
        }
    }
    out
}

/// 16 bytes of hex context around `offset` (up to 8 bytes before).
fn hex_context(bytes: &[u8], offset: usize) -> String {
    let start = offset.saturating_sub(8);
    let end = (start + 16).min(bytes.len());
    format!(
        "{start:04x}: {}\n",
        bytes[start..end]
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<Vec<_>>()
            .join(" ")
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    /// Env vars are process-global; serialize tests that mutate them and give
    /// each test its own directory.
    static ENV_LOCK: Mutex<()> = Mutex::new(());

    /// SAFETY: called only from tests serialized on ENV_LOCK; every test
    /// clears UPDATE_GOLDEN/CI on entry and removes GOLDEN_DIR on exit, so no
    /// other thread observes a torn or stale value during the critical section.
    fn test_set(key: &str, value: impl AsRef<std::ffi::OsStr>) {
        unsafe { env::set_var(key, value) }
    }

    /// SAFETY: same serialization and restore contract as [`test_set`].
    fn test_remove(key: &str) {
        unsafe { env::remove_var(key) }
    }

    fn with_dir(name: &str, f: impl FnOnce(&std::path::Path)) {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let dir = std::env::temp_dir().join(format!("obs-golden-test-{name}"));
        let _ = fs::remove_dir_all(&dir);
        test_set("GOLDEN_DIR", &dir);
        test_remove("UPDATE_GOLDEN");
        test_remove("CI");
        f(&dir);
        test_remove("GOLDEN_DIR");
    }

    fn panic_message(f: impl FnOnce() + std::panic::UnwindSafe) -> String {
        let r = std::panic::catch_unwind(f).unwrap_err();
        r.downcast_ref::<String>()
            .cloned()
            .or_else(|| r.downcast_ref::<&str>().map(|s| s.to_string()))
            .expect("panic message")
    }

    #[test]
    fn matching_golden_passes() {
        with_dir("match", |dir| {
            fs::create_dir_all(dir).unwrap();
            fs::write(dir.join("demo.golden"), b"hello\n").unwrap();
            assert_golden_str("demo", "hello\n");
            assert_golden_bytes("demo", b"hello\n");
        });
    }

    #[test]
    fn str_mismatch_reports_first_line_and_offset() {
        with_dir("str-mismatch", |dir| {
            fs::create_dir_all(dir).unwrap();
            fs::write(dir.join("m.golden"), "a\nb\nX\n").unwrap();
            let msg = panic_message(|| assert_golden_str("m", "a\nb\nY\n"));
            assert!(msg.contains("line 3"), "{msg}");
            assert!(msg.contains("byte offset"), "{msg}");
        });
    }

    #[test]
    fn bytes_mismatch_reports_first_byte() {
        with_dir("bytes-mismatch", |dir| {
            fs::create_dir_all(dir).unwrap();
            fs::write(dir.join("b.golden"), [1, 2, 3, 4, 5, 9, 7]).unwrap();
            let msg = panic_message(|| assert_golden_bytes("b", &[1, 2, 3, 4, 5, 8, 7]));
            assert!(msg.contains("first difference at byte 5"), "{msg}");
        });
    }

    #[test]
    fn update_mode_rewrites_golden() {
        with_dir("update", |dir| {
            fs::create_dir_all(dir).unwrap();
            test_set("UPDATE_GOLDEN", "1");
            assert_golden_str("new", "fresh\n");
            assert_golden_bytes("bin", &[9, 9]);
            test_remove("UPDATE_GOLDEN");
            assert_eq!(fs::read(dir.join("new.golden")).unwrap(), b"fresh\n");
            assert_eq!(fs::read(dir.join("bin.golden")).unwrap(), [9, 9]);
        });
    }

    #[test]
    #[should_panic(expected = "forbidden in CI")]
    fn update_forbidden_in_ci() {
        with_dir("ci-guard", |dir| {
            fs::create_dir_all(dir).unwrap();
            test_set("UPDATE_GOLDEN", "1");
            test_set("CI", "true");
            assert_golden_str("x", "y");
        });
    }

    #[test]
    #[should_panic(expected = "UPDATE_GOLDEN=1")]
    fn missing_golden_says_how_to_create() {
        with_dir("missing", |dir| {
            fs::create_dir_all(dir).unwrap();
            assert_golden_str("nope", "x");
        });
    }

    /// P0-13.4 demo: the committed `tests/golden/demo.golden`.
    #[test]
    fn demo_golden() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        test_set("GOLDEN_DIR", "tests/golden"); // cargo test cwd = crate root
        test_remove("UPDATE_GOLDEN");
        test_remove("CI");
        assert_golden_str("demo", "obs-golden demo\n42\n");
        test_remove("GOLDEN_DIR");
    }
}
