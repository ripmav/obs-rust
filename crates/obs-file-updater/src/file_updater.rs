//! S-024 M10 — `shared/file-updater` pure helpers (P1-33.2).
//!
//! The path joining (`get_path`) and the ETag header-line cleaning of the
//! reference. The update algorithm (version comparison, copy/fetch flow)
//! is bridged in `obs-ffi::abi_file_updater`.

/// `get_path(dir, file)` (S-024 §4.10): `dir` + `"/"` (only if `dir` is
/// non-empty and does not end with `/` or `\`) + `file`. An empty or NULL
/// dir yields just `file`. No normalisation of the parts.
#[must_use]
pub fn get_path(dir: &str, file: &str) -> String {
    if dir.is_empty() {
        file.to_string()
    } else if dir.ends_with('/') || dir.ends_with('\\') {
        format!("{dir}{file}")
    } else {
        format!("{dir}/{file}")
    }
}

/// The ETag header-line cleaning (S-024 §4.10): only a line starting
/// exactly with `"ETag: "` counts; an empty value is ignored; the value
/// is truncated at the first `\r`, then at the first `\n`.
#[must_use]
pub fn clean_etag_header(line: &str) -> Option<&str> {
    let rest = line.strip_prefix("ETag: ")?;
    if rest.is_empty() {
        return None;
    }
    let cut_r = rest.find('\r').unwrap_or(rest.len());
    let no_cr = &rest[..cut_r];
    let cut_n = no_cr.find('\n').unwrap_or(no_cr.len());
    // The non-emptiness check is on the PRE-truncation remainder (S-024
    // §4.10: "a non-empty remainder"): "ETag: \r\n" stores the empty
    // string, "ETag: " (nothing after the prefix) is ignored.
    Some(&no_cr[..cut_n])
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The `fu-get-path` vector cases (`@null` and "" dirs behave
    /// identically in the reference — both yield just the file).
    #[test]
    fn get_path_vectors() {
        let cases: &[(&str, &str, &str)] = &[
            ("/cache", "file.json", "/cache/file.json"),
            ("/cache/", "file.json", "/cache/file.json"),
            (r"C:\cache\", "file.json", r"C:\cache\file.json"),
            (r"C:\cache", "file.json", r"C:\cache/file.json"),
            ("", "file.json", "file.json"),
            ("/a/b", "sub/file.json", "/a/b/sub/file.json"),
            (".", "../x", "./../x"),
        ];
        for (dir, file, want) in cases {
            assert_eq!(&get_path(dir, file), want, "{dir:?} + {file:?}");
        }
    }

    /// The `fu-etag-forms` vector cases (the runner's script parser
    /// unescapes `\r`/`\n`/`\"`, so the header lines carry real CR/LF).
    #[test]
    fn etag_forms() {
        assert_eq!(clean_etag_header("etag: \"lower\"\r\n"), None);
        assert_eq!(clean_etag_header("ETag: "), None);
        assert_eq!(clean_etag_header("ETag: \r\n"), Some(""));
        assert_eq!(
            clean_etag_header("ETag: W/\"weak\"\nignored"),
            Some("W/\"weak\"")
        );
        assert_eq!(clean_etag_header("ETag: bare"), Some("bare"));
        assert_eq!(clean_etag_header("ETag: \"e1\"\r\n"), Some("\"e1\""));
        assert_eq!(clean_etag_header("ETag: a\r\nb"), Some("a"));
        assert_eq!(clean_etag_header("X-ETag: y"), None);
    }
}
