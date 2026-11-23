# E50 — Services, auth, updater

**Milestone:** M8 — Phase 6: frontend (section E)
**Subtasks:** [phase-6-subtasks.md](../rust-rewrite/subtasks/phase-6-subtasks.md) § E

**Why:** service accounts, auth and self-updating.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E50S1** P6-29 OAuth framework | Port OAuth framework to Rust (`AuthListener.hpp`, `OAuth.hpp`) | code received | open |
| **E50S2** P6-30 YouTube integration | Port YouTube integration to Rust (`OBSYoutubeActions.hpp`) | UI bookend passes | open |
| **E50S3** P6-31 Streaming services UI | Port Streaming services UI to Rust (`OBSBasicSettings_Stream.c`) | spec S-026; vectors: rtmpservices/; list matches C | open |
| **E50S4** P6-32 Updater | Port Updater to Rust (`manifest.h`, `http.c`) | local server | open |
| **E50S5** P6-33 Plugin manager | Port Plugin manager to Rust (`PluginManager.hpp`) | UI bookend passes | open |
| **E50S6** P6-34 Virtual camera config | Port Virtual camera config to Rust (`OBSBasicVCamConfig.hpp`) | UI bookend passes | open |
| **E50S7** P6-35 Debug dialogs | Port Debug dialogs to Rust (`OBSIdianPlayground.hpp`) | decision recorded | open |
