# Phase 7 subtasks

Parent file: [../phase-7-retire-c.md](../phase-7-retire-c.md). Each subtask has **Files**, **Steps**, **Done when**.

**Delete recipe** (used below): (1) `grep -r` for includes/link references and fix them; (2) tag the last commit that contains the code; (3) delete in one PR with the CMake/CI references; (4) update docs; (5) full CI on 3 OSes; (6) keep the tag listed in `docs/rust-rewrite/retired.md`.

Verified: the top-level directories are `libobs`, `libobs-opengl`, `libobs-d3d11`, `libobs-metal` (Swift), `libobs-winrt`, `frontend`, `plugins`, `deps`, `shared`, `test`, `cmake`, `build-aux`, `additional_install_files`, `docs` (sphinx). Ownership outside this phase: packaging `build-aux/`/`additional_install_files/` by P6-42…P6-44 (Windows install-tree layout P6-42.1), `docs/sphinx` by P7-14.1; `cmake/` and the root CMake files are removed by P7-06.3. `deps/` contains `blake2`, `glad`, `json11`, `libcaption`, `libdshowcapture`, `w32-pthreads` (`libnsgif` is not in `deps/`: it is `libobs/graphics/libnsgif/` and goes away with P7-01). `shared/` contains `bpm`, `file-updater`, `happy-eyeballs`, `ipc-util`, `media-playback`, `obs-d3d8-api`, `obs-hook-config`, `obs-inject-library`, `obs-scripting`, `obs-shared-memory-queue`, `obs-tiny-nv12-scale`, `opts-parser`, `properties-view`, `qt`. Line/file counts are measured in the read-only C reference checkout. The 9 CI workflows: `analyze-project.yaml`, `build-project.yaml`, `check-format.yaml`, `dispatch.yaml`, `pr-pull.yaml`, `publish.yaml`, `push.yaml`, `scheduled.yaml`, `sign-windows.yaml`.

## A. Removal

### P7-01 Delete C libobs and backends
- [ ] **P7-01.1 Preconditions** — `wgpu` default for ≥2 releases, no open P0/P1 graphics bugs, legacy fallback usage < agreed threshold from telemetry or issue count.
  - Files: release-branch tags (two releases with wgpu default), the open P0/P1 graphics issue list, fallback-usage telemetry/issue count.
  - Steps: 1. confirm wgpu default shipped in two release branches; 2. verify no open P0/P1 graphics bug; 3. record fallback usage against the agreed threshold; 4. sign off in `docs/rust-rewrite/retired.md`.
  - Done when: checklist signed off.
- [ ] **P7-01.2 Tag** `last-c-libobs`.
  - Files: none (git tag only).
  - Steps: 1. `git tag last-c-libobs` on the last commit containing `libobs/`; 2. push the tag.
  - Done when: tag exists and is listed in `docs/rust-rewrite/retired.md`.
- [ ] **P7-01.3 Delete** `libobs/`, `libobs-opengl/`, `libobs-d3d11/`, `libobs-metal/`, `libobs-winrt/`, plus the Rust legacy backends `crates/obs-d3d11` + `crates/obs-graphics-metal` (D-13), update `CMakeLists.txt`/CI per delete recipe.
  - Files: `libobs/` (265 files / 94,777 lines, incl. `libobs/graphics/libnsgif/` 6 / 3,389), `libobs-opengl/` (27 / 8,984), `libobs-d3d11/` (16 / 7,340), `libobs-metal/` (37 / 8,530), `libobs-winrt/` (5 / 745) — 350 files / 120,376 lines; root `CMakeLists.txt` lines `add_subdirectory(libobs)`, `add_subdirectory(libobs-d3d11)`, `add_subdirectory(libobs-winrt)`, `add_subdirectory(libobs-opengl)`, `add_subdirectory(libobs-metal)` (+ `add_subdirectory(test/test-input)`, see Pitfalls).
  - Steps: 1. extract `libobs/util/windows/obfuscate.{c,h}` (54 lines; `OBS::obfuscate`, `libobs/cmake/os-windows.cmake:1-5`) into the kept win-capture tree — `inject-helper` builds against it (the C `graphics-hook` is retired by P5-31.7, D-08); 2. `git rm -r libobs libobs-opengl libobs-d3d11 libobs-metal libobs-winrt`; 3. remove the five `add_subdirectory(libobs*)` lines from root `CMakeLists.txt`; 4. `git grep -n "libobs" -- CMakeLists.txt '*.cmake' .github build-aux additional_install_files` — any hit outside the P7-03.3 keep set is fixed in this PR; 5. list the tag in `docs/rust-rewrite/retired.md`.
  - Pitfalls: `libnsgif` lives in `libobs/graphics/libnsgif/`, not `deps/` — it goes here, not in P7-04. `plugins/CMakeLists.txt` hard-fails without the `libobs` target, and root `CMakeLists.txt`'s `add_subdirectory(test/test-input)` links the C libobs — delete `test/` in the same PR wave (P7-25). The `OBS_ENABLE_RUST` compat feature (option from P0-08.2 in root `CMakeLists.txt` + `CMakePresets.json` variant) dies with the C fallback — remove option and preset variant in this PR wave; the Rust-only build becomes the only path. Version single-source: `libobs/cmake/obs-version.cmake` + `LIBOBS_API_VER` in `libobs/obs-config.h` must move to xtask/`build.rs` (P7-06.2) in this PR.
  - Spec: S-042 (obscore), S-043 (graphics-api), S-058 (obsvideo) — the ported areas that replace the C core.
  - Vectors: `test/conformance/scripts/obscore/`, `graphics/`, `glbackend/`, `obsvideo/`.
  - Done when: build and tests green, and the no-reference grep of step 4 is clean.

### P7-02 Delete C++ frontend
- [ ] **P7-02.1 Preconditions** — parity checklist (P6-48.1) 100%, beta feedback closed.
  - Files: `docs/rust-rewrite/parity-checklist.md` (P6-48.1), P6-47 beta feedback log.
  - Steps: 1. bring the P6-48.1 parity checklist to 100%; 2. close all open P6-47 beta items; 3. sign off.
  - Done when: checklist signed off.
- [ ] **P7-02.2 Tag** `last-cpp-frontend`.
  - Files: none (git tag only).
  - Steps: 1. `git tag last-cpp-frontend` on the last commit containing `frontend/`; 2. push the tag.
  - Done when: tag exists and is listed in `docs/rust-rewrite/retired.md`.
- [ ] **P7-02.3 Delete** `frontend/` including `.ui` forms (themes/locale moved to the Rust app's data dir first), per delete recipe.
  - Files: `frontend/` (845 files / 216,895 lines: 36 `.ui` forms, `frontend/data/locale/` 77 `.ini` + `frontend/data/locale.ini`, `frontend/data/themes/` 183 files); root `CMakeLists.txt` lines `option(ENABLE_FRONTEND ...)` + `add_subdirectory(frontend)`; `frontend/api/CMakeLists.txt` (defines `obs-frontend-api`) and its install/symlink rules in `cmake/linux/helpers.cmake:93-97` and `cmake/macos/helpers.cmake:140-142`; `.github/workflows/check-format.yaml` job `qt-xml-validator`.
  - Steps: 1. move `frontend/data/locale/` (+`locale.ini`) and `frontend/data/themes/` into the Rust app's data dir; 2. `git rm -r frontend`; 3. delete the `ENABLE_FRONTEND` option + `add_subdirectory(frontend)` from root `CMakeLists.txt` and the `obs-frontend-api` install/symlink rules from the two cmake helpers; 4. remove the `qt-xml-validator` job from `check-format.yaml`; 5. `git grep -n "frontend" -- CMakeLists.txt '*.cmake' .github build-aux additional_install_files` — packaging refs in `build-aux/`/`additional_install_files/` (P6-42…P6-44) may remain.
  - Pitfalls: `deps/blake2` and `deps/json11` are consumed **only** by `frontend/` (`frontend/updater/CMakeLists.txt:31`, `frontend/cmake/feature-macos-update.cmake:5-6`, `frontend/CMakeLists.txt:15-16`) — they can go only in P7-04, after this wave.
  - Done when: installers still contain locale and themes, CI green on 3 OSes, tag listed in `docs/rust-rewrite/retired.md`.

### P7-03 Delete C plugins
- [ ] **P7-03.1 List** — plugin → status table from P5-50.
  - Files: new "P7-03 plugin table" section in this file (41 rows).
  - Steps: 1. list all 41 `plugins/<dir>` entries (counts below); 2. mark each ported (P5-49 table) or kept (P5-50 FFI-shim table); 3. commit the table.
  - Done when: table committed with 41 rows; every kept dir names its decision ID.
- [ ] **P7-03.2 Delete** each fully ported plugin dir in its own commit, tagged `last-c-plugin-<name>` on the last commit containing it first (so `git revert` per plugin works); CMake/CI references in the same PR per delete recipe.
  - Files: `plugins/` (41 dirs, 3,443 files / 322,646 lines) minus the P7-03.3 keep boundary; one `add_core_module(<name> ...)` line per plugin in `plugins/CMakeLists.txt` (plus the `check_obs_browser()` / `check_obs_websocket()` macros); CI: `push.yaml` and `pr-pull.yaml` jobs with `checkGlob: plugins/win-capture/data/*.json` and `checkGlob: plugins/rtmp-services/data/*.json`.
  - Steps: 1. per plugin: `git tag last-c-plugin-<name>` on the last commit containing it; 2. `git rm -r plugins/<name>` + its `add_core_module` line in `plugins/CMakeLists.txt` in the same commit; 3. remove the per-plugin CI jobs (win-capture and rtmp-services checkGlob in `push.yaml`/`pr-pull.yaml`); 4. `git grep -n "<name>" -- CMakeLists.txt .github` — no hits outside the keep set; 5. full CI on 3 OSes.
  - Pitfalls: `decklink-captions` (6 / 373), `decklink-output-ui` (8 / 884), `aja-output-ui` (46 / 1,184) sit next to the kept SDK glue (`plugins/decklink`, `plugins/aja`) — delete each only if the P7-03.1 table marks it ported (P5-42/43). `plugins/obs-websocket` is a git submodule: `git submodule deinit` + `git rm` the dir **and** its `.gitmodules` entry in the same commit (the kept `plugins/obs-browser` is one too — leave its entry alone).
  - Done when: only the P7-03.3 leftovers remain under `plugins/` and the tags are listed in `docs/rust-rewrite/retired.md`.
- [ ] **P7-03.3 FFI leftovers** — document `libdshowcapture` (INTERFACE lib; header surface `src/dshowcapture.hpp`), win-capture `inject-helper` and `get-graphics-offsets` (the C `graphics-hook` DLL is not a leftover — P5-31.7 ports it per D-08), and the `mac-virtualcam` extension host, with owner, next review date and the decision IDs (D-08, D-14, D-19). decklink/aja leave no OBS C/C++ (D-10: glue is Rust; only the vendored `decklink-sdk/` trees remain, D-04); obs-vst likewise (D-11: host is Rust, vendored `vst_header/` stays as FFI input); obs-browser likewise (D-09: glue + page processes are Rust over the CEF C API).
  - Files (keep boundary — these stay, everything else under `plugins/` goes via P7-03.2): the vendored `plugins/decklink/{linux,mac,win}/decklink-sdk/` trees (vendor SDK headers + Linux COM dispatch; D-04/D-10 — the glue is Rust); the vendored `plugins/obs-vst/vst_header/` (VST2 ABI headers; D-11 — the host is Rust); `plugins/win-capture/graphics-hook/` (19 / 8,345), `plugins/win-capture/inject-helper/` (2 / 170), `plugins/win-capture/get-graphics-offsets/` (7 / 698) (D-08); `plugins/mac-virtualcam/src/` (95 / 6,656; D-14 — `src/dal-plugin/` already dropped per D-14 in P5-37.3); `deps/libdshowcapture/` (D-19, stays via P7-04.2). Go: the `plugins/win-capture` root sources/`cmake/`/`data/` and `plugins/mac-virtualcam` non-`src/` parts with the ported plugin.
  - Steps: 1. commit the keep list above with owner, next review date and decision ID per item; 2. verify every remaining `plugins/<dir>` is in the list (`ls plugins/` vs table); 3. record the retired dirs in `docs/rust-rewrite/retired.md`.
  - Pitfalls: the keep set is a build boundary — the kept win-capture helpers' CMakeLists pull the kept `shared/` modules (P7-05.2) and `OBS::obfuscate` (extracted in P7-01.3); deleting any of those breaks the helper builds. `plugins/mac-virtualcam/src/dal-plugin/` is **not** kept — D-14 removes it (P5-37.3).
  - Done when: list committed and every `plugins/` dir not in it is deleted.

### P7-04 Delete vendored deps
- [ ] **P7-04.1 Scan** — for each `deps/*` entry, `grep` consumers.
  - Files: `deps/` — `blake2` (6 / 779), `json11` (5 / 1,062), `glad` (10 / 23,223), `libcaption` (31 / 7,465), `w32-pthreads` (417 / 68,102), `libdshowcapture` (keep, D-19).
  - Steps: 1. `git grep -n "blake2\|json11\|\bglad\b\|libcaption\|w32-pthreads" -- '*.cmake' CMakeLists.txt .github`; 2. record each consumer and the wave that deletes it (P7-01 `libobs`, P7-02 `frontend`, P7-03 plugins, P7-05 `shared`); 3. commit the table.
  - Done when: scan table committed; every consumer is scheduled before this wave.
- [ ] **P7-04.2 Delete** unused ones (`blake2`, `json11`, `glad`, `w32-pthreads`, `libcaption` if ported), tagged `last-c-deps` first, per delete recipe; keep `libdshowcapture` (D-19: FFI, listed in P7-03.3) with its nested `capture-device-support` submodule (elgatosf; referenced by `deps/libdshowcapture/CMakeLists.txt`).
  - Files: `deps/blake2/` (6 / 779), `deps/json11/` (5 / 1,062), `deps/glad/` (10 / 23,223), `deps/w32-pthreads/` (417 / 68,102), `deps/libcaption/` (31 / 7,465, if ported) — 469 files / 100,631 lines incl. `libcaption` (438 / 93,166 without); keep `deps/libdshowcapture/` (45 / 9,189 incl. the `src/` submodule).
  - Steps: 1. `git tag last-c-deps` on the last commit containing `deps/blake2`; 2. `git rm -r deps/blake2 deps/json11 deps/glad deps/w32-pthreads [deps/libcaption]`; 3. `git grep -n "blake2\|json11\|\bglad\b\|w32-pthreads\|libcaption" -- '*.cmake' CMakeLists.txt .github build-aux` — no hits; 4. CI green on 3 OSes; 5. list the tag in `docs/rust-rewrite/retired.md`.
  - Pitfalls: the nested `capture-device-support` submodule under `deps/libdshowcapture/src/` (uninitialized; `deps/libdshowcapture/CMakeLists.txt:33-40` and `src/CMakeLists.txt:77-80,116`) is untouched. `w32-pthreads` is safe only after P7-01+P7-03+P7-05 (all its consumers gone — `libobs/cmake/os-windows.cmake`, `shared/{file-updater,obs-scripting}`, `frontend`, `plugins/{vlc-video,win-capture,obs-ffmpeg,win-dshow}`); the kept hook trio does not reference it.
  - Done when: no CMake/CI references remain, CI green on 3 OSes, tag listed in `docs/rust-rewrite/retired.md`.

### P7-05 Delete `shared/`
- [ ] **P7-05.1 Mapping table** — `bpm`, `file-updater`, `happy-eyeballs`, `ipc-util`, `media-playback`, `obs-d3d8-api`, `obs-hook-config`, `obs-inject-library`, `obs-scripting`, `obs-shared-memory-queue`, `obs-tiny-nv12-scale`, `opts-parser`, `properties-view`, `qt` → Rust crate or "still used by hook DLL".
  - Files: the 14 modules (141 files / 23,323 lines) with measured CMake consumers: `bpm` (4 / 740; `frontend:24`, `plugins/obs-outputs:16`), `file-updater` (3 / 575; `rtmp-services:15`, `win-capture:21`), `happy-eyeballs` (3 / 935; `obs-outputs:8`), `ipc-util` (6 / 421; `win-capture:17`, `win-capture/graphics-hook:7`), `media-playback` (11 / 2,878; `plugins/obs-ffmpeg`), `obs-d3d8-api` (5 / 3,342; `graphics-hook:15`, `get-graphics-offsets:4`), `obs-hook-config` (4 / 220; `graphics-hook:19`, `get-graphics-offsets:8`), `obs-inject-library` (3 / 160; `win-capture:8`, `inject-helper:8`), `obs-scripting` (26 / 7,005; `frontend-tools:69`), `obs-shared-memory-queue` (3 / 262; `win-dshow:13`, `win-dshow/virtualcam-module:60`), `obs-tiny-nv12-scale` (3 / 239; `win-dshow:9`, `virtualcam-module:56`), `opts-parser` (3 / 120; `obs-nvenc:13`, `obs-outputs:12`, `obs-x264:6`), `properties-view` (8 / 2,831; `aja-output-ui:20`, `decklink-output-ui:17`, `frontend-tools:13`), `qt` (52 / 3,595; `frontend:28`, `frontend-tools:17,21`).
  - Steps: 1. map each module to its Rust replacement (e.g. `media-playback` → the S-036 crate) or "kept by the hook trio"; 2. commit the table in this section.
  - Done when: table committed; every module named with its consumers or keep reason.
- [ ] **P7-05.2 Delete** the migrated ones per delete recipe, tagged `last-c-shared` first; keep those still used by the C `inject-helper`/`get-graphics-offsets` (or by the C hook fallback during the P5-31.7 port window, D-08).
  - Files: the 10 migrated modules (123 files / 19,180 lines) per the P7-05.1 table; keep exactly 4 modules (18 files / 4,143 lines): `shared/obs-d3d8-api/`, `shared/obs-hook-config/`, `shared/ipc-util/`, `shared/obs-inject-library/`.
  - Steps: 1. `git tag last-c-shared` on the last commit containing `shared/bpm`; 2. `git rm -r shared/<migrated>` per the table; 3. `git grep -n "shared/" -- '*.cmake' CMakeLists.txt` — hits only in `plugins/win-capture/{graphics-hook,inject-helper,get-graphics-offsets}/`; 4. CI green on 3 OSes; 5. list the tag in `docs/rust-rewrite/retired.md`.
  - Pitfalls: `graphics-hook/CMakeLists.txt:7-19` and `inject-helper/CMakeLists.txt:8` `add_subdirectory` all four kept modules — deleting any of them breaks the kept hook DLL build.
  - Done when: only the P7-05.1-mapped leftovers remain, CI green on 3 OSes, tag listed in `docs/rust-rewrite/retired.md`.

### P7-25 Delete the C test harness (added)
- [ ] **P7-25.1 Preconditions** — prove the four cmocka suites (`test_bitstream.c`, `test_darray.c`, `test_os_path.c`, `test_serializer.c`) and `test/test-input` are superseded: every exercised behaviour has a Rust test or a `test/conformance/` area.
  - Files: new coverage table in `docs/tests/`.
  - Steps: 1. map `test_bitstream.c` (`util/bitstream.h`) → area `serial` (S-010), `test_darray.c` → `darray` (S-005), `test_os_path.c` (`util/platform.h`) → `platform` (S-023), `test_serializer.c` → `serial` (S-010); 2. map the `test-input` plugin checks (sync/audio-buffering/filter/sinewave/random sources) → `obssource` (S-040) + the Rust crates' own tests; 3. record every behaviour with its Rust test or conformance script; 4. commit the table.
  - Spec: S-005, S-010, S-023, S-040.
  - Vectors: `test/conformance/scripts/darray/`, `serial/`, `platform/`, `obssource/`.
  - Done when: coverage table committed in `docs/tests/`.
- [ ] **P7-25.2 Tag** `last-c-test`.
  - Files: none (git tag only).
  - Steps: 1. `git tag last-c-test` on the last commit containing `test/cmocka/`; 2. push the tag.
  - Done when: tag exists and is listed in `docs/rust-rewrite/retired.md`.
- [ ] **P7-25.3 Delete** `test/` per the delete recipe (CMake/CI references in the same PR).
  - Files: `test/cmocka/` (5 files / 186 lines), `test/test-input/` (11 / 1,013), `test/win/` (2 / 245), `test/osx/` (2 / 204), `test/CMakeLists.txt` (6) — 21 files / 1,654 lines; **keep `test/conformance/`** (59 areas — the replacement); root `CMakeLists.txt` line `add_subdirectory(test/test-input)`.
  - Steps: 1. `git rm -r test/cmocka test/test-input test/win test/osx test/CMakeLists.txt` — do not touch `test/conformance/`; 2. remove `add_subdirectory(test/test-input)` from root `CMakeLists.txt`; 3. `git grep -n "test-input\|cmocka" -- CMakeLists.txt .github` — no hits; 4. full CI on 3 OSes.
  - Pitfalls: `test/conformance/` lives **inside** `test/` — a blanket `git rm -r test` destroys the replacement suite. Same PR wave as P7-01…P7-05: cmocka links the C libobs that P7-01 removes.
  - Done when: build green on 3 OSes and `test/` holds only `test/conformance/`.

## B. Build system

### P7-06 Cargo-first build
- [ ] **P7-06.1 xtask** — commands `build`, `package`, `install`, `test`, `lint`, `fmt`, `deps` (fetch native libs).
  - Files: `xtask/` crate (new) + `.cargo/config.toml` (`alias xtask = "run --package xtask --"`).
  - Steps: 1. implement the seven commands; 2. port the dependency-preset download logic from `cmake/{common,macos,windows}/buildspec*.cmake` into `xtask deps` (pins from P7-07.1); 3. replace the documented CMake commands in the build docs (P7-14.3) and wire CI to `cargo xtask build`/`test`.
  - Done when: replaces documented CMake commands.
- [ ] **P7-06.2 C++ shims** — `build.rs` with `cc`/`cmake` for the leftovers from P7-03.3.
  - Files (shim boundary — what the shims compile): `plugins/win-capture/{inject-helper,get-graphics-offsets}/` (the C `graphics-hook` is retired by P5-31.7, D-08; obs-browser has no shim — D-09 Rust glue over the CEF C API), `plugins/mac-virtualcam/src/` (Swift/ObjC), `deps/libdshowcapture/` (INTERFACE lib, `dshowcapture.hpp`), plus the extracted `obfuscate.{c,h}` (P7-01.3) and the four kept `shared/` modules (P7-05.2).
  - Steps: 1. add a `build.rs` (or `cmake` crate) per leftover; 2. re-home `OBS::obfuscate` (its definition lived in `libobs/cmake/os-windows.cmake:1-5`) and the four kept `shared/` modules under the win-capture shim; 3. build on 3 OSes.
  - Pitfalls: `inject-helper` links `OBS::obfuscate` and the kept `shared/` modules — after P7-01/P7-05 those definitions only exist inside the shim; a missing one breaks the inject-helper build.
  - Done when: builds on 3 OSes.
- [ ] **P7-06.3 Remove CMake** — `CMakeLists.txt`, `CMakePresets.json`, `cmake/`; keep `build-aux/` packaging. Only after P7-01…P7-05 and P7-25 are done and P7-06.2 builds the leftovers.
  - Files: root `CMakeLists.txt` (37 lines), `CMakePresets.json` (227 lines), `cmake/` (72 files / 23,380 lines); keep `build-aux/` (packaging, P6-42…P6-44) and `additional_install_files/` (P6-42.1).
  - Steps: 1. verify P7-01…P7-05 + P7-25 done and P7-06.2 builds the leftovers; 2. `git rm CMakeLists.txt CMakePresets.json` + `git rm -r cmake`; 3. convert all 9 workflows to `cargo xtask` (CMake jobs in `build-project.yaml` — `macos-build`, `ubuntu-build`, `flatpak-build`, `windows-build` — plus `push.yaml`, `pr-pull.yaml`, `scheduled.yaml`, `analyze-project.yaml`, `dispatch.yaml`, `publish.yaml`, `sign-windows.yaml`); 4. `git grep -n "cmake" -- .github build-aux` — only `build-aux/` packaging refs may remain.
  - Done when: CI uses xtask only.

### P7-07 Dependency management
- [ ] **P7-07.1 Pins** — FFmpeg, x264, CEF, SRT, RIST, Qt (if kept), with versions and SHA-256 (the WebRTC stack is webrtc-rs — Rust crates, pinned by Cargo.lock, not this manifest, D-16).
  - Files: `xtask` `deps` manifest (new).
  - Steps: 1. collect the current versions from `cmake/*/buildspec*.cmake` + `deps/`; 2. record version + SHA-256 per native dep in the manifest; 3. `xtask deps` verifies every hash on fetch.
  - Done when: every pinned dep has a version and SHA-256 and fetch verifies.
- [ ] **P7-07.2 Vendoring/mirrors** policy and offline build docs.
  - Files: `docs/rust-rewrite/vendor-policy.md` (new) + `xtask deps` mirror logic.
  - Steps: 1. write the vendoring/mirror policy; 2. write the offline-build doc; 3. run an offline build from the pinned manifest.
  - Done when: an offline build from the pinned manifest succeeds on 3 OSes.

### P7-08 Cross-compilation and caches
- [ ] **P7-08.1 Windows x64/arm64**.
  - Files: CI build matrix (windows jobs in `build-project.yaml`/`push.yaml`), `xtask build`.
  - Steps: 1. add x64 + arm64 cross-compile jobs; 2. build all artifacts for both.
  - Done when: both targets build in CI.
- [ ] **P7-08.2 macOS universal**.
  - Files: CI macos jobs, `xtask package`.
  - Steps: 1. build the universal (x86_64 + arm64) binary via lipo; 2. smoke-test it.
  - Done when: universal binary builds and runs in CI.
- [ ] **P7-08.3 Linux x86_64/aarch64**.
  - Files: CI linux jobs, `xtask build`.
  - Steps: 1. add x86_64 + aarch64 jobs; 2. build all artifacts for both.
  - Done when: both targets build in CI.
- [ ] **P7-08.4 `sccache`/`nextest`** timings: CI < 30 min.
  - Files: CI cache config, `cargo-nextest` wiring.
  - Steps: 1. add `sccache` + `cargo-nextest` to all build jobs; 2. record timings.
  - Done when: full CI under 30 minutes on all 3 OSes.

### P7-09 Reproducible and signed builds
- [ ] **P7-09.1 Determinism** — `SOURCE_DATE_EPOCH`, path remapping; two builds hash-equal.
  - Files: `xtask build` (env + `-C remap-path-prefix`).
  - Steps: 1. honour `SOURCE_DATE_EPOCH` and remap paths in every step; 2. build twice from clean checkouts and compare hashes.
  - Done when: two clean builds are hash-equal on 3 OSes.
- [ ] **P7-09.2 SBOM** — `cargo-cyclonedx` + native deps list.
  - Files: `xtask package` (SBOM step).
  - Steps: 1. generate the CycloneDX SBOM; 2. append the P7-07.1 native deps list.
  - Done when: SBOM ships with the RC.
- [ ] **P7-09.3 Provenance** — attestations and signature publishing.
  - Files: `publish.yaml` (attestation + signing jobs).
  - Steps: 1. add build attestations; 2. publish signatures alongside artifacts.
  - Done when: attestations and signatures ship with the RC.

## C. Compatibility layer and SDK

### P7-10 C ABI as opt-in compat feature
- [ ] **P7-10.1 Feature** — move C exports of `obs-ffi` behind `compat-c-abi` (default on until deprecation).
  - Files: `obs-ffi` `Cargo.toml` (new `compat-c-abi` feature) + the C-export module.
  - Steps: 1. gate the C exports on the feature (default on); 2. load a legacy C plugin with the feature on; 3. build once with it off.
  - Done when: a legacy C plugin loads with the feature on; build passes with it off.
- [ ] **P7-10.2 Supported headers** — document API versions supported.
  - Files: SDK docs (supported `LIBOBS_API_VER` list).
  - Steps: 1. list the supported `LIBOBS_API_VER`s; 2. publish with the SDK docs.
  - Done when: the supported-header list is published.
- [ ] **P7-10.3 Deprecation ADR** — record the D-05 timeline (announce with SDK 1.0, remove after 3 releases).
  - Files: new ADR (`docs/adr/`), citing D-05 in `docs/rust-rewrite/DECISIONS-NEEDED.md`.
  - Steps: 1. write the ADR with the D-05 timeline; 2. publish the timeline with the SDK 1.0 release notes.
  - Pitfalls: D-05 is binding — announce with SDK 1.0, remove the C ABI only after 3 Rust releases; the ADR must name the removal release.
  - Done when: ADR merged and timeline published with the SDK 1.0 notes.

### P7-11 Native plugin SDK 1.0
- [ ] **P7-11.1 Freeze review** — public API audit.
  - Files: `obs-plugin-api` crate (public surface).
  - Steps: 1. audit every public item; 2. freeze the API (semver 1.0).
  - Done when: `obs-plugin-api` is frozen.
- [ ] **P7-11.2 Semver checks** — `cargo-semver-checks` in CI.
  - Files: CI workflow (new semver job).
  - Steps: 1. add the `cargo-semver-checks` job; 2. verify a deliberate breaking change fails it.
  - Done when: CI fails on a breaking API change.
- [ ] **P7-11.3 Docs/template** published.
  - Files: docs.rs publication + `cargo-generate` template (new).
  - Steps: 1. publish API docs; 2. ship the template; 3. generate a plugin from it.
  - Done when: the template generates a working plugin.
- [ ] **P7-11.4 Migration guide** — side-by-side C vs Rust for source, filter, encoder, output, service.
  - Files: `docs/rust-rewrite/migration-c-to-rust.md` (new).
  - Steps: 1. write side-by-side examples for all five plugin types; 2. use the guide's three running examples: `gain_filter` (from `obs-filters`), the x264 encoder (`obs-x264`), FLV/MP4 outputs (`obs-outputs`); 3. publish.
  - Done when: guide published with a C and a Rust version of each type.

### P7-12 WASM sandbox research
- [ ] **P7-12.1 Spike** — load a WASM component source plugin (wasmtime).
  - Files: spike crate (new) + wasmtime.
  - Steps: 1. build a WASM component source; 2. load it via wasmtime in a headless test.
  - Done when: one WASM source plugin renders a frame.
- [ ] **P7-12.2 Overhead** — measure per-frame cost.
  - Files: benchmark for the spike crate.
  - Steps: 1. measure per-frame cost vs the native path; 2. record the numbers.
  - Done when: per-frame overhead numbers committed.
- [ ] **P7-12.3 ADR** — findings.
  - Files: new ADR (`docs/adr/`).
  - Steps: 1. write the findings ADR (adopt/defer/drop).
  - Done when: findings ADR committed.

### P7-13 Scripting API stability
- [ ] **P7-13.1 Corpus** — run all documented script examples + community scripts.
  - Files: script corpus (new, from the `docs/sphinx` examples + community scripts).
  - Steps: 1. collect every documented Lua/Python example; 2. run the corpus against the Rust bindings.
  - Done when: the corpus runs green against the Rust bindings.
- [ ] **P7-13.2 Differences** documented.
  - Files: `docs/rust-rewrite/scripting-differences.md` (new).
  - Steps: 1. record every behavioural difference found in P7-13.1.
  - Done when: differences documented.

## D. Documentation and community

### P7-14 Rewrite docs
- [ ] **P7-14.1 API reference** — rustdoc + cbindgen headers; replaces `docs/sphinx`.
  - Files: rustdoc site + cbindgen-generated headers; `docs/sphinx` (redirect).
  - Steps: 1. generate the rustdoc + cbindgen reference; 2. redirect `docs/sphinx` to it (do not just delete — plugin authors search it).
  - Done when: the rustdoc + cbindgen reference replaces `docs/sphinx` (with a redirect).
- [ ] **P7-14.2 Contributor docs** — README, CONTRIBUTING, CODESTYLE.md (Rust).
  - Files: `README.md`, `CONTRIBUTING.md`, `CODESTYLE.md`.
  - Steps: 1. rewrite all three for the Rust tree; 2. merge.
  - Done when: docs merged and consistent with the Rust layout.
- [ ] **P7-14.3 Build docs** per OS.
  - Files: build docs (one per OS).
  - Steps: 1. document `cargo xtask` (P7-06.1) per OS; 2. verify a clean clone follows each doc.
  - Done when: a clean clone follows the per-OS build docs.

### P7-15 Contributor onboarding
- [ ] **P7-15.1 Architecture overview** + crate map diagram.
  - Files: `docs/rust-rewrite/architecture.md` (new) + crate map diagram.
  - Steps: 1. write the overview; 2. draw the crate map; 3. publish.
  - Done when: overview and diagram published.
- [ ] **P7-15.2 Good first issues** list.
  - Files: `docs/rust-rewrite/good-first-issues.md` (new).
  - Steps: 1. collect label-matched issues; 2. publish the list.
  - Done when: list published.

### P7-16 Plugin author outreach
- [ ] **P7-16.1 Announcement**.
  - Files: announcement post (new).
  - Steps: 1. write the announcement (timeline, D-05 deprecation, migration guide); 2. publish.
  - Done when: announcement is out.
- [ ] **P7-16.2 Compat runner** — tool to test a third-party plugin on the Rust core.
  - Files: runner tool (new; xtask subcommand or standalone crate).
  - Steps: 1. implement the runner (load a C plugin, run a smoke scenario); 2. pass it on three known third-party plugins from the P4-36 matrix.
  - Done when: the runner passes on three known third-party plugins.
- [ ] **P7-16.3 Support window**.
  - Files: support-window page (new).
  - Steps: 1. publish the migration support window dates.
  - Done when: support window published.

### P7-17 Translator workflow
- [ ] **P7-17.1 Verify** locale flow (Crowdin) unchanged.
  - Files: Crowdin config, `.ini` flow (77 locale `.ini` moved in P7-02.3).
  - Steps: 1. trigger a real Crowdin update; 2. verify it flows to `.ini` in the new data dir.
  - Done when: a real Crowdin locale update flows to `.ini` and ships with the format unchanged.
- [ ] **P7-17.2 Docs update**.
  - Files: translator docs (new/updated).
  - Steps: 1. update the translation workflow docs for the new data dir.
  - Done when: docs committed.

## E. Hardening and release

### P7-18 Security audit
- [ ] **P7-18.1 Scope/auditor** selection.
  - Files: audit scope doc (new).
  - Steps: 1. write the scope (unsafe blocks, network parsing, updater, OAuth); 2. select the auditor.
  - Done when: scope committed and auditor picked.
- [ ] **P7-18.2 Threat model** — plugins, network inputs, updater, importers, OAuth, scripting.
  - Files: `docs/rust-rewrite/threat-model.md` (new).
  - Steps: 1. model the six attack surfaces; 2. commit.
  - Done when: threat model committed.
- [ ] **P7-18.3 Fix findings**.
  - Files: per finding.
  - Steps: 1. fix or accept each finding with a recorded reason.
  - Done when: findings fixed or accepted.
- [ ] **P7-18.4 `SECURITY.md`** update.
  - Files: `SECURITY.md`.
  - Steps: 1. update reporting contacts + scope.
  - Done when: `SECURITY.md` is current.

### P7-19 Fuzzing in CI
- [ ] **P7-19.1 Targets** — `obs-data` JSON, NAL/OBU parsers, effect parser, FLV/RTMP, MP4 input, importers, config, websocket requests, ffmpeg-mux pipe.
  - Files: fuzz-target crates (new, one per target).
  - Steps: 1. add a fuzzer target per parser listed; 2. run each against its committed corpus.
  - Done when: every target builds and fuzzes.
- [ ] **P7-19.2 OSS-Fuzz** integration.
  - Files: OSS-Fuzz project config (new).
  - Steps: 1. register the targets with OSS-Fuzz; 2. verify reports land.
  - Done when: OSS-Fuzz builds and reports on all targets.
- [ ] **P7-19.3 Triage** process and corpus.
  - Files: triage doc (new) + committed corpus.
  - Steps: 1. write the triage process; 2. commit regression corpora.
  - Done when: documented triage process and committed corpus.

### P7-20 Performance acceptance
- [ ] **P7-20.1 Setups** — 3 reference per OS.
  - Files: `docs/rust-rewrite/benchmarks.md` (P0-17.4 machine spec) + 3 reference setups per OS.
  - Steps: 1. define the 3 reference setups per OS on the spec'd machine; 2. commit them.
  - Done when: 3 setups per OS committed.
- [ ] **P7-20.2 Runs** on `last-c-*` tags vs release candidate.
  - Files: benchmark runs (P0-17 harness).
  - Steps: 1. run the `last-c-*` tags and the RC on every setup; 2. record CPU, RAM, startup, GPU time, dropped frames.
  - Done when: runs recorded for all setups.
- [ ] **P7-20.3 Report** `final-benchmarks.md`.
  - Files: `docs/rust-rewrite/final-benchmarks.md` (new).
  - Steps: 1. write the report with pass/fail vs the acceptance criteria.
  - Done when: report published and the release passes the acceptance criteria.

### P7-21 Unsafe budget report
- [ ] **P7-21.1 `cargo-geiger`**.
  - Files: geiger summary (committed).
  - Steps: 1. run `cargo-geiger` over the workspace; 2. commit the summary.
  - Done when: summary committed.
- [ ] **P7-21.2 Justify/remove** outside allowed crates.
  - Files: every `unsafe` block outside `*-sys`, `ffi`, platform crates.
  - Steps: 1. justify or remove each block; 2. keep the justifications with the code.
  - Done when: every `unsafe` block outside the allowed crates is justified or removed.

### P7-22 Release candidates
- [ ] **P7-22.1 RC1**.
  - Files: RC build via `cargo xtask package`.
  - Steps: 1. cut RC1; 2. start crash-rate tracking by signature.
  - Done when: RC1 published and the crash-rate dashboard live.
- [ ] **P7-22.2 RC2** after triage.
  - Files: RC2 build.
  - Steps: 1. triage RC1 crashes/regressions; 2. cut RC2.
  - Done when: RC2 published with RC1 triage closed.
- [ ] **P7-22.3 RC3** and go/no-go.
  - Files: RC3 build + go/no-go record.
  - Steps: 1. cut RC3; 2. record the go/no-go decision with the crash-rate data (criteria written before RC1).
  - Done when: go/no-go recorded with crash-rate data.

### P7-23 1.0 release
- [ ] **P7-23.1 Notes** and migration guide.
  - Files: release notes + migration guide (P7-11.4).
  - Steps: 1. write the 1.0 notes; 2. link the migration guide.
  - Done when: notes and migration guide live.
- [ ] **P7-23.2 Artifacts/packages** (Flatpak, winget, Homebrew, installers).
  - Files: channel repos + `cargo xtask package` artifacts (P6-42…P6-44 packaging).
  - Steps: 1. publish artifacts to Flatpak, winget, Homebrew and the installer channels.
  - Done when: 1.0 artifacts published on all channels.
- [ ] **P7-23.3 Website** download pages.
  - Files: website download pages.
  - Steps: 1. point the download pages at 1.0.
  - Done when: download pages live.

### P7-24 Retrospective
- [ ] **P7-24.1 Feedback** from team and plugin authors.
  - Files: collected feedback (new doc).
  - Steps: 1. collect feedback from team + plugin authors (P7-16 window).
  - Done when: feedback collected and committed.
- [ ] **P7-24.2 Write** `retrospective.md`.
  - Files: `docs/rust-rewrite/retrospective.md` (new).
  - Steps: 1. write what worked and what did not.
  - Done when: `retrospective.md` is committed.
- [ ] **P7-24.3 Close** `rust-rewrite` label.
  - Files: none (GitHub label).
  - Steps: 1. close the `rust-rewrite` label when no issues remain.
  - Done when: label closed.
