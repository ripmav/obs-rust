# E41 — Platform capture and audio

**Milestone:** M7 — Phase 5: plugins (section C)
**Subtasks:** [phase-5-subtasks.md](../rust-rewrite/subtasks/phase-5-subtasks.md) § C

**Why:** the largest plugin group: capture and audio backends for all three OSes.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E41S1** P5-23 `linux-pipewire` | Port `linux-pipewire` to Rust (`linux-pipewire.c`) | spec S-056; vectors: pipewire/; docs/tests/plugin-linux-pipewire-inventory.md committed; S2 passes | open |
| **E41S2** P5-24 `linux-capture` | Port `linux-capture` to Rust (`linux-capture.c`) | spec S-049; vectors: linuxcapture/; docs/tests/plugin-linux-capture-inventory.md committed; S2 passes | open |
| **E41S3** P5-25 `linux-v4l2` | Port `linux-v4l2` to Rust (`linux-v4l2.c`) | spec S-049; vectors: linuxinputs/; docs/tests/plugin-linux-v4l2-inventory.md committed; S2 passes | open |
| **E41S4** P5-26 `linux-pulseaudio` | Port `linux-pulseaudio` to Rust (`linux-pulseaudio.c`) | spec S-049; vectors: linuxinputs/; docs/tests/plugin-linux-pulseaudio-inventory.md committed; S2 passes | open |
| **E41S5** P5-27 `linux-alsa` | Port `linux-alsa` to Rust (`linux-alsa.c`) | spec S-049; docs/tests/plugin-linux-alsa-inventory.md committed; S2 passes | open |
| **E41S6** P5-28 `linux-jack` | Port `linux-jack` to Rust (`linux-jack.c`) | spec S-049; docs/tests/plugin-linux-jack-inventory.md committed; S2 passes | open |
| **E41S7** P5-29 `oss-audio`, `sndio` | Port `oss-audio`, `sndio` to Rust (`oss-audio.c`, `sndio.c`) | docs/tests/plugin-oss-audio-inventory.md + docs/tests/plugin-sndio-inventory.md committed; S2 passes | open |
| **E41S8** P5-30 `win-wasapi` | Port `win-wasapi` to Rust (`plugin-main.c`) | spec S-051; vectors: wincapture/; docs/tests/plugin-win-wasapi-inventory.md committed; S2 passes | open |
| **E41S9** P5-31 `win-capture` | Port `win-capture` to Rust (`plugin-main.c`) | spec S-051; vectors: wincapture/; docs/tests/plugin-win-capture-inventory.md committed; S2 passes | open |
| **E41S10** P5-32 `win-dshow` | Port `win-dshow` to Rust (`dshow-plugin.c`) | spec S-051, S-081; vectors: wincapture/; docs/tests/plugin-win-dshow-inventory.md committed; S2 passes | open |
| **E41S11** P5-33 `libobs-winrt` | Port `libobs-winrt` to Rust (`winrt-capture.c`, `winrt-capture.h`) | window capture via WGC | open |
| **E41S12** P5-34 `mac-capture` | Port `mac-capture` to Rust (`plugin-main.c`) | spec S-052; vectors: maccapture/; docs/tests/plugin-mac-capture-inventory.md committed; S2 passes | open |
| **E41S13** P5-35 `mac-avcapture` | Port `mac-avcapture` to Rust (`plugin-main.m`, `plugin-main.h`) | spec S-052; vectors: maccapture/; docs/tests/plugin-mac-avcapture-inventory.md committed; S2 passes | open |
| **E41S14** P5-36 `mac-syphon` | Port `mac-syphon` to Rust (`plugin-main.c`) | docs/tests/plugin-mac-syphon-inventory.md committed; S2 passes | open |
| **E41S15** P5-37 `mac-virtualcam` | Port `mac-virtualcam` to Rust (`plugin-main.m`, `MachProtocol.h`) | spec S-052; vectors: maccapture/ (virtualcam); docs/tests/plugin-mac-virtualcam-inventory.md committed; module loads | open |
| **E41S16** P5-38 `mac-videotoolbox` | Port `mac-videotoolbox` to Rust (`encoder.c`) | spec S-052; vectors: maccapture/; docs/tests/plugin-mac-videotoolbox-inventory.md committed; S2 passes | open |
