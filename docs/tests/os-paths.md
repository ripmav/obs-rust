# os_paths (S-023 §5.6, P1-07.3) — test matrix

WP-30 (P1-07.3 config/data paths), crate `crates/obs-util`, module
`os_paths` (`src/os_paths.rs`). Spec: S-023 §5.6 (R74–R81, Q-11). The
tests encode the committed C reference vectors
`test/conformance/scripts/platform/25,26,33`; the vectors are the
oracle. R80's "neither `XDG_CONFIG_HOME` nor `HOME` set" case is fatal
(`bcrash`, the C `bcrash("Could not get $HOME\n")`): the crash half is
pinned by vector 26's `try config_path` lines (a crash in the child
process), the path-building half by the env-var cases below; the
`os_paths::set_platform_fatal_hook` seam lets the obs-ffi side route the
fatal to the C `bcrash`.

| Area                                                                                                                                                                                                                                                                                                                                 | Pinned by test      | Conformance vector                  |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------- | ----------------------------------- |
| **os_get_program_data_path(\_ptr) (R81)**                                                                                                                                                                                                                                                                                            |                     |                                     |
| buffer form = `snprintf(dst, size, "/usr/local/share/<name>")`: full path rc, truncation (rc is the full text length, buffer holds the prefix), NULL/empty name -> no suffix, ptr form with `None`/`""` leaves the trailing `/`                                                                                                      | `program_data_path` | `26-config-paths` (`program_data*`) |
| **os_get_abs_path(\_ptr) (R79)**                                                                                                                                                                                                                                                                                                     |                     |                                     |
| resolves to the `realpath` of the same path (temp dirs may contain links); symlinks resolved; missing / empty / NULL path -> 0; NULL buffer -> 0 (a measurement needs a buffer); size 0 writes nothing and returns the full length; truncation NUL-terminates and returns the full length; the ptr form gives `None` on missing/NULL | `abs_path`          | `25-abs-path-cwd`                   |
| **os_get_free_disk_space (R75)**                                                                                                                                                                                                                                                                                                     |                     |                                     |
| non-zero on `/`, 0 on a missing path, 0 on NULL                                                                                                                                                                                                                                                                                      | `free_disk_space`   | `33-system-contracts`               |
| **os_get_config_path(\_ptr) (R80, Q-11)**                                                                                                                                                                                                                                                                                            |                     |                                     |
| `XDG_CONFIG_HOME` set (even empty) wins; else `HOME/.config`; name appended with `/` when non-empty; the ptr form ALWAYS appends the separator before the name, so NULL/empty name leaves a trailing slash (Q-11), the buffer form does not; env vars restored by a drop guard                                                       | `config_path`       | `26-config-paths`                   |
