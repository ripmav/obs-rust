# E40 — Network and muxing

**Milestone:** M7 — Phase 5: plugins (section B)
**Subtasks:** [phase-5-subtasks.md](../rust-rewrite/subtasks/phase-5-subtasks.md) § B

**Why:** network outputs and muxing: RTMP (D-15), WHIP/WebRTC (D-16), ffmpeg mux, websocket on tokio-tungstenite.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E40S1** P5-11 `obs-ffmpeg` software encoders | Port `obs-ffmpeg` software encoders to Rust (`obs-ffmpeg.c`) | spec S-025, S-047; vectors: encopts/, ffsources/; docs/tests/plugin-obs-ffmpeg-inventory.md committed; S2 passes | open |
| **E40S2** P5-12 VAAPI | Port VAAPI to Rust (`obs-ffmpeg-vaapi.c`, `obs-ffmpeg.c`) | spec S-050; vectors: hwencoders/ (scripts 50–57); inventory rows committed; S2 passes | open |
| **E40S3** P5-13 NVENC (FFmpeg legacy) | Port NVENC (FFmpeg legacy) to Rust (`obs-ffmpeg-nvenc.c`, `obs-ffmpeg.c`) | spec S-050; vectors: hwencoders/ (scripts 40–45); inventory rows committed; S2 passes | open |
| **E40S4** P5-14 `ffmpeg-mux` helper | Port `ffmpeg-mux` helper to Rust (`obs-ffmpeg-mux.c`) | spec S-037, S-046; vectors: ffmuxproto/; inventory rows committed; S2 passes | open |
| **E40S5** P5-15 `obs-ffmpeg` outputs | Port `obs-ffmpeg` outputs to Rust (`obs-ffmpeg-output.c`, `obs-ffmpeg.c`) | spec S-046, S-059; vectors: ffoutputs/; inventory rows committed; S2 passes | open |
| **E40S6** P5-16 `obs-ffmpeg` media source | Port `obs-ffmpeg` media source to Rust (`obs-ffmpeg-source.c`, `obs-ffmpeg.c`) | spec S-036, S-047; vectors: mediaplayback/; inventory rows committed; S2 passes | open |
| **E40S7** P5-17 `obs-outputs` RTMP/FLV | Port `obs-outputs` RTMP/FLV to Rust (`obs-outputs.c`) | spec S-021, S-031, S-045; vectors: flv/; docs/tests/plugin-obs-outputs-inventory.md committed; S2 passes | open |
| **E40S8** P5-18 `obs-outputs` FTL, null, MP4 | Port `obs-outputs` FTL, null, MP4 to Rust (`obs-outputs.c`) | spec S-022; vectors: mp4/; inventory rows committed; S2 passes | open |
| **E40S9** P5-19 `obs-webrtc` | Port `obs-webrtc` to Rust (`obs-webrtc.c`, `whip-output.c`) | docs/tests/plugin-obs-webrtc-inventory.md committed; S2 passes | open |
| **E40S10** P5-20 `obs-websocket` (submodule, tag 5.7.6) | Port `obs-websocket` (submodule, tag 5.7.6) to Rust (`obs-websocket.c`, `obs-websocket.h`) | docs/tests/plugin-obs-websocket-inventory.md committed; S2 passes | partial — P5-20.1/.2, P5-20.3, P5-20.4, P5-20.5, P5-20.6, P5-20.7, P5-20.8, P5-20.9, P5-20.10/.11 open |
| **E40S11** P5-21 `obs-vst` | Port `obs-vst` to Rust (`obs-vst.c`) | docs/tests/plugin-obs-vst-inventory.md committed; S2 passes | open |
| **E40S12** P5-22 `vlc-video` | Port `vlc-video` to Rust (`vlc-video-plugin.c`, `vlc-video-plugin.h`) | docs/tests/plugin-vlc-video-inventory.md committed; S2 passes | open |
