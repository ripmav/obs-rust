# E1 — Filesystem & platform paths (WP-30, P1-07)

**Milestone:** M1 — Phase 1: code & recipe

Filesystem & platform paths (WP-30, P1-07)
**Why:** the next unported `libobs/util` area in roadmap order; unblocks CMake swap-set growth.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E1S1** Open/rename/mkdir (P1-07.1) | port `os_fopen`, `os_renamedir`, `os_mkdir`, `os_remove_path`, `os_rename_path`, `os_stat` semantics | [platform/](https://github.com/ripmav/obs-rust/tree/main/test/conformance/scripts/platform) conformance vectors pass against the Rust build; `apple/` files handled per P1-14.1 inventory | open |
| **E1S2** Dir iteration & glob (P1-07.2) | `os_opendir/readdir/closedir`, `os_glob`/`os_globfree` per S-023 §5.6 | sorted `ls`/`glob` results byte-equal C (vectors exist in `platform/`); regression test proves the port **copies the caller's path** (S-023 D-5 — the reference keeps the pointer, a UAF) | open |
| **E1S3** Config/data paths (P1-07.3) | `os_get_config_path`, `os_get_program_data_path`, `os_get_abs_path(_ptr)`, `os_get_free_disk_space` + Flatpak portal/dbus helpers | Flatpak path test passes; Q-11 rule tested (`os_get_config_path_ptr(NULL/"")` ends with `/`, the buffer form does not); `utilrest/` dbus vectors | open |
| **E1S4** CMake swap-set extension (P1-07.4, code half) | `OBS_ENABLE_RUST=ON` excludes the ported platform files | `ctest` green with the option ON, with the option provided as the in-repo patch file `test/conformance/obs-enable-rust.patch` (D3, decided 2026-11-23); cbindgen half is owned by E2S1 | open |

> D3 decided 2026-11-23 (Option 1): the `OBS_ENABLE_RUST` CMake change ships as a patch file inside
> this repo (`test/conformance/obs-enable-rust.patch`), applied by the CI/nightly jobs — the rewrite
> owns its own build integration. The patch is regenerated against the 33.0.1 pin at WP-30 start; an
> optional fork PR against `obsproject/obs-studio` stays possible later.
