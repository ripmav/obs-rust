# Decisions needed (user / maintainer input)

Blocking decisions for the Rust rewrite, collected from the plan, the phase files and the
coverage audit of 2026-11-01. **The work below may start before each decision is answered**:
every entry names the default that will be used until someone says otherwise. If a default is
wrong, the cost is bounded by the entry's "Blocked" column.

Legend: **Blocked** = tasks that wait on the answer. **Default** = what the plan does meanwhile.

## A. Strategy

| ID | Decision | Blocked | Default (proceed with this) |
|---|---|---|---|
| D-01 | **GUI toolkit**: `cxx-qt` (keep Qt6, migrate `.ui` forms) vs `Slint` / `egui` / `iced` / `Tauri` | P0-04 (prototype), all Phase 6 tasks, `shared/qt` + `shared/properties-view` porting | `cxx-qt`. P0-04 prototypes the alternatives; Phase 6 task wording changes if the winner differs (P0-04.5 updates the phase files in the same PR) |
| D-02 | **C plugin ABI compatibility** a hard requirement for the first Rust release? | P0-02, P4-34…P4-37, P7-10 | Yes — the C ABI (`obs.h` & friends) is frozen until Phase 7 and served by `obs-ffi` from Phase 1 onward |
| D-03 | **Graphics**: `wgpu` as the one backend, dropping native GL/D3D11/Metal after parity? | P0-03, P3-13…P3-23, P7-01 | Yes, with the legacy backends kept selectable (`--graphics wgpu\|legacy`) as fallback until parity (P3-22 adapters) |
| D-04 | **Platforms / vendor SDKs in scope for the first all-Rust release** | P5-39…P5-43, P7-23 | Linux + Windows + macOS, x86_64 + arm64; vendor SDKs (NVENC, QSV/AMF, VideoToolbox, DeckLink, AJA, CEF) remain FFI shims, never "rewritten" |
| D-05 | **C-ABI deprecation timeline** after Phase 7 | P7-10.3 | Announce with SDK 1.0, remove after 3 Rust releases |
| D-06 | **Native plugin API transport**: `cdylib` + proc-macro vs `abi_stable` vs WASM | P5-46, P7-11, P7-12 | Native `cdylib` + `#[obs_module]` proc-macro (simplest, no codegen layer); WASM stays research (P7-12) |
| D-07 | **License of the rewrite** (GPL-2.0-or-later assumed by `clean-room/PROCESS.md`) and the crate allow-list | P0-05, P0-10 | GPL-2.0-or-later; `deny.toml` allow-list = MIT/Apache-2.0/BSD/ISC/Zlib/MPL-2.0 per review (P0-10.1) |

## B. Scope and platform code that stays C/C++

| ID | Decision | Blocked | Default |
|---|---|---|---|
| D-08 | **win-capture game-capture hook DLL** (D3D8/9/10/11/12, DXGI, GL, Vulkan hooks) — port to Rust or keep C/C++ forever? | P5-31.7, P7-03.3 | Keep C/C++: it is injected into third-party games and is ABI-sensitive; the host side is Rust |
| D-09 | **`obs-browser` CEF glue** stays C++? | P5-45, P7-03.3 | Yes; only the surrounding plugin is ported |
| D-10 | **DeckLink / AJA SDK glue** stays C++? | P5-42/43, P7-03.3 | Yes (vendor SDKs ship C++/COM interfaces) |
| D-11 | **`obs-vst`** — port the VST2 host to Rust or keep C++? | P5-21.6 | Keep C++ behind the plugin ABI (editor-window embedding per OS is the hard part; low value for the risk) |
| D-12 | **BSD support tier** (`oss-audio`, `sndio`, FreeBSD crash handler, `LoaderPaths_BSD`) — full support or best-effort? | P5-29, P6-44, P6-33.3 | Best-effort: plugins ported, no BSD job in the required-CI matrix |
| D-13 | **Legacy D3D11/Metal backends**: spec + port, or keep as C behind the P3-22 adapter until they are deleted in P7-01? | P3-22, P7-01, spec backlog (no `S-0xx` exists for D3D11/Metal; only OpenGL has S-057) | Keep as C adapters, delete in P7-01. No new specs — wgpu is the path forward |
| D-14 | **`mac-virtualcam` DAL plug-in** (`src/dal-plugin/`, legacy CoreMediaIO DAL) — remove now that the system extension exists, or keep both? | P5-37 | Keep the DAL plug-in building until the extension flow is the default, then drop it in the port (record removal in P5-37.3) |
| D-34 | **Windows installer**: does the Rust build get a real installer (MSIX/NSIS), or parity with the C build, which ships a portable ZIP only (`CPACK_GENERATOR ZIP`, `cmake/windows/cpackconfig.cmake`)? | P6-42, P6-42.1 | Parity: portable ZIP via CPack + code signing + VC runtime handling, no new installer format in 1.0; MSIX/NSIS is a post-1.0 follow-up if requested |

## C. Dependency and library choices (each is a spike, then a one-line ADR note)

| ID | Decision | Blocked | Default |
|---|---|---|---|
| D-15 | **RTMP client**: port `plugins/obs-outputs/librtmp` to Rust or keep it as FFI? | P5-17.4 | Port — S-021/S-031 already specify the protocol and it is network-facing parsing (the memory-safety win worth most in Phase 5) |
| D-16 | **WHIP/WebRTC**: `webrtc-rs` vs FFI to libdatachannel | P5-19.3 | FFI to libdatachannel first (it is what ships today); revisit `webrtc-rs` per P5-19.3 spike result |
| D-17 | **SRT / RIST transport in `obs-ffmpeg-mpegts`**: wrap `srt`/`rist` libraries or port the native C? | P5-15.4/5 (behaviour specified by S-059) | FFI wrappers over the same `srt`/`rist` libraries the C build links; behaviour must match S-059 vectors |
| D-18 | **`deps/libcaption`** (CEA-608/708): port or FFI? | P2-21.1 | FFI (stable, small, well-tested parser); port only if FFI proves awkward |
| D-19 | **`deps/libdshowcapture`** (submodule): port or FFI? | P5-32.3 | FFI (DirectShow COM is the job; the host side is Rust) |
| D-20 | **FreeType text**: `freetype-rs` (metrics-identical) vs `cosmic-text`/`swash` (modern layout)? | P5-05.3 | Spike decides (P5-05.3); default `freetype-rs` because S-038 vectors pin the current glyph metrics |
| D-21 | **Noise suppression**: `nnnoiseless` crate vs keep vendored RNNoise FFI | P5-01.7 | `nnnoiseless` crate (same algorithm, maintained) |
| D-22 | **x264 bindings**: `x264` crate vs hand-rolled FFI shim | P5-07.3 | `x264` crate if current, else FFI shim; bit-exactness test is the gate either way |
| D-23 | **RSA key usage** (`OBSPublicRSAKey.pem`): in C it signs the What's-New feed only (P6-23.2) — the updater verifies patches by blake2b manifest hashes + Authenticode (VC redist), no key signature. Keep RSA for the feed or move to ed25519? | P6-23.2, P6-32.2 | Keep RSA (same key, same verification path); ed25519 only if D-07 review wants it |
| D-24 | **MSRV policy** | P0-05.2 | Track latest stable, bump within one release cycle |
| D-25 | **FTL output** (`ftl-stream.c`, vendored FTL): keep or drop? | P5-18.3 | Drop — verified 2026-11-03: no FTL sources or CMake references remain in the 33.0.1 checkout (removed upstream); FTL has had no releases since 2021 |
| D-26 | **`deps/w32-pthreads`**: absorbed by the Rust threading crate (P1-05) and deleted (P7-04) — confirm nobody needs it out-of-tree | P1-05.3, P7-04.2 | Absorb + delete |

## D. Spec backlog — work to schedule, not forks in the road

These are not forks: the plan proceeds, but a spec author must be assigned. Numbers are from the
2026-11-01 audit (`tools/task-coverage.py` + `tools/coverage.py`).

| ID | Item | Why it matters | Suggested |
|---|---|---|---|
| D-27 | **Threading primitives spec** (`os_event`, `os_sem`, `pthread_mutex` wrappers — old backlog S-017) | P1-05 is one of the first implementation tasks and has no clean-room spec; P1-05 subtasks therefore read the C | Write S-061 before starting P1-05 |
| D-28 | **`obs-websocket` spec** (protocol is documented upstream; no S-xxx yet) | P5-20 is XL and the single most-integrated third-party plugin | Write S-062 from the in-tree generated spec `plugins/obs-websocket/docs/generated/protocol.json` (5.7.6: 147 requests / 60 events / 7 enums, each with `rpcVersion`/`initialVersion`/`deprecated`) + vectors from the C build |
| D-29 | **Python scripting backend spec** — 25 `pyscript` conformance vectors already exist, but S-039 explicitly excludes the backends and S-060 covers Lua only | P6-37.2 would otherwise be implemented by reading the C | Extend S-039/S-060 into a backend pair, or write S-063 from the existing `pyscript` vectors |
| D-30 | **Wayland hotkey-platform spec** — S-058 explicitly does not cover `obs-nix-wayland.c` (42 KB: `wl_keyboard` + xkbcommon hotkey platform; no portal code — the portals live in `platform-nix-dbus.c`/`platform-nix-portal.c`) | P4-32.2 would otherwise read the C | Extend S-058 or write S-064 (public Wayland + xkbcommon protocols) |
| D-31 | **Audio-monitoring win32/osx/null specs** — S-055 covers PulseAudio only | P2-15.2…P2-15.4 would otherwise read the C | Small spec (S-065) or accept reading the C (it is thin platform glue) |
| D-32 | **`win-wasapi` spec** — S-051 covers window matching/hooks/vqueue, not WASAPI enumeration/capture | P5-30 would otherwise read the C | Add to S-051 or write S-066 |
| D-33 | **Vendor-SDK behavioural specs** (obs-nvenc test helpers, `coreaudio-encoder`, `nv-filters`, `vlc-video`, `decklink`, `aja`) | Mostly "kept as FFI" per D-04/D-10; specs would be behavioural tables | Only if a port is attempted; otherwise document the FFI surface in the plugin task |

## How to unblock

Answer in a `docs:` PR (or commit) by editing the **Default** column to the chosen value and
setting the row's status; the task files then need no change for A/C entries (they already
encode the default). B entries flip the named tasks. D entries become spec-writing tasks and
should be added to `docs/rust-rewrite/specs/README.md` as S-061 onward.
