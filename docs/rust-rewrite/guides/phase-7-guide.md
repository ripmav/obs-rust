# Phase 7 guide: retire C/C++ and release

Read [00-developer-primer.md](00-developer-primer.md) first. Tasks: [../phase-7-retire-c.md](../phase-7-retire-c.md); subtasks: [../subtasks/phase-7-subtasks.md](../subtasks/phase-7-subtasks.md).

## What this phase is (and is not)

It is mostly *deleting*, *build system replacement*, and *stabilising what third parties build on*. No new functionality. It starts only when each deletion has a documented precondition (parity checklist, two releases on the new default, no open blocking bugs).

## Deletion order and dependencies

```
libobs/ + libobs-{opengl,d3d11,metal,winrt}/   ← one PR (P7-01): the backends are the C fallback and go with libobs;
                                                after wgpu default for two clean releases, no open regressions,
                                                fallback usage below the agreed threshold (P7-01.1)
frontend/                                      ← after the P6-48.1 parity checklist is 100% and beta feedback is closed
plugins/<name>                                 ← each when its Rust port shipped; one plugin per commit, FFI leftovers in P7-03.3
deps/{blake2,json11,glad,w32-pthreads,libcaption}  ← after the last consumer is gone; libdshowcapture stays (D-19)
shared/*                                       ← per the P7-05.1 mapping; hook-DLL modules stay with the C game-capture code (D-08)
test/                                          ← after the P7-25.1 supersession table; same PR wave as P7-01…P7-05
                                                (cmocka links the C libobs that P7-01 removes)
```
Keep a git tag per deletion (`last-c-libobs`, `last-cpp-frontend`, `last-c-plugin-<name>`, `last-c-deps`, `last-c-shared`, `last-c-test`) and list them in `docs/rust-rewrite/retired.md` so history is easy to find.

## What must remain C/C++ (documented leftovers)

Decided per item (D-08, D-09, D-10, D-14, D-19); record owner and review date in P7-03.3:
- `plugins/win-capture/graphics-hook` and `inject-helper`: injected into third-party processes (smallest dependency surface, anti-cheat constraints).
- `plugins/obs-browser` CEF glue (Chromium Embedded Framework is C++).
- DeckLink and AJA vendor SDK glue.
- `libdshowcapture` (submodule) for DirectShow.
- macOS camera extension host (`plugins/mac-virtualcam/src`) if it must be Swift/ObjC.
- Any C shim required by variadic/`va_list` APIs unless the plugin ABI drops them (primer §5.5).

## Build system replacement (P7-06…09)

Today: CMake presets + `cmake/` helpers + dependency preset that downloads prebuilt Qt/FFmpeg/CEF etc. (`cmake/common/buildspec_common.cmake`, `cmake/macos/buildspec.cmake`, `cmake/windows/buildspec.cmake`; Linux uses system packages — there is no root `buildspec.json` in this checkout). Target: `cargo xtask` for build/package/test, with native dependencies pinned and fetched by a script. Things that are easy to forget:
- Installer/packaging inputs live in `build-aux/` (Flatpak manifest `com.obsproject.Studio.json`, Steam build files), `cmake/{windows,macos,linux}/` (CPack generators: Windows portable ZIP, Linux DEB/TGZ, FreeBSD; macOS bundle layout, `Info.plist`, entitlements, export options) and `additional_install_files/` (Windows install-tree layout, P6-42.1) — port the logic, not just the build. There is no NSIS in this checkout.
- Version numbers: `libobs/cmake/obs-version.cmake` and `cmake/common/versionconfig.cmake` (version derived from git tags unless `OBS_VERSION_OVERRIDE` is set) define the OBS version; `LIBOBS_API_VER` lives in `libobs/obs-config.h`; keep them single-sourced.
- Code signing and notarisation steps are part of the release pipeline (`.github/workflows/`: `build-project`, `push`, `pr-pull`, `publish`, `sign-windows`, `scheduled`, `analyze-project`, `check-format`, `dispatch`).

## Public interfaces to freeze (P7-10, P7-11)

1. **C ABI**: after the last C consumers are gone, it becomes an opt-in compatibility feature. Publish the list of supported `LIBOBS_API_VER`s and the deprecation timeline (ADR).
2. **Native Rust plugin API** (`obs-plugin-api`): semver-checked (`cargo-semver-checks`). Provide a migration guide covering each plugin type with a C and a Rust version side by side (use `gain_filter` as the filter example, the x264 encoder from `obs-x264`, and the FLV/MP4 outputs from `obs-outputs`).
3. **Scripting API** (Lua/Python): names and behaviour must match the SWIG-generated bindings; run all documented examples.
4. **Frontend API**: the 99 exported functions and 44 events of `frontend/api/obs-frontend-api.h` remain supported.

## Release engineering

- Release candidates with crash-rate tracking by signature; go/no-go criteria written before RC1.
- Reproducible build checks (two builds → same hash), SBOM, signed artifacts.
- Security: external audit of `unsafe`, network parsing (RTMP, WHIP, WebSocket), updater, importers, OAuth token handling, scripting; continuous fuzzing of all parsers (list in P7-19.1).
- Performance acceptance (P7-20): compare against the last C tag on the P7-20.1 reference setups, on the machine spec'd in `docs/rust-rewrite/benchmarks.md` (P0-17.4).

## Pitfalls

- Deleting `libobs/` removes the headers third-party plugin authors compile against. Ship the C headers (generated or frozen copies) in the SDK or the SDK must tell authors what to use instead.
- `docs/sphinx` documents the C API; plugin authors will search it. Redirect, do not just delete.
- The updater and installers must handle a user upgrading from the last C version directly (settings, plugins, disabled-modules list, portable mode).
- Keep `last-*` tags reachable (protected branches or release branches) for emergency security fixes.

## Verification checklist

- [ ] A clean clone builds all release artifacts with `cargo xtask package` on each OS.
- [ ] Upgrade test from the last C release to the first Rust release preserves profiles, collections, plugins and hotkeys.
- [ ] Third-party plugin matrix (P4-36) re-run against the release candidate.
- [ ] Security audit closed; fuzzers running in CI.
- [ ] Documentation and migration guide published.
