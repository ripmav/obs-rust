# E47 — Core UI components

**Milestone:** M8 — Phase 6: frontend (section B)
**Subtasks:** [phase-6-subtasks.md](../rust-rewrite/subtasks/phase-6-subtasks.md) § B

**Why:** docks, scene/source tree, mixer — the daily OBS UX.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E47S1** P6-07 Preview widget | Port Preview widget to Rust (`OBSQTDisplay.hpp`) | renders on all | open |
| **E47S2** P6-08 Scene list | Port Scene list to Rust (`SceneTree.hpp`) | UI bookend passes | open |
| **E47S3** P6-09 Source tree | Port Source tree to Rust (`SourceTreeModel.hpp`, `SourceTreeItem.hpp`) | UI bookend passes | open |
| **E47S4** P6-10 Audio mixer | Port Audio mixer to Rust (`VolumeMeter.hpp`) | spec S-032; vectors: audiomix/; pixel compare with reference frames | open |
| **E47S5** P6-11 Transitions dock | Port Transitions dock to Rust (`OBSBasic_Transitions.c`) | spec S-029; vectors: transitions/; UI bookend passes | open |
| **E47S6** P6-12 Controls dock | Port Controls dock to Rust (`OBSBasicControls.c`, `OBSBasic_Streaming.c`) | UI bookend passes | open |
| **E47S7** P6-13 Stats dock | Port Stats dock to Rust (`OBSBasicStats.c`) | values match a live C session | open |
| **E47S8** P6-14 Source toolbars | Port Source toolbars to Rust (`ColorSourceToolbar.c`, `TextSourceToolbar.c`) | UI bookends pass | open |
| **E47S9** P6-15 Dock framework | Port Dock framework to Rust (`OBSDock.hpp`) | spec S-048; state persists | open |
| **E47S10** P6-16 Properties view | Port Properties view to Rust (`properties-view.hpp`, `properties-view.moc.hpp`) | spec S-015; vectors: props/; model reflects all 13 property kinds (OBS_PROPERTY_* minus OBS_PROPERTY_INVALID) | open |
| **E47S11** P6-17 Media controls | Port Media controls to Rust (`MediaControls.hpp`) | spec S-036; vectors: mediaplayback/; UI bookend passes | open |
| **E47S12** P6-18 Accessibility pass | Port Accessibility pass to Rust (`AccessibleAlignmentSelector.c`, `AccessibleAlignmentCell.c`) | report | open |
