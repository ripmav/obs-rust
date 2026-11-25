# file_updater pure helpers (S-024 M10, P1-33.2) — test matrix

Test files: `crates/obs-file-updater/src/file_updater.rs`
Issue: WP-34 (P1-33.2)

Pure helpers of `shared/file-updater` in `crates/obs-file-updater`
(S-024 M10): the path joining (`get_path`) and the ETag header-line
cleaning. The update algorithm (version comparison, copy/fetch flows)
is bridged in `obs-ffi::abi_file_updater`. The module has no hashes
(P1-33.2 scope note).

| Area                                                                                                                                                                                                                     | Pinned by test     | Conformance vector                                                        |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------ | ------------------------------------------------------------------------- |
| **`get_path` (S-024 §4.10)**                                                                                                                                                                                             |                    |                                                                           |
| `dir` + `/` + `file` when `dir` is non-empty and has no trailing `/`/`\`; trailing-slash dirs and Windows `\` join directly; empty dir → just `file`; no normalisation (`.`/`..` kept)                                   | `get_path_vectors` | `helpers/fu-get-path`                                                     |
| **`clean_etag_header` (S-024 §4.10)**                                                                                                                                                                                    |                    |                                                                           |
| only lines starting exactly `"ETag: "` count; value truncated at the first `\r` then `\n`; an empty value is ignored but `"ETag: \r\n"` stores the empty string (the non-empty check is on the pre-truncation remainder) | `etag_forms`       | `helpers/fu-etag`, `helpers/fu-etag-forms`, `helpers/fu-etag-meta-noetag` |
